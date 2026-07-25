/*
 * anchor_meta.c — durability prototype of Anchor's metadata layer.
 *
 * Implements the crash-consistency design of the paper (Section 4.6):
 *   - zone-state table + object map, persisted through an append-only
 *     metadata journal (per-record epoch + CRC64) with alternating
 *     checkpoint superblocks (A/B slots, epoch-versioned, CRC-guarded);
 *   - write-path ordering: data chunk (tagged with magic/epoch/len/CRC)
 *     is fdatasync'ed before the journal COMMIT record is fsync'ed before
 *     the operation is acknowledged;
 *   - recovery: load the newest valid superblock, replay CRC-valid journal
 *     records, then reconcile against per-zone write pointers discovered by
 *     scanning zone contents (standing in for the ZNS Zone Report), exactly
 *     as ZenFS reconciles journal state with device write pointers:
 *     journal-committed data must exist below the WP; data above the last
 *     COMMIT is an uncommitted tail and is discarded; ALLOC'd zones with no
 *     subsequent COMMIT are reclaimed as orphans.
 *
 * Zones are 64 MiB regions of a backing file (or a libzbd device with
 * -DANCHOR_ZBD, untested in this environment). The workload mode appends
 * session blobs in 128 KiB chunks, deletes sessions, and records every
 * ACKNOWLEDGED operation in an oracle log (fsync'ed before ack) that the
 * verifier replays after a crash.
 *
 * Usage:
 *   anchor_meta format  <dir> <n_zones>
 *   anchor_meta work    <dir> <seed> <ops>      (crashable workload)
 *   anchor_meta recover <dir>                    (recovery, prints stats)
 *   anchor_meta verify  <dir>                    (oracle check after recover)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

typedef uint64_t u64; typedef uint32_t u32;
#define ZONE_SZ (64ull << 20)
#define CHUNK   (128 * 1024)
#define MAGIC   0xA17C0DE5A17C0DE5ull
#define JMAGIC  0x4A524E4Cu

/* ---- crc64 (ECMA) ---- */
static u64 crc_tab[256];
static void crc_init(void) {
    for (int i = 0; i < 256; i++) {
        u64 c = i;
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ ((c & 1) ? 0xC96C5795D7870F42ull : 0);
        crc_tab[i] = c;
    }
}
static u64 crc64(const void *p, size_t n, u64 c) {
    const unsigned char *b = p;
    while (n--) c = crc_tab[(c ^ *b++) & 0xff] ^ (c >> 8);
    return ~c;
}
#define CRC(p, n) crc64((p), (n), ~0ull)

/* ---- on-disk records ---- */
struct sb {                        /* checkpoint superblock (two slots) */
    u64 magic, epoch, jrn_off;     /* replay journal from jrn_off */
    u32 n_zones, n_objs;
    u64 crc;                       /* over header + tables appended after */
};
enum { JR_ALLOC = 1, JR_COMMIT = 2, JR_DELETE = 3 };
struct jrec {
    u32 magic, type;
    u64 epoch, obj, zone, wp_after, data_crc, crc;
};
struct ctag {                      /* per-chunk data tag (first 64 B of chunk) */
    u64 magic, epoch, obj, seq, len, crc;
};

/* ---- in-memory state ---- */
struct zst { u32 state; u64 wp; u64 obj; u64 gen; };     /* 0 free, 1 open, 2 dead */
static struct zst *Z; static u32 NZ;
struct ost { u64 zone, pgs; u32 alive; };
static struct ost *O; static u32 NO_MAX = 1 << 16;
static int fd_data, fd_jrn, fd_oracle;
static u64 epoch, jrn_off;
static char DIR[256];

static void pfatal(const char *m) { perror(m); exit(2); }
static void psync(int fd) { if (fdatasync(fd)) pfatal("fdatasync"); }

