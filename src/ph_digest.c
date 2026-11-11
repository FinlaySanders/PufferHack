/* ph_digest.c: the per-key state digest of the stock differential (include/ph_digest.h). This file is the one definition: the
   library compiles it with the fork's headers; the verifier's stock driver compiles the same file with PHD_STOCK against stock
   NLE 0.9.0's headers, where the file statics come from the caller. Pointer-free: objects, monsters and timers by id. It reads
   the game and never writes it. The section body is compiled twice from this file (hash, then text).

   A caller may pass a workspace (ph_digest_ws_size bytes, zeroed before a game): parts that rarely change (map columns, object
   and monster records, object classes, vision, mvitals, level info, both random streams' tables, observation buffers) keep a
   copy of the memory they were computed from and are recomputed only when that memory differs, so the result is the same with
   or without one. */
#ifndef PHD_BODY
#include <stdint.h>
#include <string.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#include "hack.h"
#include "timeout.h"
#include "lev.h"
#include "region.h"
#include "isaac64.h"
#ifndef PHD_STOCK
#include "nletypes.h"
extern isaac64_ctx *FDECL(nle_rng_state, (int));
#endif
#include "ph_digest.h"

#ifdef PHD_STOCK
#define PHD_LFL level.flags
#define PHD_KILLC u.uconduct.killer
#define PHD_LINFO_FL flags
#define PHD_CELL(x, y) levl[(x)][(y)]
#define PHD_OBJ_AT(x, y) level.objects[(x)][(y)]
#define PHD_LS_FLAGS(l) (l)->flags
#define PHD_MON_AT(x, y) level.monsters[(x)][(y)]
/* the arrays a map column's words come from (column-major, [COLNO][ROWNO]): the cells, their top objects, their monsters (in
   the fork's cell), lastseentyp */
#define PHD_LOC_ARR (&levl[0][0])
#define PHD_MON_ARR (&level.monsters[0][0])
#define PHD_OBJ_ARR (&level.objects[0][0])
#else
#undef killer
#define PHD_LFL level.lflags
#define PHD_KILLC u.uconduct.killcount
#define PHD_LINFO_FL linfo_flags
#define PHD_CELL(x, y) NH_TILE((x), (y))
#define PHD_OBJ_AT(x, y) level.objs[(x)][(y)]
#define PHD_LS_FLAGS(l) (l)->ls_flags
#define PHD_MON_AT(x, y) NH_MON_CELL((x), (y))
#define PHD_LOC_ARR (&level.locations[0][0])
#define PHD_OBJ_ARR (&level.objs[0][0])
#endif

/* what the digest reads besides plain game globals: file statics and the two random streams */
struct phd_statics {
    struct engr **head_engr;
    timer_element **timer_base;
    short *disco;
    light_source **light_base;
    NhRegion ***regions;
    int *n_regions;
    boolean *artiexist;
    const struct isaac64_ctx *rng[2];
};

static const char *const phd_names[PHD_NCOMP] = {
    "core", "disp", "msg", "xyt", "done", "key",
    "u", "attr", "prop", "skill", "spell", "inv", "mon", "floor", "buried", "migr", "trap", "engr", "timer", "light", "region",
    "cell", "vis", "room", "oc", "disco", "ctx", "lflags", "mv", "linfo",
    "obs.gl", "obs.ch", "obs.co", "obs.sp", "obs.bl", "obs.msg", "obs.ps", "obs.in", "obs.ig", "obs.is", "obs.il", "obs.io",
    "obs.tc", "obs.tk", "obs.tu", "obs.mi"
};
enum { SDS_U, SDS_ATTR, SDS_PROP, SDS_SKILL, SDS_SPELL, SDS_INV, SDS_MON, SDS_FLOOR, SDS_BURIED, SDS_MIGR, SDS_TRAP, SDS_ENGR,
       SDS_TIMER, SDS_LIGHT, SDS_REGION, SDS_CELL, SDS_VIS, SDS_ROOM, SDS_OC, SDS_DISCO, SDS_CTX, SDS_LFLAGS, SDS_MV, SDS_LINFO };
static const unsigned phd_obs_size[PHD_NOBS] = PHD_OBS_SIZES;
#define PHD_OBS_MAX (55 * 80)

/* hashing: 64-bit words through four lanes of 128-bit multiply folding (wyhash's mix), eight words per round; byte buffers as
   little-endian words, the tail zero-padded, the length mixed in at the end */
#define PHD_K0 0xa0761d6478bd642fULL
#define PHD_K1 0xe7037ed1a0b428dbULL
#define PHD_K2 0x8ebc6af09c88c6e3ULL
#define PHD_K3 0x589965cc75374cc3ULL
#define PHD_K4 0x1d8e4e27c47d124fULL
static inline uint64_t phd_mum(uint64_t a, uint64_t b)
{
    __uint128_t r = (__uint128_t) a * b;
    return (uint64_t) r ^ (uint64_t) (r >> 64);
}
struct phd_lanes {
    uint64_t a, b, c, d, n;
};
static inline void phd_lanes_init(struct phd_lanes *l, uint64_t seed)
{
    l->a = seed ^ PHD_K0;
    l->b = seed ^ PHD_K1;
    l->c = seed ^ PHD_K2;
    l->d = seed ^ PHD_K3;
    l->n = 0;
}
/* n words, n a multiple of 8 */
static inline void phd_lanes_words(struct phd_lanes *l, const uint64_t *w, size_t n)
{
    uint64_t a = l->a, b = l->b, c = l->c, d = l->d;
    for (size_t i = 0; i < n; i += 8) {
        a = phd_mum(w[i] ^ PHD_K1, w[i + 1] ^ a);
        b = phd_mum(w[i + 2] ^ PHD_K2, w[i + 3] ^ b);
        c = phd_mum(w[i + 4] ^ PHD_K3, w[i + 5] ^ c);
        d = phd_mum(w[i + 6] ^ PHD_K4, w[i + 7] ^ d);
    }
    l->a = a, l->b = b, l->c = c, l->d = d, l->n += n;
}
static inline uint64_t phd_lanes_end(const struct phd_lanes *l, uint64_t len)
{
    uint64_t h = phd_mum(l->a ^ PHD_K1, l->b ^ PHD_K2) ^ phd_mum(l->c ^ PHD_K3, l->d ^ PHD_K4);
    return phd_mum(h ^ PHD_K0, len ^ PHD_K1);
}
static inline uint64_t phd_merge(uint64_t acc, uint64_t v)
{
    return phd_mum(acc ^ PHD_K2, v ^ PHD_K3);
}
static uint64_t phd_bytes(const void *p, size_t n, uint64_t seed)
{
    struct phd_lanes l;
    uint64_t w[8];
    const unsigned char *q = (const unsigned char *) p;
    size_t i = 0;
    phd_lanes_init(&l, seed);
    for (; i + 64 <= n; i += 64) {
        memcpy(w, q + i, 64);
        phd_lanes_words(&l, w, 8);
    }
    if (i < n) {
        memset(w, 0, sizeof w);
        memcpy(w, q + i, n - i);
        phd_lanes_words(&l, w, 8);
    }
    return phd_lanes_end(&l, n);
}
/* nw words (up to 64), zero-padded to a multiple of 8 */
static uint64_t phd_words(const uint64_t *src, int nw, uint64_t seed)
{
    struct phd_lanes l;
    uint64_t w[64];
    int n8 = (nw + 7) & ~7;
    memcpy(w, src, (size_t) nw * 8);
    memset(w + nw, 0, (size_t) (n8 - nw) * 8);
    phd_lanes_init(&l, seed);
    phd_lanes_words(&l, w, (size_t) n8);
    return phd_lanes_end(&l, (uint64_t) nw);
}

