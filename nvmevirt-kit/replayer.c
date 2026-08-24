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
static int REQUESTED_KSTRIPE = 8;
static u64 MAX_ACTIVE_ZONES_ARG = 0, DEVICE_MAX_ACTIVE_ZONES = 0, DEVICE_MAX_OPEN_ZONES = 0, ZNS_ACTIVE_ZONE_LIMIT = 0;
static u64 PEAK_KV_ACTIVE_ZONES = 0, PEAK_LIVE_KV_SESSIONS = 0;
static int KV_SHARED = 0, KV_SHARED_LANES = 0;
#define ZNS_META_ACTIVE_ZONES 2        /* one memlog append zone + one index append zone */
static u64 DEV_PGS, ZONE_PGS, ZONE_CAP_PGS, N_ZONES;
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
static void obj_add_ext_nomerge(obj_t *o, u64 s, u64 n) {
    if (o->n_ext == o->cap_ext) { o->cap_ext = o->cap_ext ? o->cap_ext * 2 : 8; o->ext = realloc(o->ext, o->cap_ext * sizeof(ext_t)); }
    o->ext[o->n_ext++] = (ext_t){ s, n }; o->pgs += n;
}
static void ext_vec_add(ext_t **v, int *n, int *cap, u64 s, u64 pgs) {
    if (!pgs) return;
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 8; *v = realloc(*v, *cap * sizeof(**v)); }
    (*v)[(*n)++] = (ext_t){ s, pgs };
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
static u64 zns_zone_cap_rem(u64 pg) {
    u64 off = pg % ZONE_PGS;
    return off < ZONE_CAP_PGS ? ZONE_CAP_PGS - off : 0;
}
static u64 zns_report_zone_capacity_pgs(void) {
#ifdef HAVE_BLKZONED
    size_t sz = sizeof(struct blk_zone_report) + sizeof(struct blk_zone);
    struct blk_zone_report *rep = calloc(1, sz);
    if (!rep) return ZONE_PGS;
    rep->sector = 0;
    rep->nr_zones = 1;
    if (ioctl(FD, BLKREPORTZONE, rep) == 0 && rep->nr_zones > 0) {
        u64 cap_sec = rep->zones[0].capacity ? rep->zones[0].capacity : rep->zones[0].len;
        u64 len_sec = rep->zones[0].len;
        free(rep);
        if (!cap_sec || cap_sec > len_sec) cap_sec = len_sec;
        u64 cap_pgs = cap_sec * 512 / PGSZ;
        return cap_pgs ? cap_pgs : ZONE_PGS;
    }
    perror("BLKREPORTZONE");
    free(rep);
#endif
    return ZONE_PGS;
}
/* index LSM + memlog FIFO state (single-threaded via ix/ml mutexes) */
static int *ix_zone_of, *ix_slot_of, *zn_live; static int *ixz, n_ixz, ix_open = -1, IX_BUDGET_Z;
static u64 ix_open_wp;
static int *mlz, n_mlz, ml_budget_z, ml_open = -1; static u64 ml_open_wp; static u64 *ml_page_zone;
static pthread_mutex_t ix_mu = PTHREAD_MUTEX_INITIALIZER, ml_mu = PTHREAD_MUTEX_INITIALIZER;

/* Shared KV-lane mode is used only when the active-zone budget cannot give each
 * live session its own tail zone. KV operations are serialized so shared lane
 * write pointers and relocation updates cannot race with reads. */
static int *kv_lane_zone;
static u64 *kv_lane_wp, *kv_zone_live;
static unsigned char *kv_zone_used;
static pthread_mutex_t kv_shared_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- raw I/O helpers ---------------- */
static void *iobuf(void) { static __thread void *b = NULL; if (!b) { if (posix_memalign(&b, PGSZ, CMDSZ)) exit(9); memset(b, 0xA5, CMDSZ); } return b; }
static void pio(int wr, u64 pg, u64 npg) {          /* synchronous single command */
    void *b = iobuf();
    ssize_t r = wr ? pwrite(FD, b, npg * PGSZ, pg * PGSZ) : pread(FD, b, npg * PGSZ, pg * PGSZ);
    if (r < 0) { perror(wr ? "pwrite" : "pread"); exit(8); }
}

