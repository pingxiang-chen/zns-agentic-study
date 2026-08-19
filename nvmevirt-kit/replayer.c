/*
 * replayer.c — replay the agentic-LLM offloading trace (gen_trace.py format)
 * against a REAL block device: an NVMeVirt conventional-SSD namespace or an
 * NVMeVirt ZNS namespace (also works on real NVMe / ZNS drives).
 *
 * Conventional mode (-m conv):
 *   - user-space extent allocator over the raw LBA space (first-fit, coalescing),
 *     mirrors the simulator's host layer; DELETE issues BLKDISCARD (TRIM).
 * ZNS mode (-m zns):
 *   - zone-per-session KV blobs, FIFO memlog zones, log-structured index zones
 *     with host compaction; DELETE issues BLKRESETZONE.
 *   - intra-zone writes at QD1 (write-pointer rule); reads at full QD.
 *
 * I/O: O_DIRECT, 128 KiB commands, per-burst QD via POSIX AIO (portable;
 * use taskset to pin if desired). Latencies recorded per class; summary with
 * p50/p90/p99/p99.9 printed and raw CSVs dumped, same format as the simulator.
 *
 * Build: gcc -O2 -pthread -o replayer replayer.c -lrt -lm
 * Usage: sudo ./replayer -t trace.txt -d /dev/nvme1n1 -m conv -o out_conv
 *        sudo ./replayer -t trace.txt -d /dev/nvme2n2 -m zns  -o out_zns
 *   [-s time_scale]  e.g. -s 0.25 replays 4x faster (gaps scaled)
 *   [-q qd]          burst queue depth (default 32)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <aio.h>
#include <time.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#ifdef HAVE_BLKZONED
#include <linux/blkzoned.h>
#endif

typedef uint64_t u64; typedef uint32_t u32;
#define PGSZ 4096ULL
#define CMDSZ (128 * 1024ULL)
#define CMD_PGS (CMDSZ / PGSZ)

static const char *cls_name[] =
 { "kv_read_cmd", "kv_read_op", "kv_write_cmd", "kv_write_op", "memlog_write_cmd", "memlog_read_cmd",
   "index_write_cmd", "index_read_cmd", "resume_burst", "turn_storage_stall" };
enum { C_KVRD, C_KVRD_OP, C_KVWR, C_KVWR_OP, C_MLWR, C_MLRD, C_IXWR, C_IXRD, C_RESUME, C_TURNSTALL, NCLS };
enum { OP_WKV, OP_RKV, OP_WML, OP_RML, OP_WIX, OP_RIX, OP_DEL };
static const char *op_name(uint8_t op) {
    switch (op) {
    case OP_WKV: return "WKV";
    case OP_RKV: return "RKV";
    case OP_WML: return "WML";
    case OP_RML: return "RML";
    case OP_WIX: return "WIX";
    case OP_RIX: return "RIX";
    case OP_DEL: return "DEL";
    default: return "UNKNOWN";
    }
}

static double TSCALE = 1.0;
static int QD = 32, MODE_ZNS = 0, FD = -1;
static int KSTRIPE = 8;      /* -k: parallel zone chains per KV blob (zns mode).
                                Each chain is written strictly sequentially
                                (write-pointer safe); K chains in parallel
                                recover the stripe bandwidth that one QD1
                                zone stream cannot exploit (ZenFS-style). */
static u64 DEV_PGS, ZONE_PGS, N_ZONES;
static u64 IX_PGS, ML_PGS, t_end_ns, meas_start_ns;

/* ---------------- stats ---------------- */
typedef struct { u32 *v; u64 n, cap; pthread_mutex_t mu; } lvec;
static lvec LAT[NCLS];
static void lv_add(int c, u64 ns) {
    lvec *L = &LAT[c];
    pthread_mutex_lock(&L->mu);
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 65536; L->v = realloc(L->v, L->cap * 4); }
    u64 us = (ns + 999) / 1000; if (us > 0xFFFFFFFF) us = 0xFFFFFFFF;
    L->v[L->n++] = (u32)us;
    pthread_mutex_unlock(&L->mu);
}
static u64 now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ULL + ts.tv_nsec; }
static u64 T0;

/* ---------------- conv extent allocator (mirrors simulator) ---------------- */
typedef struct fext { u64 start, len; struct fext *next; } fext_t;
static fext_t *fhead; static pthread_mutex_t fs_mu = PTHREAD_MUTEX_INITIALIZER;
static void fs_free(u64 start, u64 len) {
    pthread_mutex_lock(&fs_mu);
    fext_t **pp = &fhead; fext_t *prev = NULL;
    while (*pp && (*pp)->start < start) { prev = *pp; pp = &(*pp)->next; }
    fext_t *n = malloc(sizeof *n); n->start = start; n->len = len; n->next = *pp; *pp = n;
    if (n->next && n->start + n->len == n->next->start) { fext_t *x = n->next; n->len += x->len; n->next = x->next; free(x); }
    if (prev && prev->start + prev->len == n->start) { prev->len += n->len; prev->next = n->next; free(n); }
    pthread_mutex_unlock(&fs_mu);
}
static int64_t fs_alloc(u64 want, u64 *got) {
    pthread_mutex_lock(&fs_mu);
    if (!fhead) { pthread_mutex_unlock(&fs_mu); return -1; }
    fext_t *e = fhead;
    u64 take = e->len < want ? e->len : want, s = e->start;
    e->start += take; e->len -= take;
    if (!e->len) { fhead = e->next; free(e); }
    pthread_mutex_unlock(&fs_mu);
    *got = take; return (int64_t)s;
}

