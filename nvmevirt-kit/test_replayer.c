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
    (void)fd; (void)buf; (void)off;
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
    MODE_ZNS = zns;
    DEV_PGS = full ? 1 : 1024;
    if (no_trim) setenv("BLKDISCARD_DISABLE", "1", 1);
    else unsetenv("BLKDISCARD_DISABLE");
    test_setup_device();
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

int main(int argc, char **argv) {
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
    for (int workers = 1; workers <= 4; workers += 3) {
        for (int mode = 0; mode < 3; mode++) {
            for (int reuse = 0; reuse < 2; reuse++) {
                pid_t child = fork();
                assert(child >= 0);
                if (!child) {
                    alarm(10); /* A blocked worker or dependency cycle must fail. */
                    test_eviction(workers, mode == 2, mode == 1, reuse, 0);
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