/* the workspace (see the top of the file) */
#define PHD_OSLOTS 4096
#define PHD_MSLOTS 1024
struct phd_ws {
    uint64_t magic;
    struct {
        int ok;
        uint64_t h, rm[2 * ISAAC64_SZ];
    } rng[2];
    struct {
        int ok;
        uint64_t h;
        unsigned char raw[PHD_OBS_MAX];
    } obs[PHD_NOBS];
    struct {
        int ok;
        uint32_t nptr[COLNO]; /* rows with an object or a monster when computed (a bit each) */
        uint64_t h[COLNO], ids[COLNO][ROWNO], w[COLNO][3 * ROWNO]; /* each column's hash, its cells' ids and words */
        struct rm loc[COLNO * ROWNO];
        void *obj[COLNO * ROWNO];
#ifdef PHD_MON_ARR
        void *mon[COLNO * ROWNO];
#endif
        schar lst[COLNO * ROWNO];
    } cells;
    struct {
        int ok;
        uint64_t h;
        unsigned char raw[ROWNO * COLNO];
    } vis;
    struct {
        int ok;
        uint64_t h;
        unsigned char raw[NUM_OBJECTS * sizeof(struct objclass)];
    } oc;
    struct {
        int ok;
        uint64_t h;
        unsigned char raw[NUMMONS * 3];
    } mv;
    struct {
        int ok;
        uint64_t h;
        unsigned char raw[MAXLINFO];
    } li;
    struct {
        const void *p;
        uint64_t h;
        unsigned char raw[sizeof(struct obj)];
    } os[PHD_OSLOTS];
    struct {
        const void *p;
        uint64_t h;
        unsigned char raw[sizeof(struct monst)];
    } ms[PHD_MSLOTS];
};
#define PHD_WS_MAGIC 0x7068645f77733031ULL
static inline unsigned phd_slot(const void *p, unsigned n)
{
    return (unsigned) (((uint64_t) (uintptr_t) p * 0x9E3779B97F4A7C15ULL) >> 40) & (n - 1);
}

/* a section's word stream: values are buffered and folded into the lanes in blocks */
#define PHD_CAP 512
struct phd_t {
    struct phd_lanes l;
    int n, sec;
    FILE *f;            /* text mode */
    uint64_t *out;      /* section results */
    struct phd_ws *ws;  /* NULL: no workspace */
    uint64_t w[PHD_CAP];
};
static void phd_flush(struct phd_t *t)
{
    while (t->n & 7)
        t->w[t->n++] = 0;
    phd_lanes_words(&t->l, t->w, (size_t) t->n);
    t->n = 0;
}
static inline void phd_put(struct phd_t *t, uint64_t v)
{
    t->w[t->n++] = v;
    if (t->n == PHD_CAP)
        phd_flush(t);
}
static void phd_putstr(struct phd_t *t, const char *s)
{
    size_t n = strlen(s);
    phd_put(t, (uint64_t) n);
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = 0;
        memcpy(&v, s + i, n - i < 8 ? n - i : 8);
        phd_put(t, v);
    }
}
static void phd_sec_begin(struct phd_t *t, int sec)
{
    t->sec = sec;
    t->n = 0;
    phd_lanes_init(&t->l, 0x5ec0000ULL + (uint64_t) sec);
}
static uint64_t phd_sec_value(struct phd_t *t)
{
    uint64_t words = t->l.n + (uint64_t) t->n;
    phd_flush(t);
    return phd_lanes_end(&t->l, words);
}
static void phd_sec_end(struct phd_t *t)
{
    t->out[t->sec] = phd_sec_value(t);
}
/* equal memory, compared without a call or an early exit (spans are short) */
static inline int phd_same(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *) a, *q = (const unsigned char *) b;
    uint64_t acc = 0, x, y;
    size_t i = 0;
#ifdef __AVX2__
    __m256i va = _mm256_setzero_si256();
    for (; i + 32 <= n; i += 32)
        va = _mm256_or_si256(va, _mm256_xor_si256(_mm256_loadu_si256((const __m256i *) (p + i)),
                                                  _mm256_loadu_si256((const __m256i *) (q + i))));
    if (!_mm256_testz_si256(va, va))
        return 0;
#endif
    for (; i + 8 <= n; i += 8) {
        memcpy(&x, p + i, 8);
        memcpy(&y, q + i, 8);
        acc |= x ^ y;
    }
    for (; i < n; i++)
        acc |= (uint64_t) (p[i] ^ q[i]);
    return acc == 0;
}
static inline int phd_copy(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
    return 1;
}
/* a cached section: its memory equal to the copy -> the copy's value */
static int phd_cache_hit(int *ok, const void *raw, const void *mem, size_t n)
{
    return *ok && phd_same(raw, mem, n);
}
/* a name: its hash, or 0 for none */
static uint64_t phd_name(const char *s)
{
    return s ? phd_bytes(s, strlen(s), 0x4e4dULL) | 1 : 0;
}

/* text mode: one line per record */
static void phd_tbeg(struct phd_t *t, const char *tag)
{
    fprintf(t->f, "%s %s", phd_names[PHD_SEC0 + t->sec], tag);
}
static void phd_tbegi(struct phd_t *t, const char *tag, long i)
{
    fprintf(t->f, "%s %s%ld", phd_names[PHD_SEC0 + t->sec], tag, i);
}

#define PHD_BODY 1
#define PHD_TEXT 0
#include "ph_digest.c"
#undef PHD_TEXT
#define PHD_TEXT 1
#include "ph_digest.c"
#undef PHD_TEXT

/* the digest-line components: both random streams (position, a, b, c and the two tables), the message as the digest line
   prints it (to the first NUL, newlines as spaces, outer whitespace stripped), the hero's x-1/y and the turn, done */
static uint64_t phd_rng(const struct isaac64_ctx *c, uint64_t seed, struct phd_ws *ws, int k)
{
    uint64_t rm;
    if (ws && ws->rng[k].ok && phd_same(ws->rng[k].rm, c->r, sizeof c->r) && phd_same(ws->rng[k].rm + ISAAC64_SZ, c->m, sizeof c->m))
        rm = ws->rng[k].h;
    else {
        struct phd_lanes l;
        phd_lanes_init(&l, seed);
        phd_lanes_words(&l, c->r, ISAAC64_SZ);
        phd_lanes_words(&l, c->m, ISAAC64_SZ);
        rm = phd_lanes_end(&l, 2 * ISAAC64_SZ);
        if (ws) {
            memcpy(ws->rng[k].rm, c->r, sizeof c->r);
            memcpy(ws->rng[k].rm + ISAAC64_SZ, c->m, sizeof c->m);
            ws->rng[k].h = rm, ws->rng[k].ok = 1;
        }
    }
    uint64_t w[5] = { rm, (uint64_t) c->n, c->a, c->b, c->c };
    return phd_words(w, 5, seed);
}
static int phd_space(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f);
}
static uint64_t phd_msg(const unsigned char *m)
{
    unsigned char b[257];
    size_t n = 0, s = 0;
    if (!m)
        return phd_bytes("", 0, 0x3e5ULL);
    while (n < 256 && m[n])
        b[n] = m[n] == '\n' ? ' ' : m[n], n++;
    while (n > 0 && phd_space(b[n - 1]))
        n--;
    while (s < n && phd_space(b[s]))
        s++;
    return phd_bytes(b + s, n - s, 0x3e5ULL);
}
static uint64_t phd_xyt(void)
{
    uint64_t w[3] = { (uint64_t) ((long) u.ux - 1), (uint64_t) (long) u.uy, (uint64_t) (long) moves };
    return phd_words(w, 3, 0x787974ULL);
}
static uint64_t phd_obs1(const void *p, int i, struct phd_ws *ws)
{
    size_t n = phd_obs_size[i];
    if (!p)
        return PHD_UNBOUND;
    if (ws && phd_cache_hit(&ws->obs[i].ok, ws->obs[i].raw, p, n))
        return ws->obs[i].h;
    uint64_t h = phd_bytes(p, n, 0x0b5ULL + (uint64_t) i);
    if (ws) {
        memcpy(ws->obs[i].raw, p, n);
        ws->obs[i].h = h, ws->obs[i].ok = 1;
    }
    return h;
}

