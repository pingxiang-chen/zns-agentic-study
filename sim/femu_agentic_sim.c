/*
 * femu_agentic_sim.c
 *
 * FEMU-lineage trace-driven discrete-event SSD simulator for agentic-LLM
 * KV-cache / agent-memory offloading studies (conv vs FDP-like vs ZNS).
 *
 * Device models share identical NAND geometry & timing (FEMU bbssd defaults:
 * 4 KiB page, 256 pgs/blk, 8 ch x 8 LUN, rd 40us / prog 200us / erase 2ms,
 * ch_xfer disabled, per-LUN serialization = FEMU ssd_advance_status).
 *
 *  conv : port of FEMU bbssd FTL (femu @ e2d5413, hw/femu/bbssd/ftl.c):
 *         page-level mapping, single write pointer, superblock "lines" across
 *         all LUNs, greedy min-vpc victim PQ, forced inline GC at high
 *         watermark (in write path), background GC when idle, TRIM
 *         (metadata-only), GC copies re-injected through the write pointer.
 *  fdp  : conv + per-stream write pointers (KV / memlog / index) + GC wp
 *         (FDP-like RUH separation; same GC machinery).
 *  zns  : ConfZNS-style zones striped over a LUN group (default 16 LUNs =
 *         16 MiB zones, scaled ZN540 ratio), per-zone sequential WP, no device
 *         GC. Host: zone-per-session KV (reset on death), FIFO memlog zones,
 *         LSM index zones with host compaction (copies+resets fully counted).
 *
 * The same logical trace drives all modes. Sessions are closed loops (op
 * chains with think-time gaps); each op is a burst of 128 KiB commands with
 * bounded queue depth.
 *
 * Build: gcc -O2 -o sim femu_agentic_sim.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <assert.h>

typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8; typedef int64_t i64;

/* ------------------------- FEMU default parameters ------------------------ */
static int PGS_PER_BLK   = 256;    /* 1 MiB block  */
static int BLKS_PER_PL   = 256;    /* 256 lines -> 16 GiB raw */
static int LUNS_PER_CH   = 8;
static int NCHS          = 8;
static u64 RD_LAT        = 40000;   /* ns, FEMU NAND_READ_LATENCY  */
static u64 WR_LAT        = 200000;  /* ns, FEMU NAND_PROG_LATENCY  */
static u64 ER_LAT        = 2000000; /* ns, FEMU NAND_ERASE_LATENCY */
static double GC_PCT       = 0.75;  /* FEMU gc_thres_pcent (bg GC)     */
static double GC_PCT_HIGH  = 0.95;  /* FEMU gc_thres_pcent_high (forced)*/
static double OP_FRAC      = 0.07;
static int    ZNS_ZONE_LUNS = 64;   /* full-stripe 64 MiB zones (large-zone ZNS, ATC'21-style) */
static int    ZNS_LAZY_ERASE = 1;   /* charge block erases at zone re-allocation (deferred reset) */
static int    ZNS_RSV_ZONES = 2;
static int    QD            = 32;
static int    CMD_PGS       = 32;   /* 128 KiB commands */
/* reviewer-demanded variants */
static int    SUSP          = 0;    /* -S: idealized bounded read-suspension (enterprise upper bound) */
static u64    SUSP_CAP      = 100000; /* ns: max interference a read sees from an in-flight prog/erase */
static u64    SUSP_OVH      = 50000;  /* ns: suspend/resume penalty charged to the interrupted op */
static int    KV_RUHS       = 1;    /* -R N: per-session FDP reclaim-unit handles (obj % N) */
static int    ADAPTIVE_RUH  = 0;    /* -A: exclusive handle per live session, spill when exhausted */
static int    SEP_GC_WP     = 0;    /* -G: conv uses a dedicated GC write pointer (real-drive-like;
                                       FEMU ships a shared user/GC pointer) */
static int    NO_TRIM       = 0;    /* -N: deletes do not reach the device (deployed filesystems:
                                       discard off, periodic fstrim); pages die only on LBA reuse */

static int NLUNS, PGS_PER_LINE, TT_LINES; static u64 TT_PGS, USABLE_PGS;

enum { DEV_CONV = 0, DEV_FDP = 1, DEV_ZNS = 2 };
enum { PG_FREE = 0, PG_VALID = 1, PG_INVALID = 2 };
enum { ST_KV = 0, ST_ML = 1, ST_IX = 2, N_STREAMS = 3 };
enum { C_KVRD = 0, C_KVWR, C_MLWR, C_MLRD, C_IXWR, C_IXRD, C_RESUME, C_TURNSTALL,
       C_TTFT_HIT, C_TTFT_MISS, NCLS };
static const char *cls_name[NCLS] =
 { "kv_read_cmd", "kv_write_cmd", "memlog_write_cmd", "memlog_read_cmd",
   "index_write_cmd", "index_read_cmd", "resume_burst", "turn_storage_stall",
   "ttft_stall_hit_turn", "ttft_stall_miss_turn" };

/* ------------------------------- statistics ------------------------------- */
typedef struct { u32 *v; u64 n, cap; u64 sample_mod, seen; } lvec;
static void lv_add(lvec *L, u64 ns) {
    L->seen++;
    if (L->sample_mod > 1 && (L->seen % L->sample_mod)) return;
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 1 << 16; L->v = realloc(L->v, L->cap * 4); }
    u64 us = (ns + 999) / 1000; if (us > 0xFFFFFFFFULL) us = 0xFFFFFFFFULL;
    L->v[L->n++] = (u32)us;
}
static int cmp_u32(const void *a, const void *b) { u32 x = *(const u32*)a, y = *(const u32*)b; return x < y ? -1 : x > y; }
static double lv_pct(lvec *L, double p) { if (!L->n) return 0; u64 i = (u64)(p / 100.0 * (L->n - 1) + 0.5); return L->v[i]; }

/* --------------------------------- device --------------------------------- */
typedef struct { int id, vpc, ipc, state, pos; } line_t;  /* pos: heap idx+1 (0 = not in heap) */
enum { LINE_FREE, LINE_OPEN, LINE_CLOSED };               /* CLOSED = full list or victim PQ */

typedef struct { int ch, lun, pg; line_t *curline; } wp_t;

typedef struct {
    int mode;
    u64 *lun_avail;
    u64 *lun_rd_avail;                     /* separate read timeline under -S suspension */
    /* conv/fdp */
    u32 *maptbl, *rmap; u8 *pgst;
    u8 *pgstream;                          /* stream tag per ppa (victim-composition breakdown) */
    u64 gc_victim_mix[N_STREAMS + 2];      /* copied pages by stream (+GC-rewritten, +unknown) */
    line_t *lines; u32 *line_erases;
    int *freelist; int fl_head, fl_tail, fl_cnt;
    line_t **heap; int heap_n;
    int full_cnt;
    wp_t *wp;                              /* conv: wp[0]; fdp: KV_RUHS + ML + IX + GC */
    int gc_thres_lines, gc_thres_lines_high;
    /* zns */
    int zone_luns, zone_pgs, n_zones, zns_usable;
    int *zone_wp, *zone_state, *zone_group; u32 *zone_erases; u8 *zone_dirty;
    int *zfree; int zf_head, zf_tail, zf_cnt;
    /* stats (measurement window only) */
    u64 user_wr_pgs, user_rd_pgs, gc_copy_pgs, host_copy_pgs, erases, zone_resets;
    u64 gc_count, gc_busy_ns, gc_max_ns;
    u64 tot_user_wr_pgs;                   /* whole run, for aging accounting */
    lvec lat[NCLS]; lvec gc_dur;
    u64 *tl_uw, *tl_ur, *tl_gc; u64 tl_n;
    FILE *gclog;
} ssd_dev_t;

