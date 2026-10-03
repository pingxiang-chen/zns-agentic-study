/* Exercise the real scheduler and object allocator with all device I/O mocked.
 * Linker wrappers ensure these tests never access a block device. */
#define main replayer_program_main
#include "replayer.c"
#undef main
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>

static pthread_mutex_t test_mu = PTHREAD_MUTEX_INITIALIZER;
static int test_active, test_record = 1, test_delay = 1;
static char test_events[64];
static u64 *test_wp;
static int test_reset_failure;
static size_t test_event_count;

static void test_begin_io(void) {
    pthread_mutex_lock(&test_mu);
    test_active++;
    pthread_mutex_unlock(&test_mu);
}

static void test_finish_io(char event) {
    pthread_mutex_lock(&test_mu);
    assert(test_active > 0);
    test_active--;
    if (test_record) {
        assert(test_event_count + 1 < sizeof(test_events));
        test_events[test_event_count++] = event;
    }
    pthread_mutex_unlock(&test_mu);
}

int __wrap_aio_write(struct aiocb *cb) {
    cb->aio_lio_opcode = LIO_WRITE;
    test_begin_io();
    return 0;
}

int __wrap_aio_read(struct aiocb *cb) {
    cb->aio_lio_opcode = LIO_READ;
    test_begin_io();
    return 0;
}

int __wrap_aio_suspend(const struct aiocb *const list[], int n,
                       const struct timespec *timeout) {
    (void)list; (void)timeout;
    assert(n == 1); /* QD1 makes the mock completion order deterministic. */
    if (test_delay) usleep(100000);
    return 0;
}

int __wrap_aio_error(const struct aiocb *cb) {
    (void)cb;
    return 0;
}

ssize_t __wrap_aio_return(struct aiocb *cb) {
    test_finish_io(cb->aio_lio_opcode == LIO_WRITE ? 'W' : 'R');
    return (ssize_t)cb->aio_nbytes;
}

ssize_t __wrap_pwrite(int fd, const void *buf, size_t n, off_t off) {
    (void)fd; (void)buf;
    if (test_wp) {
        pthread_mutex_lock(&test_mu);
        u64 pg = off / PGSZ, z = pg / ZONE_PGS;
        assert(z < N_ZONES && pg % ZONE_PGS == test_wp[z]);
        assert(n <= CMDSZ && test_wp[z] + n / PGSZ <= ZONE_CAP_PGS);
        test_wp[z] += n / PGSZ;
        pthread_mutex_unlock(&test_mu);
    }
    test_begin_io();
    if (test_delay) usleep(100000);
    test_finish_io('W');
    return (ssize_t)n;
}

ssize_t __wrap_pread(int fd, void *buf, size_t n, off_t off) {
    (void)fd; (void)buf; (void)off;
    test_begin_io();
    if (test_delay) usleep(100000);
    test_finish_io('R');
    return (ssize_t)n;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    assert(request == BLKDISCARD || request == BLKRESETZONE);
    if (request == BLKRESETZONE && test_reset_failure) { errno = EIO; return -1; }
    if (request == BLKRESETZONE && test_wp) {
        va_list ap; va_start(ap, request);
        struct blk_zone_range *r = va_arg(ap, struct blk_zone_range *);
        u64 z = r->sector / (ZONE_PGS * (PGSZ / 512));
        assert(z < N_ZONES);
        test_wp[z] = 0;
        va_end(ap);
    }
    pthread_mutex_lock(&test_mu);
    if (test_record) {
        /* Extents/zones must not be recycled while I/O is outstanding. */
        assert(test_active == 0);
        assert(test_event_count + 1 < sizeof(test_events));
        test_events[test_event_count++] = 'D';
    }
    pthread_mutex_unlock(&test_mu);
    return 0;
}

static op_t test_op(int kind, u64 gap, u64 off, u64 len) {
    return (op_t){ .op = kind, .obj = 2, .gap = gap, .off = off, .len = len };
}