/* every component but the caller's key: obs[i] the canonical buffers (NULL = unbound), msg the message buffer (NULL =
   unbound) */
static void phd_compute(const struct phd_statics *st, const void *const *obs, const unsigned char *msg, int done,
                        unsigned mode, struct phd_ws *ws, uint64_t *out)
{
    if (ws && ws->magic != PHD_WS_MAGIC) {
        memset(ws, 0, sizeof *ws);
        ws->magic = PHD_WS_MAGIC;
    }
    out[PHD_CORE] = phd_rng(st->rng[0], 0xc0ULL, ws, 0);
    out[PHD_DISP] = phd_rng(st->rng[1], 0xd1ULL, ws, 1);
    out[PHD_MSG] = phd_msg(msg);
    out[PHD_XYT] = phd_xyt();
    out[PHD_DONE] = (uint64_t) done;
    if (mode & PHD_STATE) {
        struct phd_t t;
        t.f = NULL;
        t.out = out + PHD_SEC0;
        t.ws = ws;
        phd_sections_hash(&t, st);
        for (int i = 0; i < PHD_NOBS; i++)
            out[PHD_OBS0 + i] = phd_obs1(obs[i], i, ws);
    } else {
        for (int i = PHD_SEC0; i < PHD_NCOMP; i++)
            out[i] = 0;
    }
}

static void phd_text(const struct phd_statics *st, const void *const *obs, FILE *f)
{
    struct phd_t t;
    uint64_t sec[PHD_NSEC];
    t.f = f;
    t.out = sec;
    t.ws = NULL;
    for (int k = 0; k < 2; k++)
        fprintf(f, "%s n=%u r0=%016llx m0=%016llx a=%016llx b=%016llx c=%016llx\n", phd_names[PHD_CORE + k],
                st->rng[k]->n, (unsigned long long) st->rng[k]->r[0], (unsigned long long) st->rng[k]->m[0],
                (unsigned long long) st->rng[k]->a, (unsigned long long) st->rng[k]->b, (unsigned long long) st->rng[k]->c);
    fprintf(f, "xyt x=%ld y=%ld t=%ld\n", (long) u.ux - 1, (long) u.uy, (long) moves);
    phd_sections_text(&t, st);
    for (int i = 0; i < PHD_NOBS; i++) {
        const unsigned char *p = (const unsigned char *) obs[i];
        if (!p)
            continue;
        for (unsigned r = 0; r * 79 < phd_obs_size[i]; r++) {
            fprintf(f, "%s %u ", phd_names[PHD_OBS0 + i], r);
            for (unsigned q = r * 79; q < phd_obs_size[i] && q < r * 79 + 79; q++)
                fprintf(f, "%02x", p[q]);
            fputc('\n', f);
        }
    }
}

/* the definition's id: the sha256 of this file and of ph_digest.h (first 16 hex digits each, PHD_SRC_SHA_C/_H from the build:
   any edit changes the id, and the verifier pins it), the names and the sizes */
#ifndef PHD_SRC_SHA_C
#define PHD_SRC_SHA_C 0 /* built without the source hash: an id no pin matches */
#endif
#ifndef PHD_SRC_SHA_H
#define PHD_SRC_SHA_H 0
#endif
static uint64_t phd_id(void)
{
    uint64_t h = phd_merge(phd_bytes("ph_digest v5", 12, 0), (uint64_t) PHD_SRC_SHA_C);
    h = phd_merge(h, (uint64_t) PHD_SRC_SHA_H);
    for (int i = 0; i < PHD_NCOMP; i++)
        h = phd_merge(h, phd_bytes(phd_names[i], strlen(phd_names[i]), (uint64_t) i));
    for (int i = 0; i < PHD_NOBS; i++)
        h = phd_merge(h, phd_obs_size[i]);
    return h;
}

#ifndef PHD_STOCK
/* the fork: statics from the game's context; observations from the caller's nle_obs */
static void phd_fork_statics(struct phd_statics *s)
{
    s->head_engr = &nh_cur->g_engrave_c_head_engr;
    s->timer_base = (timer_element **) &nh_cur->g_timeout_c_timer_base;
    s->disco = nh_cur->g_o_init_c_disco;
    s->light_base = (light_source **) &nh_cur->g_light_c_light_base;
    s->regions = (NhRegion ***) &nh_cur->g_region_c_regions_p;
    s->n_regions = &nh_cur->g_region_c_n_regions;
    s->artiexist = nh_cur->g_artifact_c_artiexist;
    s->rng[0] = nle_rng_state(0);
    s->rng[1] = nle_rng_state(1);
}
static void phd_fork_obs(const nle_obs *o, const void **b, int *in9)
{
    if (o->internal) {
        memcpy(in9, o->internal, 9 * sizeof *in9);
        in9[6] = 0;
    }
    const void *v[PHD_NOBS] = { o->glyphs, o->chars, o->colors, o->specials, o->blstats, o->message, o->prog_state,
                                o->internal ? in9 : NULL, o->inv_glyphs, o->inv_strs, o->inv_letters, o->inv_oclasses,
                                o->tty_chars, o->tty_colors, o->tty_cursor, o->misc };
    memcpy(b, v, sizeof v);
}

unsigned long long
ph_digest_id(void)
{
    return phd_id();
}

const char *
ph_digest_name(int comp)
{
    return comp >= 0 && comp < PHD_NCOMP ? phd_names[comp] : NULL;
}

unsigned long
ph_digest_ws_size(void)
{
    return sizeof(struct phd_ws);
}

int
ph_digest(nle_ctx_t *nle, const nle_obs *obs, unsigned mode, void *ws, unsigned long long *out)
{
    struct nh_ctx *saved = nh_cur;
    struct phd_statics st;
    const void *b[PHD_NOBS];
    int in9[9];
    uint64_t o[PHD_NCOMP];

    nh_cur = (struct nh_ctx *) nle->nh;
    phd_fork_statics(&st);
    phd_fork_obs(obs, b, in9);
    phd_compute(&st, b, obs->message, obs->done, mode, (struct phd_ws *) ws, o);
    nh_cur = saved;
    for (int i = 0; i < PHD_NCOMP; i++)
        out[i] = o[i];
    out[PHD_KEY] = 0;
    return PHD_NCOMP;
}

int
ph_digest_text(nle_ctx_t *nle, const nle_obs *obs, FILE *f)
{
    struct nh_ctx *saved = nh_cur;
    struct phd_statics st;
    const void *b[PHD_NOBS];
    int in9[9];

    nh_cur = (struct nh_ctx *) nle->nh;
    phd_fork_statics(&st);
    phd_fork_obs(obs, b, in9);
    phd_text(&st, b, f);
    nh_cur = saved;
    return 0;
}
#endif