/* ---------------- objects ---------------- */
typedef struct { u64 lpn_start, npgs; } ext_t;
#define MAXK 16
typedef struct {
    ext_t *ext; int n_ext, cap_ext; u64 pgs;
    /* zns: KSTRIPE independent zone chains. KV data uses ext[] as the                                                                                                                     
        logical-to-physical map because trace appends are not 128 KiB aligned. */ 
    int *zc[MAXK]; int zc_n[MAXK], zc_cap[MAXK]; u64 zc_wp[MAXK]; /* pages in chain's last zone */
    pthread_mutex_t mu;
} obj_t;
static obj_t *objs; static u64 n_objs;
static void obj_add_ext(obj_t *o, u64 s, u64 n) {
    if (o->n_ext && o->ext[o->n_ext-1].lpn_start + o->ext[o->n_ext-1].npgs == s) { o->ext[o->n_ext-1].npgs += n; o->pgs += n; return; }
    if (o->n_ext == o->cap_ext) { o->cap_ext = o->cap_ext ? o->cap_ext * 2 : 8; o->ext = realloc(o->ext, o->cap_ext * sizeof(ext_t)); }
    o->ext[o->n_ext++] = (ext_t){ s, n }; o->pgs += n;
}
static u64 obj_lpn(obj_t *o, u64 off) {
    for (int i = 0; i < o->n_ext; i++) { if (off < o->ext[i].npgs) return o->ext[i].lpn_start + off; off -= o->ext[i].npgs; }
    fprintf(stderr, "obj offset past end\n"); exit(4);
}

/* ---------------- ZNS zone management ---------------- */
static int *zfree; static u64 zf_head, zf_tail, zf_cnt; static pthread_mutex_t z_mu = PTHREAD_MUTEX_INITIALIZER;
static int zns_alloc_zone(void) {
    pthread_mutex_lock(&z_mu);
    if (!zf_cnt) { pthread_mutex_unlock(&z_mu); return -1; }
    int z = zfree[zf_head % N_ZONES]; zf_head++; zf_cnt--;
    pthread_mutex_unlock(&z_mu);
    return z;
}
static void zns_reset_zone(int z) {
#ifdef HAVE_BLKZONED
    struct blk_zone_range r = { .sector = (u64)z * ZONE_PGS * (PGSZ / 512), .nr_sectors = ZONE_PGS * (PGSZ / 512) };
    if (ioctl(FD, BLKRESETZONE, &r) < 0) perror("BLKRESETZONE");
#endif
    pthread_mutex_lock(&z_mu);
    zfree[zf_tail % N_ZONES] = z; zf_tail++; zf_cnt++;
    pthread_mutex_unlock(&z_mu);
}
/* index LSM + memlog FIFO state (single-threaded via ix/ml mutexes) */
static int *ix_zone_of, *ix_slot_of, *zn_live; static int *ixz, n_ixz, ix_open = -1, IX_BUDGET_Z;
static u64 ix_open_wp;
static int *mlz, n_mlz, ml_budget_z, ml_open = -1; static u64 ml_open_wp; static u64 *ml_page_zone;
static pthread_mutex_t ix_mu = PTHREAD_MUTEX_INITIALIZER, ml_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- raw I/O helpers ---------------- */
static void *iobuf(void) { static __thread void *b = NULL; if (!b) { if (posix_memalign(&b, PGSZ, CMDSZ)) exit(9); memset(b, 0xA5, CMDSZ); } return b; }
static void pio(int wr, u64 pg, u64 npg) {          /* synchronous single command */
    void *b = iobuf();
    ssize_t r = wr ? pwrite(FD, b, npg * PGSZ, pg * PGSZ) : pread(FD, b, npg * PGSZ, pg * PGSZ);
    if (r < 0) { perror(wr ? "pwrite" : "pread"); exit(8); }
}

/* burst of commands with QD via POSIX AIO; entries: (pg, npg) */
typedef struct { u64 pg, npg; } io_t;

static void obj_emit_ios(obj_t *o, u64 off, u64 len, io_t *ios, int *nio) {
    for (int i = 0; i < o->n_ext && len; i++) {
        if (off >= o->ext[i].npgs) { off -= o->ext[i].npgs; continue; }
        u64 ext_off = off;
        while (ext_off < o->ext[i].npgs && len) {
            u64 pg = o->ext[i].lpn_start + ext_off;
            u64 n = o->ext[i].npgs - ext_off;
            u64 zrem = ZONE_PGS - (pg % ZONE_PGS);
            if (n > len) n = len;
            if (n > CMD_PGS) n = CMD_PGS;
            if (n > zrem) n = zrem;
            ios[(*nio)++] = (io_t){ pg, n };
            len -= n;
            ext_off += n;
        }
        off = 0;
    }
    if (len) { fprintf(stderr, "obj offset past end\n"); exit(4); }
}

static u64 burst(int wr, io_t *ios, int n, int qd, int cls) {
    u64 t0 = now_ns();
    struct aiocb cbs[256]; void *bufs[256];
    if (qd > 256) qd = 256;
    int issued = 0, done = 0;
    struct aiocb *live[256]; int nlive = 0;
    while (done < n) {
        while (issued < n && nlive < qd) {
            struct aiocb *cb = &cbs[issued % 256]; memset(cb, 0, sizeof *cb);
            if (posix_memalign(&bufs[issued % 256], PGSZ, CMDSZ)) exit(9);
            cb->aio_fildes = FD; cb->aio_buf = bufs[issued % 256];
            cb->aio_nbytes = ios[issued].npg * PGSZ; cb->aio_offset = ios[issued].pg * PGSZ;
            u64 c0 = now_ns();
            if (wr ? aio_write(cb) : aio_read(cb)) { perror("aio"); exit(8); }
            cb->aio_sigevent.sigev_value.sival_ptr = (void *)(uintptr_t)c0;
            live[nlive++] = cb; issued++;
        }
        const struct aiocb *waitlist[256];
        for (int i = 0; i < nlive; i++) waitlist[i] = live[i];
        aio_suspend(waitlist, nlive, NULL);
        for (int i = 0; i < nlive; i++) {
            if (aio_error(live[i]) != EINPROGRESS) {
                aio_return(live[i]);
                u64 c0 = (u64)(uintptr_t)live[i]->aio_sigevent.sigev_value.sival_ptr;
                if (cls >= 0) lv_add(cls, now_ns() - c0);
                free((void *)live[i]->aio_buf);
                live[i] = live[--nlive]; i--; done++;
            }
        }
    }
    return now_ns() - t0;
}