static void test_setup_adaptive(u64 zones, u64 capacity, int lanes);

static void test_setup_device(void) {
    if (MODE_ZNS) {
        KSTRIPE = 1;
        ZONE_PGS = ZONE_CAP_PGS = 64;
        N_ZONES = 8;
        zfree = malloc(N_ZONES * sizeof(*zfree));
        zactive = calloc(N_ZONES, sizeof(*zactive));
        for (u64 i = 0; i < N_ZONES; i++) zfree[i] = (int)i;
        zf_tail = zf_cnt = N_ZONES;
    } else {
        fs_free(0, DEV_PGS);
        if (IX_PGS + ML_PGS) {
            u64 got;
            int64_t s = fs_alloc(IX_PGS + ML_PGS, &got);
            assert(s >= 0 && got == IX_PGS + ML_PGS);
            obj_add_ext(&objs[0], (u64)s, IX_PGS);
            obj_add_ext(&objs[1], (u64)s + IX_PGS, ML_PGS);
        }
    }
    QD = 1;
    RPROG_INTERVAL = UINT64_MAX;
    meas_start_ns = UINT64_MAX;
    for (int c = 0; c < NCLS; c++) pthread_mutex_init(&LAT[c].mu, NULL);
    rprog_count_trace();
    T0 = now_ns();
}

static void test_eviction(int workers, int zns, int no_trim, int reuse, int full) {
    const u64 ms = 1000000;
    n_objs = 3;
    n_sess = reuse ? 3 : 2;
    objs = calloc(n_objs + 2, sizeof(*objs));
    for (u64 i = 0; i < n_objs + 2; i++) pthread_mutex_init(&objs[i].mu, NULL);
    sess = calloc(n_sess, sizeof(*sess));
    sess[0].n = 3;
    sess[0].ops = calloc(3, sizeof(op_t));
    sess[0].ops[0] = test_op(OP_WKV, 0, 0, 1);
    sess[0].ops[1] = test_op(OP_RKV, 20 * ms, 0, 1);
    sess[0].ops[2] = test_op(OP_WKV, 20 * ms, 1, 1);
    /* Nominal DEL is after all three operations; 100 ms of real I/O latency
     * makes it due before the victim's pending read and final append. */
    sess[1].t0 = 60 * ms;
    sess[1].n = 1;
    sess[1].ops = calloc(1, sizeof(op_t));
    sess[1].ops[0] = test_op(OP_DEL, 0, 0, 0);
    if (reuse) {
        sess[2].t0 = 80 * ms;
        sess[2].n = 3;
        sess[2].ops = calloc(3, sizeof(op_t));
        sess[2].ops[0] = test_op(OP_WKV, 0, 0, 1);
        sess[2].ops[1] = test_op(OP_RKV, 20 * ms, 0, 1);
        sess[2].ops[2] = test_op(OP_DEL, 0, 0, 0);
    }
    SESSION_WORKERS = workers;
    MODE_ZNS = zns != 0;
    DEV_PGS = full ? 1 : 1024;
    if (no_trim) setenv("BLKDISCARD_DISABLE", "1", 1);
    else unsetenv("BLKDISCARD_DISABLE");
    test_setup_device();
    if (zns == 2) {
        test_setup_adaptive(64, 64, 12);
        KSTRIPE = 1;
        test_record = test_delay = 1;
    }
    run_sessions_bounded();
    assert(sched_del_deferrals > 0);
    if (full) {
        assert(stop_code() == 5);
        assert(!strcmp(test_events, "WR"));
        return;
    }
    assert(!stop_requested());
    assert(RPROG_ISSUED_OPS == RPROG_TOTAL_OPS);
    assert(objs[2].pgs == 0 && objs[2].n_ext == 0);
    const char *expected = no_trim ? (reuse ? "WRWWR" : "WRW")
                                   : (reuse ? "WRWDWRD" : "WRWD");
    assert(!strcmp(test_events, expected));
}