static ssd_dev_t D;
static u64 t_end_ns, meas_start_ns, t_final = 0;
static inline int in_meas(u64 t) { return t >= meas_start_ns; }
static inline void tl_add(u64 *arr, u64 t, u64 v) { u64 b = t / 1000000000ULL; if (b < D.tl_n) arr[b] += v; }

/* --------------------------- conv/fdp FTL (FEMU) -------------------------- */
/* ppa = line*PGS_PER_LINE + s;  s: ch fastest, then lun, then pg (FEMU wp order).
 * lun(s) = s % NLUNS, pg(s) = s / NLUNS. */
static inline int ppa_lun(u64 ppa) { return (int)(ppa % (u64)NLUNS); }
static inline int ppa_line(u64 ppa) { return (int)(ppa / (u64)PGS_PER_LINE); }

static void heap_swap(int i, int j) { line_t *a = D.heap[i], *b = D.heap[j]; D.heap[i] = b; D.heap[j] = a; b->pos = i + 1; a->pos = j + 1; }
static void heap_up(int i) { while (i > 0) { int p = (i - 1) / 2; if (D.heap[p]->vpc <= D.heap[i]->vpc) break; heap_swap(p, i); i = p; } }
static void heap_down(int i) {
    for (;;) { int l = 2 * i + 1, r = l + 1, m = i;
        if (l < D.heap_n && D.heap[l]->vpc < D.heap[m]->vpc) m = l;
        if (r < D.heap_n && D.heap[r]->vpc < D.heap[m]->vpc) m = r;
        if (m == i) break; heap_swap(i, m); i = m; }
}
static void heap_insert(line_t *ln) { D.heap[D.heap_n] = ln; ln->pos = D.heap_n + 1; D.heap_n++; heap_up(D.heap_n - 1); }
static void heap_fix(line_t *ln) { int i = ln->pos - 1; heap_up(i); heap_down(i); }
static line_t *heap_pop_min(void) {
    if (!D.heap_n) return NULL; line_t *m = D.heap[0];
    D.heap_n--; if (D.heap_n) { D.heap[0] = D.heap[D.heap_n]; D.heap[0]->pos = 1; heap_down(0); }
    m->pos = 0; return m;
}
static void fl_push(int id) { D.freelist[D.fl_tail] = id; D.fl_tail = (D.fl_tail + 1) % (TT_LINES + 1); D.fl_cnt++; D.lines[id].state = LINE_FREE; }
static int fl_pop(void) { if (!D.fl_cnt) return -1; int id = D.freelist[D.fl_head]; D.fl_head = (D.fl_head + 1) % (TT_LINES + 1); D.fl_cnt--; return id; }

static void wp_open_line(wp_t *w) {
    int id = fl_pop();
    if (id < 0) { fprintf(stderr, "FATAL: out of free lines\n"); exit(2); }
    w->curline = &D.lines[id]; w->curline->state = LINE_OPEN; w->ch = 0; w->lun = 0; w->pg = 0;
}
/* FEMU get_new_page(): current wp position (page will be marked valid by caller) */
static u64 wp_cur_ppa(wp_t *w) {
    if (!w->curline) wp_open_line(w);
    int s = w->pg * NLUNS + (w->lun * NCHS + w->ch);
    return (u64)w->curline->id * PGS_PER_LINE + s;
}
/* FEMU ssd_advance_write_pointer(): called after mark_page_valid */
static void wp_advance(wp_t *w) {
    line_t *ln = w->curline;
    w->ch++;
    if (w->ch != NCHS) return;
    w->ch = 0; w->lun++;
    if (w->lun != LUNS_PER_CH) return;
    w->lun = 0; w->pg++;
    if (w->pg != PGS_PER_BLK) return;
    /* line filled: FEMU moves to full list if vpc==pgs_per_line else victim PQ */
    ln->state = LINE_CLOSED;
    if (ln->vpc == PGS_PER_LINE) { D.full_cnt++; ln->pos = 0; }
    else heap_insert(ln);
    w->curline = NULL; w->pg = 0;
}
/* FEMU mark_page_invalid (line part) */
static void line_page_invalid(u64 ppa) {
    line_t *ln = &D.lines[ppa_line(ppa)];
    int was_full = (ln->state == LINE_CLOSED && ln->pos == 0);
    ln->ipc++; ln->vpc--;
    if (ln->state == LINE_CLOSED) {
        if (ln->pos) heap_fix(ln);
        else if (was_full) { D.full_cnt--; heap_insert(ln); }
    }
}
static void conv_page_invalidate(u64 ppa) {
    D.pgst[ppa] = PG_INVALID; D.rmap[ppa] = UINT32_MAX; line_page_invalid(ppa);
}
static int cur_stream = 0;                     /* stream of the op being serviced */
/* FEMU ssd_write inner loop for one lpn (invalidate-old, place, program) */
static u64 conv_program_page(wp_t *w, u32 lpn, u64 t, int is_gc) {
    u64 old = D.maptbl[lpn];
    if (old != UINT32_MAX) conv_page_invalidate(old);
    u64 ppa = wp_cur_ppa(w);
    line_t *ln = w->curline;
    assert(D.pgst[ppa] == PG_FREE);
    D.pgst[ppa] = PG_VALID; D.maptbl[lpn] = (u32)ppa; D.rmap[ppa] = lpn;
    if (D.pgstream) D.pgstream[ppa] = (u8)(is_gc ? N_STREAMS : cur_stream);
    ln->vpc++;
    wp_advance(w);
    int lu = ppa_lun(ppa);
    u64 st = D.lun_avail[lu] > t ? D.lun_avail[lu] : t;
    D.lun_avail[lu] = st + WR_LAT;
    if (is_gc) { if (in_meas(t)) D.gc_copy_pgs++; }
    else { D.tot_user_wr_pgs++; if (in_meas(t)) D.user_wr_pgs++; }
    return D.lun_avail[lu];
}
static inline int should_gc(void)      { return D.fl_cnt <= D.gc_thres_lines; }
static inline int should_gc_high(void) { return D.fl_cnt <= D.gc_thres_lines_high; }

/* user read of one page on LUN lu at time t.
 * Default: FEMU semantics (full per-LUN serialization behind writes/GC/erases).
 * -S: idealized bounded suspension — the read waits at most SUSP_CAP behind the
 * in-flight program/erase (which pays SUSP_OVH), and otherwise only serializes
 * with other reads on the same LUN. Optimistic upper bound on preemptive firmware. */