/* ---------------- trace ---------------- */
typedef struct { u64 gap; u32 obj; u64 off, len, line_no; uint8_t op, ts; } op_t;
typedef struct { op_t *ops; u32 n; u64 t0; } sess_t;
static sess_t *sess; static u32 n_sess;
static const char *TRACE_PATH;
static u64 RPROG_INTERVAL = 10000;
static u64 RPROG_TOTAL_OPS, RPROG_TOTAL_LOG_WRITES, RPROG_TOTAL_LOG_PAGES;
static u64 RPROG_ISSUED_OPS, RPROG_ISSUED_LOG_WRITES, RPROG_ISSUED_LOG_PAGES;
static pthread_mutex_t rprog_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int active, code;
    char reason[64];
    u32 sid, op_idx, obj;
    u64 line_no, trace_ns, gap, off, len;
    uint8_t op;
} stop_info_t;
static stop_info_t STOP_INFO;
static pthread_mutex_t stop_mu = PTHREAD_MUTEX_INITIALIZER;

static int stop_requested(void) {
    pthread_mutex_lock(&stop_mu);
    int active = STOP_INFO.active;
    pthread_mutex_unlock(&stop_mu);
    return active;
}

static void request_storage_stop(int code, const char *reason, u32 sid, u32 op_idx, u64 trace_ns, const op_t *op) {
    int first = 0;
    pthread_mutex_lock(&stop_mu);
    if (!STOP_INFO.active) {
        STOP_INFO.active = 1;
        STOP_INFO.code = code;
        snprintf(STOP_INFO.reason, sizeof STOP_INFO.reason, "%s", reason);
        STOP_INFO.sid = sid;
        STOP_INFO.op_idx = op_idx;
        STOP_INFO.trace_ns = trace_ns;
        if (op) {
            STOP_INFO.line_no = op->line_no;
            STOP_INFO.gap = op->gap;
            STOP_INFO.obj = op->obj;
            STOP_INFO.off = op->off;
            STOP_INFO.len = op->len;
            STOP_INFO.op = op->op;
        }
        first = 1;
    }
    pthread_mutex_unlock(&stop_mu);

    if (first) {
        if (op) {
            fprintf(stderr,
                    "storage exhausted: %s at %s:%" PRIu64
                    " session=%u op_index=%u trace_ns=%" PRIu64
                    " op=%s obj=%u off=%" PRIu64 " len=%" PRIu64 "\n",
                    reason, TRACE_PATH ? TRACE_PATH : "(unknown trace)", op->line_no,
                    sid, op_idx, trace_ns, op_name(op->op), op->obj, op->off, op->len);
        } else {
            fprintf(stderr, "storage exhausted: %s before trace replay\n", reason);
        }
    }
}

static int stop_code(void) {
    pthread_mutex_lock(&stop_mu);
    int code = STOP_INFO.active ? STOP_INFO.code : 0;
    pthread_mutex_unlock(&stop_mu);
    return code;
}

static int rprog_is_log_write(const op_t *op) {
    return op && (op->op == OP_WKV || op->op == OP_WML || op->op == OP_WIX);
}

static u64 rprog_atomic_read_u64(u64 *v) {
    return __sync_add_and_fetch(v, 0);
}

static void load_trace(const char *path) {
    FILE *fp = fopen(path, "r"); if (!fp) { perror("trace"); exit(1); }
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == 'H') sscanf(line, "H %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                                   &n_objs, &t_end_ns, &IX_PGS, &ML_PGS, &meas_start_ns);
        else if (line[0] == 'A') n_sess++;
    }
    rewind(fp);
    sess = calloc(n_sess + 1, sizeof(sess_t));
    objs = calloc(n_objs + 2, sizeof(obj_t));
    for (u64 i = 0; i < n_objs + 2; i++) pthread_mutex_init(&objs[i].mu, NULL);
    u32 sid = 0; sess_t *cur = NULL; u32 cap = 0;
    u64 line_no = 0;
    while (fgets(line, sizeof line, fp)) {
        line_no++;
        if (line[0] == 'A') {
            u64 d, t0; sscanf(line, "A %" SCNu64 " %" SCNu64, &d, &t0);
            cur = &sess[sid++]; cur->t0 = t0; cap = 0;
        } else if (line[0] == 'O' && cur) {
            u64 gap, off, len; u32 obj, ts; char kind[6];
            if (sscanf(line, "O %" SCNu64 " %5s %u %" SCNu64 " %" SCNu64 " %u", &gap, kind, &obj, &off, &len, &ts) != 6) continue;
            if (cur->n == cap) { cap = cap ? cap * 2 : 16; cur->ops = realloc(cur->ops, cap * sizeof(op_t)); }
            op_t *o = &cur->ops[cur->n++];
            o->gap = gap; o->obj = obj; o->off = off; o->len = len; o->ts = ts;
            o->line_no = line_no;
            o->op = !strcmp(kind,"WKV")?OP_WKV:!strcmp(kind,"RKV")?OP_RKV:!strcmp(kind,"WML")?OP_WML:
                    !strcmp(kind,"RML")?OP_RML:!strcmp(kind,"WIX")?OP_WIX:!strcmp(kind,"RIX")?OP_RIX:OP_DEL;
        }
    }
    fclose(fp);
}