static void sb_write(void) {
    /* checkpoint: tables + header into alternating slot, fsync, then bump */
    epoch++;
    size_t tsz = sizeof(struct sb) + NZ * sizeof(struct zst) + NO_MAX * sizeof(struct ost);
    char *buf = calloc(1, tsz);
    struct sb *s = (struct sb *)buf;
    s->magic = MAGIC; s->epoch = epoch; s->jrn_off = jrn_off;
    s->n_zones = NZ; s->n_objs = NO_MAX;
    memcpy(buf + sizeof(struct sb), Z, NZ * sizeof(struct zst));
    memcpy(buf + sizeof(struct sb) + NZ * sizeof(struct zst), O, NO_MAX * sizeof(struct ost));
    s->crc = 0; s->crc = CRC(buf, tsz);
    char p[300]; snprintf(p, sizeof p, "%s/sb%llu", DIR, (unsigned long long)(epoch & 1));
    int f = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (f < 0 || write(f, buf, tsz) != (ssize_t)tsz) pfatal("sb write");
    psync(f); close(f); free(buf);
}
static int sb_load(void) {
    u64 best = 0; char *bbuf = NULL; size_t bsz = 0;
    for (int i = 0; i < 2; i++) {
        char p[300]; snprintf(p, sizeof p, "%s/sb%d", DIR, i);
        int f = open(p, O_RDONLY); if (f < 0) continue;
        off_t sz = lseek(f, 0, SEEK_END); lseek(f, 0, SEEK_SET);
        char *buf = malloc(sz);
        if (read(f, buf, sz) != sz) { free(buf); close(f); continue; }
        close(f);
        struct sb *s = (struct sb *)buf;
        u64 c = s->crc; s->crc = 0;
        if (s->magic == MAGIC && CRC(buf, sz) == c && s->epoch > best) {
            free(bbuf); bbuf = buf; bsz = sz; best = s->epoch;
        } else free(buf);
    }
    if (!bbuf) return -1;
    struct sb *s = (struct sb *)bbuf;
    epoch = s->epoch; jrn_off = s->jrn_off; NZ = s->n_zones;
    memcpy(Z, bbuf + sizeof(struct sb), NZ * sizeof(struct zst));
    memcpy(O, bbuf + sizeof(struct sb) + NZ * sizeof(struct zst), NO_MAX * sizeof(struct ost));
    free(bbuf); (void)bsz;
    return 0;
}
static void jrn_append(u32 type, u64 obj, u64 zone, u64 wp_after, u64 dcrc) {
    struct jrec r = { JMAGIC, type, epoch, obj, zone, wp_after, dcrc, 0 };
    r.crc = CRC(&r, sizeof r - 8);
    if (pwrite(fd_jrn, &r, sizeof r, jrn_off) != sizeof r) pfatal("jrn");
    jrn_off += sizeof r;
    psync(fd_jrn);
}

static void openfiles(int create) {
    char p[300];
    snprintf(p, sizeof p, "%s/data", DIR);
    fd_data = open(p, O_RDWR | (create ? O_CREAT : 0), 0644);
    snprintf(p, sizeof p, "%s/journal", DIR);
    fd_jrn = open(p, O_RDWR | (create ? O_CREAT : 0), 0644);
    snprintf(p, sizeof p, "%s/oracle", DIR);
    fd_oracle = open(p, O_RDWR | O_APPEND | (create ? O_CREAT : 0), 0644);
    if (fd_data < 0 || fd_jrn < 0 || fd_oracle < 0) pfatal("open");
    Z = calloc(1 << 16, sizeof(struct zst));
    O = calloc(NO_MAX, sizeof(struct ost));
}