static void test_setup_adaptive(u64 zones, u64 capacity, int lanes) {
    MODE_ZNS = KV_SHARED = KV_ADAPTIVE = KV_ADAPTIVE_READY = 1;
    KSTRIPE = 4;
    KV_SHARED_LANES = lanes;
    N_ZONES = zones;
    ZONE_PGS = ZONE_CAP_PGS = capacity;
    zfree = calloc(zones, sizeof(*zfree));
    zactive = calloc(zones, 1);
    kv_zone_live = calloc(zones, sizeof(*kv_zone_live));
    kv_zone_written = calloc(zones, sizeof(*kv_zone_written));
    kv_zone_used = calloc(zones, 1);
    kv_zone_refs = calloc(zones, sizeof(*kv_zone_refs));
    kv_lane_zone = malloc(lanes * sizeof(*kv_lane_zone));
    kv_lane_wp = calloc(lanes, sizeof(*kv_lane_wp));
    kv_lane_owner = calloc(lanes, sizeof(*kv_lane_owner));
    kv_obj_slot = malloc((n_objs + 2) * sizeof(*kv_obj_slot));
    kv_active_obj = calloc(n_objs + 2, sizeof(*kv_active_obj));
    kv_obj_gc_seen = calloc(n_objs + 2, sizeof(*kv_obj_gc_seen));
    for (u64 i = 0; i < zones; i++) zfree[i] = (int)i;
    for (int i = 0; i < lanes; i++) kv_lane_zone[i] = -1;
    for (u64 i = 0; i < n_objs + 2; i++) kv_obj_slot[i] = -1;
    zf_head = 0; zf_tail = zf_cnt = zones;
    test_wp = calloc(zones, sizeof(*test_wp));
    test_record = test_delay = 0;
}

static void test_validate_mapping(void) {
    u64 *live = calloc(N_ZONES, sizeof(*live));
    for (u64 o = 2; o < n_objs + 2; o++) {
        u64 total = 0;
        for (int i = 0; i < objs[o].n_ext; i++) {
            u64 pg = objs[o].ext[i].lpn_start, left = objs[o].ext[i].npgs;
            total += left;
            while (left) {
                u64 z = pg / ZONE_PGS, n = zns_zone_cap_rem(pg);
                if (n > left) n = left;
                assert(z < N_ZONES && n && kv_zone_used[z]);
                assert(pg % ZONE_PGS + n <= test_wp[z]);
                live[z] += n; pg += n; left -= n;
            }
        }
        assert(total == objs[o].pgs);
    }
    for (u64 z = 0; z < N_ZONES; z++) assert(live[z] == kv_zone_live[z]);
    free(live);
}

static void test_adaptive_op(int kind, u32 obj, u64 len) {
    op_t op = { .op = kind, .obj = obj, .off = kind == OP_WKV ? objs[obj].pgs : 0, .len = len };
    u64 stall = 0;
    assert(run_op(&op, &stall, 0, 0, 0, 0) == 0);
    assert(!stop_requested());
    test_validate_mapping();
}