static inline u64 dev_read_page(int lu, u64 t) {
    if (!SUSP) {
        u64 st = D.lun_avail[lu] > t ? D.lun_avail[lu] : t;
        D.lun_avail[lu] = st + RD_LAT;
        return D.lun_avail[lu];
    }
    u64 rs = D.lun_rd_avail[lu] > t ? D.lun_rd_avail[lu] : t;
    u64 rem = D.lun_avail[lu] > rs ? D.lun_avail[lu] - rs : 0;
    u64 itf = rem > SUSP_CAP ? SUSP_CAP : rem;
    u64 fin = rs + itf + RD_LAT;
    D.lun_rd_avail[lu] = fin;
    if (itf) D.lun_avail[lu] += SUSP_OVH;
    return fin;
}

/* FEMU do_gc(): greedy min-vpc victim; !force requires ipc >= line/8 */
static u64 do_gc(int force, u64 t, int *ok) {
    *ok = 0;
    if (!D.heap_n) return t;
    line_t *v = D.heap[0];
    if (!force && v->ipc < PGS_PER_LINE / 8) return t;
    heap_pop_min();
    *ok = 1;
    u64 tmax = t, f;
    wp_t *gw = (D.mode == DEV_FDP) ? &D.wp[KV_RUHS + 2] : &D.wp[SEP_GC_WP ? 1 : 0];
    u64 base = (u64)v->id * PGS_PER_LINE;
    for (int lu = 0; lu < NLUNS; lu++) {
        for (int pg = 0; pg < PGS_PER_BLK; pg++) {
            u64 ppa = base + (u64)pg * NLUNS + lu;
            if (D.pgst[ppa] == PG_VALID) {
                u64 st = D.lun_avail[lu] > t ? D.lun_avail[lu] : t;   /* gc read */
                D.lun_avail[lu] = st + RD_LAT;
                u32 lpn = D.rmap[ppa];
                if (D.pgstream && in_meas(t)) D.gc_victim_mix[D.pgstream[ppa]]++;
                D.pgst[ppa] = PG_FREE; D.rmap[ppa] = UINT32_MAX; D.maptbl[lpn] = UINT32_MAX;
                f = conv_program_page(gw, lpn, t, 1);
                if (f > tmax) tmax = f;
            } else { D.pgst[ppa] = PG_FREE; D.rmap[ppa] = UINT32_MAX; }
        }
        u64 st = D.lun_avail[lu] > t ? D.lun_avail[lu] : t;           /* erase */
        D.lun_avail[lu] = st + ER_LAT;
        if (D.lun_avail[lu] > tmax) tmax = D.lun_avail[lu];
        if (in_meas(t)) D.erases++;
    }
    D.line_erases[v->id]++;
    v->vpc = 0; v->ipc = 0; fl_push(v->id);
    if (in_meas(t)) {
        D.gc_count++;
        u64 dur = tmax - t; D.gc_busy_ns += dur; if (dur > D.gc_max_ns) D.gc_max_ns = dur;
        lv_add(&D.gc_dur, dur);
        tl_add(D.tl_gc, t, 1);
        if (D.gclog) fprintf(D.gclog, "%.6f,%.6f\n", t / 1e9, tmax / 1e9);
    }
    return tmax;
}

/* ------------------------------- ZNS device ------------------------------- */
static inline int z_lun(int zone, int idx) { return D.zone_group[zone] * D.zone_luns + (idx % D.zone_luns); }
static u64 ML_PGS, IX_PGS;
static u64 *ml_page_zone;
static void zns_charge_erase(int z, u64 t) {
    for (int i = 0; i < D.zone_luns; i++) {
        int lu = D.zone_group[z] * D.zone_luns + i;
        u64 st = D.lun_avail[lu] > t ? D.lun_avail[lu] : t;
        D.lun_avail[lu] = st + ER_LAT;
        if (in_meas(t)) D.erases++;
    }
    D.zone_erases[z]++;
}
static int zns_alloc_zone(u64 t) {
    if (!D.zf_cnt) { fprintf(stderr, "FATAL: ZNS out of free zones\n"); exit(3); }
    int z = D.zfree[D.zf_head]; D.zf_head = (D.zf_head + 1) % (D.n_zones + 1); D.zf_cnt--;
    if (D.zone_dirty[z]) { zns_charge_erase(z, t); D.zone_dirty[z] = 0; }   /* deferred reset */
    D.zone_state[z] = 1; D.zone_wp[z] = 0; return z;
}
static void zns_reset_zone(int z, u64 t) {
    if (ZNS_LAZY_ERASE) D.zone_dirty[z] = 1;            /* erase deferred to re-allocation */
    else { zns_charge_erase(z, t); D.zone_dirty[z] = 0; }
    D.zone_state[z] = 0; D.zone_wp[z] = 0;
    D.zfree[D.zf_tail] = z; D.zf_tail = (D.zf_tail + 1) % (D.n_zones + 1); D.zf_cnt++;
    if (in_meas(t)) D.zone_resets++;
}
static u64 zns_append_page(int z, u64 t, int host_copy) {
    int idx = D.zone_wp[z]++;
    assert(idx < D.zone_pgs);
    int lu = z_lun(z, idx);
    u64 st = D.lun_avail[lu] > t ? D.lun_avail[lu] : t;
    D.lun_avail[lu] = st + WR_LAT;
    if (host_copy) { if (in_meas(t)) D.host_copy_pgs++; }
    else { D.tot_user_wr_pgs++; if (in_meas(t)) D.user_wr_pgs++; }
    return D.lun_avail[lu];
}
static u64 dev_read_page(int lu, u64 t);
static u64 zns_read_page(int z, int idx, u64 t) {
    return dev_read_page(z_lun(z, idx), t);
}

/* ------------------------------ host objects ------------------------------ */
typedef struct { u64 lpn_start, npgs; } ext_t;
typedef struct {
    ext_t *ext; int n_ext, cap_ext; u64 pgs;
    int memo_ext; u64 memo_base;               /* sequential-lookup memo */
    int *zones; int n_zones, cap_z;
    int ruh;                                   /* adaptive-FDP handle (-1 = none) */
    u8 alive;
} obj_t;
static obj_t *objs;                                    /* fwd (defined with n_objs below) */
/* adaptive handle pool */
static int *ruh_free; static int ruh_free_n; static u64 ruh_spills, ruh_assigns;
static int obj_ruh(obj_t *o) {
    if (!ADAPTIVE_RUH) return (int)((o - objs) % (u64)KV_RUHS);
    if (o->ruh < 0) {
        if (ruh_free_n > 0) { o->ruh = ruh_free[--ruh_free_n]; ruh_assigns++; }
        else { o->ruh = KV_RUHS - 1; ruh_spills++; }      /* shared spill handle */
    }
    return o->ruh;
}
static obj_t *objs; static u64 n_objs;

