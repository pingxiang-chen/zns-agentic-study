/*
 * replayer.c — replay the agentic-LLM offloading trace (gen_trace.py format)
 * against a REAL block device: an NVMeVirt conventional-SSD namespace or an
 * NVMeVirt ZNS namespace (also works on real NVMe / ZNS drives).
 *
 * Conventional mode (-m conv):
 *   - user-space extent allocator over the raw LBA space (first-fit, coalescing),
 *     mirrors the simulator's host layer; DELETE issues BLKDISCARD (TRIM).
 * ZNS mode (-m zns):
 *   - KV blobs use adaptive private session tails with shared overflow and a
 *     dedicated GC destination; --kv-policy legacy retains fixed placement.
 *   - FIFO memlog zones, log-structured index zones with host compaction;
 *     DELETE issues BLKRESETZONE.
 *   - host-managed GC event counters are printed to stderr every 5 seconds.
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
 *   [-j workers]     bounded session replay workers (default auto, capped at 256)
 *   [-A zones]       override max_open_zones; --max-active-zones overrides active zones
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
static int QD = 32, MODE_ZNS = 0, FD = -1, SESSION_WORKERS = 0;
static int KSTRIPE = 8;      /* -k: maximum private streams in adaptive mode;
                                parallel zone chains per blob in legacy mode.
                                Each chain is written strictly sequentially
                                (write-pointer safe); K chains in parallel
                                recover the stripe bandwidth that one QD1
                                zone stream cannot exploit (ZenFS-style). */
static int REQUESTED_KSTRIPE = 8;
static u64 MAX_OPEN_ZONES_ARG = 0, MAX_ACTIVE_ZONES_ARG = 0;
static u64 DEVICE_MAX_ACTIVE_ZONES = 0, DEVICE_MAX_OPEN_ZONES = 0, ZNS_OPEN_ZONE_LIMIT = 0;
static u64 ZNS_ACTIVE_ZONE_LIMIT = 0, ZNS_ACTIVE_ZONES = 0, PEAK_ZNS_ACTIVE_ZONES = 0;
static u64 KV_OPEN_ZONE_LIMIT = 0, KV_OPEN_ZONES = 0, PEAK_KV_OPEN_ZONES = 0;
static u64 LIVE_KV_SESSIONS = 0, PEAK_LIVE_KV_SESSIONS = 0;
static int KV_SHARED = 0, KV_SHARED_LANES = 0;
/* Adaptive policy uses lane 0 for GC, lane 1 for shared overflow, and
 * dynamically assigns the remaining lanes to private session tails. */
static int KV_ADAPTIVE = 1, KV_ADAPTIVE_READY;
static u32 *kv_lane_owner;
static u64 *kv_zone_written;
static int kv_pressure;
static u64 kv_private_pages, kv_shared_pages, kv_policy_changes, kv_peak_tail_pages;
static u64 kv_metadata_reserve;