static void test_adaptive(int scenario) {
    n_objs = 100;
    objs = calloc(n_objs + 2, sizeof(*objs));
    for (u64 i = 0; i < n_objs + 2; i++) pthread_mutex_init(&objs[i].mu, NULL);
    test_setup_adaptive(64, 1024, 12);
    if (scenario == 0) {
        test_adaptive_op(OP_WKV, 2, 99);
        assert(kv_private_pages == 99 && zf_cnt == 63);
        test_adaptive_op(OP_DEL, 2, 0);
        assert(zf_cnt == 64 && ZNS_TOTAL_HOST_COPY_PAGES == 0);
        assert(KV_OPEN_ZONES == 0 && ZNS_ACTIVE_ZONES == 0);
    } else if (scenario == 1) {
        test_adaptive_op(OP_WKV, 2, 8 * ZONE_CAP_PGS + 7);
        assert(PEAK_KV_OPEN_ZONES > 1);
        test_adaptive_op(OP_RKV, 2, objs[2].pgs);
        test_adaptive_op(OP_DEL, 2, 0);
        assert(zf_cnt == N_ZONES && ZNS_TOTAL_HOST_COPY_PAGES == 0);
    } else if (scenario == 2) {
        /* Exhaust private slots, then share without opening one tail/session. */
        for (u32 o = 2; o < 40; o++) test_adaptive_op(OP_WKV, o, 13);
        assert(kv_shared_pages > 0 && KV_OPEN_ZONES <= 11);
        for (u32 o = 2; o < 40; o++) test_adaptive_op(OP_DEL, o, 0);
        assert(zf_cnt == N_ZONES);
    } else if (scenario == 3) {
        /* A full mixed zone: delete one owner, compact survivors to lane 0. */
        u64 pg;
        kv_lane_owner[1] = UINT32_MAX;
        assert(kv_shared_append_locked(2, 1, 512, &pg) == 0);
        pio(1, pg, 32); /* seed model below for remaining simulated writes */
        test_wp[pg / ZONE_PGS] = 512;
        obj_add_ext(&objs[2], pg, 512);
        assert(kv_shared_append_locked(3, 1, 512, &pg) == 0);
        test_wp[pg / ZONE_PGS] = 1024;
        obj_add_ext(&objs[3], pg, 512);
        int victim = (int)(pg / ZONE_PGS);
        test_adaptive_op(OP_DEL, 2, 0);
        op_t op = { .op = OP_WKV, .obj = 3 };
        assert(kv_shared_compact_zone_locked(victim, 0, 0, 0, &op, 0) == 0);
        assert(ZNS_TOTAL_HOST_COPY_PAGES == 512);
        assert(obj_lpn(&objs[3], 0) / ZONE_PGS != (u64)victim);
        test_adaptive_op(OP_RKV, 3, 512);
        test_adaptive_op(OP_DEL, 3, 0);
        assert(zf_cnt == N_ZONES);
    } else if (scenario == 4) {
        /* Ordinary allocations cannot consume the GC reserve. */
        while (zns_alloc_zone() >= 0) {}
        assert(zf_cnt == 1);
        assert(zns_alloc_zone_reserved(0) >= 0 && zf_cnt == 0);
    } else if (scenario == 5) {
        test_adaptive_op(OP_WKV, 2, 13);
        test_reset_failure = 1;
        test_adaptive_op(OP_DEL, 2, 0);
        assert(0); /* reset failure must terminate without recycling the zone */
    } else if (scenario == 7) {
        /* Writable capacity may be smaller than the zone address span. */
        ZONE_CAP_PGS = 768;
        test_adaptive_op(OP_WKV, 2, 3000);
        test_adaptive_op(OP_RKV, 2, 3000);
        test_adaptive_op(OP_DEL, 2, 0);
        assert(zf_cnt == N_ZONES);
    } else {
        ZNS_ACTIVE_ZONE_LIMIT = 6;
        for (u32 o = 2; o < 50; o++) test_adaptive_op(OP_WKV, o, 13);
        assert(PEAK_ZNS_ACTIVE_ZONES < ZNS_ACTIVE_ZONE_LIMIT);
        assert(kv_shared_pages > 0);
        for (u32 o = 2; o < 50; o++) test_adaptive_op(OP_DEL, o, 0);
        assert(zf_cnt == N_ZONES);
    }
}

static int test_time_cmp(const void *a, const void *b) {
    const kv_order_ev_t *x = a, *y = b;
    if (x->trace_ns != y->trace_ns) return x->trace_ns < y->trace_ns ? -1 : 1;
    if (x->sid != y->sid) return x->sid < y->sid ? -1 : 1;
    return x->op_idx < y->op_idx ? -1 : x->op_idx > y->op_idx;
}

/* Capacity/placement replay in nominal trace order, not a latency simulation.
 * Reserve metadata capacity; only KV writes/deletes exercise this allocator. */