static void rprog_count_trace(void) {
    RPROG_TOTAL_OPS = 0;
    RPROG_TOTAL_LOG_WRITES = 0;
    RPROG_TOTAL_LOG_PAGES = 0;
    for (u32 s = 0; s < n_sess; s++) {
        RPROG_TOTAL_OPS += sess[s].n;
        for (u32 i = 0; i < sess[s].n; i++) {
            op_t *op = &sess[s].ops[i];
            if (rprog_is_log_write(op)) {
                RPROG_TOTAL_LOG_WRITES++;
                RPROG_TOTAL_LOG_PAGES += op->len;
            }
        }
    }
}

static void rprog_emit(const char *tag, u64 issued_ops,
                       u64 issued_log_writes, u64 issued_log_pages,
                       u32 sid, u32 op_idx, u64 trace_ns, const op_t *op) {
    double elapsed = T0 ? (now_ns() - T0) / 1000000000.0 : 0.0;
    pthread_mutex_lock(&rprog_mu);
    fprintf(stderr,
            "progress[%s]: issued_trace_ops=%" PRIu64 "/%" PRIu64
            " issued_log_writes=%" PRIu64 "/%" PRIu64
            " issued_log_pages=%" PRIu64 "/%" PRIu64
            " elapsed_s=%.3f",
            tag, issued_ops, RPROG_TOTAL_OPS,
            issued_log_writes, RPROG_TOTAL_LOG_WRITES,
            issued_log_pages, RPROG_TOTAL_LOG_PAGES, elapsed);
    if (op) {
        fprintf(stderr,
                " last_session=%u last_op_index=%u last_line=%" PRIu64
                " last_trace_ns=%" PRIu64 " last_op=%s last_obj=%u last_off=%" PRIu64
                " last_len=%" PRIu64,
                sid, op_idx, op->line_no, trace_ns, op_name(op->op), op->obj, op->off, op->len);
    }
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&rprog_mu);
}

static void rprog_note_issued(u32 sid, u32 op_idx, u64 trace_ns, const op_t *op) {
    if (!RPROG_INTERVAL) return;

    u64 issued_ops = __sync_add_and_fetch(&RPROG_ISSUED_OPS, 1);
    u64 issued_log_writes = rprog_atomic_read_u64(&RPROG_ISSUED_LOG_WRITES);
    u64 issued_log_pages = rprog_atomic_read_u64(&RPROG_ISSUED_LOG_PAGES);
    int is_log_write = rprog_is_log_write(op);
    if (is_log_write) {
        issued_log_writes = __sync_add_and_fetch(&RPROG_ISSUED_LOG_WRITES, 1);
        issued_log_pages = __sync_add_and_fetch(&RPROG_ISSUED_LOG_PAGES, op->len);
    }

    int emit = (issued_ops == 1 || issued_ops == RPROG_TOTAL_OPS ||
                issued_ops % RPROG_INTERVAL == 0);
    if (is_log_write) {
        emit = emit || issued_log_writes == RPROG_TOTAL_LOG_WRITES ||
               issued_log_writes % RPROG_INTERVAL == 0;
    }
    if (emit)
        rprog_emit("issued", issued_ops, issued_log_writes, issued_log_pages,
                   sid, op_idx, trace_ns, op);
}

static void rprog_finish(void) {
    if (!RPROG_INTERVAL) return;
    rprog_emit("final",
               rprog_atomic_read_u64(&RPROG_ISSUED_OPS),
               rprog_atomic_read_u64(&RPROG_ISSUED_LOG_WRITES),
               rprog_atomic_read_u64(&RPROG_ISSUED_LOG_PAGES),
               0, 0, 0, NULL);
}

/* ---------------- per-session execution ---------------- */
static int sleep_until(u64 target_rel_ns) {
    u64 t = (u64)((double)target_rel_ns * TSCALE);
    for (;;) {
        if (stop_requested()) return 0;
        u64 now = now_ns() - T0;
        if (t <= now) return 1;
        u64 wait = t - now;
        if (wait > 100000000ULL) wait = 100000000ULL;
        struct timespec ts = { wait / 1000000000ULL, wait % 1000000000ULL };
        nanosleep(&ts, NULL);
    }
}
static int gap_sleep(u64 gap_ns) {
    u64 g = (u64)((double)gap_ns * TSCALE);
    u64 end = now_ns() + g;
    while (g) {
        if (stop_requested()) return 0;
        u64 wait = g > 100000000ULL ? 100000000ULL : g;
        struct timespec ts = { wait / 1000000000ULL, wait % 1000000000ULL };
        nanosleep(&ts, NULL);
        u64 now = now_ns();
        if (now >= end) return 1;
        g = end - now;
    }
    return !stop_requested();
}

/* K parallel zone-chain workers; worker w handles ios[w], ios[w+K], ...
 * so each chain stays ordered while chains proceed in parallel. */
struct sw_arg { io_t *ios; int nio, w, cls; };
static void *sw_worker(void *a_) {
    struct sw_arg *a = a_;
    char *buf; if (posix_memalign((void **)&buf, PGSZ, CMDSZ)) return NULL;
    memset(buf, 0x5A, CMDSZ);
    for (int i = a->w; i < a->nio; i += KSTRIPE) {
        u64 c0 = now_ns();
        if (pwrite(FD, buf, a->ios[i].npg * PGSZ, a->ios[i].pg * PGSZ) < 0) { perror("zns pwrite"); exit(8); }
        if (a->cls >= 0) lv_add(a->cls, now_ns() - c0);
    }
    free(buf);
    return NULL;
}
static u64 zns_striped_write(io_t *ios, int nio, int cls) {
    u64 t0 = now_ns();
    pthread_t th[MAXK]; struct sw_arg args[MAXK];
    int k = KSTRIPE < nio ? KSTRIPE : (nio ? nio : 1);
    for (int w = 0; w < k; w++) {
        args[w] = (struct sw_arg){ ios, nio, w, cls };
        pthread_create(&th[w], NULL, sw_worker, &args[w]);
    }
    for (int w = 0; w < k; w++) pthread_join(th[w], NULL);
    return now_ns() - t0;
}