#define KV_GC_MIN_FREE_ZONE_RESERVE 1
#define ZNS_META_OPEN_ZONES 2        /* one memlog append zone + one index append zone */
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
static int *zfree; static unsigned char *zactive; static u64 zf_head, zf_tail, zf_cnt; static pthread_mutex_t z_mu = PTHREAD_MUTEX_INITIALIZER;
static u64 zns_allocatable_zone_count(void) {
    pthread_mutex_lock(&z_mu);
    u64 n = zf_cnt;
    if (ZNS_ACTIVE_ZONE_LIMIT) {
        u64 active_room = ZNS_ACTIVE_ZONES < ZNS_ACTIVE_ZONE_LIMIT ?
            ZNS_ACTIVE_ZONE_LIMIT - ZNS_ACTIVE_ZONES : 0;
        if (n > active_room) n = active_room;
    }
    pthread_mutex_unlock(&z_mu);
    return n;
}
static int zns_alloc_zone_reserved(u64 reserve) {
    pthread_mutex_lock(&z_mu);
    if (zf_cnt <= reserve || (ZNS_ACTIVE_ZONE_LIMIT &&
        ZNS_ACTIVE_ZONES + reserve >= ZNS_ACTIVE_ZONE_LIMIT)) {
        pthread_mutex_unlock(&z_mu);
        return -1;
    }
    int z = zfree[zf_head % N_ZONES]; zf_head++; zf_cnt--;
    if (!zactive[z]) {
        zactive[z] = 1;
        ZNS_ACTIVE_ZONES++;
        if (ZNS_ACTIVE_ZONES > PEAK_ZNS_ACTIVE_ZONES)
            PEAK_ZNS_ACTIVE_ZONES = ZNS_ACTIVE_ZONES;
    }
    pthread_mutex_unlock(&z_mu);
    return z;
}
static int zns_alloc_zone(void) {
    return zns_alloc_zone_reserved(KV_ADAPTIVE_READY ? 1 : 0);
}
static void zns_note_zone_full(int z) {
    pthread_mutex_lock(&z_mu);
    if (zactive && z >= 0 && (u64)z < N_ZONES && zactive[z]) {
        zactive[z] = 0;
        if (ZNS_ACTIVE_ZONES) ZNS_ACTIVE_ZONES--;
    }
    pthread_mutex_unlock(&z_mu);
}
static void zns_reset_zone(int z) {
#ifdef HAVE_BLKZONED
    struct blk_zone_range r = { .sector = (u64)z * ZONE_PGS * (PGSZ / 512), .nr_sectors = ZONE_PGS * (PGSZ / 512) };
    if (ioctl(FD, BLKRESETZONE, &r) < 0) { perror("BLKRESETZONE"); exit(3); }
#endif
    pthread_mutex_lock(&z_mu);
    if (zactive && z >= 0 && (u64)z < N_ZONES && zactive[z]) {
        zactive[z] = 0;
        if (ZNS_ACTIVE_ZONES) ZNS_ACTIVE_ZONES--;
    }
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

/* Runtime KV-lane mode multiplexes live sessions over the open-zone budget left
 * after reserving one memlog append zone and one index append zone. KV
 * operations are serialized so shared lane write pointers, live-session state,
 * and relocation updates cannot race with reads. */
typedef struct kv_zone_ref { u32 obj; struct kv_zone_ref *next; } kv_zone_ref_t;
static int *kv_lane_zone;
static u64 *kv_lane_wp, *kv_zone_live;
static unsigned char *kv_zone_used;
static int *kv_obj_slot;
static u32 *kv_active_obj;
static kv_zone_ref_t **kv_zone_refs;
static u64 *kv_obj_gc_seen, kv_gc_epoch;
static pthread_mutex_t kv_shared_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- raw I/O helpers ---------------- */
static void *iobuf(void) { static __thread void *b = NULL; if (!b) { if (posix_memalign(&b, PGSZ, CMDSZ)) exit(9); memset(b, 0xA5, CMDSZ); } return b; }
static void pio(int wr, u64 pg, u64 npg) {          /* synchronous single command */
    void *b = iobuf();
    ssize_t r = wr ? pwrite(FD, b, npg * PGSZ, pg * PGSZ) : pread(FD, b, npg * PGSZ, pg * PGSZ);
    if (r != (ssize_t)(npg * PGSZ)) { fprintf(stderr, "short or failed %s\n", wr ? "pwrite" : "pread"); exit(8); }
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
            ios[(*nio)++] = (io_t){ pg, n, 0 };
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
typedef struct op {
    u64 gap; u32 obj; u64 off, len, line_no; uint8_t op, ts;
    /* Protected by sched_mu during replay; released after actual I/O completes. */
    struct op *kv_next;
    u32 kv_wait_sid;
    int kv_ready;
} op_t;
typedef struct { op_t *ops; u32 n; u64 t0; } sess_t;
static sess_t *sess; static u32 n_sess;
static int is_kv_op(const op_t *o) {
    return o->op == OP_WKV || o->op == OP_RKV || o->op == OP_DEL;
}
static const char *TRACE_PATH;
static u64 RPROG_INTERVAL = 10000;
static u64 RPROG_TOTAL_OPS, RPROG_TOTAL_LOG_WRITES, RPROG_TOTAL_LOG_PAGES;
static u64 RPROG_ISSUED_OPS, RPROG_ISSUED_LOG_WRITES, RPROG_ISSUED_LOG_PAGES;
static pthread_mutex_t rprog_mu = PTHREAD_MUTEX_INITIALIZER;

enum { ZNS_HOST_GC_KV = 0, ZNS_HOST_GC_INDEX = 1, ZNS_HOST_GC_MEMLOG = 2 };
#define ZNS_HOST_GC_EVENT_INTERVAL_NS (5ULL * 1000000000ULL)
static u64 ZNS_USER_WRITE_PAGES, ZNS_TOTAL_USER_WRITE_PAGES;
static u64 ZNS_HOST_COPY_PAGES, ZNS_TOTAL_HOST_COPY_PAGES;
static u64 ZNS_HOST_GC_COUNT, ZNS_TOTAL_HOST_GC_COUNT;
static u64 ZNS_HOST_GC_KV_COUNT, ZNS_TOTAL_HOST_GC_KV_COUNT;
static u64 ZNS_HOST_GC_INDEX_COUNT, ZNS_TOTAL_HOST_GC_INDEX_COUNT;
static u64 ZNS_HOST_GC_MEMLOG_COUNT, ZNS_TOTAL_HOST_GC_MEMLOG_COUNT;
static u64 ZNS_HOST_GC_RESET_ZONES, ZNS_TOTAL_HOST_GC_RESET_ZONES;
static pthread_t ZNS_HOST_GC_EVENT_THREAD;
static int ZNS_HOST_GC_EVENT_THREAD_STARTED, ZNS_HOST_GC_EVENT_STOP;

static void zns_count_pages(u64 *measured, u64 *total, u64 pages, int measure) {
    if (!MODE_ZNS || !pages) return;
    __sync_fetch_and_add(total, pages);
    if (measure) __sync_fetch_and_add(measured, pages);
}

static void zns_note_user_write_pages(u64 pages, int measure) {
    zns_count_pages(&ZNS_USER_WRITE_PAGES, &ZNS_TOTAL_USER_WRITE_PAGES, pages, measure);
}

static void zns_note_host_copy_pages(u64 pages, int measure) {
    zns_count_pages(&ZNS_HOST_COPY_PAGES, &ZNS_TOTAL_HOST_COPY_PAGES, pages, measure);
}

static void zns_note_host_gc(int kind, u64 reset_zones, int measure) {
    u64 *measured_kind = NULL, *total_kind = NULL;

    if (!MODE_ZNS) return;

    __sync_fetch_and_add(&ZNS_TOTAL_HOST_GC_COUNT, 1);
    __sync_fetch_and_add(&ZNS_TOTAL_HOST_GC_RESET_ZONES, reset_zones);
    if (measure) {
        __sync_fetch_and_add(&ZNS_HOST_GC_COUNT, 1);
        __sync_fetch_and_add(&ZNS_HOST_GC_RESET_ZONES, reset_zones);
    }

    switch (kind) {
    case ZNS_HOST_GC_KV:
        measured_kind = &ZNS_HOST_GC_KV_COUNT;
        total_kind = &ZNS_TOTAL_HOST_GC_KV_COUNT;
        break;
    case ZNS_HOST_GC_INDEX:
        measured_kind = &ZNS_HOST_GC_INDEX_COUNT;
        total_kind = &ZNS_TOTAL_HOST_GC_INDEX_COUNT;
        break;
    case ZNS_HOST_GC_MEMLOG:
        measured_kind = &ZNS_HOST_GC_MEMLOG_COUNT;
        total_kind = &ZNS_TOTAL_HOST_GC_MEMLOG_COUNT;
        break;
    default:
        return;
    }

    __sync_fetch_and_add(total_kind, 1);
    if (measure) __sync_fetch_and_add(measured_kind, 1);
}

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

static u64 kv_hash64(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static u64 kv_shared_gc_reserve_locked(void) {
    if (KV_ADAPTIVE_READY) return 1;
    u64 r = KV_SHARED_LANES < KSTRIPE ? (u64)KV_SHARED_LANES : (u64)KSTRIPE;
    if (r < KV_GC_MIN_FREE_ZONE_RESERVE) r = KV_GC_MIN_FREE_ZONE_RESERVE;
    return r;
}

static void kv_zone_ref_add_locked(int z, u32 obj) {
    if (!kv_zone_refs || z < 0 || (u64)z >= N_ZONES) return;
    kv_zone_ref_t *r = malloc(sizeof *r);
    if (!r) exit(9);
    r->obj = obj;
    r->next = kv_zone_refs[z];
    kv_zone_refs[z] = r;
}

static void kv_zone_refs_clear_locked(int z) {
    if (!kv_zone_refs || z < 0 || (u64)z >= N_ZONES) return;
    kv_zone_ref_t *r = kv_zone_refs[z];
    while (r) {
        kv_zone_ref_t *next = r->next;
        free(r);
        r = next;
    }
    kv_zone_refs[z] = NULL;
}


static void kv_shared_note_live_obj_locked(u32 obj) {
    if (!kv_obj_slot || obj >= n_objs + 2 || kv_obj_slot[obj] >= 0) return;
    kv_obj_slot[obj] = (int)LIVE_KV_SESSIONS;
    kv_active_obj[LIVE_KV_SESSIONS++] = obj;
    if (LIVE_KV_SESSIONS > PEAK_LIVE_KV_SESSIONS)
        PEAK_LIVE_KV_SESSIONS = LIVE_KV_SESSIONS;
}

static void kv_shared_note_deleted_obj_locked(u32 obj) {
    if (!kv_obj_slot || obj >= n_objs + 2 || kv_obj_slot[obj] < 0) return;
    int slot = kv_obj_slot[obj];
    u64 last = LIVE_KV_SESSIONS - 1;
    if ((u64)slot != last) {
        u32 moved = kv_active_obj[last];
        kv_active_obj[slot] = moved;
        kv_obj_slot[moved] = slot;
    }
    kv_obj_slot[obj] = -1;
    LIVE_KV_SESSIONS--;
}

static void kv_shared_note_zone_open_locked(void) {
    KV_OPEN_ZONES++;
    if (KV_OPEN_ZONES > PEAK_KV_OPEN_ZONES)
        PEAK_KV_OPEN_ZONES = KV_OPEN_ZONES;
}

static void kv_shared_note_zone_closed_locked(void) {
    if (KV_OPEN_ZONES) KV_OPEN_ZONES--;
}

static int kv_shared_lane_for_locked(u32 obj, int stripe) {
    if (KV_SHARED_LANES <= 0) return 0;
    u64 h = kv_hash64(((u64)obj << 32) ^ (u32)stripe);
    return (int)(h % (u64)KV_SHARED_LANES);
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

static int kv_shared_append_locked(u32 obj, int lane, u64 n, u64 *pg) {
    if (lane < 0 || lane >= KV_SHARED_LANES || !n) return -1;
    if (kv_lane_zone[lane] < 0 || kv_lane_wp[lane] == ZONE_CAP_PGS) {
        int z = KV_ADAPTIVE_READY && lane == 0 ? zns_alloc_zone_reserved(0) : zns_alloc_zone();
        if (z < 0) return -1;
        kv_lane_zone[lane] = z;
        kv_lane_wp[lane] = 0;
        kv_zone_used[z] = 1;
        kv_shared_note_zone_open_locked();
    }
    if (n > ZONE_CAP_PGS - kv_lane_wp[lane]) return -1;
    int z = kv_lane_zone[lane];
    *pg = (u64)z * ZONE_PGS + kv_lane_wp[lane];
    kv_lane_wp[lane] += n;
    if (kv_zone_written) kv_zone_written[z] += n;
    kv_zone_live[z] += n;
    kv_zone_used[z] = 1;
    kv_zone_ref_add_locked(z, obj);
    if (kv_lane_wp[lane] == ZONE_CAP_PGS) {
        kv_shared_note_zone_closed_locked();
        zns_note_zone_full(z);
    }
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
    /* Callers hold kv_shared_mu and have completed all KV writes. A dead
     * partial zone remains an append destination until it is physically full. */
    if (kv_shared_zone_is_open_locked(z)) {
        fprintf(stderr, "refusing to reset a partial KV zone %d\n", z);
        exit(4);
    }
    int was_open = 0;
    for (int i = 0; i < KV_SHARED_LANES; i++) {
        if (kv_lane_zone[i] == z) {
            if (kv_lane_wp[i] < ZONE_CAP_PGS) was_open = 1;
            kv_lane_zone[i] = -1;
            kv_lane_wp[i] = 0;
            if (kv_lane_owner) kv_lane_owner[i] = 0;
        }
    }
    if (was_open) kv_shared_note_zone_closed_locked();
    kv_zone_live[z] = 0;
    kv_zone_used[z] = 0;
    if (kv_zone_written) kv_zone_written[z] = 0;
    kv_zone_refs_clear_locked(z);
    zns_reset_zone(z);
}

static int kv_shared_pick_victim_locked(int allow_relocation) {
    int best = -1;
    u64 best_live = UINT64_MAX;
    for (u64 z = 0; z < N_ZONES; z++) {
        if (!kv_zone_used[z]) continue;
        /* Keep dead tails too: later objects can consume their capacity. */
        if (kv_shared_zone_is_open_locked((int)z)) continue;
        if (!kv_zone_live[z]) return (int)z;
        if (kv_zone_live[z] >= ZONE_CAP_PGS) continue;
        if (!allow_relocation) continue;
        if (kv_zone_live[z] < best_live) {
            best_live = kv_zone_live[z];
            best = (int)z;
        }
    }
    return best;
}

static int kv_shared_relocate_range_locked(u32 obj, u64 logical, u64 oldpg, u64 len,
                                           ext_t **new_ext, int *new_n, int *new_cap,
                                           u32 sid, u32 op_idx, u64 trace_ns, const op_t *op,
                                           int measure) {
    while (len) {
        int stripe = (int)((logical / CMD_PGS) % (u64)KSTRIPE);
        int lane = KV_ADAPTIVE_READY ? 0 : kv_shared_lane_for_locked(obj, stripe);
        u64 n = len;
        u64 old_zrem = zns_zone_cap_rem(oldpg);
        if (!old_zrem) { fprintf(stderr, "shared KV relocation extent beyond zone capacity\n"); exit(4); }
        u64 cmd_rem = CMD_PGS - (logical % CMD_PGS);
        u64 lane_rem = kv_shared_lane_rem_locked(lane);
        if (n > old_zrem) n = old_zrem;
        if (n > cmd_rem) n = cmd_rem;
        if (n > lane_rem) n = lane_rem;

        u64 newpg = 0;
        if (!n || kv_shared_append_locked(obj, lane, n, &newpg) < 0) {
            request_storage_stop(3, "zns_shared_gc_out_of_zones", sid, op_idx, trace_ns, op);
            return -1;
        }
        pio(0, oldpg, n);
        pio(1, newpg, n);
        zns_note_host_copy_pages(n, measure);
        kv_shared_dec_live_range_locked(oldpg, n);
        ext_vec_add(new_ext, new_n, new_cap, newpg, n);
        oldpg += n;
        logical += n;
        len -= n;
    }
    return 0;
}

static int kv_shared_compact_obj_from_zone_locked(u32 obj, int victim,
                                                     u32 sid, u32 op_idx, u64 trace_ns,
                                                     const op_t *op, int measure) {
    if (obj < 2 || obj >= n_objs + 2) return 0;

    obj_t *ob = &objs[obj];
    pthread_mutex_lock(&ob->mu);
    if (!ob->n_ext) {
        pthread_mutex_unlock(&ob->mu);
        return 0;
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
                if (kv_shared_relocate_range_locked(obj, logical, phys, n,
                                                    &new_ext, &new_n, &new_cap,
                                                    sid, op_idx, trace_ns, op, measure) < 0) {
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
    return 0;
}

static int kv_shared_compact_zone_locked(int victim, u32 sid, u32 op_idx, u64 trace_ns, const op_t *op,
                                             int measure) {
    if (victim < 0 || (u64)victim >= N_ZONES || !kv_zone_used[victim]) return 0;
    if (kv_shared_zone_is_open_locked(victim)) return 0;

    u64 epoch = ++kv_gc_epoch;
    if (!epoch && kv_obj_gc_seen) {
        memset(kv_obj_gc_seen, 0, (size_t)(n_objs + 2) * sizeof(*kv_obj_gc_seen));
        kv_gc_epoch = 1;
        epoch = kv_gc_epoch;
    }

    if (kv_zone_refs && kv_zone_refs[victim]) {
        for (kv_zone_ref_t *r = kv_zone_refs[victim]; r; r = r->next) {
            u32 obj = r->obj;
            if (obj < 2 || obj >= n_objs + 2) continue;
            if (kv_obj_gc_seen) {
                if (kv_obj_gc_seen[obj] == epoch) continue;
                kv_obj_gc_seen[obj] = epoch;
            }
            if (kv_shared_compact_obj_from_zone_locked(obj, victim, sid, op_idx, trace_ns, op, measure) < 0)
                return -1;
        }
    }

    if (kv_zone_live[victim]) {
        for (u64 obj = 2; obj < n_objs + 2; obj++) {
            if (kv_obj_gc_seen && kv_obj_gc_seen[obj] == epoch) continue;
            if (kv_obj_gc_seen) kv_obj_gc_seen[obj] = epoch;
            if (kv_shared_compact_obj_from_zone_locked((u32)obj, victim, sid, op_idx, trace_ns, op, measure) < 0)
                return -1;
        }
    }

    if (kv_zone_live[victim]) {
        fprintf(stderr, "warning: shared KV compaction left zone %d with %" PRIu64 " live pages\n",
                victim, kv_zone_live[victim]);
    }
    if (kv_zone_live[victim]) return -1;
    kv_shared_reset_zone_locked(victim);
    zns_note_host_gc(ZNS_HOST_GC_KV, 1, measure);
    return 0;
}

static int kv_shared_gc_locked(u32 sid, u32 op_idx, u64 trace_ns, const op_t *op, int measure) {
    for (u64 pass = 0; pass < N_ZONES; pass++) {
        int victim = kv_shared_pick_victim_locked(0);
        if (victim < 0) return 0;
        if (kv_shared_compact_zone_locked(victim, sid, op_idx, trace_ns, op, measure) < 0) return -1;
    }
    return 0;
}

static u64 kv_shared_estimate_new_zones_locked(u32 obj, u64 off, u64 len) {
    if (KV_SHARED_LANES <= 0 || !len) return 0;
    u64 *lane_rem = malloc((size_t)KV_SHARED_LANES * sizeof(*lane_rem));
    if (!lane_rem) exit(9);
    for (int i = 0; i < KV_SHARED_LANES; i++)
        lane_rem[i] = (kv_lane_zone[i] >= 0 && kv_lane_wp[i] < ZONE_CAP_PGS) ?
            ZONE_CAP_PGS - kv_lane_wp[i] : 0;

    u64 need = 0;
    for (u64 p = 0; p < len; ) {
        u64 lg = off + p;
        u64 j = lg / CMD_PGS, in = lg % CMD_PGS;
        int c = (int)(j % (u64)KSTRIPE);
        int lane = kv_shared_lane_for_locked(obj, c);
        u64 n = len - p;
        if (n > CMD_PGS - in) n = CMD_PGS - in;
        if (!lane_rem[lane]) {
            need++;
            lane_rem[lane] = ZONE_CAP_PGS;
        }
        if (n > lane_rem[lane]) n = lane_rem[lane];
        if (!n) { free(lane_rem); return UINT64_MAX; }
        lane_rem[lane] -= n;
        p += n;
    }
    free(lane_rem);
    return need;
}

static int kv_shared_gc_until_allocatable_locked(u64 needed, u32 sid, u32 op_idx,
                                                 u64 trace_ns, const op_t *op, int measure) {
    u64 reserve = kv_shared_gc_reserve_locked();
    u64 target = needed + reserve;

    for (u64 pass = 0; pass < N_ZONES; pass++) {
        u64 before = zns_allocatable_zone_count();
        if (before >= target || (pass && before >= needed)) return 0;

        int allow_relocation = before > 0;
        int victim = kv_shared_pick_victim_locked(allow_relocation);
        if (victim < 0) break;
        if (kv_shared_compact_zone_locked(victim, sid, op_idx, trace_ns, op, measure) < 0)
            return -1;

        u64 after = zns_allocatable_zone_count();
        if (after <= before && after >= needed) return 0;
        if (after <= before && !allow_relocation) break;
    }

    if (zns_allocatable_zone_count() < needed) {
        request_storage_stop(3, "zns_shared_out_of_zones", sid, op_idx, trace_ns, op);
        return -1;
    }
    return 0;
}

static int kv_shared_prepare_write_locked(u32 obj, u64 off, u64 len,
                                          u32 sid, u32 op_idx, u64 trace_ns,
                                          const op_t *op, int measure) {
    u64 needed = kv_shared_estimate_new_zones_locked(obj, off, len);
    if (needed == UINT64_MAX) {
        request_storage_stop(3, "zns_shared_bad_write_shape", sid, op_idx, trace_ns, op);
        return -1;
    }
    return kv_shared_gc_until_allocatable_locked(needed, sid, op_idx, trace_ns, op, measure);
}

/* Snapshot under kv_shared_mu; metadata's zone counters use z_mu. */
static void kv_space_report(FILE *out, const char *tag) {
    u64 live = 0, written = 0, tails = 0;
    for (u64 z = 0; z < N_ZONES; z++) {
        if (!kv_zone_used[z]) continue;
        live += kv_zone_live[z];
        written += kv_zone_written ? kv_zone_written[z] : kv_zone_live[z];
    }
    for (int i = 0; i < KV_SHARED_LANES; i++)
        if (kv_lane_zone[i] >= 0 && kv_lane_wp[i] < ZONE_CAP_PGS)
            tails += ZONE_CAP_PGS - kv_lane_wp[i];
    if (tails > kv_peak_tail_pages) kv_peak_tail_pages = tails;
    pthread_mutex_lock(&z_mu);
    u64 empty = zf_cnt, active = ZNS_ACTIVE_ZONES;
    pthread_mutex_unlock(&z_mu);
    fprintf(out, "ZNS_SPACE[%s] policy=%s live_pages=%" PRIu64
            " invalid_pages=%" PRIu64 " tail_pages=%" PRIu64
            " peak_tail_pages=%" PRIu64 " empty_zones=%" PRIu64
            " active_zones=%" PRIu64 " allocatable_zones=%" PRIu64
            " pressure=%d private_pages=%" PRIu64 " shared_pages=%" PRIu64
            " policy_changes=%" PRIu64 "\n", tag,
            KV_ADAPTIVE_READY ? "adaptive" : "legacy", live,
            written >= live ? written - live : 0, tails, kv_peak_tail_pages,
            empty, active, zns_allocatable_zone_count(), kv_pressure,
            kv_private_pages, kv_shared_pages, kv_policy_changes);
}

static void kv_adaptive_pressure(void) {
    u64 live = 0, tails = 0;
    for (u64 z = 0; z < N_ZONES; z++) live += kv_zone_live[z];
    for (int i = 0; i < KV_SHARED_LANES; i++)
        if (kv_lane_zone[i] >= 0 && kv_lane_wp[i] < ZONE_CAP_PGS)
            tails += ZONE_CAP_PGS - kv_lane_wp[i];
    if (tails > kv_peak_tail_pages) kv_peak_tail_pages = tails;
    u64 available = zns_allocatable_zone_count();
    u64 low = kv_metadata_reserve + 2 + N_ZONES / 20;
    u64 high = low + 2 + N_ZONES / 20;
    int old = kv_pressure;
    if (available <= low || live > (N_ZONES * ZONE_CAP_PGS / 100) * 85)
        kv_pressure = 1;
    else if (available >= high && live < (N_ZONES * ZONE_CAP_PGS / 100) * 80)
        kv_pressure = 0;
    if (old != kv_pressure) kv_policy_changes++;
}

/* Additional streams need both a large pending append and a per-session share
 * of the slot budget. Drain existing tails when the allowance shrinks. */
static int kv_adaptive_lane(u32 obj, u64 remaining, u64 round) {
    kv_adaptive_pressure();
    int owned[MAXK], count = 0, free_lane = -1, shared = -1, reusable = -1;
    for (int i = 1; i < KV_SHARED_LANES; i++) {
        int partial = kv_lane_zone[i] >= 0 && kv_lane_wp[i] < ZONE_CAP_PGS;
        if (!partial) {
            kv_lane_owner[i] = 0;
            if (i >= 2 && free_lane < 0) free_lane = i;
        } else if (!kv_zone_live[kv_lane_zone[i]]) {
            /* The old owner has no live bytes here. Preserve the WP and
             * transfer the fullest tail before opening another zone. */
            if (reusable < 0 || kv_lane_wp[i] > kv_lane_wp[reusable]) reusable = i;
        } else if (kv_lane_owner[i] == obj && count < MAXK) owned[count++] = i;
        else if (kv_lane_owner[i] == UINT32_MAX &&
                 (shared < 0 || kv_lane_wp[i] > kv_lane_wp[shared])) shared = i;
    }
    int limit = 1;
    u64 sessions = LIVE_KV_SESSIONS ? LIVE_KV_SESSIONS : 1;
    u64 spare = zns_allocatable_zone_count();
    if (!kv_pressure && remaining >= 2 * ZONE_CAP_PGS) {
        u64 fair = (u64)(KV_SHARED_LANES - 2) / sessions;
        u64 capacity = spare > kv_metadata_reserve + 1 ?
            (spare - kv_metadata_reserve - 1) / sessions : 0;
        u64 large = remaining / ZONE_CAP_PGS;
        limit = KSTRIPE;
        if ((u64)limit > fair) limit = (int)fair;
        if ((u64)limit > capacity) limit = (int)capacity;
        if ((u64)limit > large) limit = (int)large;
        if (limit < 1) limit = 1;
    }
    if (count < limit && reusable >= 0) {
        kv_lane_owner[reusable] = obj;
        return reusable;
    }
    if (count < limit && shared >= 0) return shared;
    if (!kv_pressure && count < limit && free_lane >= 0 &&
        spare > kv_metadata_reserve + 2) {
        kv_lane_owner[free_lane] = obj;
        return free_lane;
    }
    if (count) {
        if (count > limit || kv_pressure) {
            int best = owned[0];
            for (int j = 1; j < count; j++)
                if (kv_lane_wp[owned[j]] > kv_lane_wp[best]) best = owned[j];
            return best;
        }
        return owned[round % (u64)count];
    }
    if (shared >= 0) return shared;
    if (spare > kv_metadata_reserve + 1) {
        kv_lane_owner[1] = UINT32_MAX;
        return 1;
    }
    /* No new slot: lend one existing private tail to the shared pool. Its
     * prior owner keeps all extent mappings, but loses exclusive reclamation. */
    int best = -1;
    for (int i = 2; i < KV_SHARED_LANES; i++) {
        if (kv_lane_zone[i] < 0 || kv_lane_wp[i] == ZONE_CAP_PGS) continue;
        if (best < 0 || kv_lane_wp[i] > kv_lane_wp[best]) best = i;
    }
    if (best >= 0) {
        kv_lane_owner[best] = UINT32_MAX;
        kv_policy_changes++;
        return best;
    }
    kv_lane_owner[1] = UINT32_MAX;
    return 1;
}

static int kv_adaptive_make_room(u32 sid, u32 op_idx, u64 trace_ns,
                                 const op_t *op, int measure) {
    u64 target = kv_metadata_reserve + 2;
    for (u64 pass = 0; pass < N_ZONES; pass++) {
        if (zns_allocatable_zone_count() >= target) return 0;
        int victim = kv_shared_pick_victim_locked(1);
        if (victim < 0) break;
        /* The dedicated GC tail plus one protected empty zone provide enough
         * destination capacity. No foreground I/O is pending at this point. */
        u64 rem = kv_lane_zone[0] >= 0 && kv_lane_wp[0] < ZONE_CAP_PGS ?
            ZONE_CAP_PGS - kv_lane_wp[0] : 0;
        if (kv_zone_live[victim] > rem && !zns_allocatable_zone_count()) break;
        if (kv_shared_compact_zone_locked(victim, sid, op_idx, trace_ns, op, measure) < 0)
            return -1;
    }
    return zns_allocatable_zone_count() >= target ? 0 : -1;
}

static void kv_shared_delete_obj_locked(u32 obj, obj_t *ob) {
    for (int i = 0; i < ob->n_ext; i++)
        kv_shared_dec_live_range_locked(ob->ext[i].lpn_start, ob->ext[i].npgs);
    kv_shared_note_deleted_obj_locked(obj);
    ob->n_ext = 0;
    ob->pgs = 0;
}

static u64 rprog_atomic_read_u64(u64 *v) {
    return __sync_add_and_fetch(v, 0);
}

typedef struct {
    u64 sample_ns;
    u64 host_gc_count;
    u64 host_gc_kv_count;
    u64 host_gc_index_count;
    u64 host_gc_memlog_count;
    u64 host_gc_reset_zones;
    u64 host_copy_pages;
} zns_host_gc_event_sample_t;

static void zns_host_gc_event_sample(zns_host_gc_event_sample_t *s) {
    s->sample_ns = now_ns();
    s->host_gc_count = rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_COUNT);
    s->host_gc_kv_count = rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_KV_COUNT);
    s->host_gc_index_count = rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_INDEX_COUNT);
    s->host_gc_memlog_count = rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_MEMLOG_COUNT);
    s->host_gc_reset_zones = rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_RESET_ZONES);
    s->host_copy_pages = rprog_atomic_read_u64(&ZNS_TOTAL_HOST_COPY_PAGES);
}

static u64 zns_host_gc_event_delta(u64 now, u64 prev) {
    return now >= prev ? now - prev : 0;
}

static int zns_host_gc_event_stop_requested(void) {
    return __sync_add_and_fetch(&ZNS_HOST_GC_EVENT_STOP, 0);
}

static int zns_host_gc_event_sleep_until(u64 target_ns) {
    for (;;) {
        if (zns_host_gc_event_stop_requested()) return 0;
        u64 now = now_ns();
        if (now >= target_ns) return 1;
        u64 wait = target_ns - now;
        if (wait > 100000000ULL) wait = 100000000ULL;
        struct timespec ts = { wait / 1000000000ULL, wait % 1000000000ULL };
        nanosleep(&ts, NULL);
    }
}

static void zns_host_gc_event_emit(const char *tag,
                                   const zns_host_gc_event_sample_t *prev,
                                   const zns_host_gc_event_sample_t *cur) {
    double elapsed = T0 && cur->sample_ns >= T0 ? (cur->sample_ns - T0) / 1000000000.0 : 0.0;
    double window = cur->sample_ns >= prev->sample_ns ?
        (cur->sample_ns - prev->sample_ns) / 1000000000.0 : 0.0;
    u64 count_delta = zns_host_gc_event_delta(cur->host_gc_count, prev->host_gc_count);
    u64 kv_delta = zns_host_gc_event_delta(cur->host_gc_kv_count, prev->host_gc_kv_count);
    u64 index_delta = zns_host_gc_event_delta(cur->host_gc_index_count, prev->host_gc_index_count);
    u64 memlog_delta = zns_host_gc_event_delta(cur->host_gc_memlog_count, prev->host_gc_memlog_count);
    u64 reset_delta = zns_host_gc_event_delta(cur->host_gc_reset_zones, prev->host_gc_reset_zones);
    u64 copy_delta = zns_host_gc_event_delta(cur->host_copy_pages, prev->host_copy_pages);

    pthread_mutex_lock(&rprog_mu);
    fprintf(stderr,
            "ZNS_HOST_MANAGEMENT_GC_EVENT[%s]: elapsed_s=%.3f window_s=%.3f"
            " host_gc_count_delta=%" PRIu64
            " host_gc_kv_count_delta=%" PRIu64
            " host_gc_index_count_delta=%" PRIu64
            " host_gc_memlog_count_delta=%" PRIu64
            " host_gc_reset_zones_delta=%" PRIu64
            " host_copy_pgs_delta=%" PRIu64
            " total_run_host_gc_count=%" PRIu64
            " total_run_host_gc_kv_count=%" PRIu64
            " total_run_host_gc_index_count=%" PRIu64
            " total_run_host_gc_memlog_count=%" PRIu64
            " total_run_host_gc_reset_zones=%" PRIu64
            " total_run_host_copy_pgs=%" PRIu64 "\n",
            tag, elapsed, window, count_delta, kv_delta, index_delta, memlog_delta,
            reset_delta, copy_delta, cur->host_gc_count, cur->host_gc_kv_count,
            cur->host_gc_index_count, cur->host_gc_memlog_count,
            cur->host_gc_reset_zones, cur->host_copy_pages);
    pthread_mutex_unlock(&rprog_mu);
}

static void *zns_host_gc_event_thread(void *arg) {
    (void)arg;
    zns_host_gc_event_sample_t prev, cur;
    zns_host_gc_event_sample(&prev);
    u64 next_emit_ns = now_ns() + ZNS_HOST_GC_EVENT_INTERVAL_NS;

    while (zns_host_gc_event_sleep_until(next_emit_ns)) {
        zns_host_gc_event_sample(&cur);
        zns_host_gc_event_emit("interval", &prev, &cur);
        prev = cur;
        next_emit_ns += ZNS_HOST_GC_EVENT_INTERVAL_NS;
        if (next_emit_ns <= now_ns())
            next_emit_ns = now_ns() + ZNS_HOST_GC_EVENT_INTERVAL_NS;
    }

    zns_host_gc_event_sample(&cur);
    if (cur.host_gc_count != prev.host_gc_count ||
        cur.host_gc_reset_zones != prev.host_gc_reset_zones ||
        cur.host_copy_pages != prev.host_copy_pages)
        zns_host_gc_event_emit("final", &prev, &cur);
    return NULL;
}

static void zns_host_gc_event_start(void) {
    if (!MODE_ZNS || !RPROG_INTERVAL) return;
    ZNS_HOST_GC_EVENT_STOP = 0;
    if (pthread_create(&ZNS_HOST_GC_EVENT_THREAD, NULL, zns_host_gc_event_thread, NULL) == 0) {
        ZNS_HOST_GC_EVENT_THREAD_STARTED = 1;
    } else {
        perror("pthread_create zns host gc event monitor");
    }
}

static void zns_host_gc_event_stop(void) {
    if (!ZNS_HOST_GC_EVENT_THREAD_STARTED) return;
    __sync_lock_test_and_set(&ZNS_HOST_GC_EVENT_STOP, 1);
    pthread_join(ZNS_HOST_GC_EVENT_THREAD, NULL);
    ZNS_HOST_GC_EVENT_THREAD_STARTED = 0;
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

static u64 zns_metadata_active_reserve(void) {
    if (!ZONE_CAP_PGS) return ZNS_META_OPEN_ZONES;
    u64 ix_budget = 2 * ((IX_PGS + ZONE_CAP_PGS - 1) / ZONE_CAP_PGS) + 1;
    u64 ml_budget = ((ML_PGS + ZONE_CAP_PGS - 1) / ZONE_CAP_PGS) + 1;
    u64 reserve = ix_budget + ml_budget;
    return reserve > ZNS_META_OPEN_ZONES ? reserve : ZNS_META_OPEN_ZONES;
}

static void zns_configure_open_zone_mux(void) {
    REQUESTED_KSTRIPE = KSTRIPE;
    u64 meta_active_reserve = zns_metadata_active_reserve();
    ZNS_OPEN_ZONE_LIMIT = MAX_OPEN_ZONES_ARG ? MAX_OPEN_ZONES_ARG : DEVICE_MAX_OPEN_ZONES;

    if (!ZNS_OPEN_ZONE_LIMIT && ZNS_ACTIVE_ZONE_LIMIT) {
        u64 active_room = 0;
        if (ZNS_ACTIVE_ZONE_LIMIT > meta_active_reserve + KV_GC_MIN_FREE_ZONE_RESERVE)
            active_room = ZNS_ACTIVE_ZONE_LIMIT - meta_active_reserve - KV_GC_MIN_FREE_ZONE_RESERVE;
        u64 kv_lanes = (u64)KSTRIPE;
        if (kv_lanes > active_room) kv_lanes = active_room;
        ZNS_OPEN_ZONE_LIMIT = kv_lanes + ZNS_META_OPEN_ZONES;
    }

    if (KV_ADAPTIVE && !ZNS_OPEN_ZONE_LIMIT)
        ZNS_OPEN_ZONE_LIMIT = N_ZONES;
    if (!ZNS_OPEN_ZONE_LIMIT) {
        fprintf(stderr,
                "zns open-zone mux: mode=exclusive max_open_zones=unlimited"
                " max_active_zones=unlimited requested_k=%d effective_k=%d\n",
                REQUESTED_KSTRIPE, KSTRIPE);
        return;
    }

    if (ZNS_OPEN_ZONE_LIMIT <= ZNS_META_OPEN_ZONES ||
        (ZNS_ACTIVE_ZONE_LIMIT && ZNS_ACTIVE_ZONE_LIMIT <= meta_active_reserve)) {
        fprintf(stderr,
                "zns zone limits too small: max_open_zones=%" PRIu64
                " max_active_zones=%" PRIu64 " metadata_open_reserve=%d"
                " metadata_active_reserve=%" PRIu64 "\n",
                ZNS_OPEN_ZONE_LIMIT, ZNS_ACTIVE_ZONE_LIMIT, ZNS_META_OPEN_ZONES,
                meta_active_reserve);
        request_storage_stop(3, "zns_zone_limits_too_small", 0, 0, 0, NULL);
        return;
    }

    KV_OPEN_ZONE_LIMIT = ZNS_OPEN_ZONE_LIMIT - ZNS_META_OPEN_ZONES;
    if (ZNS_ACTIVE_ZONE_LIMIT) {
        u64 active_kv_lanes = ZNS_ACTIVE_ZONE_LIMIT - meta_active_reserve;
        if (active_kv_lanes > KV_GC_MIN_FREE_ZONE_RESERVE)
            active_kv_lanes -= KV_GC_MIN_FREE_ZONE_RESERVE;
        else
            active_kv_lanes = 0;
        if (KV_OPEN_ZONE_LIMIT > active_kv_lanes)
            KV_OPEN_ZONE_LIMIT = active_kv_lanes;
    }
    if (!KV_OPEN_ZONE_LIMIT) {
        request_storage_stop(3, "zns_no_kv_open_zone_budget", 0, 0, 0, NULL);
        return;
    }
    if (KV_OPEN_ZONE_LIMIT > 0x7fffffffULL) KV_OPEN_ZONE_LIMIT = 0x7fffffffULL;
    KV_SHARED = 1;
    KV_SHARED_LANES = (int)KV_OPEN_ZONE_LIMIT;
    /* Legacy hashing should fill a small set of tails, rather than leave
     * one partial zone per available device open slot. */
    if (!KV_ADAPTIVE && KV_SHARED_LANES > KSTRIPE)
        KV_SHARED_LANES = KSTRIPE;
    if (KV_ADAPTIVE && KV_SHARED_LANES < 3) {
        request_storage_stop(3, "zns_adaptive_needs_three_kv_slots", 0, 0, 0, NULL);
        return;
    }

    fprintf(stderr,
            "zns open-zone mux: mode=runtime-shared max_open_zones=%" PRIu64
            " max_active_zones=%" PRIu64
            " metadata_open_reserve=%d metadata_active_reserve=%" PRIu64
            " requested_k=%d runtime_k_max=%d kv_lanes=%d\n",
            ZNS_OPEN_ZONE_LIMIT, ZNS_ACTIVE_ZONE_LIMIT, ZNS_META_OPEN_ZONES,
            meta_active_reserve, REQUESTED_KSTRIPE,
            KV_SHARED_LANES < KSTRIPE ? KV_SHARED_LANES : KSTRIPE, KV_SHARED_LANES);
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
        if (pwrite(FD, buf, a->ios[i].npg * PGSZ, a->ios[i].pg * PGSZ) != (ssize_t)(a->ios[i].npg * PGSZ)) { perror("zns pwrite"); exit(8); }
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
    if (KV_ADAPTIVE_READY) {
        int lanes[MAXK], used = 0;
        for (int i = 0; i < nio; i++) {
            int j = 0;
            while (j < used && lanes[j] != ios[i].stream) j++;
            if (j == used) {
                if (used == MAXK) { fprintf(stderr, "too many adaptive batch streams\n"); exit(4); }
                lanes[used++] = ios[i].stream;
            }
            ios[i].stream = j;
        }
        k = used;
    }
    if (!k) return 0;
    if (k == 1) {
        args[0] = (struct sw_arg){ ios, nio, 0, cls, 1, 1 };
        sw_worker(&args[0]);
    } else {
        int started[MAXK] = {0};
        for (int w = 0; w < k; w++) {
            args[w] = (struct sw_arg){ ios, nio, w, cls, k, 1 };
            started[w] = pthread_create(&th[w], NULL, sw_worker, &args[w]) == 0;
            if (!started[w]) sw_worker(&args[w]);
        }
        for (int w = 0; w < k; w++) if (started[w]) pthread_join(th[w], NULL);
    }
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

/* Build bounded batches of 1 MiB placement chunks; NVMe commands remain
 * 128 KiB. All batches finish before GC can move mappings or reset zones. */
static int kv_adaptive_write(op_t *o, io_t *ios, u32 sid, u32 op_idx,
                             u64 trace_ns, int measure, u64 *duration) {
    obj_t *ob = &objs[o->obj];
    u64 start = now_ns(), done = 0, round = 0;
    kv_shared_note_live_obj_locked(o->obj);
    while (done < o->len) {
        int nio = 0;
        u64 batch_pages = 0;
        for (int chunk = 0; chunk < KSTRIPE && done < o->len; chunk++) {
            int lane = kv_adaptive_lane(o->obj, o->len - done, round++);
            int needs_zone = kv_lane_zone[lane] < 0 || kv_lane_wp[lane] == ZONE_CAP_PGS;
            if (needs_zone && zns_allocatable_zone_count() <= kv_metadata_reserve + 1) {
                if (nio) break; /* Submit before any relocation. */
                if (kv_adaptive_make_room(sid, op_idx, trace_ns, o, measure) < 0) {
                    kv_space_report(stderr, "allocation-failed");
                    request_storage_stop(3, "zns_adaptive_no_reclaimable_space", sid, op_idx, trace_ns, o);
                    return -1;
                }
                /* GC changed tails, counts and mappings: select again. */
                lane = kv_adaptive_lane(o->obj, o->len - done, round++);
            }
            u64 n = o->len - done;
            if (n > 256) n = 256; /* placement chunk, in 4 KiB pages */
            u64 rem = kv_shared_lane_rem_locked(lane);
            if (n > rem) n = rem;
            u64 pg;
            if (kv_shared_append_locked(o->obj, lane, n, &pg) < 0) {
                /* With reservations held under kv_shared_mu this indicates
                 * metadata consumed the remaining slot. Flush before retry. */
                if (nio) break;
                kv_space_report(stderr, "append-failed");
                request_storage_stop(3, "zns_adaptive_out_of_zones", sid, op_idx, trace_ns, o);
                return -1;
            }
            pthread_mutex_lock(&ob->mu);
            obj_add_ext(ob, pg, n);
            pthread_mutex_unlock(&ob->mu);
            for (u64 p = 0; p < n; ) {
                u64 cmd = n - p > CMD_PGS ? CMD_PGS : n - p;
                ios[nio++] = (io_t){ pg + p, cmd, lane };
                p += cmd;
            }
            if (kv_lane_owner[lane] == o->obj) kv_private_pages += n;
            else kv_shared_pages += n;
            done += n;
            batch_pages += n;
        }
        zns_striped_write(ios, nio, measure ? C_KVWR : -1);
        zns_note_user_write_pages(batch_pages, measure);
    }
    kv_adaptive_pressure();
    *duration = now_ns() - start;
    return 0;
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

        if (o->op == OP_DEL) {
            pthread_mutex_lock(&ob->mu); ob_locked = 1;
            kv_shared_delete_obj_locked(o->obj, ob);
            pthread_mutex_unlock(&ob->mu); ob_locked = 0;
            if (kv_shared_gc_locked(sid, op_idx, trace_ns, o, measure) < 0) goto fail;
            pthread_mutex_unlock(&kv_shared_mu); kv_locked = 0;
            free(ios); return 0;
        }

        if (o->op == OP_WKV) {
            pthread_mutex_lock(&ob->mu); ob_locked = 1;
            if (o->off != ob->pgs) {
                fprintf(stderr, "non-append WKV obj %u off %" PRIu64
                        " expected %" PRIu64 "\n", o->obj, o->off, ob->pgs);
                exit(4);
            }
            pthread_mutex_unlock(&ob->mu); ob_locked = 0;

            if (KV_ADAPTIVE_READY) {
                if (kv_adaptive_write(o, ios, sid, op_idx, trace_ns, measure, &dur) < 0) goto fail;
                pthread_mutex_unlock(&kv_shared_mu); kv_locked = 0;
                goto acct;
            }

            if (kv_shared_prepare_write_locked(o->obj, o->off, o->len,
                                               sid, op_idx, trace_ns, o, measure) < 0)
                goto fail;

            pthread_mutex_lock(&ob->mu); ob_locked = 1;
            if (o->off != ob->pgs) {
                fprintf(stderr, "non-append WKV obj %u off %" PRIu64
                        " expected %" PRIu64 "\n", o->obj, o->off, ob->pgs);
                exit(4);
            }
            kv_shared_note_live_obj_locked(o->obj);
            for (u64 p = 0; p < o->len; ) {
                u64 lg = o->off + p;
                u64 j = lg / CMD_PGS, in = lg % CMD_PGS;
                int c = (int)(j % (u64)KSTRIPE);
                int lane = kv_shared_lane_for_locked(o->obj, c);
                u64 n = o->len - p;
                u64 lane_rem = kv_shared_lane_rem_locked(lane);
                if (n > CMD_PGS - in) n = CMD_PGS - in;
                if (n > lane_rem) n = lane_rem;

                u64 pg = 0;
                if (!n || kv_shared_append_locked(o->obj, lane, n, &pg) < 0) {
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

        pthread_mutex_lock(&ob->mu); ob_locked = 1;
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
            ios[nio++] = (io_t){ obj_lpn(ob, lg), n, 0 };     /* contiguous within extent approx */
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
                if (ob->zc_wp[c] == ZONE_CAP_PGS)
                    zns_note_zone_full(ob->zc[c][ob->zc_n[c]-1]);
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
                            zns_reset_zone(oz);
                            zns_note_host_gc(ZNS_HOST_GC_MEMLOG, 1, measure);
                            memmove(mlz, mlz + 1, (--n_mlz) * 4);
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
                    ios[nio++] = (io_t){ (u64)ml_open * ZONE_PGS + slot, n, 0 };
                    ml_open_wp += n;
                    if (ml_open_wp == ZONE_CAP_PGS)
                        zns_note_zone_full(ml_open);
                    p += n;
                } else {
                    u64 lg = (o->off + p) % ML_PGS;
                    u64 zs = ml_page_zone[lg]; if (zs == UINT64_MAX) { p++; continue; }
                    ios[nio++] = (io_t){ (zs >> 32) * ZONE_PGS + (u32)zs, 1, 0 };
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
                                    pio(1, (u64)ix_open * ZONE_PGS + ix_open_wp, 1);
                                    zns_note_host_copy_pages(1, measure);
                                    ix_open_wp++;
                                    if (ix_open_wp == ZONE_CAP_PGS)
                                        zns_note_zone_full(ix_open);
                                }
                                zn_live[vz] = 0; zns_reset_zone(vz);
                                zns_note_host_gc(ZNS_HOST_GC_INDEX, 1, measure);
                                memmove(&ixz[vi], &ixz[vi + 1], (n_ixz - vi - 1) * 4); n_ixz--;
                            }
                        }
                    }
                    if (ix_zone_of[e] >= 0) zn_live[ix_zone_of[e]]--;
                    ix_zone_of[e] = ix_open; ix_slot_of[e] = ix_open_wp; zn_live[ix_open]++;
                    ios[nio++] = (io_t){ (u64)ix_open * ZONE_PGS + ix_open_wp, 1, 0 };
                    ix_open_wp++;
                    if (ix_open_wp == ZONE_CAP_PGS)
                        zns_note_zone_full(ix_open);
                } else {
                    if (ix_zone_of[e] < 0) continue;
                    ios[nio++] = (io_t){ (u64)ix_zone_of[e] * ZONE_PGS + ix_slot_of[e], 1, 0 };
                }
            }
            pthread_mutex_unlock(&ix_mu); ix_locked = 0;
            cls = (o->op == OP_WIX) ? C_IXWR : C_IXRD;
        }
        pthread_mutex_unlock(&ob->mu); ob_locked = 0;
        dur = burst(wr, ios, nio, wr ? 1 : QD, measure ? cls : -1);
    }