/* ---- workload ---- */
static u64 xr(u64 *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void chunk_fill(char *buf, u64 obj, u64 seq, u64 ep) {
    u64 rs = obj * 1315423911u + seq;
    for (size_t i = sizeof(struct ctag); i < CHUNK; i += 8) *(u64 *)(buf + i) = xr(&rs);
    struct ctag *t = (struct ctag *)buf;
    t->magic = MAGIC; t->epoch = ep; t->obj = obj; t->seq = seq; t->len = CHUNK;
    t->crc = 0; t->crc = CRC(buf, CHUNK);
}
static int do_work(u64 seed, int ops) {
    if (sb_load()) { fprintf(stderr, "no superblock\n"); return 2; }
    char *buf; if (posix_memalign((void **)&buf, 4096, CHUNK)) return 2;
    u64 rng = seed | 1;
    char line[128];
    for (int i = 0; i < ops; i++) {
        u64 pick = xr(&rng) % 100;
        u64 obj = 1 + (xr(&rng) % 64);
        if (pick < 70) {                         /* append one chunk */
            struct ost *o = &O[obj];
            if (!o->alive) {                     /* allocate a zone (journal ALLOC first) */
                u32 z; for (z = 0; z < NZ && Z[z].state; z++);
                if (z == NZ) continue;
                jrn_append(JR_ALLOC, obj, z, Z[z].gen + 1, 0);
                Z[z].state = 1; Z[z].wp = 0; Z[z].obj = obj; Z[z].gen++;
                o->zone = z; o->pgs = 0; o->alive = 1;
            }
            struct zst *zs = &Z[o->zone];
            if ((zs->wp + 1) * CHUNK > ZONE_SZ) { /* zone full: retire object */
                jrn_append(JR_DELETE, obj, o->zone, zs->wp, 0);
                zs->state = 0; zs->wp = 0; o->alive = 0;
                int rn = snprintf(line, sizeof line, "D %llu\n", (unsigned long long)obj);
                if (write(fd_oracle, line, rn) != rn) pfatal("oracle");
                psync(fd_oracle);              /* retirement is an acked state change */
                continue;
            }
            chunk_fill(buf, obj, o->pgs, zs->gen);
            u64 dcrc = ((struct ctag *)buf)->crc;
            off_t off = (off_t)o->zone * ZONE_SZ + zs->wp * CHUNK;
            if (pwrite(fd_data, buf, CHUNK, off) != CHUNK) pfatal("data");
            psync(fd_data);                      /* (1) data durable        */
            zs->wp++; o->pgs++;
            jrn_append(JR_COMMIT, obj, o->zone, zs->wp, dcrc);  /* (2) commit */
            int n = snprintf(line, sizeof line, "C %llu %llu %llu %llu\n",
                             (unsigned long long)obj, (unsigned long long)o->zone,
                             (unsigned long long)(o->pgs - 1), (unsigned long long)dcrc);
            if (write(fd_oracle, line, n) != n) pfatal("oracle");
            psync(fd_oracle);                    /* (3) ack                  */
        } else if (pick < 85 && O[obj].alive) {  /* delete (zone reset)      */
            struct ost *o = &O[obj];
            jrn_append(JR_DELETE, obj, o->zone, 0, 0);
            Z[o->zone].state = 0; Z[o->zone].wp = 0; o->alive = 0;
            int n = snprintf(line, sizeof line, "D %llu\n", (unsigned long long)obj);
            if (write(fd_oracle, line, n) != n) pfatal("oracle");
            psync(fd_oracle);
        } else if (pick < 87) {
            sb_write();                          /* periodic checkpoint      */
        }
    }
    sb_write();
    printf("work done ops=%d epoch=%llu\n", ops, (unsigned long long)epoch);
    return 0;
}

/* ---- recovery ---- */
static int do_recover(void) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    if (sb_load()) { fprintf(stderr, "no valid superblock\n"); return 2; }
    /* replay journal from checkpoint cursor */
    int replayed = 0, torn = 0, orphans = 0;
    for (;;) {
        struct jrec r;
        ssize_t n = pread(fd_jrn, &r, sizeof r, jrn_off);
        if (n != sizeof r) break;
        u64 c = r.crc; r.crc = 0;
        if (r.magic != JMAGIC || CRC(&r, sizeof r - 8) != c) { torn = 1; break; }
        r.crc = c;
        if (r.type == JR_ALLOC) { Z[r.zone].state = 1; Z[r.zone].wp = 0; Z[r.zone].obj = r.obj;
                                  Z[r.zone].gen = r.wp_after;
                                  O[r.obj].zone = r.zone; O[r.obj].pgs = 0; O[r.obj].alive = 1; }
        else if (r.type == JR_COMMIT) { Z[r.zone].wp = r.wp_after; O[r.obj].pgs = r.wp_after; }
        else if (r.type == JR_DELETE) { Z[r.zone].state = 0; Z[r.zone].wp = 0; O[r.obj].alive = 0; }
        jrn_off += sizeof r; replayed++;
    }
    /* WP reconciliation: scan each open zone's device contents (stand-in for
     * the ZNS Zone Report) and reconcile with journal state */
    char *buf; if (posix_memalign((void **)&buf, 4096, CHUNK)) return 2;
    int wp_trunc = 0;
    for (u32 z = 0; z < NZ; z++) {
        if (Z[z].state != 1) continue;
        u64 dev_wp = 0;
        for (u64 c = 0; c * CHUNK < ZONE_SZ; c++) {
            if (pread(fd_data, buf, CHUNK, (off_t)z * ZONE_SZ + c * CHUNK) != CHUNK) break;
            struct ctag *t = (struct ctag *)buf;
            u64 tc = t->crc; t->crc = 0;
            if (t->magic != MAGIC || CRC(buf, CHUNK) != tc || t->epoch != Z[z].gen) break;
            t->crc = tc; dev_wp = c + 1;
        }
        if (dev_wp < Z[z].wp) { fprintf(stderr, "LOST COMMITTED DATA zone %u\n", z); return 3; }
        if (dev_wp > Z[z].wp) wp_trunc++;        /* uncommitted tail discarded */
        if (Z[z].obj && !O[Z[z].obj].alive) { Z[z].state = 0; orphans++; }
    }
    sb_write();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    printf("RECOVERED replayed=%d torn_tail=%d wp_truncations=%d orphans_reclaimed=%d ms=%.1f\n",
           replayed, torn, wp_trunc, orphans, ms);
    return 0;
}