typedef struct fext { u64 start, len; struct fext *next; } fext_t;
static fext_t *fhead; static u64 conv_free_pgs;
static void fs_free(u64 start, u64 len) {
    conv_free_pgs += len;
    fext_t **pp = &fhead; fext_t *prev = NULL;
    while (*pp && (*pp)->start < start) { prev = *pp; pp = &(*pp)->next; }
    fext_t *n = malloc(sizeof *n); n->start = start; n->len = len; n->next = *pp; *pp = n;
    if (n->next && n->start + n->len == n->next->start) { fext_t *x = n->next; n->len += x->len; n->next = x->next; free(x); }
    if (prev && prev->start + prev->len == n->start) { prev->len += n->len; prev->next = n->next; free(n); }
}
static i64 fs_alloc1(void) {   /* allocate exactly one page, first fit */
    fext_t **pp = &fhead;
    if (!*pp) return -1;
    fext_t *e = *pp; u64 s = e->start;
    e->start++; e->len--;
    if (!e->len) { *pp = e->next; free(e); }
    conv_free_pgs--; return (i64)s;
}
static void obj_add_ext(obj_t *o, u64 s, u64 n) {
    if (o->n_ext && o->ext[o->n_ext-1].lpn_start + o->ext[o->n_ext-1].npgs == s) { o->ext[o->n_ext-1].npgs += n; o->pgs += n; return; }
    if (o->n_ext == o->cap_ext) { o->cap_ext = o->cap_ext ? o->cap_ext * 2 : 4; o->ext = realloc(o->ext, o->cap_ext * sizeof(ext_t)); }
    o->ext[o->n_ext].lpn_start = s; o->ext[o->n_ext].npgs = n; o->n_ext++; o->pgs += n;
}
static u64 obj_lpn(obj_t *o, u64 off) {
    int i = 0; u64 base = 0;
    if (o->memo_ext < o->n_ext && off >= o->memo_base) { i = o->memo_ext; base = o->memo_base; }
    for (; i < o->n_ext; i++) {
        u64 rel = off - base;
        if (rel < o->ext[i].npgs) { o->memo_ext = i; o->memo_base = base; return o->ext[i].lpn_start + rel; }
        base += o->ext[i].npgs;
    }
    fprintf(stderr, "FATAL: obj offset past end\n"); exit(4);
}
static void obj_add_zone(obj_t *o, u64 t) {
    if (o->n_zones == o->cap_z) { o->cap_z = o->cap_z ? o->cap_z * 2 : 4; o->zones = realloc(o->zones, o->cap_z * 4); }
    o->zones[o->n_zones++] = zns_alloc_zone(t);
}

/* ZNS index LSM + memlog FIFO state */
static int *ix_zone_of, *ix_slot_of, *zn_live;
static int *ixz, n_ixz, cap_ixz, ix_open_zone = -1, IX_BUDGET_Z;
static int *mlz, n_mlz, ml_budget_z, ml_open = -1;

static void ix_compact(u64 t) {
    int vi = -1, vz = -1, best = 1 << 30;
    for (int k = 0; k < n_ixz; k++) {
        int z = ixz[k];
        if (z == ix_open_zone || D.zone_wp[z] != D.zone_pgs) continue;
        if (zn_live[z] < best) { best = zn_live[z]; vz = z; vi = k; }
    }
    if (vz < 0) return;
    for (u64 e = 0; e < IX_PGS; e++) if (ix_zone_of[e] == vz) {
        zns_read_page(vz, ix_slot_of[e], t);
        if (ix_open_zone < 0 || D.zone_wp[ix_open_zone] == D.zone_pgs) {
            ix_open_zone = zns_alloc_zone(t);
            if (n_ixz == cap_ixz) { cap_ixz = cap_ixz ? cap_ixz * 2 : 16; ixz = realloc(ixz, cap_ixz * 4); }
            ixz[n_ixz++] = ix_open_zone;
        }
        ix_zone_of[e] = ix_open_zone; ix_slot_of[e] = D.zone_wp[ix_open_zone];
        zn_live[ix_open_zone]++;
        zns_append_page(ix_open_zone, t, 1);
    }
    zn_live[vz] = 0; zns_reset_zone(vz, t);
    memmove(&ixz[vi], &ixz[vi + 1], (n_ixz - vi - 1) * 4); n_ixz--;
}

/* ---------------------------- trace / op chains --------------------------- */
typedef struct { u64 gap; u32 obj; u64 off, len; u8 op; u8 turn_start; } op_t;
enum { OP_WKV = 0, OP_RKV, OP_WML, OP_RML, OP_WIX, OP_RIX, OP_DEL };
static const int op_cls_rd[] = { -1, C_KVRD, -1, C_MLRD, -1, C_IXRD, -1 };
static const int op_cls_wr[] = { C_KVWR, -1, C_MLWR, -1, C_IXWR, -1, -1 };

typedef struct { op_t *ops; u32 n, cap, cur; u64 t0; u64 turn_stall, turn_pre;
                 int turn_open, turn_pre_ops, turn_resume; u64 turns_done; } sess_t;
static sess_t *sess; static u32 n_sess;

typedef struct { u64 next_pg, end_pg; int inflight; u64 t_start, t_maxdone; } burst_t;
static burst_t *bst;
static int inflight_total = 0;
static u64 idle_cursor = 0;

/* ------------------------------ event engine ------------------------------ */
typedef struct { u64 t; u32 sid; u32 kind; } ev_t;   /* kind 0 = op start, 1 = cmd done */
static ev_t *evh; static u64 ev_n, ev_cap;
static void ev_push(u64 t, u32 sid, u32 kind) {
    if (ev_n == ev_cap) { ev_cap = ev_cap ? ev_cap * 2 : 1 << 16; evh = realloc(evh, ev_cap * sizeof(ev_t)); }
    u64 i = ev_n++; evh[i] = (ev_t){ t, sid, kind };
    while (i) { u64 p = (i - 1) / 2; if (evh[p].t <= evh[i].t) break; ev_t tmp = evh[p]; evh[p] = evh[i]; evh[i] = tmp; i = p; }
}
static ev_t ev_pop(void) {
    ev_t top = evh[0]; ev_n--;
    if (ev_n) { evh[0] = evh[ev_n]; u64 i = 0;
        for (;;) { u64 l = 2*i+1, r = l+1, m = i;
            if (l < ev_n && evh[l].t < evh[m].t) m = l;
            if (r < ev_n && evh[r].t < evh[m].t) m = r;
            if (m == i) break; ev_t tmp = evh[m]; evh[m] = evh[i]; evh[i] = tmp; i = m; } }
    return top;
}