acct:
    if (MODE_ZNS && wr && !(KV_ADAPTIVE_READY && o->op == OP_WKV))
        zns_note_user_write_pages(o->len, measure);
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

typedef struct { u64 due_ns; u32 sid; } sched_ev_t;
typedef struct {
    u32 next_op;
    u64 trace_ns, turn_stall;
    int turn_open, turn_measured;
} sess_run_t;

static sched_ev_t *sched_heap;
static sess_run_t *sess_run;
static u32 sched_heap_n, sched_done;
static int sched_stop;
static pthread_mutex_t sched_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sched_cv;
static u64 sched_kv_deferrals, sched_del_deferrals;

typedef struct { op_t *op; u64 trace_ns; u32 sid, op_idx; } kv_order_ev_t;

static int kv_order_cmp(const void *a, const void *b) {
    const kv_order_ev_t *x = a, *y = b;
    if (x->op->obj != y->op->obj) return x->op->obj < y->op->obj ? -1 : 1;
    if (x->trace_ns != y->trace_ns) return x->trace_ns < y->trace_ns ? -1 : 1;
    if (x->sid != y->sid) return x->sid < y->sid ? -1 : 1;
    return x->op_idx < y->op_idx ? -1 : x->op_idx > y->op_idx;
}

static void init_kv_order(void) {
    /* Session-relative I/O delays can let another session's eviction overtake
     * a victim's reads or appends. Preserve nominal trace order per KV object,
     * including DEL, so freeing extents/resetting zones waits for prior I/O.
     * Including subsequent uses also preserves delete-and-recreate lifetimes.
     * Same-time ties follow the scheduler's session order, then operation order.
     */
    size_t n = 0;
    for (u32 s = 0; s < n_sess; s++)
        for (u32 i = 0; i < sess[s].n; i++)
            if (is_kv_op(&sess[s].ops[i])) n++;
    if (!n) return;
    kv_order_ev_t *events = malloc(n * sizeof(*events));
    if (!events) exit(9);
    size_t j = 0;
    for (u32 s = 0; s < n_sess; s++) {
        u64 t = sess[s].t0;
        for (u32 i = 0; i < sess[s].n; i++) {
            op_t *o = &sess[s].ops[i];
            t += o->gap;
            if (!is_kv_op(o)) continue;
            o->kv_next = NULL;
            o->kv_wait_sid = UINT32_MAX;
            o->kv_ready = 0;
            events[j++] = (kv_order_ev_t){ o, t, s, i };
        }
    }
    qsort(events, n, sizeof(*events), kv_order_cmp);
    for (size_t i = 0; i < n; i++) {
        op_t *o = events[i].op;
        if (i && events[i - 1].op->obj == o->obj)
            events[i - 1].op->kv_next = o;
        else
            o->kv_ready = 1;
    }
    free(events);
}