#else /* PHD_BODY: the snapshot sections, PHD_TEXT 0 = hash, 1 = text. FB(name, value, bits): a field of the given width
         (the width of the value's type or more, so packing is injective); hash mode packs a record's fields into 64-bit
         words */
#if PHD_TEXT
#define PHD_FN(name) name##_text
#define SEC(s) (t->sec = (s))
#define SECEND() ((void) 0)
#define REC(tag) phd_tbeg(t, tag)
#define RECI(tag, i) phd_tbegi(t, tag, (long) (i))
#define FB(name, v, bits) fprintf(t->f, " " name "=%lld", (long long) (v))
#define FS(name, s) fprintf(t->f, " " name "=%s", (s))
#define END() fputc('\n', t->f)
#define PK_DECL
#else
#define PHD_FN(name) name##_hash
#define SEC(s) phd_sec_begin(t, (s))
#define SECEND() phd_sec_end(t)
#define PK_DECL uint64_t pk_acc = 0; int pk_sh = 0;
#define PK_FLUSH() do { if (pk_sh) { phd_put(t, pk_acc); pk_acc = 0; pk_sh = 0; } } while (0)
#define REC(tag) do { PK_FLUSH(); phd_put(t, (uint64_t) (unsigned char) (tag)[0] | (uint64_t) (unsigned char) (tag)[1] << 8); } while (0)
#define RECI(tag, i) do { REC(tag); FB("", (i), 32); } while (0)
#define FB(name, v, bits)                                                                                       \
    do {                                                                                                        \
        uint64_t pv_ = (bits) == 64 ? (uint64_t) (long long) (v) : (uint64_t) (long long) (v) & ((1ULL << ((bits) & 63)) - 1); \
        if (pk_sh + (bits) > 64)                                                                                \
            PK_FLUSH();                                                                                         \
        pk_acc |= pk_sh ? pv_ << (pk_sh & 63) : pv_;                                                            \
        pk_sh += (bits);                                                                                        \
    } while (0)
#define FS(name, s) do { PK_FLUSH(); phd_putstr(t, (s)); } while (0)
#define END() PK_FLUSH()
#endif
#define F(name, v) FB(name, v, 64)
#define F1(name, v) FB(name, v, 1)
#define F8(name, v) FB(name, v, 8)
#define F16(name, v) FB(name, v, 16)
#define F32(name, v) FB(name, v, 32)

/* an object's own fields (its name and contents are hashed beside it) */
#define PHD_OBJ_FIELDS(o)                                                                                              \
    F32("id", (o)->o_id); F32("wt", (o)->owt);                                                                         \
    F16("typ", (o)->otyp); F8("cls", (o)->oclass); F8("spe", (o)->spe); F8("let", (o)->invlet); F8("art", (o)->oartifact); \
    F8("where", (o)->where); F8("tm", (o)->timed);                                                                     \
    F8("x", (o)->ox); F8("y", (o)->oy);                                                                                \
    F1("bl", (o)->blessed); F1("cu", (o)->cursed); F1("kn", (o)->known); F1("dk", (o)->dknown); F1("bk", (o)->bknown);  \
    F1("rk", (o)->rknown); F1("ck", (o)->cknown); F1("lk", (o)->lknown); FB("er", (o)->oeroded, 2);                    \
    FB("er2", (o)->oeroded2, 2); F1("erp", (o)->oerodeproof); F1("gr", (o)->greased); F1("lit", (o)->lamplit);         \
    FB("rech", (o)->recharged, 3); F1("trap", (o)->otrapped); F1("nom", (o)->nomerge); F1("uncl", (o)->was_thrown);    \
    F1("inuse", (o)->in_use); F1("ubill", (o)->unpaid); F1("nx", (o)->no_charge);                                      \
    F("q", (o)->quan); F("worn", (long) (o)->owornmask); F("age", (o)->age); F32("eat", (o)->oeaten);                  \
    F32("cnm", (o)->corpsenm)

/* a monster's own fields (name, extensions and inventory beside it) */
#define PHD_MON_FIELDS(m)                                                                                              \
    F32("id", (m)->m_id); F16("pm", monsndx((m)->data)); F16("mnum", (m)->mnum);                                       \
    F32("hp", (m)->mhp); F32("hpmax", (m)->mhpmax);                                                                    \
    F8("x", (m)->mx); F8("y", (m)->my); F8("lev", (m)->m_lev); F8("tm", (m)->mtame); F8("fz", (m)->mfrozen);           \
    F8("flt", (m)->mfleetim); F8("bl", (m)->mblinded); F8("ux", (m)->mux);                                             \
    F8("uy", (m)->muy); F8("wrm", (m)->wormno); F8("apt", (m)->m_ap_type); F8("wchk", (m)->weapon_check);              \
    F8("mal", (m)->malign); FB("spd", (m)->mspeed, 2); FB("pspd", (m)->permspeed, 2);                                  \
    F1("pf", (m)->mpeaceful); F1("sl", (m)->msleeping); F1("cm", (m)->mcanmove); F1("fl", (m)->mflee);                 \
    F1("cs", (m)->mcansee); F1("inv", (m)->minvis); F1("invb", (m)->invis_blkd); F1("pinv", (m)->perminvis);           \
    F1("trp", (m)->mtrapped); F1("und", (m)->mundetected); F1("can", (m)->mcan); F1("rev", (m)->mrevived);             \
    F1("clo", (m)->mcloned); F1("av", (m)->mavenge); F1("wiz", (m)->iswiz); F1("conf", (m)->mconf);                    \
    F1("stun", (m)->mstun); F1("lsh", (m)->mleashed); F1("shk", (m)->isshk); F1("min", (m)->isminion);                 \
    F1("gd", (m)->isgd); F1("pri", (m)->ispriest); F1("fem", (m)->female);                                             \
    F16("mv", (m)->movement); F16("cham", (m)->cham); F32("msu", (m)->mspec_used);                                     \
    F("str", (unsigned long) (m)->mstrategy); F("wc", (unsigned long) (m)->misc_worn_check);                           \
    F32("trs", (unsigned long) (m)->mtrapseen & 0xffffffffUL); /* fork: 32-bit field; TRAPNUM < 32 */                  \
    F32("app", (m)->mappearance); F32("eat", (m)->meating); F16("mext", (unsigned) (m)->mextrinsics)

#if !PHD_TEXT
/* a record's hash from its packed fields; with a workspace, cached by the record's memory */
static uint64_t phd_orec(struct phd_ws *ws, const struct obj *o)
{
    struct phd_t rt, *t = &rt;
    unsigned s = 0;
    PK_DECL
    if (ws) {
        s = phd_slot(o, PHD_OSLOTS);
        if (ws->os[s].p == o && phd_same(ws->os[s].raw, o, sizeof *o))
            return ws->os[s].h;
    }
    phd_sec_begin(t, 0x0b);
    PHD_OBJ_FIELDS(o);
    END();
    uint64_t h = phd_sec_value(t);
    if (ws) {
        ws->os[s].p = o;
        memcpy(ws->os[s].raw, o, sizeof *o);
        ws->os[s].h = h;
    }
    return h;
}
static uint64_t phd_mrec(struct phd_ws *ws, const struct monst *m)
{
    struct phd_t rt, *t = &rt;
    unsigned s = 0;
    PK_DECL
    if (ws) {
        s = phd_slot(m, PHD_MSLOTS);
        if (ws->ms[s].p == m && phd_same(ws->ms[s].raw, m, sizeof *m))
            return ws->ms[s].h;
    }
    phd_sec_begin(t, 0x3e);
    PHD_MON_FIELDS(m);
    END();
    uint64_t h = phd_sec_value(t);
    if (ws) {
        ws->ms[s].p = m;
        memcpy(ws->ms[s].raw, m, sizeof *m);
        ws->ms[s].h = h;
    }
    return h;
}
#endif

static void PHD_FN(phd_objs)(struct phd_t *t, const char *tag, struct obj *o, int depth)
{
    PK_DECL
    for (; o; o = o->nobj) {
        RECI(tag, depth);
#if PHD_TEXT
        PHD_OBJ_FIELDS(o);
#else
        PK_FLUSH();
        phd_put(t, phd_orec(t->ws, o));
#endif
#if PHD_TEXT
        FS("named", has_oname(o) ? ONAME(o) : "-");
#else
        phd_put(t, phd_name(has_oname(o) ? ONAME(o) : NULL));
#endif
        END();
        if (Has_contents(o))
            PHD_FN(phd_objs)(t, tag, o->cobj, depth + 1);
    }
}

static void PHD_FN(phd_mons)(struct phd_t *t, const char *tag, struct monst *m)
{
    PK_DECL
    for (; m; m = m->nmon) {
        REC(tag);
#if PHD_TEXT
        PHD_MON_FIELDS(m);
#else
        PK_FLUSH();
        phd_put(t, phd_mrec(t->ws, m));
#endif
#if PHD_TEXT
        FS("named", has_mname(m) ? MNAME(m) : "-");
#else
        phd_put(t, phd_name(has_mname(m) ? MNAME(m) : NULL));
#endif
        END();
        if (has_edog(m)) {
            struct edog *e = EDOG(m);
            REC("ED");
            F32("id", m->m_id); F32("dropd", e->dropdist); F("drop", e->droptime); F32("app", e->apport);
            F32("ab", e->abuse); F("wh", e->whistletime); F("hu", e->hungrytime); F8("gx", e->ogoal.x); F8("gy", e->ogoal.y);
            F32("rev", e->revivals); F32("pen", e->mhpmax_penalty); F1("ku", e->killed_by_u);
            END();
        }
        if (has_eshk(m)) {
            struct eshk *e = ESHK(m);
            REC("ES");
            F32("id", m->m_id); F32("bill", e->billct); F("deb", e->debit); F("cred", e->credit); F("rob", e->robbed);
            F("loan", e->loan); F8("sx", e->shk.x); F8("sy", e->shk.y); F8("dx", e->shd.x); F8("dy", e->shd.y);
            F8("room", e->shoproom); F8("fol", e->following); F8("sur", e->surcharge); F8("kops", e->dismiss_kops);
            F32("vis", e->visitct); FS("cust", e->customer);
            END();
            for (int i = 0; i < e->billct; i++) {
                REC("BI");
                F32("id", m->m_id); F32("bo", e->bill_p[i].bo_id); F("use", e->bill_p[i].useup); F("pr", e->bill_p[i].price);
                F("bq", e->bill_p[i].bquan);
                END();
            }
        }
        if (has_epri(m)) {
            REC("EP");
            F32("id", m->m_id); F8("room", EPRI(m)->shroom); F8("al", EPRI(m)->shralign); F8("x", EPRI(m)->shrpos.x);
            F8("y", EPRI(m)->shrpos.y);
            END();
        }
        if (has_emin(m)) {
            REC("EM");
            F32("id", m->m_id); F8("al", EMIN(m)->min_align); F8("ren", EMIN(m)->renegade);
            END();
        }
        if (has_egd(m)) {
            struct egd *e = EGD(m);
            REC("EG");
            F32("id", m->m_id); F32("fcb", e->fcbeg); F32("fce", e->fcend); F8("room", e->vroom); F8("gx", e->gdx);
            F8("gy", e->gdy); F8("ogx", e->ogx); F8("ogy", e->ogy); F8("warn", e->warncnt); F8("done", e->gddone);
            F8("wit", e->witness);
            END();
        }
        if (m->minvent) {
            REC("MI");
            F32("id", m->m_id);
            END();
            PHD_FN(phd_objs)(t, "MO", m->minvent, 0);
        }
    }
}

#if !PHD_TEXT
/* one map cell's three words (typ, seenv, doormask, roomno, the five flag bits and lastseentyp; glyph and the top object's
   id; the monster's id); a column hashes its 21 cells' 63 words, the section the 80 column values */
static inline uint64_t phd_cell(int x, int y, uint64_t *w)
{
    struct rm *c = &PHD_CELL(x, y);
    struct obj *top = PHD_OBJ_AT(x, y);
    struct monst *mm = PHD_MON_AT(x, y);
    unsigned oid = top ? top->o_id : 0U, mid = mm ? mm->m_id : 0U;
    w[0] = (uint64_t) (unsigned char) c->typ | (uint64_t) (unsigned char) c->seenv << 8
           | (uint64_t) (unsigned) c->doormask << 16 | (uint64_t) (unsigned) c->roomno << 32
           | (uint64_t) ((unsigned) c->horizontal | (unsigned) c->lit << 1 | (unsigned) c->waslit << 2
                         | (unsigned) c->edge << 3 | (unsigned) c->candig << 4) << 40
           | (uint64_t) (unsigned char) lastseentyp[x][y] << 48;
    w[1] = (uint64_t) (uint32_t) c->glyph | (uint64_t) oid << 32;
    w[2] = (uint64_t) mid;
    return (uint64_t) oid << 32 | mid;
}
/* cells whose memory differs from the copy (es bytes per cell, the arrays [COLNO][ROWNO]): compared 32 bytes at a time, a
   differing block marking the cells it overlaps */
static inline int phd_celldiff(const void *a, const void *b, size_t es, unsigned char *dirty)
{
    const unsigned char *pa = (const unsigned char *) a, *pb = (const unsigned char *) b;
    size_t n = es * COLNO * ROWNO, i;
    int any = 0;
#ifdef __AVX2__
    __m256i acc0 = _mm256_setzero_si256(), acc1 = _mm256_setzero_si256();
    for (i = 0; i + 64 <= n; i += 64) {
        acc0 = _mm256_or_si256(acc0, _mm256_xor_si256(_mm256_loadu_si256((const __m256i *) (pa + i)),
                                                      _mm256_loadu_si256((const __m256i *) (pb + i))));
        acc1 = _mm256_or_si256(acc1, _mm256_xor_si256(_mm256_loadu_si256((const __m256i *) (pa + i + 32)),
                                                      _mm256_loadu_si256((const __m256i *) (pb + i + 32))));
    }
    acc0 = _mm256_or_si256(acc0, acc1);
    if (_mm256_testz_si256(acc0, acc0) && phd_same(pa + i, pb + i, n - i))
        return 0; /* nothing differs */
#endif
    for (i = 0; i < n; i += 32) {
        size_t m = n - i < 32 ? n - i : 32;
#ifdef __AVX2__
        int differs;
        if (m == 32) {
            __m256i x = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *) (pa + i)),
                                         _mm256_loadu_si256((const __m256i *) (pb + i)));
            differs = !_mm256_testz_si256(x, x);
        } else
            differs = !phd_same(pa + i, pb + i, m);
        if (differs) {
#else
        if (!phd_same(pa + i, pb + i, m)) {
#endif
            for (size_t c = i / es; c <= (i + m - 1) / es; c++)
                dirty[c] = 1;
            any = 1;
        }
    }
    return any;
}
static void phd_cells(struct phd_t *t)
{
    struct phd_ws *ws = t->ws;
    uint64_t w[3 * ROWNO];
    unsigned char dirty[COLNO * ROWNO], cold[COLNO];
    int x, y;
    if (!ws) {
        for (x = 0; x < COLNO; x++) {
            for (y = 0; y < ROWNO; y++)
                (void) phd_cell(x, y, w + 3 * y);
            phd_put(t, phd_words(w, 3 * ROWNO, 0xce11ULL));
        }
        return;
    }
    memset(dirty, !ws->cells.ok, sizeof dirty);
    if (ws->cells.ok) {
        phd_celldiff(ws->cells.loc, PHD_LOC_ARR, sizeof(struct rm), dirty);
        phd_celldiff(ws->cells.obj, PHD_OBJ_ARR, sizeof(void *), dirty);
#ifdef PHD_MON_ARR
        phd_celldiff(ws->cells.mon, PHD_MON_ARR, sizeof(void *), dirty);
#endif
        phd_celldiff(ws->cells.lst, &lastseentyp[0][0], 1, dirty);
        for (x = 0; x < COLNO; x++) /* the same pointers may name new objects or monsters */
            for (uint32_t b = ws->cells.nptr[x]; b; b &= b - 1) {
                y = __builtin_ctz(b);
                struct obj *top = PHD_OBJ_AT(x, y);
                struct monst *mm = PHD_MON_AT(x, y);
                if (ws->cells.ids[x][y] != ((uint64_t) (top ? top->o_id : 0U) << 32 | (mm ? mm->m_id : 0U)))
                    dirty[x * ROWNO + y] = 1;
            }
    }
    memset(cold, 0, sizeof cold);
    for (int c = 0; c < COLNO * ROWNO; c++) {
        if ((c & 7) == 0) { /* eight clean cells at once */
            uint64_t d8;
            memcpy(&d8, dirty + c, 8);
            if (!d8) {
                c += 7;
                continue;
            }
        }
        if (dirty[c]) {
            x = c / ROWNO, y = c % ROWNO;
            uint64_t id = phd_cell(x, y, ws->cells.w[x] + 3 * y);
            ws->cells.ids[x][y] = id;
            ws->cells.nptr[x] = (ws->cells.nptr[x] & ~(1U << y)) | (uint32_t) (id != 0) << y;
            cold[x] = 1;
            ((struct rm *) ws->cells.loc)[c] = ((const struct rm *) PHD_LOC_ARR)[c];
            ws->cells.obj[c] = ((void *const *) PHD_OBJ_ARR)[c];
#ifdef PHD_MON_ARR
            ws->cells.mon[c] = ((void *const *) PHD_MON_ARR)[c];
#endif
            ws->cells.lst[c] = (&lastseentyp[0][0])[c];
        }
    }
    for (x = 0; x < COLNO; x++) {
        if (cold[x])
            ws->cells.h[x] = phd_words(ws->cells.w[x], 3 * ROWNO, 0xce11ULL);
        phd_put(t, ws->cells.h[x]);
    }
    ws->cells.ok = 1;
}
#endif