/* execute one command (<= CMD_PGS pages) of session sid's current op at time t */
static u64 exec_cmd(u32 sid, u64 t) {
    sess_t *S = &sess[sid]; op_t *o = &S->ops[S->cur]; burst_t *B = &bst[sid];
    obj_t *ob = &objs[o->obj];
    u64 pg0 = B->next_pg, n = B->end_pg - pg0; if (n > (u64)CMD_PGS) n = CMD_PGS;
    B->next_pg += n;
    u64 fin = t, f;
    int wr = (o->op == OP_WKV || o->op == OP_WML || o->op == OP_WIX);

    if (D.mode != DEV_ZNS) {
        if (wr) {
            /* FEMU ssd_write: forced GC loop before servicing */
            while (should_gc_high()) { int ok; do_gc(1, t, &ok); if (!ok) break; }
            cur_stream = (o->op == OP_WKV) ? ST_KV : (o->op == OP_WML) ? ST_ML : ST_IX;
            wp_t *w;
            if (D.mode == DEV_FDP) {
                if (o->op == OP_WKV)      w = &D.wp[obj_ruh(ob)];
                else if (o->op == OP_WML) w = &D.wp[KV_RUHS];
                else                      w = &D.wp[KV_RUHS + 1];
            } else w = &D.wp[0];
            for (u64 i = 0; i < n; i++) {
                u64 lgc = o->off + pg0 + i;    /* logical page within object */
                u64 lpn;
                if (o->op == OP_WKV) {
                    if (lgc != ob->pgs && ob->pgs == 0 && lgc > 0) continue;  /* evicted mid-write */
                    if (lgc >= ob->pgs) {      /* append */
                        i64 s2 = fs_alloc1();
                        if (s2 < 0) { fprintf(stderr, "FATAL: conv namespace full\n"); exit(5); }
                        obj_add_ext(ob, (u64)s2, 1); lpn = (u64)s2;
                    } else lpn = obj_lpn(ob, lgc);
                } else if (o->op == OP_WML) lpn = obj_lpn(ob, lgc % ML_PGS);
                else                        lpn = obj_lpn(ob, lgc % IX_PGS);
                f = conv_program_page(w, (u32)lpn, t, 0); if (f > fin) fin = f;
            }
        } else {
            for (u64 i = 0; i < n; i++) {
                u64 lgc = o->off + pg0 + i;
                u64 lpn = (o->op == OP_RKV) ? (lgc < ob->pgs ? obj_lpn(ob, lgc) : UINT64_MAX)
                        : (o->op == OP_RML) ? obj_lpn(ob, lgc % ML_PGS)
                                            : obj_lpn(ob, lgc % IX_PGS);
                if (lpn == UINT64_MAX) continue;
                u64 ppa = D.maptbl[lpn];
                if (ppa == UINT32_MAX) continue;              /* unwritten: FEMU skips */
                u64 fr = dev_read_page(ppa_lun(ppa), t);
                if (fr > fin) fin = fr;
            }
            if (in_meas(t)) D.user_rd_pgs += n;
        }
    } else {   /* ---------------- ZNS ---------------- */
        if (o->op == OP_WKV) {
            for (u64 i = 0; i < n; i++) {
                u64 lgc = o->off + pg0 + i;
                if (lgc != ob->pgs && ob->pgs == 0 && lgc > 0) continue;   /* evicted mid-write: drop */
                assert(lgc == ob->pgs);        /* KV blobs are append-only */
                if (!ob->n_zones || D.zone_wp[ob->zones[ob->n_zones-1]] == D.zone_pgs) obj_add_zone(ob, t);
                f = zns_append_page(ob->zones[ob->n_zones-1], t, 0); ob->pgs++;
                if (f > fin) fin = f;
            }
        } else if (o->op == OP_WML) {
            for (u64 i = 0; i < n; i++) {
                if (ml_open < 0 || D.zone_wp[ml_open] == D.zone_pgs) {
                    if (n_mlz >= ml_budget_z) {
                        int oldz = mlz[0];
                        for (u64 p = 0; p < ML_PGS; p++) if (ml_page_zone[p] != UINT64_MAX && (int)(ml_page_zone[p] >> 32) == oldz) ml_page_zone[p] = UINT64_MAX;
                        zns_reset_zone(oldz, t); memmove(mlz, mlz + 1, (--n_mlz) * 4);
                    }
                    ml_open = zns_alloc_zone(t); mlz[n_mlz++] = ml_open;
                }
                u64 lg = (o->off + pg0 + i) % ML_PGS;
                ml_page_zone[lg] = ((u64)ml_open << 32) | (u32)D.zone_wp[ml_open];
                f = zns_append_page(ml_open, t, 0); if (f > fin) fin = f;
            }
        } else if (o->op == OP_WIX) {
            for (u64 i = 0; i < n; i++) {
                u64 e = (o->off + pg0 + i) % IX_PGS;
                if (ix_open_zone < 0 || D.zone_wp[ix_open_zone] == D.zone_pgs) {
                    ix_open_zone = zns_alloc_zone(t);
                    if (n_ixz == cap_ixz) { cap_ixz = cap_ixz ? cap_ixz * 2 : 16; ixz = realloc(ixz, cap_ixz * 4); }
                    ixz[n_ixz++] = ix_open_zone;
                    if (n_ixz > IX_BUDGET_Z) ix_compact(t);
                }
                if (ix_zone_of[e] >= 0) zn_live[ix_zone_of[e]]--;
                ix_zone_of[e] = ix_open_zone; ix_slot_of[e] = D.zone_wp[ix_open_zone];
                zn_live[ix_open_zone]++;
                f = zns_append_page(ix_open_zone, t, 0); if (f > fin) fin = f;
            }
        } else {
            for (u64 i = 0; i < n; i++) {
                u64 lgc = o->off + pg0 + i;
                if (o->op == OP_RKV) {
                    if (lgc >= ob->pgs) continue;
                    f = zns_read_page(ob->zones[lgc / D.zone_pgs], (int)(lgc % D.zone_pgs), t);
                } else if (o->op == OP_RML) {
                    u64 zs = ml_page_zone[lgc % ML_PGS]; if (zs == UINT64_MAX) continue;
                    f = zns_read_page((int)(zs >> 32), (int)(u32)zs, t);
                } else {
                    u64 e = lgc % IX_PGS; if (ix_zone_of[e] < 0) continue;
                    f = zns_read_page(ix_zone_of[e], ix_slot_of[e], t);
                }
                if (f > fin) fin = f;
            }
            if (in_meas(t)) D.user_rd_pgs += n;
        }
    }
    if (in_meas(t)) {
        if (wr) tl_add(D.tl_uw, t, n); else tl_add(D.tl_ur, t, n);
        int cl = wr ? op_cls_wr[o->op] : op_cls_rd[o->op];
        if (cl >= 0) lv_add(&D.lat[cl], fin - t);
    }
    return fin;
}

static void op_del(u32 sid) {
    sess_t *S = &sess[sid]; op_t *o = &S->ops[S->cur]; obj_t *ob = &objs[o->obj];
    if (D.mode != DEV_ZNS) {
        for (int i = 0; i < ob->n_ext; i++) {
            if (!NO_TRIM) {
                for (u64 p = 0; p < ob->ext[i].npgs; p++) {
                    u64 lpn = ob->ext[i].lpn_start + p; u64 ppa = D.maptbl[lpn];
                    if (ppa != UINT32_MAX) { conv_page_invalidate(ppa); D.maptbl[lpn] = UINT32_MAX; }
                }
            }
            fs_free(ob->ext[i].lpn_start, ob->ext[i].npgs);   /* host frees LBAs either way */
        }
        ob->n_ext = 0; ob->memo_ext = 0; ob->memo_base = 0;
        if (ADAPTIVE_RUH && ob->ruh >= 0 && ob->ruh != KV_RUHS - 1)
            ruh_free[ruh_free_n++] = ob->ruh;          /* release exclusive handle */
        if (ADAPTIVE_RUH) ob->ruh = -1;
    } else {
        for (int i = 0; i < ob->n_zones; i++) zns_reset_zone(ob->zones[i], bst[sid].t_start);
        ob->n_zones = 0;
    }
    ob->pgs = 0; ob->alive = 0;
}