static u64 scaled_ns(u64 ns) {
    return (u64)((double)ns * TSCALE);
}

static int sched_less(sched_ev_t a, sched_ev_t b) {
    return a.due_ns < b.due_ns || (a.due_ns == b.due_ns && a.sid < b.sid);
}

static void sched_push_locked(sched_ev_t ev) {
    u32 i = sched_heap_n++;
    sched_heap[i] = ev;
    while (i) {
        u32 p = (i - 1) / 2;
        if (!sched_less(sched_heap[i], sched_heap[p])) break;
        sched_ev_t tmp = sched_heap[i]; sched_heap[i] = sched_heap[p]; sched_heap[p] = tmp;
        i = p;
    }
}

static sched_ev_t sched_pop_locked(void) {
    sched_ev_t ret = sched_heap[0];
    sched_heap[0] = sched_heap[--sched_heap_n];
    for (u32 i = 0; ; ) {
        u32 l = i * 2 + 1, r = l + 1, m = i;
        if (l < sched_heap_n && sched_less(sched_heap[l], sched_heap[m])) m = l;
        if (r < sched_heap_n && sched_less(sched_heap[r], sched_heap[m])) m = r;
        if (m == i) break;
        sched_ev_t tmp = sched_heap[i]; sched_heap[i] = sched_heap[m]; sched_heap[m] = tmp;
        i = m;
    }
    return ret;
}