static void PHD_FN(phd_sections)(struct phd_t *t, const struct phd_statics *st)
{
    int x, y, i;
    PK_DECL

    SEC(SDS_U);
    REC("u0");
    F("moves", moves); F("mmoves", monstermoves); F32("multi", multi);
    END();
    REC("u1");
    F8("x", u.ux); F8("y", u.uy); F8("x0", u.ux0); F8("y0", u.uy0); F8("dn", u.uz.dnum); F8("dl", u.uz.dlevel);
    F8("dn0", u.uz0.dnum); F8("dl0", u.uz0.dlevel);
    F32("hp", u.uhp); F32("hpmax", u.uhpmax); F32("en", u.uen); F32("enmax", u.uenmax);
    F32("lev", u.ulevel); F32("levmax", u.ulevelmax); F8("luck", u.uluck); F8("mluck", u.moreluck); F8("al", u.ualign.type);
    F8("ab0", u.ualignbase[0]); F8("ab1", u.ualignbase[1]); F32("hs", u.uhs);
    F("exp", u.uexp); F("rexp", u.urexp); F32("hunger", u.uhunger); F("alr", u.ualign.record);
    F32("gang", u.ugangr); F32("gifts", u.ugifts); F32("bless", u.ublessed); F32("blcnt", u.ublesscnt);
    F32("inv", u.uinvulnerable); F32("mon", u.umonnum); F32("monst", u.umonster); F32("mh", u.mh); F32("mhmax", u.mhmax);
    F32("mtd", u.mtimedone); F32("lycn", u.ulycn); F32("sprot", u.uspellprot); F32("sptime", u.usptime);
    F32("spmtime", u.uspmtime); F32("steed", u.usteed ? u.usteed->m_id : 0U); F32("stuck", u.ustuck ? u.ustuck->m_id : 0U);
    F8("swl", u.uswallow); F32("swlt", u.uswldtim); F32("cream", u.ucreamed); F8("inw", u.uinwater); F8("und", u.uundetected);
    F32("grave", u.ugrave_arise); F32("trapt", u.utraptype); F32("mort", u.umortality); F32("trap", u.utrap);
    F("trs", (unsigned long) u.utrap);
    F("c0", u.uconduct.unvegetarian); F("c1", u.uconduct.unvegan); F("c2", u.uconduct.food); F("c3", u.uconduct.gnostic);
    F("c4", u.uconduct.weaphit); F("c5", PHD_KILLC); F("c6", u.uconduct.literate); F("c7", u.uconduct.polypiles);
    F("c8", u.uconduct.polyselfs); F("c9", u.uconduct.wishes); F("c10", u.uconduct.wisharti); F1("h0", u.uhave.amulet);
    F1("h1", u.uhave.bell); F1("h2", u.uhave.book); F1("h3", u.uhave.menorah); F1("h4", u.uhave.questart);
    END();
    REC("ev");
    F1("e0", u.uevent.minor_oracle); F1("e1", u.uevent.major_oracle); F1("e2", u.uevent.read_tribute);
    FB("e3", u.uevent.qcalled, 2); F1("e4", u.uevent.qexpelled); F1("e5", u.uevent.qcompleted);
    FB("e6", u.uevent.uheard_tune, 2); F1("e7", u.uevent.uopened_dbridge); F1("e8", u.uevent.invoked);
    F1("e9", u.uevent.gehennom_entered); F1("e10", u.uevent.udemigod); F1("e11", u.uevent.uvibrated);
    F1("e12", u.uevent.ascended); FB("e13", u.uevent.uhand_of_elbereth, 2); F1("a0", u.uachieve.amulet);
    F1("a1", u.uachieve.bell); F1("a2", u.uachieve.book); F1("a3", u.uachieve.menorah); F1("a4", u.uachieve.enter_gehennom);
    F1("a5", u.uachieve.ascended); F1("a6", u.uachieve.mines_luckstone); F1("a7", u.uachieve.finish_sokoban);
    F1("a8", u.uachieve.killed_medusa); F8("rpb", u.uroleplay.blind); F8("rpn", u.uroleplay.nudist);
    END();
    REC("wo");
    F32("wep", uwep ? uwep->o_id : 0U); F32("swap", uswapwep ? uswapwep->o_id : 0U); F32("quiv", uquiver ? uquiver->o_id : 0U);
    F32("arm", uarm ? uarm->o_id : 0U); F32("armc", uarmc ? uarmc->o_id : 0U); F32("armh", uarmh ? uarmh->o_id : 0U);
    F32("arms", uarms ? uarms->o_id : 0U); F32("armg", uarmg ? uarmg->o_id : 0U); F32("armf", uarmf ? uarmf->o_id : 0U);
    F32("armu", uarmu ? uarmu->o_id : 0U); F32("amul", uamul ? uamul->o_id : 0U); F32("left", uleft ? uleft->o_id : 0U);
    F32("right", uright ? uright->o_id : 0U); F32("blindf", ublindf ? ublindf->o_id : 0U);
    F32("ball", uball ? uball->o_id : 0U); F8("twoweap", u.twoweap); F8("unweapon", unweapon);
    END();
    SECEND();

    SEC(SDS_ATTR);
    for (i = 0; i < A_MAX; i++) {
        RECI("a", i);
        F16("cur", ACURR(i)); F8("acurr", u.acurr.a[i]); F8("abon", u.abon.a[i]); F8("amax", u.amax.a[i]);
        F8("atemp", u.atemp.a[i]); F8("atime", u.atime.a[i]); F8("ex", u.aexe.a[i]);
        END();
    }
    SECEND();

    SEC(SDS_PROP);
    for (i = 1; i <= LAST_PROP; i++)
        if (u.uprops[i].intrinsic || u.uprops[i].extrinsic || u.uprops[i].blocked) {
            RECI("p", i);
            F("in", u.uprops[i].intrinsic); F("ex", u.uprops[i].extrinsic); F("bl", u.uprops[i].blocked);
            END();
        }
    SECEND();

    SEC(SDS_SKILL);
    for (i = 0; i < P_NUM_SKILLS; i++) {
        RECI("s", i);
        F8("sk", u.weapon_skills[i].skill); F8("max", u.weapon_skills[i].max_skill); F16("adv", u.weapon_skills[i].advance);
        END();
    }
    REC("ad");
    F32("adv", u.skills_advanced);
    END();
    SECEND();

    SEC(SDS_SPELL);
    for (i = 0; i < MAXSPELL && spl_book[i].sp_id != NO_SPELL; i++) {
        RECI("s", i);
        F16("id", spl_book[i].sp_id); F8("lev", spl_book[i].sp_lev); F32("know", spl_book[i].sp_know);
        END();
    }
    SECEND();

    SEC(SDS_INV);
    PHD_FN(phd_objs)(t, "I", invent, 0);
    SECEND();
    SEC(SDS_MON);
    PHD_FN(phd_mons)(t, "M", fmon);
    SECEND();
    SEC(SDS_FLOOR);
    PHD_FN(phd_objs)(t, "F", fobj, 0);
    SECEND();
    SEC(SDS_BURIED);
    PHD_FN(phd_objs)(t, "B", level.buriedobjlist, 0);
    SECEND();
    SEC(SDS_MIGR);
    PHD_FN(phd_objs)(t, "MO", migrating_objs, 0);
    PHD_FN(phd_objs)(t, "BO", billobjs, 0);
    PHD_FN(phd_mons)(t, "MM", migrating_mons);
    SECEND();

    SEC(SDS_TRAP);
    for (struct trap *tr = ftrap; tr; tr = tr->ntrap) {
        REC("t");
        F8("x", tr->tx); F8("y", tr->ty); F8("typ", tr->ttyp); F1("seen", tr->tseen); F1("once", tr->once);
        F1("byu", tr->madeby_u); F8("dn", tr->dst.dnum); F8("dl", tr->dst.dlevel);
        END();
    }
    SECEND();

    SEC(SDS_ENGR);
    for (struct engr *e = *st->head_engr; e; e = e->nxt_engr) {
        REC("e");
        F8("x", e->engr_x); F8("y", e->engr_y); F8("typ", e->engr_type); F("time", e->engr_time); FS("txt", e->engr_txt);
        END();
    }
    SECEND();

    SEC(SDS_TIMER);
    for (timer_element *tm = *st->timer_base; tm; tm = tm->next) {
        long arg = tm->kind == TIMER_OBJECT ? (long) tm->arg.a_obj->o_id
                   : tm->kind == TIMER_MONSTER ? (long) tm->arg.a_monst->m_id : tm->arg.a_long;
        REC("t");
        F("t", tm->timeout); F("id", (unsigned long) tm->tid); F16("kind", tm->kind); F16("fn", tm->func_index); F("arg", arg);
        END();
    }
    SECEND();

    SEC(SDS_LIGHT);
    for (light_source *l = *st->light_base; l; l = l->next) {
        long id = l->type == LS_OBJECT ? (long) l->id.a_obj->o_id
                  : l->type == LS_MONSTER ? (long) l->id.a_monst->m_id : l->id.a_long;
        REC("l");
        F8("x", l->x); F8("y", l->y); F16("r", l->range); F16("f", PHD_LS_FLAGS(l)); F16("typ", l->type); F("id", id);
        END();
    }
    SECEND();

    SEC(SDS_REGION);
    for (i = 0; i < *st->n_regions; i++) {
        NhRegion *r = (*st->regions)[i];
        RECI("r", i);
        F8("lx", r->bounding_box.lx); F8("ly", r->bounding_box.ly); F8("hx", r->bounding_box.hx); F8("hy", r->bounding_box.hy);
        F16("nr", r->nrects); F("ttl", r->ttl); F8("u", r->attach_2_u); F32("m", (unsigned) r->attach_2_m);
        F16("in", r->inside_f); F16("leave", r->leave_f); F16("exp", r->expire_f); F8("vis", r->visible); F32("gl", r->glyph);
        F("arg", (long) r->arg.a_int); F32("pl", r->player_flags);
        END();
        for (int j = 0; j < r->nrects; j++) {
            RECI("rr", i);
            F8("lx", r->rects[j].lx); F8("ly", r->rects[j].ly); F8("hx", r->rects[j].hx); F8("hy", r->rects[j].hy);
            END();
        }
        for (int j = 0; j < r->n_monst; j++) {
            RECI("rm", i);
            F32("m", r->monsters[j]);
            END();
        }
    }
    SECEND();

    SEC(SDS_CELL);
#if PHD_TEXT
    for (x = 0; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            struct rm *c = &PHD_CELL(x, y);
            struct obj *top = PHD_OBJ_AT(x, y);
            struct monst *mm = PHD_MON_AT(x, y);
            fprintf(t->f, "cell %d,%d typ=%d gl=%d sv=%d fl=%d hz=%d lit=%d wl=%d rn=%d ed=%d cd=%d lst=%d o=%u m=%u\n", x, y,
                    (int) c->typ, (int) c->glyph, (int) c->seenv, (int) c->doormask, (int) c->horizontal, (int) c->lit,
                    (int) c->waslit, (int) c->roomno, (int) c->edge, (int) c->candig, (int) lastseentyp[x][y],
                    top ? top->o_id : 0U, mm ? mm->m_id : 0U);
        }
#else
    phd_cells(t);
#endif
    SECEND();

    SEC(SDS_VIS);
    if (viz_array) {
#if PHD_TEXT
        for (y = 0; y < ROWNO; y++) {
            fprintf(t->f, "vis %d ", y);
            for (x = 0; x < COLNO; x++)
                fprintf(t->f, "%02x", (unsigned char) viz_array[y][x]);
            fputc('\n', t->f);
        }
#else
        int same = t->ws && t->ws->vis.ok;
        for (y = 0; y < ROWNO && same; y++)
            same = phd_same(t->ws->vis.raw + y * COLNO, viz_array[y], COLNO);
        if (same) {
            t->out[t->sec] = t->ws->vis.h;
            goto vis_done;
        }
        for (y = 0; y < ROWNO; y++) {
            uint64_t w[COLNO / 8];
            memcpy(w, viz_array[y], sizeof w);
            for (x = 0; x < COLNO / 8; x++)
                phd_put(t, w[x]);
            if (t->ws)
                memcpy(t->ws->vis.raw + y * COLNO, viz_array[y], COLNO);
        }
#endif
    }
    SECEND();
#if !PHD_TEXT
    if (t->ws && viz_array)
        t->ws->vis.h = t->out[t->sec], t->ws->vis.ok = 1;
vis_done:
#endif

    SEC(SDS_ROOM);
    for (i = 0; i < nroom; i++) {
        RECI("r", i);
        F8("lx", rooms[i].lx); F8("ly", rooms[i].ly); F8("hx", rooms[i].hx); F8("hy", rooms[i].hy); F8("typ", rooms[i].rtype);
        F8("lit", rooms[i].rlit); F8("fdoor", rooms[i].fdoor); F8("doors", rooms[i].doorct); F8("sub", rooms[i].nsubrooms);
        F8("irr", rooms[i].irregular);
        END();
    }
    for (i = 0; i < nsubroom; i++) {
        RECI("s", i);
        F8("lx", subrooms[i].lx); F8("ly", subrooms[i].ly); F8("hx", subrooms[i].hx); F8("hy", subrooms[i].hy);
        F8("typ", subrooms[i].rtype); F8("lit", subrooms[i].rlit); F8("fdoor", subrooms[i].fdoor);
        F8("doors", subrooms[i].doorct);
        END();
    }
    for (i = 0; i < doorindex; i++) {
        RECI("d", i);
        F8("x", doors[i].x); F8("y", doors[i].y);
        END();
    }
    SECEND();

    SEC(SDS_OC);
    {
        const struct objclass *oc = objects;
#if PHD_TEXT
        for (i = 0; i < NUM_OBJECTS; i++)
            fprintf(t->f, "oc %d nk=%d d=%d pd=%d %s\n", i, (int) oc[i].oc_name_known, (int) oc[i].oc_descr_idx,
                    (int) oc[i].oc_pre_discovered, oc[i].oc_uname ? oc[i].oc_uname : "-");
#else
        /* the copy decides only while no type has a called name (a name's text can change under the same pointer) */
        int named = 0, same = t->ws && phd_cache_hit(&t->ws->oc.ok, t->ws->oc.raw, oc, sizeof t->ws->oc.raw);
        if (same) {
            t->out[t->sec] = t->ws->oc.h;
            goto oc_done;
        }
        for (i = 0; i < NUM_OBJECTS; i++) {
            phd_put(t, (uint64_t) (unsigned) oc[i].oc_name_known | (uint64_t) (unsigned short) oc[i].oc_descr_idx << 8
                           | (uint64_t) (unsigned) oc[i].oc_pre_discovered << 24 | (uint64_t) (oc[i].oc_uname != NULL) << 32);
            if (oc[i].oc_uname)
                phd_putstr(t, oc[i].oc_uname), named = 1;
        }
        t->out[t->sec] = phd_sec_value(t);
        if (t->ws) {
            memcpy(t->ws->oc.raw, oc, sizeof t->ws->oc.raw);
            t->ws->oc.h = t->out[t->sec], t->ws->oc.ok = !named;
        }
        goto oc_done;
#endif
    }
    SECEND();
#if !PHD_TEXT
oc_done:
#endif

    SEC(SDS_DISCO);
    REC("d");
    for (i = 0; i < NUM_OBJECTS && st->disco[i]; i++)
        F16("o", st->disco[i]);
    END();
    REC("a");
    for (i = 0; i <= NROFARTIFACTS; i++)
        F8("x", st->artiexist[i]);
    END();
    SECEND();

    SEC(SDS_CTX);
    REC("c");
    F32("ident", context.ident); F32("wiz", context.no_of_wizards); F8("run", context.run); F8("trav", context.travel);
    F8("trav1", context.travel1); F8("nopick", context.nopick); F8("ff", context.forcefight); F8("move", context.move);
    F8("mv", context.mv); F8("byp", context.bypasses); F32("warn", context.warnlevel); F32("rnd", context.rndencode);
    F("steth", context.stethoscope_move); F8("trib", context.tribute.enabled); F8("tribb", context.tribute.bookstock);
    F8("tribd", context.tribute.Deathnotice); F("wto", (unsigned long) context.warntype.obj);
    F("wtp", (unsigned long) context.warntype.polyd);
    END();
    SECEND();

    SEC(SDS_LFLAGS);
    REC("f");
    F8("ft", PHD_LFL.nfountains); F8("sk", PHD_LFL.nsinks); F1("shop", PHD_LFL.has_shop); F1("vault", PHD_LFL.has_vault);
    F1("zoo", PHD_LFL.has_zoo); F1("court", PHD_LFL.has_court); F1("morgue", PHD_LFL.has_morgue);
    F1("bee", PHD_LFL.has_beehive); F1("bar", PHD_LFL.has_barracks); F1("temple", PHD_LFL.has_temple);
    F1("swamp", PHD_LFL.has_swamp); F1("nt", PHD_LFL.noteleport); F1("hf", PHD_LFL.hardfloor); F1("nm", PHD_LFL.nommap);
    F1("hm", PHD_LFL.hero_memory); F1("ss", PHD_LFL.shortsighted); F1("gy", PHD_LFL.graveyard);
    F1("sok", PHD_LFL.sokoban_rules); F1("maze", PHD_LFL.is_maze_lev); F1("cav", PHD_LFL.is_cavernous_lev);
    F1("arb", PHD_LFL.arboreal); F1("cm", PHD_LFL.corrmaze);
    END();
    SECEND();

    SEC(SDS_MV);
#if !PHD_TEXT
    {
        unsigned char raw[NUMMONS * 3];
        for (i = 0; i < NUMMONS; i++)
            raw[3 * i] = mvitals[i].born, raw[3 * i + 1] = mvitals[i].died, raw[3 * i + 2] = mvitals[i].mvflags;
        if (t->ws && phd_cache_hit(&t->ws->mv.ok, t->ws->mv.raw, raw, sizeof raw)) {
            t->out[t->sec] = t->ws->mv.h;
            goto mv_done;
        }
        if (t->ws)
            memcpy(t->ws->mv.raw, raw, sizeof raw);
    }
#endif
    for (i = LOW_PM; i < NUMMONS; i++)
        if (mvitals[i].born || mvitals[i].died || mvitals[i].mvflags) {
            RECI("m", i);
            F8("born", mvitals[i].born); F8("died", mvitals[i].died); F8("fl", mvitals[i].mvflags);
            END();
        }
    SECEND();
#if !PHD_TEXT
    if (t->ws)
        t->ws->mv.h = t->out[t->sec], t->ws->mv.ok = 1;
mv_done:
#endif

    SEC(SDS_LINFO);
#if !PHD_TEXT
    {
        unsigned char raw[MAXLINFO];
        for (i = 0; i < MAXLINFO; i++)
            raw[i] = level_info[i].PHD_LINFO_FL;
        if (t->ws && phd_cache_hit(&t->ws->li.ok, t->ws->li.raw, raw, sizeof raw)) {
            t->out[t->sec] = t->ws->li.h;
            goto li_done;
        }
        if (t->ws)
            memcpy(t->ws->li.raw, raw, sizeof raw);
    }
#endif
    for (i = 0; i < MAXLINFO; i++)
        if (level_info[i].PHD_LINFO_FL) {
            RECI("l", i);
            F8("fl", level_info[i].PHD_LINFO_FL);
            END();
        }
    SECEND();
#if !PHD_TEXT
    if (t->ws)
        t->ws->li.h = t->out[t->sec], t->ws->li.ok = 1;
li_done:
    ;
#endif
}
#undef PHD_FN
#undef SEC
#undef SECEND
#undef REC
#undef RECI
#undef F
#undef F1
#undef F8
#undef F16
#undef F32
#undef FB
#undef FS
#undef END
#undef PK_DECL
#undef PHD_OBJ_FIELDS
#undef PHD_MON_FIELDS
#ifdef PK_FLUSH
#undef PK_FLUSH
#endif
#endif /* PHD_BODY */