static void start_op(u32 sid, u64 t);
static void finish_op(u32 sid, u64 t) {
    sess_t *S = &sess[sid]; op_t *o = &S->ops[S->cur];
    if (t > t_final) t_final = t;
    if (in_meas(bst[sid].t_start)) {
        u64 lat = t - bst[sid].t_start;
        if (o->op == OP_RKV) lv_add(&D.lat[C_RESUME], lat);
        S->turn_stall += lat;
        if (o->op == OP_RKV || o->op == OP_RIX || o->op == OP_RML) { S->turn_pre += lat; S->turn_pre_ops++; }
        if (o->op == OP_RKV) S->turn_resume = 1;
    }
    S->cur++;
    int turn_ends = (S->cur == S->n) || S->ops[S->cur].turn_start;
    if (turn_ends) {
        if (S->turn_open && in_meas(t)) {
            lv_add(&D.lat[C_TURNSTALL], S->turn_stall);
            if (S->turn_pre_ops)
                lv_add(&D.lat[S->turn_resume ? C_TTFT_MISS : C_TTFT_HIT], S->turn_pre);
            S->turns_done++;
        }
        S->turn_stall = 0; S->turn_open = 0; S->turn_pre = 0; S->turn_pre_ops = 0; S->turn_resume = 0;
    }
    if (S->cur < S->n) ev_push(t + S->ops[S->cur].gap, sid, 0);
}
static void start_op(u32 sid, u64 t) {
    sess_t *S = &sess[sid]; op_t *o = &S->ops[S->cur];
    if (o->turn_start) S->turn_open = 1;
    burst_t *B = &bst[sid];
    B->t_start = t; B->t_maxdone = t;
    if (o->op == OP_DEL) { op_del(sid); finish_op(sid, t); return; }
    B->next_pg = 0; B->end_pg = o->len; B->inflight = 0;
    if (!B->end_pg) { finish_op(sid, t); return; }
    int k = 0;
    while (B->next_pg < B->end_pg && k < QD) { u64 f = exec_cmd(sid, t); B->inflight++; inflight_total++; ev_push(f, sid, 1); k++; }
}
static void cmd_done(u32 sid, u64 t) {
    burst_t *B = &bst[sid];
    B->inflight--; inflight_total--;
    if (B->t_maxdone < t) B->t_maxdone = t;
    if (B->next_pg < B->end_pg) { u64 f = exec_cmd(sid, t); B->inflight++; inflight_total++; ev_push(f, sid, 1); }
    else if (B->inflight == 0) finish_op(sid, B->t_maxdone);
}

/* ------------------------------ trace loading ----------------------------- */
static void load_trace(const char *path) {
    FILE *fp = fopen(path, "r"); if (!fp) { perror("trace"); exit(1); }
    char line[256];
    n_sess = 0; n_objs = 0;
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == 'H') sscanf(line, "H %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                                   &n_objs, &t_end_ns, &IX_PGS, &ML_PGS, &meas_start_ns);
        else if (line[0] == 'A') n_sess++;
    }
    rewind(fp);
    sess = calloc(n_sess + 1, sizeof(sess_t)); bst = calloc(n_sess + 1, sizeof(burst_t));
    objs = calloc(n_objs + 2, sizeof(obj_t));
    u32 sid = 0; sess_t *cur = NULL;
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == 'A') {
            u64 dummy, t0; sscanf(line, "A %" SCNu64 " %" SCNu64, &dummy, &t0);
            cur = &sess[sid++]; cur->t0 = t0;
        } else if (line[0] == 'O' && cur) {
            u64 gap, off, len; u32 obj, ts; char kind[6];
            if (sscanf(line, "O %" SCNu64 " %5s %u %" SCNu64 " %" SCNu64 " %u", &gap, kind, &obj, &off, &len, &ts) != 6) continue;
            if (cur->n == cur->cap) { cur->cap = cur->cap ? cur->cap * 2 : 16; cur->ops = realloc(cur->ops, cur->cap * sizeof(op_t)); }
            op_t *o = &cur->ops[cur->n++];
            o->gap = gap; o->obj = obj; o->off = off; o->len = len; o->turn_start = (u8)ts;
            o->op = !strcmp(kind,"WKV")?OP_WKV:!strcmp(kind,"RKV")?OP_RKV:!strcmp(kind,"WML")?OP_WML:
                    !strcmp(kind,"RML")?OP_RML:!strcmp(kind,"WIX")?OP_WIX:!strcmp(kind,"RIX")?OP_RIX:OP_DEL;
        }
    }
    fclose(fp);
    fprintf(stderr, "trace: %u sessions, %" PRIu64 " objs, horizon %.0fs, meas from %.0fs\n",
            n_sess, n_objs, t_end_ns / 1e9, meas_start_ns / 1e9);
}

/* ------------------------------- invariants ------------------------------- */
static void check_invariants(FILE *out) {
    if (D.mode == DEV_ZNS) {
        u64 used = 0; for (int z = 0; z < D.n_zones; z++) if (D.zone_state[z]) used++;
        fprintf(out, "INVARIANT zns zones_in_use=%" PRIu64 " free=%d total=%d\n", used, D.zf_cnt, D.n_zones);
        return;
    }
    u64 valid = 0, sum_vpc = 0;
    for (u64 p = 0; p < TT_PGS; p++) valid += (D.pgst[p] == PG_VALID);
    for (int l = 0; l < TT_LINES; l++) sum_vpc += D.lines[l].vpc;
    u64 mapped = 0;
    for (u64 p = 0; p < TT_PGS; p++) if (D.rmap[p] != UINT32_MAX) {
        mapped++;
        if (D.maptbl[D.rmap[p]] != p) { fprintf(out, "INVARIANT FAIL: map/rmap mismatch\n"); return; }
    }
    fprintf(out, "INVARIANT conv valid_pgs=%" PRIu64 " sum_line_vpc=%" PRIu64 " rmap_mapped=%" PRIu64 " %s\n",
            valid, sum_vpc, mapped, (valid == sum_vpc && valid == mapped) ? "OK" : "FAIL");
    fprintf(out, "INVARIANT conv free_pgs=%" PRIu64 " usable=%" PRIu64 " live_frac_of_raw=%.4f\n",
            conv_free_pgs, USABLE_PGS, (double)valid / TT_PGS);
}