static void sched_complete_kv_locked(op_t *o) {
    op_t *next = o->kv_next;
    if (!next) return;
    next->kv_ready = 1;
    if (next->kv_wait_sid != UINT32_MAX) {
        /* This operation was already due. Requeue it without adding its gap
         * or advancing its trace timestamp a second time. */
        sched_push_locked((sched_ev_t){ now_ns(), next->kv_wait_sid });
        next->kv_wait_sid = UINT32_MAX;
        pthread_cond_broadcast(&sched_cv);
    }
}

static void sched_arm_next_locked(u32 sid, u64 base_ns) {
    sess_run_t *R = &sess_run[sid];
    sess_t *S = &sess[sid];
    if (R->next_op >= S->n) return;
    op_t *o = &S->ops[R->next_op];
    R->trace_ns += o->gap;
    sched_push_locked((sched_ev_t){ base_ns + scaled_ns(o->gap), sid });
    pthread_cond_signal(&sched_cv);
}

static void sched_finish_turn(sess_run_t *R) {
    if (R->turn_open && R->turn_measured) lv_add(C_TURNSTALL, R->turn_stall);
    R->turn_stall = 0;
    R->turn_open = 0;
    R->turn_measured = 0;
}

static void sched_mark_done_locked(void) {
    sched_done++;
    pthread_cond_broadcast(&sched_cv);
}