static void *sr_worker(void *a_) {
    struct sw_arg *a = a_;
    char *buf; if (posix_memalign((void **)&buf, PGSZ, CMDSZ)) return NULL;
    for (int i = a->w; i < a->nio; i += KSTRIPE) {
        u64 c0 = now_ns();
        if (pread(FD, buf, a->ios[i].npg * PGSZ, a->ios[i].pg * PGSZ) < 0) { perror("zns pread"); exit(8); }
        if (a->cls >= 0) lv_add(a->cls, now_ns() - c0);
    }
    free(buf);
    return NULL;
}
static u64 zns_striped_read(io_t *ios, int nio, int cls) {
    u64 t0 = now_ns();
    pthread_t th[MAXK]; struct sw_arg args[MAXK];
    int k = KSTRIPE < nio ? KSTRIPE : (nio ? nio : 1);
    for (int w = 0; w < k; w++) {
        args[w] = (struct sw_arg){ ios, nio, w, cls };
        pthread_create(&th[w], NULL, sr_worker, &args[w]);
    }
    for (int w = 0; w < k; w++) pthread_join(th[w], NULL);
    return now_ns() - t0;
}

static int run_op(op_t *o, u64 *turn_stall, int measure, u32 sid, u32 op_idx, u64 trace_ns) {
    obj_t *ob = &objs[o->obj];
    int wr = (o->op == OP_WKV || o->op == OP_WML || o->op == OP_WIX);
    int cls = -1;
    io_t *ios = malloc(sizeof(io_t) * (o->len / CMD_PGS + 2 + o->len));
    int nio = 0;
    u64 dur = 0;
    int ob_locked = 0, ml_locked = 0, ix_locked = 0;

    if (stop_requested()) { free(ios); return -1; }

    if (o->op == OP_DEL) {
        pthread_mutex_lock(&ob->mu); ob_locked = 1;
        if (!MODE_ZNS) {
            for (int i = 0; i < ob->n_ext; i++) {
                if (!getenv("BLKDISCARD_DISABLE")) {   /* no-TRIM experiment toggle (Part 2) */
                    u64 range[2] = { ob->ext[i].lpn_start * PGSZ, ob->ext[i].npgs * PGSZ };
                    if (ioctl(FD, BLKDISCARD, &range) < 0 && errno != EOPNOTSUPP) perror("BLKDISCARD");
                }
                fs_free(ob->ext[i].lpn_start, ob->ext[i].npgs);
            }
            ob->n_ext = 0;
        } else {
            for (int c = 0; c < KSTRIPE; c++) {
                for (int i = 0; i < ob->zc_n[c]; i++) zns_reset_zone(ob->zc[c][i]);
                ob->zc_n[c] = 0; ob->zc_wp[c] = 0;
            }
            ob->n_ext = 0;
        }
        ob->pgs = 0;
        pthread_mutex_unlock(&ob->mu); ob_locked = 0;
        free(ios); return 0;
    }

    pthread_mutex_lock(&ob->mu); ob_locked = 1;
    if (!MODE_ZNS) {
        if (wr && o->op == OP_WKV) {                       /* allocate extents for append */
            u64 need = (o->off + o->len > ob->pgs) ? o->off + o->len - ob->pgs : 0;
            while (need) {
                u64 got = 0;
                int64_t s = fs_alloc(need, &got);
                if (s < 0 || !got) {
                    request_storage_stop(5, "conv_device_full", sid, op_idx, trace_ns, o);
                    goto fail;
                }
                obj_add_ext(ob, s, got);
                need -= got;
            }
        }
        u64 base = (o->op == OP_WML || o->op == OP_RML) ? ML_PGS : (o->op == OP_WIX || o->op == OP_RIX) ? IX_PGS : 0;
        for (u64 p = 0; p < o->len; ) {
            u64 n = o->len - p > CMD_PGS ? CMD_PGS : o->len - p;
            u64 lg = o->off + p; if (base) lg %= base;
            ios[nio++] = (io_t){ obj_lpn(ob, lg), n };     /* contiguous within extent approx */
            p += n;
        }
        cls = (int[]){ C_KVWR, C_KVRD, C_MLWR, C_MLRD, C_IXWR, C_IXRD }[o->op];
        pthread_mutex_unlock(&ob->mu); ob_locked = 0;
        u64 t0 = now_ns();
        dur = burst(wr, ios, nio, QD, measure ? cls : -1);
        (void)t0;
    } else {
        /* ZNS placement: KV blobs stripe across KSTRIPE zone chains; chunk j
         * (32-page slot) lives on chain j%K at chain-index j/K. Writes run K
         * parallel chains, each strictly sequential (write-pointer safe). */
        if (o->op == OP_WKV) {
             if (o->off != ob->pgs) {
                fprintf(stderr, "non-append WKV obj %u off %" PRIu64
                    " expected %" PRIu64 "\n", o->obj, o->off, ob->pgs);
                exit(4);
            }
            for (u64 p = 0; p < o->len; ) {
                u64 lg = o->off + p;
                u64 j = lg / CMD_PGS, in = lg % CMD_PGS;
                int c = (int)(j % (u64)KSTRIPE);
                u64 n = o->len - p;
                if (n > CMD_PGS - in) n = CMD_PGS - in;
                if (!ob->zc_n[c] || ob->zc_wp[c] + n > ZONE_PGS) {
                    if (ob->zc_n[c] == ob->zc_cap[c]) { ob->zc_cap[c] = ob->zc_cap[c] ? ob->zc_cap[c]*2 : 8; ob->zc[c] = realloc(ob->zc[c], ob->zc_cap[c]*4); }
                    int z = zns_alloc_zone();
                    if (z < 0) {
                        request_storage_stop(3, "zns_out_of_zones", sid, op_idx, trace_ns, o);
                        goto fail;
                    }
                    ob->zc[c][ob->zc_n[c]++] = z; ob->zc_wp[c] = 0;
                }
                u64 pg = (u64)ob->zc[c][ob->zc_n[c]-1] * ZONE_PGS + ob->zc_wp[c];
                ios[nio++] = (io_t){ pg, n };
                obj_add_ext(ob, pg, n);
                ob->zc_wp[c] += n;
                p += n;
            }
            cls = C_KVWR;
            pthread_mutex_unlock(&ob->mu); ob_locked = 0;
            dur = zns_striped_write(ios, nio, measure ? cls : -1);
            goto acct;
        } else if (o->op == OP_RKV) {
            obj_emit_ios(ob, o->off, o->len, ios, &nio);
            cls = C_KVRD;
            pthread_mutex_unlock(&ob->mu); ob_locked = 0;
            dur = zns_striped_read(ios, nio, measure ? cls : -1);
            goto acct;
        } else if (o->op == OP_WML || o->op == OP_RML) {
            pthread_mutex_lock(&ml_mu); ml_locked = 1;
            for (u64 p = 0; p < o->len; ) {
                if (o->op == OP_WML) {
                    if (ml_open < 0 || ml_open_wp == ZONE_PGS) {
                        if (n_mlz >= ml_budget_z) {
                            int oz = mlz[0];
                            for (u64 q = 0; q < ML_PGS; q++){
                                if (ml_page_zone[q] != UINT64_MAX && (int)(ml_page_zone[q] >> 32) == oz)
                                    ml_page_zone[q] = UINT64_MAX;
                            }
                            zns_reset_zone(oz); memmove(mlz, mlz + 1, (--n_mlz) * 4);
                        }
                        ml_open = zns_alloc_zone();
                        if (ml_open < 0) {
                            request_storage_stop(3, "zns_out_of_zones", sid, op_idx, trace_ns, o);
                            goto fail;
                        }
                        mlz[n_mlz++] = ml_open;
                        ml_open_wp = 0;
                    }

                    u64 n = o->len - p;
                    u64 zrem = ZONE_PGS - ml_open_wp;
                    if (n > CMD_PGS) n = CMD_PGS;
                    if (n > zrem) n = zrem;

                    u64 slot = ml_open_wp;
                    for (u64 q = 0; q < n; q++) {
                        u64 lg = (o->off + p + q) % ML_PGS;
                        ml_page_zone[lg] = ((u64)ml_open << 32) | (u32)(slot + q);
                    }
                    ios[nio++] = (io_t){ (u64)ml_open * ZONE_PGS + slot, n };
                    ml_open_wp += n;
                    p += n;
                } else {
                    u64 lg = (o->off + p) % ML_PGS;
                    u64 zs = ml_page_zone[lg]; if (zs == UINT64_MAX) { p++; continue; }
                    ios[nio++] = (io_t){ (zs >> 32) * ZONE_PGS + (u32)zs, 1 };
                    p++;
                }
            }
            pthread_mutex_unlock(&ml_mu); ml_locked = 0;
            cls = o->op == OP_WML ? C_MLWR : C_MLRD;
        } else {  /* WIX / RIX */
            pthread_mutex_lock(&ix_mu); ix_locked = 1;
            for (u64 p = 0; p < o->len; p++) {
                u64 e = (o->off + p) % IX_PGS;
                if (o->op == OP_WIX) {
                    if (ix_open < 0 || ix_open_wp == ZONE_PGS) {
                        ix_open = zns_alloc_zone();
                        if (ix_open < 0) {
                            request_storage_stop(3, "zns_out_of_zones", sid, op_idx, trace_ns, o);
                            goto fail;
                        }
                        ix_open_wp = 0; ixz[n_ixz++] = ix_open;
                        if (n_ixz > IX_BUDGET_Z) {         /* compact min-live full zone */
                            int vi = -1, vz = -1, best = 1 << 30;
                            for (int k = 0; k < n_ixz; k++) if (ixz[k] != ix_open && zn_live[ixz[k]] < best) { best = zn_live[ixz[k]]; vz = ixz[k]; vi = k; }
                            if (vz >= 0) {
                                for (u64 q = 0; q < IX_PGS; q++) if (ix_zone_of[q] == vz) {
                                    pio(0, (u64)vz * ZONE_PGS + ix_slot_of[q], 1);
                                    if (ix_open_wp == ZONE_PGS) {
                                        ix_open = zns_alloc_zone();
                                        if (ix_open < 0) {
                                            request_storage_stop(3, "zns_out_of_zones", sid, op_idx, trace_ns, o);
                                            goto fail;
                                        }
                                        ix_open_wp = 0; ixz[n_ixz++] = ix_open;
                                    }
                                    ix_zone_of[q] = ix_open; ix_slot_of[q] = ix_open_wp; zn_live[ix_open]++;
                                    pio(1, (u64)ix_open * ZONE_PGS + ix_open_wp, 1); ix_open_wp++;
                                }
                                zn_live[vz] = 0; zns_reset_zone(vz);
                                memmove(&ixz[vi], &ixz[vi + 1], (n_ixz - vi - 1) * 4); n_ixz--;
                            }
                        }
                    }
                    if (ix_zone_of[e] >= 0) zn_live[ix_zone_of[e]]--;
                    ix_zone_of[e] = ix_open; ix_slot_of[e] = ix_open_wp; zn_live[ix_open]++;
                    ios[nio++] = (io_t){ (u64)ix_open * ZONE_PGS + ix_open_wp, 1 }; ix_open_wp++;
                } else {
                    if (ix_zone_of[e] < 0) continue;
                    ios[nio++] = (io_t){ (u64)ix_zone_of[e] * ZONE_PGS + ix_slot_of[e], 1 };
                }
            }
            pthread_mutex_unlock(&ix_mu); ix_locked = 0;
            cls = (o->op == OP_WIX) ? C_IXWR : C_IXRD;
        }
        pthread_mutex_unlock(&ob->mu); ob_locked = 0;
        dur = burst(wr, ios, nio, wr ? 1 : QD, measure ? cls : -1);
    }
acct:
    if (o->op == OP_WKV && measure) lv_add(C_KVWR_OP, dur);
    if (o->op == OP_RKV && measure) lv_add(C_KVRD_OP, dur);
    if (o->op == OP_RKV && measure) lv_add(C_RESUME, dur);
    if (measure) *turn_stall += dur;
    free(ios);
    return 0;
fail:
    if (ix_locked) pthread_mutex_unlock(&ix_mu);
    if (ml_locked) pthread_mutex_unlock(&ml_mu);
    if (ob_locked) pthread_mutex_unlock(&ob->mu);
    free(ios);
    return -1;
}