static void test_adaptive_trace(const char *path) {
    load_trace(path);
    u64 zones = getenv("TEST_ZONE_COUNT") ? strtoull(getenv("TEST_ZONE_COUNT"), NULL, 10) : 640;
    test_setup_adaptive(zones, 16384, 126);
    KSTRIPE = getenv("TEST_K") ? atoi(getenv("TEST_K")) : 8;
    assert(KSTRIPE >= 1 && KSTRIPE <= MAXK);
    ZNS_ACTIVE_ZONE_LIMIT = 128;
    kv_metadata_reserve = 11;
    for (int i = 0; i < 6; i++) { int z = zns_alloc_zone(); assert(z >= 0); zns_note_zone_full(z); }
    size_t n = 0;
    for (u32 s = 0; s < n_sess; s++) n += sess[s].n;
    kv_order_ev_t *events = calloc(n, sizeof(*events));
    n = 0;
    for (u32 s = 0; s < n_sess; s++) {
        u64 t = sess[s].t0;
        for (u32 i = 0; i < sess[s].n; i++) {
            op_t *op = &sess[s].ops[i]; t += op->gap;
            if (op->op == OP_WKV || op->op == OP_DEL)
                events[n++] = (kv_order_ev_t){op, t, s, i};
        }
    }
    qsort(events, n, sizeof(*events), test_time_cmp);
    for (size_t i = 0; i < n; i++) {
        u64 stall = 0;
        assert(run_op(events[i].op, &stall, 0, events[i].sid, events[i].op_idx, events[i].trace_ns) == 0);
        if (i % 5000 == 0) { test_validate_mapping(); kv_space_report(stderr, "trace-test"); }
    }
    test_validate_mapping();
    kv_space_report(stdout, "trace-test-final");
    printf("KV copy_pages=%" PRIu64 " WAF=%.6f\n", ZNS_TOTAL_HOST_COPY_PAGES,
           1.0 + (double)ZNS_TOTAL_HOST_COPY_PAGES / ZNS_TOTAL_USER_WRITE_PAGES);
    printf("PASS: %zu KV write/delete operations, k=%d, zones=%" PRIu64 "\n", n, KSTRIPE, zones);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--zns-trace")) {
        test_adaptive_trace(argv[2]); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--trace")) {
        /* Optional accelerated replay of a real trace, still with mocked I/O. */
        test_record = test_delay = 0;
        TSCALE = 0;
        SESSION_WORKERS = 8;
        DEV_PGS = 500107862016ULL / PGSZ;
        load_trace(argv[2]);
        test_setup_device();
        run_sessions_bounded();
        assert(!stop_requested());
        assert(RPROG_ISSUED_OPS == RPROG_TOTAL_OPS);
        printf("PASS: mocked replay completed %" PRIu64 " trace operations\n", RPROG_ISSUED_OPS);
        return 0;
    }
    assert(argc == 1);
    for (int scenario = 0; scenario < 8; scenario++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { alarm(20); test_adaptive(scenario); _exit(0); }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == (scenario == 5 ? 3 : 0));
    }
    puts("PASS: adaptive ownership, streams, fallback, relocation, reserves, reset failure, active limits");
    for (int workers = 1; workers <= 4; workers += 3) {
        for (int mode = 0; mode < 4; mode++) {
            for (int reuse = 0; reuse < 2; reuse++) {
                pid_t child = fork();
                assert(child >= 0);
                if (!child) {
                    alarm(10); /* A blocked worker or dependency cycle must fail. */
                    test_eviction(workers, mode >= 2 ? mode - 1 : 0, mode == 1, reuse, 0);
                    _exit(0);
                }
                int status;
                assert(waitpid(child, &status, 0) == child);
                assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
        }
    }
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        alarm(10);
        test_eviction(4, 0, 0, 0, 1);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("PASS: delayed eviction, object reuse, TRIM/no-TRIM, ZNS, and stop with a waiting eviction");
    return 0;
}