static void sched_request_stop_locked(void) {
    sched_stop = 1;
    pthread_cond_broadcast(&sched_cv);
}

static void *sched_worker(void *arg) {
    (void)arg;
    for (;;) {
        sched_ev_t ev;
        pthread_mutex_lock(&sched_mu);
        for (;;) {
            if (sched_stop || stop_requested() || sched_done >= n_sess) {
                pthread_mutex_unlock(&sched_mu);
                return NULL;
            }
            if (!sched_heap_n) {
                pthread_cond_wait(&sched_cv, &sched_mu);
                continue;
            }
            u64 due_ns = sched_heap[0].due_ns;
            u64 now = now_ns();
            if (due_ns > now) {
                struct timespec ts = { due_ns / 1000000000ULL, due_ns % 1000000000ULL };
                pthread_cond_timedwait(&sched_cv, &sched_mu, &ts);
                continue;
            }
            ev = sched_pop_locked();
            op_t *o = &sess[ev.sid].ops[sess_run[ev.sid].next_op];
            if (is_kv_op(o) && !o->kv_ready) {
                /* Park the session, not the worker: the predecessor must be
                 * able to run even with a single worker or many evictions. */
                o->kv_wait_sid = ev.sid;
                sched_kv_deferrals++;
                if (o->op == OP_DEL) sched_del_deferrals++;
                continue;
            }
            break;
        }
        pthread_mutex_unlock(&sched_mu);

        sess_run_t *R = &sess_run[ev.sid];
        sess_t *S = &sess[ev.sid];
        if (R->next_op >= S->n) {
            sched_finish_turn(R);
            pthread_mutex_lock(&sched_mu);
            sched_mark_done_locked();
            pthread_mutex_unlock(&sched_mu);
            continue;
        }

        u32 op_idx = R->next_op;
        op_t *o = &S->ops[op_idx];
        u64 trace_ns = R->trace_ns;
        int measure = trace_ns >= meas_start_ns;
        if (o->ts) {
            if (R->turn_open && R->turn_measured) lv_add(C_TURNSTALL, R->turn_stall);
            R->turn_stall = 0;
            R->turn_open = 1;
            R->turn_measured = 0;
        }

        int rc = run_op(o, &R->turn_stall, measure, ev.sid, op_idx, trace_ns);
        if (rc == 0) {
            rprog_note_issued(ev.sid, op_idx, trace_ns, o);
            if (measure) R->turn_measured = 1;
            R->next_op++;
            if (is_kv_op(o)) {
                pthread_mutex_lock(&sched_mu);
                sched_complete_kv_locked(o);
                pthread_mutex_unlock(&sched_mu);
            }
        }

        if (rc < 0 || stop_requested()) {
            sched_finish_turn(R);
            pthread_mutex_lock(&sched_mu);
            sched_request_stop_locked();
            sched_mark_done_locked();
            pthread_mutex_unlock(&sched_mu);
            continue;
        }

        if (R->next_op >= S->n) {
            sched_finish_turn(R);
            pthread_mutex_lock(&sched_mu);
            sched_mark_done_locked();
            pthread_mutex_unlock(&sched_mu);
        } else {
            pthread_mutex_lock(&sched_mu);
            sched_arm_next_locked(ev.sid, now_ns());
            pthread_mutex_unlock(&sched_mu);
        }
    }
}