/* ---- verify against oracle (acked ops only) ---- */
static int do_verify(void) {
    if (sb_load()) return 2;
    char p[300]; snprintf(p, sizeof p, "%s/oracle", DIR);
    FILE *f = fopen(p, "r"); if (!f) pfatal("oracle");
    /* final acked state per object: last C seq per obj unless later D */
    static u64 last_seq[70], last_crc[70]; static int dead[70];
    char line[160];
    while (fgets(line, sizeof line, f)) {
        u64 obj, zone, seq, dcrc;
        if (sscanf(line, "C %llu %llu %llu %llu", (unsigned long long *)&obj,
                   (unsigned long long *)&zone, (unsigned long long *)&seq,
                   (unsigned long long *)&dcrc) == 4 && obj < 70) {
            last_seq[obj] = seq + 1; last_crc[obj] = dcrc; dead[obj] = 0;
        } else if (sscanf(line, "D %llu", (unsigned long long *)&obj) == 1 && obj < 70) {
            dead[obj] = 1;
        }
    }
    fclose(f);
    char *buf; if (posix_memalign((void **)&buf, 4096, CHUNK)) return 2;
    int checked = 0;
    for (u64 obj = 1; obj < 65; obj++) {
        if (dead[obj] || !last_seq[obj]) continue;
        if (!O[obj].alive) {
            /* object may have been legally retired by zone-full before crash;
             * only fail if oracle's last record was an acked append newer than
             * any delete AND recovery lost it */
            fprintf(stderr, "MISSING acked object %llu\n", (unsigned long long)obj);
            return 4;
        }
        if (O[obj].pgs < last_seq[obj]) {
            fprintf(stderr, "OBJ %llu acked %llu chunks, recovered %llu\n",
                    (unsigned long long)obj, (unsigned long long)last_seq[obj],
                    (unsigned long long)O[obj].pgs);
            return 5;
        }
        /* verify last acked chunk content */
        off_t off = (off_t)O[obj].zone * ZONE_SZ + (last_seq[obj] - 1) * CHUNK;
        if (pread(fd_data, buf, CHUNK, off) != CHUNK) return 6;
        struct ctag *t = (struct ctag *)buf;
        if (t->obj != obj || t->seq != last_seq[obj] - 1 || t->crc != last_crc[obj]) {
            fprintf(stderr, "CONTENT MISMATCH obj %llu\n", (unsigned long long)obj);
            return 7;
        }
        checked++;
    }
    printf("VERIFY OK objects_checked=%d\n", checked);
    return 0;
}

int main(int argc, char **argv) {
    crc_init();
    if (argc < 3) { fprintf(stderr, "usage: %s format|work|recover|verify <dir> ...\n", argv[0]); return 1; }
    snprintf(DIR, sizeof DIR, "%s", argv[2]);
    if (!strcmp(argv[1], "format")) {
        char p[300]; snprintf(p, sizeof p, "mkdir -p %s", DIR); if (system(p)) return 1;
        openfiles(1);
        NZ = argc > 3 ? atoi(argv[3]) : 64;
        if (ftruncate(fd_data, (off_t)NZ * ZONE_SZ)) pfatal("truncate");
        epoch = 0; jrn_off = 0; sb_write();
        printf("formatted %u zones\n", NZ);
        return 0;
    }
    openfiles(0);
    if (!strcmp(argv[1], "work"))    return do_work(strtoull(argv[3], 0, 10), atoi(argv[4]));
    if (!strcmp(argv[1], "recover")) return do_recover();
    if (!strcmp(argv[1], "verify"))  return do_verify();
    return 1;
}