/* burst of commands with QD via POSIX AIO; entries: (pg, npg) */
typedef struct { u64 pg, npg; int stream; } io_t;

static void obj_emit_ios(obj_t *o, u64 off, u64 len, io_t *ios, int *nio) {
    for (int i = 0; i < o->n_ext && len; i++) {
        if (off >= o->ext[i].npgs) { off -= o->ext[i].npgs; continue; }
        u64 ext_off = off;
        while (ext_off < o->ext[i].npgs && len) {
            u64 pg = o->ext[i].lpn_start + ext_off;
            u64 n = o->ext[i].npgs - ext_off;
            u64 zrem = zns_zone_cap_rem(pg);
            if (!zrem) { fprintf(stderr, "obj extent beyond zone capacity\n"); exit(4); }
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

static int read_u64_file(const char *path, u64 *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[64];
    int ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!ok) return 0;
    *out = strtoull(buf, NULL, 10);
    return 1;
}


static int kv_shared_lane_for(u32 obj, int stripe) {
    u64 sid_part = obj >= 2 ? (u64)(obj - 2) : (u64)obj;
    if (KV_SHARED_LANES <= 0) return 0;
    return (int)((sid_part * (u64)KSTRIPE + (u64)stripe) % (u64)KV_SHARED_LANES);
}

static int kv_shared_zone_is_open_locked(int z) {
    for (int i = 0; i < KV_SHARED_LANES; i++)
        if (kv_lane_zone[i] == z && kv_lane_wp[i] < ZONE_CAP_PGS) return 1;
    return 0;
}

static u64 kv_shared_lane_rem_locked(int lane) {
    if (kv_lane_zone[lane] < 0 || kv_lane_wp[lane] == ZONE_CAP_PGS) return ZONE_CAP_PGS;
    return ZONE_CAP_PGS - kv_lane_wp[lane];
}

static int kv_shared_append_locked(int lane, u64 n, u64 *pg) {
    if (lane < 0 || lane >= KV_SHARED_LANES || !n) return -1;
    if (kv_lane_zone[lane] < 0 || kv_lane_wp[lane] == ZONE_CAP_PGS) {
        int z = zns_alloc_zone();
        if (z < 0) return -1;
        kv_lane_zone[lane] = z;
        kv_lane_wp[lane] = 0;
        kv_zone_used[z] = 1;
    }
    if (n > ZONE_CAP_PGS - kv_lane_wp[lane]) return -1;
    int z = kv_lane_zone[lane];
    *pg = (u64)z * ZONE_PGS + kv_lane_wp[lane];
    kv_lane_wp[lane] += n;
    kv_zone_live[z] += n;
    kv_zone_used[z] = 1;
    return 0;
}

static void kv_shared_dec_live_range_locked(u64 pg, u64 len) {
    while (len) {
        int z = (int)(pg / ZONE_PGS);
        u64 n = len;
        u64 zrem = zns_zone_cap_rem(pg);
        if (!zrem) { fprintf(stderr, "shared KV extent beyond zone capacity\n"); exit(4); }
        if (n > zrem) n = zrem;
        if (z >= 0 && (u64)z < N_ZONES) {
            kv_zone_live[z] = kv_zone_live[z] > n ? kv_zone_live[z] - n : 0;
        }
        pg += n;
        len -= n;
    }
}

static void kv_shared_reset_zone_locked(int z) {
    if (z < 0 || (u64)z >= N_ZONES || !kv_zone_used[z]) return;
    for (int i = 0; i < KV_SHARED_LANES; i++) {
        if (kv_lane_zone[i] == z) {
            kv_lane_zone[i] = -1;
            kv_lane_wp[i] = 0;
        }
    }
    kv_zone_live[z] = 0;
    kv_zone_used[z] = 0;
    zns_reset_zone(z);
}

static int kv_shared_pick_victim_locked(void) {
    int best = -1;
    u64 best_live = UINT64_MAX;
    for (u64 z = 0; z < N_ZONES; z++) {
        if (!kv_zone_used[z]) continue;
        if (kv_shared_zone_is_open_locked((int)z)) continue;
        if (kv_zone_live[z] >= ZONE_CAP_PGS) continue;
        if (!kv_zone_live[z]) return (int)z;
        if (kv_zone_live[z] < best_live) {
            best_live = kv_zone_live[z];
            best = (int)z;
        }
    }
    return best;
}

static int kv_shared_relocate_range_locked(u32 obj, u64 logical, u64 oldpg, u64 len,
                                           ext_t **new_ext, int *new_n, int *new_cap,
                                           u32 sid, u32 op_idx, u64 trace_ns, const op_t *op) {
    while (len) {
        int stripe = (int)((logical / CMD_PGS) % (u64)KSTRIPE);
        int lane = kv_shared_lane_for(obj, stripe);
        u64 n = len;
        u64 old_zrem = zns_zone_cap_rem(oldpg);
        if (!old_zrem) { fprintf(stderr, "shared KV relocation extent beyond zone capacity\n"); exit(4); }
        u64 cmd_rem = CMD_PGS - (logical % CMD_PGS);
        u64 lane_rem = kv_shared_lane_rem_locked(lane);
        if (n > old_zrem) n = old_zrem;
        if (n > cmd_rem) n = cmd_rem;
        if (n > lane_rem) n = lane_rem;

        u64 newpg = 0;
        if (!n || kv_shared_append_locked(lane, n, &newpg) < 0) {
            request_storage_stop(3, "zns_shared_gc_out_of_zones", sid, op_idx, trace_ns, op);
            return -1;
        }
        pio(0, oldpg, n);
        pio(1, newpg, n);
        kv_shared_dec_live_range_locked(oldpg, n);
        ext_vec_add(new_ext, new_n, new_cap, newpg, n);
        oldpg += n;
        logical += n;
        len -= n;
    }
    return 0;
}

static int kv_shared_compact_zone_locked(int victim, u32 sid, u32 op_idx, u64 trace_ns, const op_t *op) {
    if (victim < 0 || (u64)victim >= N_ZONES || !kv_zone_used[victim]) return 0;
    if (kv_shared_zone_is_open_locked(victim)) return 0;

    for (u64 obj = 2; obj < n_objs + 2; obj++) {
        obj_t *ob = &objs[obj];
        pthread_mutex_lock(&ob->mu);
        if (!ob->n_ext) {
            pthread_mutex_unlock(&ob->mu);
            continue;
        }

        ext_t *new_ext = NULL;
        int new_n = 0, new_cap = 0, changed = 0;
        u64 logical = 0;
        for (int i = 0; i < ob->n_ext; i++) {
            u64 phys = ob->ext[i].lpn_start;
            u64 rem = ob->ext[i].npgs;
            while (rem) {
                int z = (int)(phys / ZONE_PGS);
                u64 n = rem;
                u64 zrem = zns_zone_cap_rem(phys);
                if (!zrem) { fprintf(stderr, "shared KV compaction extent beyond zone capacity\n"); exit(4); }
                u64 cmd_rem = CMD_PGS - (logical % CMD_PGS);
                if (n > zrem) n = zrem;
                if (n > cmd_rem) n = cmd_rem;
                if (z == victim) {
                    changed = 1;
                    if (kv_shared_relocate_range_locked((u32)obj, logical, phys, n,
                                                        &new_ext, &new_n, &new_cap,
                                                        sid, op_idx, trace_ns, op) < 0) {
                        pthread_mutex_unlock(&ob->mu);
                        free(new_ext);
                        return -1;
                    }
                } else {
                    ext_vec_add(&new_ext, &new_n, &new_cap, phys, n);
                }
                phys += n;
                logical += n;
                rem -= n;
            }
        }

        if (changed) {
            u64 pgs = ob->pgs;
            free(ob->ext);
            ob->ext = new_ext;
            ob->n_ext = new_n;
            ob->cap_ext = new_cap;
            ob->pgs = pgs;
        } else {
            free(new_ext);
        }
        pthread_mutex_unlock(&ob->mu);
    }

    if (kv_zone_live[victim]) {
        fprintf(stderr, "warning: shared KV compaction left zone %d with %" PRIu64 " live pages\n",
                victim, kv_zone_live[victim]);
    }
    kv_shared_reset_zone_locked(victim);
    return 0;
}

static int kv_shared_gc_locked(u32 sid, u32 op_idx, u64 trace_ns, const op_t *op) {
    for (u64 pass = 0; pass < N_ZONES; pass++) {
        int victim = kv_shared_pick_victim_locked();
        if (victim < 0) return 0;
        if (kv_shared_compact_zone_locked(victim, sid, op_idx, trace_ns, op) < 0) return -1;
    }
    return 0;
}

static void kv_shared_delete_obj_locked(obj_t *ob) {
    for (int i = 0; i < ob->n_ext; i++)
        kv_shared_dec_live_range_locked(ob->ext[i].lpn_start, ob->ext[i].npgs);
    ob->n_ext = 0;
    ob->pgs = 0;
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

typedef struct { u64 trace_ns; const op_t *op; } trace_event_t;

static void trace_event_add(trace_event_t **ev, u64 *n, u64 *cap, u64 trace_ns, const op_t *op) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        *ev = realloc(*ev, *cap * sizeof(**ev));
    }
    (*ev)[(*n)++] = (trace_event_t){ trace_ns, op };
}

static int cmp_trace_event(const void *a_, const void *b_) {
    const trace_event_t *a = a_, *b = b_;
    if (a->trace_ns < b->trace_ns) return -1;
    if (a->trace_ns > b->trace_ns) return 1;
    return (int)a->op->op - (int)b->op->op; /* WKV before DEL at identical time */
}

static trace_event_t *trace_kv_events(u64 *n_ev) {
    trace_event_t *ev = NULL;
    u64 n = 0, cap = 0;
    for (u32 s = 0; s < n_sess; s++) {
        sess_t *S = &sess[s];
        u64 trace_ns = S->t0;
        for (u32 i = 0; i < S->n; i++) {
            op_t *op = &S->ops[i];
            trace_ns += op->gap;
            if (op->obj >= 2 && (op->op == OP_WKV || op->op == OP_DEL))
                trace_event_add(&ev, &n, &cap, trace_ns, op);
        }
    }
    qsort(ev, n, sizeof(*ev), cmp_trace_event);
    *n_ev = n;
    return ev;
}

static u64 trace_peak_kv_active_zones_for_k(int k, u64 *peak_live_out) {
    u64 n_ev = 0;
    trace_event_t *ev = trace_kv_events(&n_ev);
    u64 n_slot = (n_objs + 2) * (u64)k;
    u64 *wp = calloc(n_slot, sizeof(*wp));
    u64 *obj_active = calloc(n_objs + 2, sizeof(*obj_active));
    unsigned char *touched = calloc(n_slot, 1);
    unsigned char *live = calloc(n_objs + 2, 1);
    u64 active = 0, peak_active = 0, live_cnt = 0, peak_live = 0;

    for (u64 e = 0; e < n_ev; e++) {
        const op_t *op = ev[e].op;
        u32 obj = op->obj;
        if (obj >= n_objs + 2) continue;

        if (op->op == OP_DEL) {
            if (live[obj]) { live[obj] = 0; live_cnt--; }
            active -= obj_active[obj];
            obj_active[obj] = 0;
            memset(&touched[(u64)obj * k], 0, k);
            memset(&wp[(u64)obj * k], 0, k * sizeof(*wp));
            continue;
        }

        if (op->op != OP_WKV) continue;
        if (!live[obj]) {
            live[obj] = 1;
            live_cnt++;
            if (live_cnt > peak_live) peak_live = live_cnt;
        }

        for (u64 p = 0; p < op->len; ) {
            u64 lg = op->off + p;
            u64 j = lg / CMD_PGS, in = lg % CMD_PGS;
            int c = (int)(j % (u64)k);
            u64 n = op->len - p;
            if (n > CMD_PGS - in) n = CMD_PGS - in;

            u64 idx = (u64)obj * k + c;
            if (!touched[idx] || wp[idx] + n > ZONE_CAP_PGS) {
                touched[idx] = 1;
                wp[idx] = 0;
                active++;
                obj_active[obj]++;
            }
            wp[idx] += n;
            if (wp[idx] == ZONE_CAP_PGS) {
                active--;
                obj_active[obj]--;
            }
            p += n;
        }
        if (active > peak_active) peak_active = active;
    }

    free(live);
    free(touched);
    free(obj_active);
    free(wp);
    free(ev);
    if (peak_live_out) *peak_live_out = peak_live;
    return peak_active;
}

static u64 trace_peak_kv_shared_active_zones(int lanes, u64 *peak_live_out) {
    u64 n_ev = 0;
    trace_event_t *ev = trace_kv_events(&n_ev);
    int *lane_open = calloc(lanes, sizeof(*lane_open));
    u64 *lane_wp = calloc(lanes, sizeof(*lane_wp));
    unsigned char *live = calloc(n_objs + 2, 1);
    u64 active = 0, peak_active = 0, live_cnt = 0, peak_live = 0;

    for (u64 e = 0; e < n_ev; e++) {
        const op_t *op = ev[e].op;
        if (op->obj >= n_objs + 2) continue;
        if (op->op == OP_DEL) {
            if (live[op->obj]) { live[op->obj] = 0; live_cnt--; }
            continue;
        }
        if (op->op != OP_WKV) continue;
        if (!live[op->obj]) {
            live[op->obj] = 1;
            live_cnt++;
            if (live_cnt > peak_live) peak_live = live_cnt;
        }
        for (u64 p = 0; p < op->len; ) {
            u64 lg = op->off + p;
            int stripe = (int)((lg / CMD_PGS) % (u64)KSTRIPE);
            int lane = (int)((((u64)(op->obj - 2) * (u64)KSTRIPE) + (u64)stripe) % (u64)lanes);
            u64 n = op->len - p;
            u64 in = lg % CMD_PGS;
            if (n > CMD_PGS - in) n = CMD_PGS - in;
            if (!lane_open[lane] || lane_wp[lane] + n > ZONE_CAP_PGS) {
                if (!lane_open[lane]) active++;
                lane_open[lane] = 1;
                lane_wp[lane] = 0;
            }
            lane_wp[lane] += n;
            if (lane_wp[lane] == ZONE_CAP_PGS) {
                lane_open[lane] = 0;
                lane_wp[lane] = 0;
                active--;
            }
            p += n;
        }
        if (active > peak_active) peak_active = active;
    }

    free(live);
    free(lane_wp);
    free(lane_open);
    free(ev);
    if (peak_live_out) *peak_live_out = peak_live;
    return peak_active;
}

static void zns_configure_active_zone_mux(void) {
    REQUESTED_KSTRIPE = KSTRIPE;
    u64 dev_limit = 0;
    if (DEVICE_MAX_ACTIVE_ZONES && DEVICE_MAX_OPEN_ZONES)
        dev_limit = DEVICE_MAX_ACTIVE_ZONES < DEVICE_MAX_OPEN_ZONES ? DEVICE_MAX_ACTIVE_ZONES : DEVICE_MAX_OPEN_ZONES;
    else
        dev_limit = DEVICE_MAX_ACTIVE_ZONES ? DEVICE_MAX_ACTIVE_ZONES : DEVICE_MAX_OPEN_ZONES;
    ZNS_ACTIVE_ZONE_LIMIT = MAX_ACTIVE_ZONES_ARG ? MAX_ACTIVE_ZONES_ARG : dev_limit;

    if (!ZNS_ACTIVE_ZONE_LIMIT) {
        PEAK_KV_ACTIVE_ZONES = trace_peak_kv_active_zones_for_k(KSTRIPE, &PEAK_LIVE_KV_SESSIONS);
        fprintf(stderr,
                "zns active-zone mux: mode=exclusive max_active_zones=unlimited peak_live_kv_sessions=%" PRIu64
                " requested_k=%d effective_k=%d peak_kv_active_zones=%" PRIu64 "\n",
                PEAK_LIVE_KV_SESSIONS, REQUESTED_KSTRIPE, KSTRIPE, PEAK_KV_ACTIVE_ZONES);
        return;
    }

    int best_k = 0;
    u64 best_peak = 0, best_live = 0;
    for (int k = KSTRIPE; k >= 1; k--) {
        u64 live = 0;
        u64 peak = trace_peak_kv_active_zones_for_k(k, &live);
        if (peak + ZNS_META_ACTIVE_ZONES <= ZNS_ACTIVE_ZONE_LIMIT) {
            best_k = k;
            best_peak = peak;
            best_live = live;
            break;
        }
    }

    if (!best_k) {
        u64 kv_lanes = ZNS_ACTIVE_ZONE_LIMIT > ZNS_META_ACTIVE_ZONES ?
                       ZNS_ACTIVE_ZONE_LIMIT - ZNS_META_ACTIVE_ZONES : 0;
        if (!kv_lanes) {
            KSTRIPE = 1;
            PEAK_KV_ACTIVE_ZONES = trace_peak_kv_active_zones_for_k(1, &PEAK_LIVE_KV_SESSIONS);
            fprintf(stderr,
                    "zns active-zone limit too small: max_active_zones=%" PRIu64
                    " metadata_reserve=%d peak_live_kv_sessions=%" PRIu64
                    " peak_kv_active_zones_at_k1=%" PRIu64 "\n",
                    ZNS_ACTIVE_ZONE_LIMIT, ZNS_META_ACTIVE_ZONES,
                    PEAK_LIVE_KV_SESSIONS, PEAK_KV_ACTIVE_ZONES);
            request_storage_stop(3, "zns_max_active_zones_too_small", 0, 0, 0, NULL);
            return;
        }
        if (kv_lanes > 0x7fffffffULL) kv_lanes = 0x7fffffffULL;
        KV_SHARED = 1;
        KV_SHARED_LANES = (int)kv_lanes;
        PEAK_KV_ACTIVE_ZONES = trace_peak_kv_shared_active_zones(KV_SHARED_LANES, &PEAK_LIVE_KV_SESSIONS);
        fprintf(stderr,
                "zns active-zone mux: mode=shared max_active_zones=%" PRIu64
                " peak_live_kv_sessions=%" PRIu64
                " requested_k=%d shared_kv_lanes=%d peak_kv_active_zones=%" PRIu64
                " active_zone_bound=%" PRIu64 "\n",
                ZNS_ACTIVE_ZONE_LIMIT, PEAK_LIVE_KV_SESSIONS, REQUESTED_KSTRIPE,
                KV_SHARED_LANES, PEAK_KV_ACTIVE_ZONES,
                PEAK_KV_ACTIVE_ZONES + ZNS_META_ACTIVE_ZONES);
        return;
    }

    KSTRIPE = best_k;
    PEAK_KV_ACTIVE_ZONES = best_peak;
    PEAK_LIVE_KV_SESSIONS = best_live;
    fprintf(stderr,
            "zns active-zone mux: mode=exclusive max_active_zones=%" PRIu64
            " peak_live_kv_sessions=%" PRIu64
            " requested_k=%d effective_k=%d peak_kv_active_zones=%" PRIu64
            " active_zone_bound=%" PRIu64 "\n",
            ZNS_ACTIVE_ZONE_LIMIT, PEAK_LIVE_KV_SESSIONS, REQUESTED_KSTRIPE, KSTRIPE,
            PEAK_KV_ACTIVE_ZONES, PEAK_KV_ACTIVE_ZONES + ZNS_META_ACTIVE_ZONES);
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

/* Write workers preserve per-stream ordering. In exclusive mode stream ==
 * stripe chain; in shared mode stream == physical shared KV lane. */
struct sw_arg { io_t *ios; int nio, w, cls, stride, by_stream; };
static int sw_should_run(struct sw_arg *a, int i) {
    return a->by_stream ? (a->ios[i].stream % a->stride) == a->w : (i % a->stride) == a->w;
}
static void *sw_worker(void *a_) {
    struct sw_arg *a = a_;
    char *buf; if (posix_memalign((void **)&buf, PGSZ, CMDSZ)) return NULL;
    memset(buf, 0x5A, CMDSZ);
    for (int i = 0; i < a->nio; i++) if (sw_should_run(a, i)) {
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
    int streams = KV_SHARED ? KV_SHARED_LANES : KSTRIPE;
    if (streams < 1) streams = 1;
    int k = streams < nio ? streams : (nio ? nio : 1);
    if (k > MAXK) k = MAXK;
    for (int w = 0; w < k; w++) {
        args[w] = (struct sw_arg){ ios, nio, w, cls, k, 1 };
        pthread_create(&th[w], NULL, sw_worker, &args[w]);
    }
    for (int w = 0; w < k; w++) pthread_join(th[w], NULL);
    return now_ns() - t0;
}

static void *sr_worker(void *a_) {
    struct sw_arg *a = a_;
    char *buf; if (posix_memalign((void **)&buf, PGSZ, CMDSZ)) return NULL;
    for (int i = 0; i < a->nio; i++) if (sw_should_run(a, i)) {
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
    if (k > MAXK) k = MAXK;
    for (int w = 0; w < k; w++) {
        args[w] = (struct sw_arg){ ios, nio, w, cls, k, 0 };
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
    int ob_locked = 0, ml_locked = 0, ix_locked = 0, kv_locked = 0;

    if (stop_requested()) { free(ios); return -1; }

    if (MODE_ZNS && KV_SHARED && (o->op == OP_WKV || o->op == OP_RKV || o->op == OP_DEL)) {
        pthread_mutex_lock(&kv_shared_mu); kv_locked = 1;
        pthread_mutex_lock(&ob->mu); ob_locked = 1;

        if (o->op == OP_DEL) {
            kv_shared_delete_obj_locked(ob);
            pthread_mutex_unlock(&ob->mu); ob_locked = 0;
            if (kv_shared_gc_locked(sid, op_idx, trace_ns, o) < 0) goto fail;
            pthread_mutex_unlock(&kv_shared_mu); kv_locked = 0;
            free(ios); return 0;
        }

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
                int lane = kv_shared_lane_for(o->obj, c);
                u64 n = o->len - p;
                u64 lane_rem = kv_shared_lane_rem_locked(lane);
                if (n > CMD_PGS - in) n = CMD_PGS - in;
                if (n > lane_rem) n = lane_rem;

                u64 pg = 0;
                if (!n || kv_shared_append_locked(lane, n, &pg) < 0) {
                    request_storage_stop(3, "zns_shared_out_of_zones", sid, op_idx, trace_ns, o);
                    goto fail;
                }
                ios[nio++] = (io_t){ pg, n, lane };
                obj_add_ext_nomerge(ob, pg, n);
                p += n;
            }
            cls = C_KVWR;
            pthread_mutex_unlock(&ob->mu); ob_locked = 0;
            dur = zns_striped_write(ios, nio, measure ? cls : -1);
            pthread_mutex_unlock(&kv_shared_mu); kv_locked = 0;
            goto acct;
        }

        obj_emit_ios(ob, o->off, o->len, ios, &nio);
        cls = C_KVRD;
        pthread_mutex_unlock(&ob->mu); ob_locked = 0;
        dur = zns_striped_read(ios, nio, measure ? cls : -1);
        pthread_mutex_unlock(&kv_shared_mu); kv_locked = 0;
        goto acct;
    }

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
                if (!ob->zc_n[c] || ob->zc_wp[c] + n > ZONE_CAP_PGS) {
                    if (ob->zc_n[c] == ob->zc_cap[c]) { ob->zc_cap[c] = ob->zc_cap[c] ? ob->zc_cap[c]*2 : 8; ob->zc[c] = realloc(ob->zc[c], ob->zc_cap[c]*4); }
                    int z = zns_alloc_zone();
                    if (z < 0) {
                        request_storage_stop(3, "zns_out_of_zones", sid, op_idx, trace_ns, o);
                        goto fail;
                    }
                    ob->zc[c][ob->zc_n[c]++] = z; ob->zc_wp[c] = 0;
                }
                u64 pg = (u64)ob->zc[c][ob->zc_n[c]-1] * ZONE_PGS + ob->zc_wp[c];
                ios[nio++] = (io_t){ pg, n, c };
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
                    if (ml_open < 0 || ml_open_wp == ZONE_CAP_PGS) {
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
                    u64 zrem = ZONE_CAP_PGS - ml_open_wp;
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
                    if (ix_open < 0 || ix_open_wp == ZONE_CAP_PGS) {
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
                                    if (ix_open_wp == ZONE_CAP_PGS) {
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
    if (kv_locked) pthread_mutex_unlock(&kv_shared_mu);
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
    if (MODE_ZNS) {
        fprintf(out,
                "ZNS_ACTIVE_MUX mode=%s max_active_zones=%" PRIu64
                " requested_k=%d effective_k=%d shared_kv_lanes=%d peak_live_kv_sessions=%" PRIu64
                " peak_kv_active_zones=%" PRIu64
                " metadata_active_reserve=%d active_zone_bound=%" PRIu64 "\n",
                KV_SHARED ? "shared" : "exclusive", ZNS_ACTIVE_ZONE_LIMIT,
                REQUESTED_KSTRIPE, KSTRIPE, KV_SHARED_LANES,
                PEAK_LIVE_KV_SESSIONS, PEAK_KV_ACTIVE_ZONES, ZNS_META_ACTIVE_ZONES,
                PEAK_KV_ACTIVE_ZONES + ZNS_META_ACTIVE_ZONES);
    }
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
        else if (!strcmp(argv[i], "--max-active-zones") || !strcmp(argv[i], "--max-open-zones") || !strcmp(argv[i], "-A"))
            MAX_ACTIVE_ZONES_ARG = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-k")) { 
            KSTRIPE = atoi(argv[++i]);
            if (KSTRIPE < 1) KSTRIPE = 1; 
            if (KSTRIPE > MAXK) KSTRIPE = MAXK; 
        }
    }
    if (!tracef || !dev) { fprintf(stderr, "usage: replayer -t trace -d /dev/nvmeXnY -m conv|zns -o out [-s scale] [-q qd] [-k kv_stripes] [-A max_active_or_open_zones] [--progress-interval n] [--no-progress]\n"); return 1; }
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
        char sysp[256];
        const char *base = strrchr(dev, '/'); base = base ? base + 1 : dev;
        snprintf(sysp, sizeof sysp, "/sys/block/%s/queue/chunk_sectors", base);
        read_u64_file(sysp, &zsec);
        if (!zsec) { fprintf(stderr, "cannot read zone size (chunk_sectors); is %s zoned?\n", dev); return 1; }
        ZONE_PGS = zsec * 512 / PGSZ; N_ZONES = DEV_PGS / ZONE_PGS;
        ZONE_CAP_PGS = zns_report_zone_capacity_pgs();
        if (!ZONE_CAP_PGS || ZONE_CAP_PGS > ZONE_PGS) ZONE_CAP_PGS = ZONE_PGS;
        fprintf(stderr, "zones: %" PRIu64 " x %.1f MiB, capacity %.1f MiB\n",
                N_ZONES, ZONE_PGS * PGSZ / 1048576.0, ZONE_CAP_PGS * PGSZ / 1048576.0);
        snprintf(sysp, sizeof sysp, "/sys/block/%s/queue/max_active_zones", base);
        read_u64_file(sysp, &DEVICE_MAX_ACTIVE_ZONES);
        snprintf(sysp, sizeof sysp, "/sys/block/%s/queue/max_open_zones", base);
        read_u64_file(sysp, &DEVICE_MAX_OPEN_ZONES);
        zns_configure_active_zone_mux();
        zfree = malloc(N_ZONES * 4);
        for (u64 i = 0; i < N_ZONES; i++) zfree[i] = (int)i;
        zf_head = 0; zf_tail = 0; zf_cnt = N_ZONES; zf_tail = N_ZONES;
        if (KV_SHARED) {
            kv_lane_zone = malloc((size_t)KV_SHARED_LANES * sizeof(*kv_lane_zone));
            kv_lane_wp = calloc((size_t)KV_SHARED_LANES, sizeof(*kv_lane_wp));
            kv_zone_live = calloc(N_ZONES, sizeof(*kv_zone_live));
            kv_zone_used = calloc(N_ZONES, 1);
            for (int i = 0; i < KV_SHARED_LANES; i++) kv_lane_zone[i] = -1;
        }
        ix_zone_of = malloc(IX_PGS * 4); ix_slot_of = malloc(IX_PGS * 4);
        for (u64 i = 0; i < IX_PGS; i++) ix_zone_of[i] = -1;
        zn_live = calloc(N_ZONES, 4);
        IX_BUDGET_Z = (int)(2 * ((IX_PGS + ZONE_CAP_PGS - 1) / ZONE_CAP_PGS)) + 1;
        ixz = malloc((IX_BUDGET_Z + 8) * 4);
        ml_budget_z = (int)((ML_PGS + ZONE_CAP_PGS - 1) / ZONE_CAP_PGS) + 1;
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