/* --------------------------------- main ----------------------------------- */
int main(int argc, char **argv) {
    const char *tracef = NULL, *outp = "out"; int mode = DEV_CONV;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) tracef = argv[++i];
        else if (!strcmp(argv[i], "-d")) { const char *m = argv[++i];
            mode = !strcmp(m,"conv")?DEV_CONV:!strcmp(m,"fdp")?DEV_FDP:DEV_ZNS; }
        else if (!strcmp(argv[i], "-o")) outp = argv[++i];
        else if (!strcmp(argv[i], "-O")) OP_FRAC = atof(argv[++i]);
        else if (!strcmp(argv[i], "-q")) QD = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-Z")) ZNS_ZONE_LUNS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-E")) ZNS_LAZY_ERASE = 0;   /* eager (inline) zone reset */
        else if (!strcmp(argv[i], "-S")) SUSP = 1;             /* idealized read suspension */
        else if (!strcmp(argv[i], "-R")) KV_RUHS = atoi(argv[++i]);  /* per-session FDP RUHs */
        else if (!strcmp(argv[i], "-A")) ADAPTIVE_RUH = 1;           /* adaptive handle pool */
        else if (!strcmp(argv[i], "-G")) SEP_GC_WP = 1;              /* separated GC write pointer */
        else if (!strcmp(argv[i], "-N")) NO_TRIM = 1;                /* deletes never reach device */
        else if (!strcmp(argv[i], "-T")) GC_PCT = atof(argv[++i]) / 100.0;  /* bg-GC occupancy threshold
                                                        (FEMU gc_thres_pcent; default 75 over-cleans at
                                                        high fill -- see validation) */
        else if (!strcmp(argv[i], "-B")) BLKS_PER_PL = atoi(argv[++i]);
    }
    if (!tracef) { fprintf(stderr, "usage: sim -t trace -d conv|fdp|zns -o outprefix [-O opfrac] [-q qd] [-Z zone_luns]\n"); return 1; }
    NLUNS = NCHS * LUNS_PER_CH; PGS_PER_LINE = PGS_PER_BLK * NLUNS;
    TT_LINES = BLKS_PER_PL; TT_PGS = (u64)PGS_PER_LINE * TT_LINES;
    USABLE_PGS = (u64)((1.0 - OP_FRAC) * TT_PGS);

    load_trace(tracef);

    memset(&D, 0, sizeof D); D.mode = mode;
    D.lun_avail = calloc(NLUNS, 8);
    D.lun_rd_avail = calloc(NLUNS, 8);
    D.wp = calloc(KV_RUHS + 3, sizeof(wp_t));
    if (ADAPTIVE_RUH) {
        for (u64 i = 0; i < n_objs + 2; i++) objs[i].ruh = -1;
        ruh_free = malloc(KV_RUHS * 4); ruh_free_n = 0;
        for (int i = KV_RUHS - 2; i >= 0; i--) ruh_free[ruh_free_n++] = i;   /* last = spill */
    }
    D.tl_n = t_end_ns / 1000000000ULL + 7200;
    D.tl_uw = calloc(D.tl_n, 8); D.tl_ur = calloc(D.tl_n, 8); D.tl_gc = calloc(D.tl_n, 8);
    D.lat[C_KVRD].sample_mod = 8;
    char fn[512];

    if (mode != DEV_ZNS) {
        D.maptbl = malloc(TT_PGS * 4); memset(D.maptbl, 0xFF, TT_PGS * 4);
        D.rmap   = malloc(TT_PGS * 4); memset(D.rmap, 0xFF, TT_PGS * 4);
        D.pgst   = calloc(TT_PGS, 1);
        D.pgstream = calloc(TT_PGS, 1);
        D.lines  = calloc(TT_LINES, sizeof(line_t));
        D.line_erases = calloc(TT_LINES, 4);
        D.freelist = malloc((TT_LINES + 1) * 4);
        D.heap = malloc(TT_LINES * sizeof(void*));
        for (int i = 0; i < TT_LINES; i++) { D.lines[i].id = i; fl_push(i); }
        D.gc_thres_lines      = (int)((1 - GC_PCT) * TT_LINES);
        D.gc_thres_lines_high = (int)((1 - GC_PCT_HIGH) * TT_LINES);
        conv_free_pgs = 0; fs_free(0, USABLE_PGS);
        /* preallocate index (obj 0) and memlog (obj 1) */
        obj_add_ext(&objs[0], 0, IX_PGS);
        obj_add_ext(&objs[1], IX_PGS, ML_PGS);
        {   /* carve from free list */
            fext_t *e = fhead; e->start += IX_PGS + ML_PGS; e->len -= IX_PGS + ML_PGS; conv_free_pgs -= IX_PGS + ML_PGS;
        }
        snprintf(fn, sizeof fn, "%s_gc.csv", outp); D.gclog = fopen(fn, "w");
        if (D.gclog) fprintf(D.gclog, "start_s,end_s\n");
    } else {
        D.zone_luns = ZNS_ZONE_LUNS; D.zone_pgs = PGS_PER_BLK * D.zone_luns;
        int groups = NLUNS / D.zone_luns;
        D.n_zones = TT_LINES * groups;
        D.zone_wp = calloc(D.n_zones, 4); D.zone_state = calloc(D.n_zones, 4);
        D.zone_group = calloc(D.n_zones, 4); D.zone_erases = calloc(D.n_zones, 4);
        D.zone_dirty = calloc(D.n_zones, 1);
        D.zfree = malloc((D.n_zones + 1) * 4);
        for (int i = 0; i < D.n_zones; i++) D.zone_group[i] = i % groups;
        for (int i = 0; i < D.n_zones - ZNS_RSV_ZONES; i++) { D.zfree[D.zf_tail] = i; D.zf_tail = (D.zf_tail + 1) % (D.n_zones + 1); D.zf_cnt++; }
        D.zns_usable = D.zf_cnt;
        ix_zone_of = malloc(IX_PGS * 4); ix_slot_of = malloc(IX_PGS * 4);
        for (u64 i = 0; i < IX_PGS; i++) ix_zone_of[i] = -1;
        zn_live = calloc(D.n_zones, 4);
        IX_BUDGET_Z = (int)(2 * ((IX_PGS + D.zone_pgs - 1) / D.zone_pgs)) + 1;
        ml_budget_z = (int)((ML_PGS + D.zone_pgs - 1) / D.zone_pgs) + 1;
        mlz = malloc((ml_budget_z + 2) * 4);
        ml_page_zone = malloc(ML_PGS * 8); memset(ml_page_zone, 0xFF, ML_PGS * 8);
    }

    for (u32 s = 0; s < n_sess; s++) if (sess[s].n) ev_push(sess[s].t0 + sess[s].ops[0].gap, s, 0);

    u64 processed = 0, t_last = 0;
    while (ev_n) {
        ev_t e = ev_pop();
        /* FEMU ftl_thread background GC when no requests pending */
        if (mode != DEV_ZNS && inflight_total == 0 && should_gc()) {
            u64 bg = t_last > idle_cursor ? t_last : idle_cursor;
            while (bg < e.t && should_gc()) {
                int ok; u64 end = do_gc(0, bg, &ok);
                if (!ok) break;
                bg = end; idle_cursor = end;
            }
        }
        t_last = e.t;
        if (e.kind == 0) start_op(e.sid, e.t); else cmd_done(e.sid, e.t);
        processed++;
    }

    /* ----------------------------- report ----------------------------- */
    snprintf(fn, sizeof fn, "%s_summary.txt", outp); FILE *out = fopen(fn, "w");
    double meas_s = (double)(t_final - meas_start_ns) / 1e9;
    u64 dev_wr = D.user_wr_pgs + D.gc_copy_pgs + D.host_copy_pgs;
    double waf = D.user_wr_pgs ? (double)dev_wr / D.user_wr_pgs : 0;
    u64 turns = 0; for (u32 s = 0; s < n_sess; s++) turns += sess[s].turns_done;
    fprintf(out, "mode=%s op_frac=%.3f qd=%d zone_luns=%d susp=%d kv_ruhs=%d raw_GiB=%.2f usable_GiB=%.2f\n",
            mode==DEV_CONV?"conv":mode==DEV_FDP?"fdp":"zns", OP_FRAC, QD, mode==DEV_ZNS?D.zone_luns:0,
            SUSP, KV_RUHS,
            TT_PGS * 4096.0 / (1 << 30),
            (mode == DEV_ZNS ? (double)D.zns_usable * D.zone_pgs : (double)USABLE_PGS) * 4096.0 / (1 << 30));
    fprintf(out, "events=%" PRIu64 " meas_window_s=%.1f makespan_s=%.1f turns=%" PRIu64 "\n",
            processed, meas_s, t_final / 1e9, turns);
    fprintf(out, "user_wr_pgs=%" PRIu64 " user_rd_pgs=%" PRIu64 " gc_copy_pgs=%" PRIu64 " host_copy_pgs=%" PRIu64 " total_run_user_wr_pgs=%" PRIu64 "\n",
            D.user_wr_pgs, D.user_rd_pgs, D.gc_copy_pgs, D.host_copy_pgs, D.tot_user_wr_pgs);
    fprintf(out, "WAF=%.4f\n", waf);
    fprintf(out, "erases=%" PRIu64 " zone_resets=%" PRIu64 " erases_per_user_GB=%.3f\n",
            D.erases, D.zone_resets, D.user_wr_pgs ? D.erases / (D.user_wr_pgs * 4096.0 / 1e9) : 0);
    fprintf(out, "gc_count=%" PRIu64 " gc_busy_s=%.3f gc_busy_pct=%.3f gc_max_ms=%.3f\n",
            D.gc_count, D.gc_busy_ns / 1e9, meas_s > 0 ? 100.0 * (D.gc_busy_ns / 1e9) / meas_s : 0, D.gc_max_ns / 1e6);
    fprintf(out, "user_write_MBps=%.2f user_read_MBps=%.2f turns_per_hour=%.1f\n",
            D.user_wr_pgs * 4096.0 / meas_s / 1e6, D.user_rd_pgs * 4096.0 / meas_s / 1e6, turns / (meas_s / 3600.0));
    for (int c = 0; c < NCLS; c++) {
        lvec *L = &D.lat[c]; if (!L->n) continue;
        qsort(L->v, L->n, 4, cmp_u32);
        double sum = 0; for (u64 i = 0; i < L->n; i++) sum += L->v[i];
        fprintf(out, "LAT %s n=%" PRIu64 " sampled=%" PRIu64 " mean_us=%.1f p50_us=%.0f p90_us=%.0f p99_us=%.0f p999_us=%.0f p9999_us=%.0f max_us=%u\n",
                cls_name[c], L->seen, L->n, sum / L->n,
                lv_pct(L, 50), lv_pct(L, 90), lv_pct(L, 99), lv_pct(L, 99.9), lv_pct(L, 99.99), L->v[L->n - 1]);
    }
    if (D.gc_dur.n) { qsort(D.gc_dur.v, D.gc_dur.n, 4, cmp_u32);
        fprintf(out, "GCDUR n=%" PRIu64 " p50_ms=%.2f p99_ms=%.2f max_ms=%.2f\n",
                D.gc_dur.n, lv_pct(&D.gc_dur, 50) / 1e3, lv_pct(&D.gc_dur, 99) / 1e3, D.gc_dur.v[D.gc_dur.n - 1] / 1e3);
    }
    if (ADAPTIVE_RUH)
        fprintf(out, "RUHPOOL assigns=%" PRIu64 " spills=%" PRIu64 " spill_frac=%.4f\n",
                ruh_assigns, ruh_spills,
                (ruh_assigns + ruh_spills) ? (double)ruh_spills / (ruh_assigns + ruh_spills) : 0);
    if (D.pgstream) {
        u64 tot = 0; for (int i = 0; i < N_STREAMS + 2; i++) tot += D.gc_victim_mix[i];
        if (tot) fprintf(out, "STREAMMIX kv=%.3f memlog=%.3f index=%.3f gc_rewrite=%.3f (of %" PRIu64 " copied)\n",
            (double)D.gc_victim_mix[ST_KV]/tot, (double)D.gc_victim_mix[ST_ML]/tot,
            (double)D.gc_victim_mix[ST_IX]/tot, (double)D.gc_victim_mix[N_STREAMS]/tot, tot);
    }
    {   /* wear spread */
        u32 emax = 0; double esum = 0; int n = 0;
        if (mode != DEV_ZNS) { for (int i = 0; i < TT_LINES; i++) { if (D.line_erases[i] > emax) emax = D.line_erases[i]; esum += D.line_erases[i]; } n = TT_LINES; }
        else { for (int i = 0; i < D.n_zones; i++) { if (D.zone_erases[i] > emax) emax = D.zone_erases[i]; esum += D.zone_erases[i]; } n = D.n_zones; }
        fprintf(out, "WEAR erase_max=%u erase_mean=%.2f spread=%.2f\n", emax, esum / n, esum > 0 ? emax / (esum / n) : 0);
    }
    check_invariants(out);
    fclose(out);

    for (int c = 0; c < NCLS; c++) {
        lvec *L = &D.lat[c]; if (!L->n) continue;
        snprintf(fn, sizeof fn, "%s_lat_%s.csv", outp, cls_name[c]);
        FILE *f2 = fopen(fn, "w"); u64 step = L->n > 200000 ? L->n / 200000 : 1;
        for (u64 i = 0; i < L->n; i += step) fprintf(f2, "%u\n", L->v[i]);
        if ((L->n - 1) % step) fprintf(f2, "%u\n", L->v[L->n - 1]);
        fclose(f2);
    }
    snprintf(fn, sizeof fn, "%s_timeline.csv", outp);
    FILE *f3 = fopen(fn, "w"); fprintf(f3, "t_s,user_wr_MB,user_rd_MB,gc_events\n");
    for (u64 b = meas_start_ns / 1000000000ULL; b <= t_final / 1000000000ULL + 1 && b < D.tl_n; b++)
        fprintf(f3, "%" PRIu64 ",%.2f,%.2f,%" PRIu64 "\n", b, D.tl_uw[b] * 4096.0 / 1e6, D.tl_ur[b] * 4096.0 / 1e6, D.tl_gc[b]);
    fclose(f3);
    if (D.gclog) fclose(D.gclog);
    fprintf(stderr, "done %s: WAF=%.3f gc=%" PRIu64 " turns=%" PRIu64 " makespan=%.0fs\n",
            mode==DEV_CONV?"conv":mode==DEV_FDP?"fdp":"zns", waf, D.gc_count, turns, t_final / 1e9);
    return 0;
}