static void *sess_thread(void *arg) {
    sess_t *S = &sess[(uintptr_t)arg];
    u32 sid = (u32)(uintptr_t)arg;
    if (!sleep_until(S->t0)) return NULL;
    u64 trace_ns = S->t0;
    u64 turn_stall = 0; int turn_open = 0, turn_measured = 0;
    for (u32 i = 0; i < S->n; i++) {
        if (stop_requested()) break;
        op_t *o = &S->ops[i];
        trace_ns += o->gap;
        int measure = trace_ns >= meas_start_ns;
        if (!gap_sleep(o->gap)) break;
        if (o->ts) {
            if (turn_open && turn_measured) lv_add(C_TURNSTALL, turn_stall);
            turn_stall = 0; turn_open = 1; turn_measured = 0;
        }
        if (run_op(o, &turn_stall, measure, sid, i, trace_ns) < 0) break;
        rprog_note_issued(sid, i, trace_ns, o);
        if (measure) turn_measured = 1;
    }
    if (turn_open && turn_measured) lv_add(C_TURNSTALL, turn_stall);
    return NULL;
}

static int cmp_u32(const void *a, const void *b) { u32 x = *(const u32*)a, y = *(const u32*)b; return x < y ? -1 : x > y; }
static u32 pct_u32(const lvec *L, double p) { return L->v[(u64)(p / 100.0 * (L->n - 1) + 0.5)]; }