static int default_session_workers(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = cpus > 0 ? (int)cpus * 4 : 64;
    if (workers < 32) workers = 32;
    if (workers > 256) workers = 256;
    return workers;
}

static void run_sessions_bounded(void) {
    if (!n_sess) return;
    init_kv_order();

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&sched_cv, &attr);
    pthread_condattr_destroy(&attr);

    sess_run = calloc(n_sess, sizeof(*sess_run));
    sched_heap = malloc((size_t)n_sess * sizeof(*sched_heap));
    if (!sess_run || !sched_heap) exit(9);

    pthread_mutex_lock(&sched_mu);
    for (u32 s = 0; s < n_sess; s++) {
        sess_run[s].trace_ns = sess[s].t0;
        if (!sess[s].n) {
            sched_done++;
        } else {
            sched_arm_next_locked(s, T0 + scaled_ns(sess[s].t0));
        }
    }
    pthread_mutex_unlock(&sched_mu);

    int workers = SESSION_WORKERS > 0 ? SESSION_WORKERS : default_session_workers();
    if ((u32)workers > n_sess) workers = (int)n_sess;
    if (workers < 1) workers = 1;
    fprintf(stderr, "session scheduler: workers=%d sessions=%u\n", workers, n_sess);

    pthread_t *th = malloc((size_t)workers * sizeof(*th));
    if (!th) exit(9);
    for (int i = 0; i < workers; i++) pthread_create(&th[i], NULL, sched_worker, NULL);
    for (int i = 0; i < workers; i++) pthread_join(th[i], NULL);
    fprintf(stderr, "KV ordering: deferred_ops=%" PRIu64 " deferred_deletes=%" PRIu64 "\n",
            sched_kv_deferrals, sched_del_deferrals);
    free(th);
    free(sched_heap); sched_heap = NULL; sched_heap_n = 0;
    free(sess_run); sess_run = NULL;
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
        if (KV_ADAPTIVE_READY) kv_space_report(out, "final");
        u64 user_wr_pgs = rprog_atomic_read_u64(&ZNS_USER_WRITE_PAGES);
        u64 host_copy_pgs = rprog_atomic_read_u64(&ZNS_HOST_COPY_PAGES);
        u64 host_write_pgs = user_wr_pgs + host_copy_pgs;
        u64 total_user_wr_pgs = rprog_atomic_read_u64(&ZNS_TOTAL_USER_WRITE_PAGES);
        u64 total_host_copy_pgs = rprog_atomic_read_u64(&ZNS_TOTAL_HOST_COPY_PAGES);
        u64 total_host_write_pgs = total_user_wr_pgs + total_host_copy_pgs;
        double waf = user_wr_pgs ? (double)host_write_pgs / (double)user_wr_pgs : 0.0;
        double total_waf = total_user_wr_pgs ?
            (double)total_host_write_pgs / (double)total_user_wr_pgs : 0.0;

        fprintf(out,
                "ZNS_OPEN_MUX mode=%s max_open_zones=%" PRIu64
                " max_active_zones=%" PRIu64
                " requested_k=%d runtime_k_max=%d shared_kv_lanes=%d"
                " observed_peak_live_kv_sessions=%" PRIu64
                " observed_peak_kv_open_zones=%" PRIu64
                " observed_peak_active_zones=%" PRIu64
                " metadata_open_reserve=%d metadata_active_reserve=%" PRIu64
                " open_zone_bound=%" PRIu64
                " kv_gc_free_zone_reserve=%" PRIu64 "\n",
                KV_SHARED ? "runtime-shared" : "exclusive", ZNS_OPEN_ZONE_LIMIT,
                ZNS_ACTIVE_ZONE_LIMIT, REQUESTED_KSTRIPE,
                KV_SHARED && KV_SHARED_LANES < KSTRIPE ? KV_SHARED_LANES : KSTRIPE,
                KV_SHARED_LANES, PEAK_LIVE_KV_SESSIONS, PEAK_KV_OPEN_ZONES,
                PEAK_ZNS_ACTIVE_ZONES, ZNS_META_OPEN_ZONES,
                zns_metadata_active_reserve(), PEAK_KV_OPEN_ZONES + ZNS_META_OPEN_ZONES,
                KV_SHARED ? kv_shared_gc_reserve_locked() : 0);
        fprintf(out,
                "ZNS_HOST_GC host_gc_count=%" PRIu64
                " host_gc_kv_count=%" PRIu64
                " host_gc_index_count=%" PRIu64
                " host_gc_memlog_count=%" PRIu64
                " host_gc_reset_zones=%" PRIu64
                " total_run_host_gc_count=%" PRIu64
                " total_run_host_gc_kv_count=%" PRIu64
                " total_run_host_gc_index_count=%" PRIu64
                " total_run_host_gc_memlog_count=%" PRIu64
                " total_run_host_gc_reset_zones=%" PRIu64 "\n",
                rprog_atomic_read_u64(&ZNS_HOST_GC_COUNT),
                rprog_atomic_read_u64(&ZNS_HOST_GC_KV_COUNT),
                rprog_atomic_read_u64(&ZNS_HOST_GC_INDEX_COUNT),
                rprog_atomic_read_u64(&ZNS_HOST_GC_MEMLOG_COUNT),
                rprog_atomic_read_u64(&ZNS_HOST_GC_RESET_ZONES),
                rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_COUNT),
                rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_KV_COUNT),
                rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_INDEX_COUNT),
                rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_MEMLOG_COUNT),
                rprog_atomic_read_u64(&ZNS_TOTAL_HOST_GC_RESET_ZONES));
        fprintf(out,
                "user_wr_pgs=%" PRIu64
                " host_copy_pgs=%" PRIu64
                " host_write_pgs=%" PRIu64
                " total_run_user_wr_pgs=%" PRIu64
                " total_run_host_copy_pgs=%" PRIu64
                " total_run_host_write_pgs=%" PRIu64 "\n",
                user_wr_pgs, host_copy_pgs, host_write_pgs,
                total_user_wr_pgs, total_host_copy_pgs, total_host_write_pgs);
        fprintf(out, "WAF=%.4f total_run_WAF=%.4f\n", waf, total_waf);
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
        else if (!strcmp(argv[i], "--session-workers") || !strcmp(argv[i], "-j")) {
            SESSION_WORKERS = atoi(argv[++i]);
            if (SESSION_WORKERS < 1) SESSION_WORKERS = 1;
        }
        else if (!strcmp(argv[i], "--progress") || !strcmp(argv[i], "--progress-interval"))
            RPROG_INTERVAL = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--no-progress")) RPROG_INTERVAL = 0;
        else if (!strcmp(argv[i], "--max-active-zones"))
            MAX_ACTIVE_ZONES_ARG = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--max-open-zones") || !strcmp(argv[i], "-A"))
            MAX_OPEN_ZONES_ARG = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--kv-policy") && i + 1 < argc) {
            const char *policy = argv[++i];
            if (!strcmp(policy, "adaptive")) KV_ADAPTIVE = 1;
            else if (!strcmp(policy, "legacy")) KV_ADAPTIVE = 0;
            else { fprintf(stderr, "unknown KV policy: %s\n", policy); return 1; }
        }
        else if (!strcmp(argv[i], "-k")) { 
            KSTRIPE = atoi(argv[++i]);
            if (KSTRIPE < 1) KSTRIPE = 1; 
            if (KSTRIPE > MAXK) KSTRIPE = MAXK; 
        }
    }
    if (!tracef || !dev) { fprintf(stderr, "usage: replayer -t trace -d /dev/nvmeXnY -m conv|zns -o out [-s scale] [-q qd] [-j session_workers] [-k kv_stripes] [-A max_open_zones] [--max-active-zones n] [--kv-policy adaptive|legacy] [--progress-interval n] [--no-progress]\n"); return 1; }
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
        ZNS_ACTIVE_ZONE_LIMIT = MAX_ACTIVE_ZONES_ARG ? MAX_ACTIVE_ZONES_ARG : DEVICE_MAX_ACTIVE_ZONES;
        snprintf(sysp, sizeof sysp, "/sys/block/%s/queue/max_open_zones", base);
        read_u64_file(sysp, &DEVICE_MAX_OPEN_ZONES);
        zns_configure_open_zone_mux();
        zfree = malloc(N_ZONES * sizeof(*zfree));
        zactive = calloc(N_ZONES, sizeof(*zactive));
        if (!zfree || !zactive) exit(9);
        for (u64 i = 0; i < N_ZONES; i++) zfree[i] = (int)i;
        zf_head = 0; zf_tail = 0; zf_cnt = N_ZONES; zf_tail = N_ZONES;
        if (KV_SHARED) {
            kv_lane_zone = malloc((size_t)KV_SHARED_LANES * sizeof(*kv_lane_zone));
            kv_lane_wp = calloc((size_t)KV_SHARED_LANES, sizeof(*kv_lane_wp));
            kv_zone_live = calloc(N_ZONES, sizeof(*kv_zone_live));
            kv_zone_used = calloc(N_ZONES, 1);
            kv_zone_refs = calloc(N_ZONES, sizeof(*kv_zone_refs));
            kv_obj_slot = malloc((size_t)(n_objs + 2) * sizeof(*kv_obj_slot));
            kv_active_obj = malloc((size_t)(n_objs + 2) * sizeof(*kv_active_obj));
            kv_obj_gc_seen = calloc((size_t)(n_objs + 2), sizeof(*kv_obj_gc_seen));
            if (!kv_lane_zone || !kv_lane_wp || !kv_zone_live || !kv_zone_used ||
                !kv_zone_refs || !kv_obj_slot || !kv_active_obj || !kv_obj_gc_seen)
                exit(9);
            if (KV_ADAPTIVE) {
                kv_lane_owner = calloc(KV_SHARED_LANES, sizeof(*kv_lane_owner));
                kv_zone_written = calloc(N_ZONES, sizeof(*kv_zone_written));
                if (!kv_lane_owner || !kv_zone_written) exit(9);
                kv_metadata_reserve = zns_metadata_active_reserve();
                KV_ADAPTIVE_READY = 1;
            }
            for (int i = 0; i < KV_SHARED_LANES; i++) kv_lane_zone[i] = -1;
            for (u64 i = 0; i < n_objs + 2; i++) kv_obj_slot[i] = -1;
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
        if (ioctl(FD, BLKRESETZONE, &r) < 0) { perror("initial BLKRESETZONE"); return 3; }
#endif
    }

    if (!stop_requested()) {
        T0 = now_ns();
        zns_host_gc_event_start();
        run_sessions_bounded();
        zns_host_gc_event_stop();
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