static int write_outputs(const char *outp, const char *dev) {
    char fn[512];
    snprintf(fn, sizeof fn, "%s_summary.txt", outp);
    FILE *out = fopen(fn, "w");
    if (!out) { perror("summary"); return 1; }

    fprintf(out, "mode=%s tscale=%.3f qd=%d device=%s\n", MODE_ZNS ? "zns" : "conv", TSCALE, QD, dev);
    pthread_mutex_lock(&stop_mu);
    if (STOP_INFO.active) {
        fprintf(out, "RUN_STATUS incomplete reason=%s", STOP_INFO.reason);
        if (STOP_INFO.line_no) {
            fprintf(out,
                    " trace=%s line=%" PRIu64 " session=%u op_index=%u trace_ns=%" PRIu64
                    " op=%s obj=%u gap=%" PRIu64 " off=%" PRIu64 " len=%" PRIu64,
                    TRACE_PATH ? TRACE_PATH : "(unknown trace)", STOP_INFO.line_no,
                    STOP_INFO.sid, STOP_INFO.op_idx, STOP_INFO.trace_ns, op_name(STOP_INFO.op),
                    STOP_INFO.obj, STOP_INFO.gap, STOP_INFO.off, STOP_INFO.len);
        }
        fprintf(out, "\n");
    } else {
        fprintf(out, "RUN_STATUS complete\n");
    }
    pthread_mutex_unlock(&stop_mu);

    for (int c = 0; c < NCLS; c++) {
        lvec *L = &LAT[c]; if (!L->n) continue;
        qsort(L->v, L->n, sizeof *L->v, cmp_u32);
        double sum = 0; for (u64 i = 0; i < L->n; i++) sum += L->v[i];
        fprintf(out, "LAT %s n=%" PRIu64 " mean_us=%.1f p50_us=%u p90_us=%u p99_us=%u p999_us=%u max_us=%u\n",
                cls_name[c], L->n, sum / L->n, pct_u32(L, 50), pct_u32(L, 90), pct_u32(L, 99), pct_u32(L, 99.9), L->v[L->n - 1]);
        snprintf(fn, sizeof fn, "%s_lat_%s.csv", outp, cls_name[c]);
        FILE *f2 = fopen(fn, "w");
        if (!f2) { perror("lat csv"); fclose(out); return 1; }
        u64 step = L->n > 200000 ? L->n / 200000 : 1;
        for (u64 i = 0; i < L->n; i += step) fprintf(f2, "%u\n", L->v[i]);
        fclose(f2);
    }
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    const char *tracef = NULL, *dev = NULL, *outp = "out";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) tracef = argv[++i];
        else if (!strcmp(argv[i], "-d")) dev = argv[++i];
        else if (!strcmp(argv[i], "-m")) MODE_ZNS = !strcmp(argv[++i], "zns");
        else if (!strcmp(argv[i], "-o")) outp = argv[++i];
        else if (!strcmp(argv[i], "-s")) TSCALE = atof(argv[++i]);
        else if (!strcmp(argv[i], "-q")) QD = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--progress") || !strcmp(argv[i], "--progress-interval"))
            RPROG_INTERVAL = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--no-progress")) RPROG_INTERVAL = 0;
        else if (!strcmp(argv[i], "-k")) { 
            KSTRIPE = atoi(argv[++i]);
            if (KSTRIPE < 1) KSTRIPE = 1; 
            if (KSTRIPE > MAXK) KSTRIPE = MAXK; 
        }
    }
    if (!tracef || !dev) { fprintf(stderr, "usage: replayer -t trace -d /dev/nvmeXnY -m conv|zns -o out [-s scale] [-q qd] [--progress-interval n] [--no-progress]\n"); return 1; }
    TRACE_PATH = tracef;
    for (int c = 0; c < NCLS; c++) pthread_mutex_init(&LAT[c].mu, NULL);
    load_trace(tracef);
    rprog_count_trace();
    if (RPROG_INTERVAL) {
        fprintf(stderr,
                "progress: total_trace_ops=%" PRIu64 " total_log_writes=%" PRIu64
                " total_log_pages=%" PRIu64 " interval=%" PRIu64 "\n",
                RPROG_TOTAL_OPS, RPROG_TOTAL_LOG_WRITES,
                RPROG_TOTAL_LOG_PAGES, RPROG_INTERVAL);
    }

    FD = open(dev, O_RDWR | O_DIRECT);
    if (FD < 0) { perror("open dev"); return 1; }
    u64 bytes; if (ioctl(FD, BLKGETSIZE64, &bytes) < 0) { perror("BLKGETSIZE64"); return 1; }
    DEV_PGS = bytes / PGSZ;
    fprintf(stderr, "device %s: %.1f GiB, mode=%s, tscale=%.2f\n", dev, bytes / 1073741824.0, MODE_ZNS ? "zns" : "conv", TSCALE);

    if (!MODE_ZNS) {
        fs_free(0, DEV_PGS);
        u64 got = 0, need = IX_PGS + ML_PGS;
        int64_t s = fs_alloc(need, &got);   /* preallocate index+memlog */
        if (s < 0 || got < need) {
            if (s >= 0 && got) fs_free((u64)s, got);
            request_storage_stop(5, "conv_metadata_prealloc_full", 0, 0, 0, NULL);
        } else {
            obj_add_ext(&objs[0], (u64)s, IX_PGS);
            obj_add_ext(&objs[1], (u64)s + IX_PGS, ML_PGS);
        }
    } else {
        u64 zsec = 0;
        char sysp[256], buf[64];
        const char *base = strrchr(dev, '/'); base = base ? base + 1 : dev;
        snprintf(sysp, sizeof sysp, "/sys/block/%s/queue/chunk_sectors", base);
        FILE *f = fopen(sysp, "r");
        if (f) { if (fgets(buf, sizeof buf, f)) zsec = strtoull(buf, NULL, 10); fclose(f); }
        if (!zsec) { fprintf(stderr, "cannot read zone size (chunk_sectors); is %s zoned?\n", dev); return 1; }
        ZONE_PGS = zsec * 512 / PGSZ; N_ZONES = DEV_PGS / ZONE_PGS;
        fprintf(stderr, "zones: %" PRIu64 " x %.1f MiB\n", N_ZONES, ZONE_PGS * PGSZ / 1048576.0);
        zfree = malloc(N_ZONES * 4);
        for (u64 i = 0; i < N_ZONES; i++) zfree[i] = (int)i;
        zf_head = 0; zf_tail = 0; zf_cnt = N_ZONES; zf_tail = N_ZONES;
        ix_zone_of = malloc(IX_PGS * 4); ix_slot_of = malloc(IX_PGS * 4);
        for (u64 i = 0; i < IX_PGS; i++) ix_zone_of[i] = -1;
        zn_live = calloc(N_ZONES, 4);
        IX_BUDGET_Z = (int)(2 * ((IX_PGS + ZONE_PGS - 1) / ZONE_PGS)) + 1;
        ixz = malloc((IX_BUDGET_Z + 8) * 4);
        ml_budget_z = (int)((ML_PGS + ZONE_PGS - 1) / ZONE_PGS) + 1;
        mlz = malloc((ml_budget_z + 2) * 4);
        ml_page_zone = malloc(ML_PGS * 8); memset(ml_page_zone, 0xFF, ML_PGS * 8);
        /* reset all zones to a clean state */
#ifdef HAVE_BLKZONED
        struct blk_zone_range r = { .sector = 0, .nr_sectors = bytes / 512 };
        if (ioctl(FD, BLKRESETZONE, &r) < 0) perror("initial BLKRESETZONE");
#endif
    }

    if (!stop_requested()) {
        T0 = now_ns();
        pthread_t *th = malloc(n_sess * sizeof(pthread_t));
        for (u32 s = 0; s < n_sess; s++) pthread_create(&th[s], NULL, sess_thread, (void *)(uintptr_t)s);
        for (u32 s = 0; s < n_sess; s++) pthread_join(th[s], NULL);
        free(th);
    }
    rprog_finish();

    int write_rc = write_outputs(outp, dev);
    int rc = stop_code();
    if (write_rc) return write_rc;
    if (rc)
        fprintf(stderr, "stopped; partial summary in %s_summary.txt\n", outp);
    else
        fprintf(stderr, "done; summary in %s_summary.txt\n", outp);
    return rc;
}
