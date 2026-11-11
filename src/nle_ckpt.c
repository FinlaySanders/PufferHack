/* Game checkpoints at the top-level command prompt.
 *
 * A checkpoint is the context struct (every per-game global), the lazy slots that hold game
 * state, and the heap NetHack's own save code serializes (current level, game state, other
 * levels). Pointers in the struct and slots are stored by kind: into the struct or a copied slot
 * (offset), into the library (offset from its base), or into the heap (dropped; NetHack's restore
 * rebuilds those, and the options/window-port fields in keep_fields keep the target's own). So a
 * checkpoint loads into any env, in this process or another. Layout tables are generated into
 * nh_ckpt_layout.c by util/gen_ckpt_layout.py. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>

#include "hack.h"
#include "lev.h"
#include "timeout.h"
#include "nle.h"
#include "nh_ckpt.h"
#include "wintty.h"
#include "tmt.h"

#define CKPT_MAGIC 0x4b43484eU
#define CKPT_VERSION 17
#define SCRATCH MAXLINFO

enum { P_NULL, P_CTX, P_LIB, P_SLOT, P_HEAP };

struct ckpt_hdr {
    unsigned magic, version, ctx_size, nptr, nslot, pad;
    int ledger, underfoot;
    unsigned long time_seed;
    int time_seed_is_set, fix_moon_phase;
    uint32_t options; /* nle_options_id(): a load into a game started with other options is refused */
    long rl_blstats[NLE_BLSTATS_SIZE], rl_cond, rl_status_sig[NLE_RL_STATUS_SIG];
    struct nle_rl_inv_ckpt rl_inv;
    int strs_gap; /* the saved game's names were not kept (nh_obs_cache strs_gap) */
    int toplin; /* the message line's state (the target keeps its tty structures; the text, toplines, travels in the image) */
    unsigned xlock_box; /* o_id of the box an interrupted lock-picking attempt resumes on (the image's pointer is a heap pointer) */
    /* the screen as the saved game has it (the target keeps its window structures); terminal and descriptions follow the record */
    short tty_cur[2], win_cur[MAXWIN][2];
    int tty_color, tty_rawprint;
    unsigned char win_set[MAXWIN], rl_ok, rl_stale, rl_map[3][(COLNO - 1) * ROWNO];
    struct nle_status_ckpt status;
#ifdef STATUS_HILITES
    struct nle_tty_status_ckpt tty_status;
#endif
};

/* options, files and window-port state: the target env keeps its own heap pointers here */
static const char *const keep_fields[] = {
    "g_iflags", "g_apelist", "g_menu_colorings", "g_tc_gbl_data", "g_fqn_prefix", "g_plinemsg_types",
    "g_Cmd", "g_botl_c_status_hilite_str_p", "g_windows_c_last_winchoice", "g_windows_c_status_fieldfmt",
    "g_windows_c_status_fieldnm", "g_windows_c_status_vals", "g_wintty_c_tty_colormasks", "g_wintty_c_wins",
    "g_wintty_c_ttydisplay", "g_files_c_config_section_chosen", "g_files_c_config_section_current",
    "g_save_c_bw_file", "g_questpgr_c_msg_file_p", "g_symset_list", "g_drawing_c_symset_store",
    "g_options_c_opt_intro", "g_mon_c_mon_pool", "g_mkobj_c_obj_pool", "g_hname", "g_cmd_c_readchar_queue",
    "g_topten_c_tt_buf_p",
};

/* window-port and buffer state the target keeps whole (its scalars describe its own heap) */
static const char *const keep_whole_prefix[] = {
    "g_wintty_c_", "g_termcap_c_", "g_topl_c_", "g_getline_c_", "g_windows_c_", "g_botl_c_", "g_tc_", "g_files_c_",
    "g_rumors_c_", "g_dlb_c_", "g_questpgr_c_", "g_options_c_", "g_save_c_", "g_restore_c_",
};
static const char *const keep_whole_names[] = {
    "g_invent_c_invbufsiz", "g_mon_c_animal_list_count", "g_nh_ctx_serial",
};
/* sizes of arrays NetHack's restore allocates itself: keep what the restore set */
static const char *const restored_names[] = {
    "g_region_c_max_regions",
};

extern int nle_rl_ckpt_get(long *, long *, long *, struct nle_rl_inv_ckpt *);
extern struct cmd nh_cmd_shared;
extern void nle_rl_ckpt_set(int, const long *, long, const long *, const struct nle_rl_inv_ckpt *);
extern int nle_rl_ckpt_display(unsigned char **, char **, uint8_t **);
extern void nle_rl_map_resolve(void);
extern void nle_vt_callback(tmt_msg_t, TMT *, const void *, void *);

static uintptr_t lib_base, lib_size;
enum { A_RELOC, A_KEEP, A_SKIP };
static unsigned char *keep_ptr, *keep_whole, *ptr_byte, *ptr_act;
static unsigned *run_off, *run_len, nrun; /* non-pointer bytes outside kept fields, put back after restore */
void nle_tty_reset_msghistory(void);
static NH_THREAD_LOCAL unsigned char *scratch; /* a copy of the target's context during a load */

struct cbuf {
    unsigned char *p;
    size_t n, cap;
};

struct crd {
    const unsigned char *p;
    size_t n, pos;
};

static void
reserve(struct cbuf *b, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1 << 19;

        while (cap < b->n + n)
            cap *= 2;
        b->p = (unsigned char *) realloc(b->p, cap);
        b->cap = cap;
    }
}

static void
put(struct cbuf *b, const void *src, size_t n)
{
    reserve(b, n);
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static const unsigned char *
take(struct crd *r, size_t n)
{
    const unsigned char *p = r->p + r->pos;

    if (n > r->n - r->pos)
        return 0;
    r->pos += n;
    return p;
}

static int
lib_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    int i;

    (void) size, (void) data;
    if (info->dlpi_addr != lib_base)
        return 0;
    for (i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_LOAD
            && info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz > lib_size)
            lib_size = info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz;
    return 1;
}

static int
field_of(unsigned off)
{
    unsigned lo = 0, hi = nh_ckpt_nfield;

    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;

        if (nh_ckpt_field[mid].off <= off)
            lo = mid;
        else
            hi = mid;
    }
    return (int) lo;
}

/* the tables every save and load uses, built once per process */
static void
ckpt_init(void)
{
    unsigned char *k = (unsigned char *) calloc(nh_ckpt_nptr, 1);
    unsigned char *w = (unsigned char *) calloc(nh_ckpt_nfield, 1);
    unsigned i, j;
    Dl_info d;

    dladdr((void *) nle_ckpt_save, &d);
    lib_base = (uintptr_t) d.dli_fbase;
    dl_iterate_phdr(lib_cb, 0);
    for (i = 0; i < nh_ckpt_nfield; i++) {
        for (j = 0; j < sizeof keep_whole_prefix / sizeof *keep_whole_prefix; j++)
            if (!strncmp(nh_ckpt_field[i].name, keep_whole_prefix[j], strlen(keep_whole_prefix[j])))
                w[i] = 1;
        for (j = 0; j < sizeof keep_whole_names / sizeof *keep_whole_names; j++)
            if (!strcmp(nh_ckpt_field[i].name, keep_whole_names[j]))
                w[i] = 1;
    }
    for (i = 0; i < nh_ckpt_nptr; i++)
        for (j = 0; j < sizeof keep_fields / sizeof *keep_fields; j++)
            if (!strcmp(nh_ckpt_field[field_of(nh_ckpt_ptr[i])].name, keep_fields[j]))
                k[i] = 1;
    ptr_byte = (unsigned char *) calloc(nh_ckpt_ctx_size, 1);
    for (i = 0; i < nh_ckpt_nptr; i++)
        memset(ptr_byte + nh_ckpt_ptr[i], 1, sizeof(void *));
    ptr_act = (unsigned char *) calloc(nh_ckpt_nptr, 1);
    for (i = 0; i < nh_ckpt_nptr; i++) {
        int f = field_of(nh_ckpt_ptr[i]);

        ptr_act[i] = w[f] || !strcmp(nh_ckpt_field[f].name, "nh_lazy") ? A_SKIP : k[i] ? A_KEEP : A_RELOC;
    }
    run_off = (unsigned *) malloc(sizeof(unsigned) * nh_ckpt_ctx_size / 2);
    run_len = (unsigned *) malloc(sizeof(unsigned) * nh_ckpt_ctx_size / 2);
    for (i = 0; i < nh_ckpt_nfield; i++) {
        unsigned b, off = nh_ckpt_field[i].off, end = off + nh_ckpt_field[i].size;

        for (j = 0; j < sizeof restored_names / sizeof *restored_names; j++)
            if (!strcmp(nh_ckpt_field[i].name, restored_names[j]))
                break;
        if (w[i] || j < sizeof restored_names / sizeof *restored_names)
            continue;
        for (b = off; b < end; b++) {
            if (ptr_byte[b])
                continue;
            if (nrun && run_off[nrun - 1] + run_len[nrun - 1] == b)
                run_len[nrun - 1]++;
            else
                run_off[nrun] = b, run_len[nrun++] = 1;
        }
    }
    keep_ptr = k, keep_whole = w;
}

static int
ckpt_enter(nle_ctx_t *nle)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    current_nle_ctx = nle;
    nh_cur = (struct nh_ctx *) nle->nh;
    if (nh_ckpt_ctx_size != sizeof(struct nh_ctx))
        return 1; /* nh_ckpt_layout.c is stale: rerun util/gen_ckpt_layout.py */
    pthread_once(&once, ckpt_init);
    return 0;
}

/* the options the game was started with, as initoptions_finish reads them: the NETHACKOPTIONS string (taken by nle_start, as
   parseoptions cuts it at its commas), or the bytes of the file it names ('@', '/' or '\\' first), read at the first checkpoint
   call */
uint32_t
nle_options_id(nle_ctx_t *nle)
{
    const char *o = settings.options;
    uint64_t h = 14695981039346656037ULL;
    FILE *f;
    int c;

    if (nle->options_set)
        return nle->options_id;
    if ((*o == '@' || *o == '/' || *o == '\\') && (f = fopen(o + (*o == '@'), "r"))) {
        while ((c = getc(f)) != EOF)
            h = (h ^ (unsigned char) c) * 1099511628211ULL;
        fclose(f);
    } else
        for (; *o; o++)
            h = (h ^ (unsigned char) *o) * 1099511628211ULL;
    nle->options_set = 1;
    return nle->options_id = (uint32_t) (h ^ h >> 32);
}

/* NetHack's restore prepends to some lists (light sources, engravings), reversing them; a checkpoint
   records every list's order by key and relinks it after the load */
typedef uint64_t (*keyfn)(const void *);

static uint64_t key_mon(const void *p) { return ((const struct monst *) p)->m_id; }
static uint64_t key_obj(const void *p) { return ((const struct obj *) p)->o_id; }
static uint64_t key_timer(const void *p) { return ((const timer_element *) p)->tid; }
static uint64_t key_trap(const void *p) { return ((uint64_t) ((const struct trap *) p)->tx << 8) | (unsigned char) ((const struct trap *) p)->ty; }
static uint64_t key_fruit(const void *p) { return (uint64_t) ((const struct fruit *) p)->fid; }

static uint64_t
key_light(const void *p)
{
    const light_source *ls = (const light_source *) p;
    uint64_t id = ls->type == LS_OBJECT ? ((struct obj *) ls->id.a_obj)->o_id
                  : ls->type == LS_MONSTER ? ((struct monst *) ls->id.a_monst)->m_id : 0;

    return ((uint64_t) ls->type << 56) | id;
}

static uint64_t
key_engr(const void *p)
{
    const struct engr *e = (const struct engr *) p;

    return ((uint64_t) (unsigned char) e->engr_x << 8) | (unsigned char) e->engr_y;
}

static uint64_t
key_damage(const void *p)
{
    const struct damage *d = (const struct damage *) p;

    return ((uint64_t) d->when << 16) | ((uint64_t) (unsigned char) d->place.x << 8) | (unsigned char) d->place.y;
}

struct ckpt_list {
    void **head;
    size_t next_off;
    keyfn key;
};

#define NEXT(p, off) (*(void **) ((char *) (p) + (off)))

static int
ckpt_lists(struct ckpt_list *L)
{
    int n = 0;

#define LIST(h, type, nextf, k) \
    (L[n].head = (void **) &(h), L[n].next_off = offsetof(type, nextf), L[n].key = (k), n++)
    LIST(fmon, struct monst, nmon, key_mon);
    LIST(fobj, struct obj, nobj, key_obj);
    LIST(level.buriedobjlist, struct obj, nobj, key_obj);
    LIST(billobjs, struct obj, nobj, key_obj);
    LIST(invent, struct obj, nobj, key_obj);
    LIST(migrating_objs, struct obj, nobj, key_obj);
    LIST(migrating_mons, struct monst, nmon, key_mon);
    LIST(nh_cur->g_timeout_c_timer_base, timer_element, next, key_timer);
    LIST(nh_cur->g_light_c_light_base, light_source, next, key_light);
    LIST(nh_cur->g_engrave_c_head_engr, struct engr, nxt_engr, key_engr);
    LIST(level.damagelist, struct damage, next, key_damage);
    LIST(ftrap, struct trap, ntrap, key_trap);
    LIST(ffruit, struct fruit, nextf, key_fruit);
#undef LIST
    return n;
}

/* engravings: restore resets engr_time and points engr_txt at the buffer start, past which
   wipe_engr_at may have advanced it */
struct engr_fix {
    uint64_t key;
    long time;
    long txt_off;
};

static void
put_order(struct cbuf *b)
{
    struct ckpt_list L[16];
    int i, nl = ckpt_lists(L), ne = 0;
    struct engr *ep;

    for (i = 0; i < nl; i++) {
        int count = 0;
        void *e;

        for (e = *L[i].head; e; e = NEXT(e, L[i].next_off))
            count++;
        put(b, &count, sizeof count);
        for (e = *L[i].head; e; e = NEXT(e, L[i].next_off)) {
            uint64_t k = L[i].key(e);

            put(b, &k, sizeof k);
        }
    }
    for (ep = (struct engr *) nh_cur->g_engrave_c_head_engr; ep; ep = ep->nxt_engr)
        ne++;
    put(b, &ne, sizeof ne);
    for (ep = (struct engr *) nh_cur->g_engrave_c_head_engr; ep; ep = ep->nxt_engr) {
        struct engr_fix f;

        f.key = key_engr(ep), f.time = ep->engr_time, f.txt_off = ep->engr_txt - (char *) (ep + 1);
        put(b, &f, sizeof f);
    }
    {
        /* makemon's align_shift caches a special level for the turn; it may not be the current one */
        s_level *lev = (s_level *) nh_cur->g_makemon_c_align_shift_lev_p;
        d_level dl = { -1, -1 };

        if (lev)
            dl = lev->dlevel;
        put(b, &dl, sizeof dl);
    }
}

/* relink each list into the recorded order; a list whose members don't match is left alone */
static int
take_order(struct crd *r)
{
    struct ckpt_list L[16];
    int i, j, nl = ckpt_lists(L), bad = 0;

    for (i = 0; i < nl; i++) {
        const int *cp = (const int *) take(r, sizeof(int));
        const uint64_t *keys;
        void **cur, **ord, *e;
        int count, m = 0;

        if (!cp || !(keys = (const uint64_t *) take(r, (size_t) *cp * sizeof(uint64_t))))
            return -1;
        count = *cp;
        for (e = *L[i].head; e; e = NEXT(e, L[i].next_off))
            m++;
        if (m != count) {
            bad++;
            continue;
        }
        if (!count)
            continue;
        cur = (void **) malloc(sizeof(void *) * count);
        ord = (void **) malloc(sizeof(void *) * count);
        for (m = 0, e = *L[i].head; e; e = NEXT(e, L[i].next_off))
            cur[m++] = e;
        for (j = 0; j < count; j++) {
            int k;

            for (k = 0; k < count && (!cur[k] || L[i].key(cur[k]) != keys[j]); k++)
                ;
            if (k == count)
                break;
            ord[j] = cur[k], cur[k] = 0;
        }
        if (j == count) {
            *L[i].head = ord[0];
            for (j = 0; j < count; j++)
                NEXT(ord[j], L[i].next_off) = j + 1 < count ? ord[j + 1] : 0;
        } else {
            bad++;
        }
        free(cur), free(ord);
    }
    {
        const int *np = (const int *) take(r, sizeof(int));
        const struct engr_fix *f;
        struct engr *ep;

        if (!np || !(f = (const struct engr_fix *) take(r, (size_t) *np * sizeof *f)))
            return -1;
        for (i = 0; i < *np; i++)
            for (ep = (struct engr *) nh_cur->g_engrave_c_head_engr; ep; ep = ep->nxt_engr)
                if (key_engr(ep) == f[i].key) {
                    ep->engr_time = f[i].time;
                    ep->engr_txt = (char *) (ep + 1) + f[i].txt_off;
                }
    }
    {
        const d_level *dl = (const d_level *) take(r, sizeof(d_level));
        s_level *lev;

        if (!dl)
            return -1;
        for (lev = sp_levchn; lev && !(dl->dnum >= 0 && on_level(&lev->dlevel, (d_level *) dl)); lev = lev->next)
            ;
        nh_cur->g_makemon_c_align_shift_lev_p = lev;
    }
    return bad;
}

/* restgamestate reads context with the source's raw pointers and relinks them only for objects
   restored after it, so relink every one by id here; the room resident too, which a dead
   shopkeeper can leave dangling */
static struct obj *
find_obj_in(struct obj *chain, unsigned id)
{
    struct obj *o;

    for (; chain; chain = chain->nobj) {
        if (chain->o_id == id)
            return chain;
        if (Has_contents(chain) && (o = find_obj_in(chain->cobj, id)) != 0)
            return o;
    }
    return 0;
}

static struct obj *
find_obj(unsigned id)
{
    struct obj *o = 0;
    struct monst *m;

    if (!id)
        return 0;
    if ((o = find_obj_in(invent, id)) || (o = find_obj_in(fobj, id)) || (o = find_obj_in(level.buriedobjlist, id))
        || (o = find_obj_in(billobjs, id)) || (o = find_obj_in(migrating_objs, id)))
        return o;
    for (m = fmon; m && !o; m = m->nmon)
        o = find_obj_in(m->minvent, id);
    for (m = migrating_mons; m && !o; m = m->nmon)
        o = find_obj_in(m->minvent, id);
    return o;
}

static struct monst *
find_mon(unsigned id, struct monst *chain)
{
    for (; chain; chain = chain->nmon)
        if (chain->m_id == id)
            return chain;
    return 0;
}

static void
relink_context(void)
{
    struct monst *m;
    int i;

    if (context.victual.piece)
        context.victual.piece = find_obj(context.victual.o_id);
    if (context.tin.tin)
        context.tin.tin = find_obj(context.tin.o_id);
    if (context.spbook.book)
        context.spbook.book = find_obj(context.spbook.o_id);
    if (context.polearm.hitmon)
        context.polearm.hitmon = find_mon(context.polearm.m_id, fmon);
    for (i = 0; i < nroom + nsubroom; i++) {
        struct mkroom *r = i < nroom ? &rooms[i] : &subrooms[i - nroom];

        for (m = fmon; m && m != r->resident; m = m->nmon)
            ;
        if (!m)
            r->resident = 0;
    }
}

/* grid cells that point at a monster standing elsewhere (stock's dog_move leaves one when the pet was moved during its own
   move); the restore rebuilds the grid from the monster lists, so they travel as (x, y, m_id) */
static void
put_stale(struct cbuf *b)
{
    unsigned n = 0, rec[3];
    size_t at = b->n;
    int x, y;

    put(b, &n, sizeof n);
    for (x = 0; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            struct monst *m = NH_MON_CELL(x, y);

            if (m && !m->wormno && (m->mx != x || m->my != y)) {
                rec[0] = x, rec[1] = y, rec[2] = m->m_id;
                put(b, rec, sizeof rec);
                n++;
            }
        }
    memcpy(b->p + at, &n, sizeof n);
}

static int
take_stale(struct crd *r)
{
    const unsigned *np = (const unsigned *) take(r, sizeof(unsigned)), *rec;
    unsigned i;

    if (!np)
        return -1;
    for (i = 0; i < *np; i++) {
        struct monst *m;

        if (!(rec = (const unsigned *) take(r, 3 * sizeof(unsigned))) || rec[0] >= COLNO || rec[1] >= ROWNO)
            return -1;
        if ((m = find_mon(rec[2], fmon)) != 0 || (m = find_mon(rec[2], migrating_mons)) != 0)
            NH_SET_MON(rec[0], rec[1], m);
    }
    return 0;
}

/* the env's high-score file: statues and corpses of past players (mk_tt_object) are drawn from it */
static void
put_record(struct cbuf *b)
{
    FILE *f = fopen(fqname(RECORD, SCOREPREFIX, 0), "rb");
    size_t n = 0, at = b->n;

    put(b, &n, sizeof n);
    if (!f)
        return;
    for (;;) {
        size_t got;

        reserve(b, 4096);
        if (!(got = fread(b->p + b->n, 1, 4096, f)))
            break;
        b->n += got, n += got;
    }
    fclose(f);
    memcpy(b->p + at, &n, sizeof n);
}

static int
take_record(struct crd *r)
{
    const size_t *np = (const size_t *) take(r, sizeof(size_t));
    const unsigned char *data;
    const char *path = fqname(RECORD, SCOREPREFIX, 0);
    unsigned char cur[4096];
    size_t got = 0;
    FILE *f;

    if (!np || !(data = take(r, *np)))
        return -1;
    if (*np <= sizeof cur && (f = fopen(path, "rb")) != 0) {
        got = fread(cur, 1, sizeof cur, f);
        fclose(f);
        if (got == *np && !memcmp(cur, data, got))
            return 0;
    }
    if (!(f = fopen(path, "wb")))
        return -1;
    if (*np)
        fwrite(data, 1, *np, f);
    fclose(f);
    return 0;
}

/* replace the pointer at p by its kind-relative offset; return the kind */
static unsigned char
canon(unsigned char *p, const struct nh_ctx *nh)
{
    uintptr_t v;
    uint64_t o = 0;
    unsigned char c = P_NULL;
    unsigned k;

    memcpy(&v, p, sizeof v);
    if (!v) {
        c = P_NULL;
    } else if (v - (uintptr_t) nh < sizeof *nh) {
        c = P_CTX, o = v - (uintptr_t) nh;
    } else if (v - lib_base < lib_size) {
        c = P_LIB, o = v - lib_base;
    } else {
        c = P_HEAP, o = v;
        for (k = 0; k < nh_ckpt_nslot; k++) {
            uintptr_t b = (uintptr_t) nh->nh_lazy[nh_ckpt_slot[k].slot];

            if (b && v - b < nh_ckpt_slot[k].bytes) {
                c = P_SLOT, o = ((uint64_t) k << 32) | (v - b);
                break;
            }
        }
    }
    memcpy(p, &o, sizeof o);
    return c;
}

/* inverse of canon for this env; heap pointers come back null */
static void
reloc(unsigned char *p, unsigned char c, const struct nh_ctx *nh)
{
    uint64_t o;
    uintptr_t v = 0;

    memcpy(&o, p, sizeof o);
    if (c == P_CTX)
        v = (uintptr_t) nh + o;
    else if (c == P_LIB)
        v = lib_base + o;
    else if (c == P_SLOT)
        v = (uintptr_t) nh->nh_lazy[nh_ckpt_slot[o >> 32].slot] + (uint32_t) o;
    memcpy(p, &v, sizeof v);
}

/* the cells' kept bits are a cache of the display buffer: an image carries none, so it holds the bytes it always held (a load
   rebuilds them, nh_kept_rebuild) */
static void
strip_kept(unsigned char *cells)
{
    struct rm m;
    const unsigned char *mb = (const unsigned char *) &m;
    unsigned i, k;

    memset(&m, 0, sizeof m);
    m.msum.disp_kept = 1;
    for (k = 0; !mb[k]; k++)
        ;
    for (i = 0; i < COLNO * ROWNO; i++)
        cells[i * sizeof m + k] &= (unsigned char) ~mb[k];
}

#define SD_CELLS ((COLNO - 1) * ROWNO)

static void
get_display(struct ckpt_hdr *h)
{
    unsigned char *map[3], *stale;
    char *sd;
    int w;

    if (ttyDisplay) {
        h->tty_cur[0] = ttyDisplay->curx, h->tty_cur[1] = ttyDisplay->cury;
        h->tty_color = ttyDisplay->color, h->tty_rawprint = ttyDisplay->rawprint;
    }
    for (w = 0; w < MAXWIN; w++)
        if (wins[w])
            h->win_set[w] = 1, h->win_cur[w][0] = wins[w]->curx, h->win_cur[w][1] = wins[w]->cury;
    nle_status_ckpt(&h->status, FALSE);
#ifdef STATUS_HILITES
    nle_tty_status_ckpt(&h->tty_status, FALSE);
#endif
    nle_rl_map_resolve();
    if (!nle_rl_ckpt_display(map, &sd, &stale)) {
        h->rl_ok = 1, h->rl_stale = *stale;
        for (w = 0; w < 3; w++)
            memcpy(h->rl_map[w], map[w], SD_CELLS);
    }
}

/* the screen descriptions (each cell to its last nonzero byte) and the terminal */
static void
put_display(struct cbuf *b, nle_ctx_t *nle)
{
    unsigned char *map[3], *stale;
    char *sd;
    size_t n = 0;
    unsigned i;

    if (!nle_rl_ckpt_display(map, &sd, &stale))
        for (i = 0; i < SD_CELLS; i++) {
            const char *c = sd + i * NLE_SCREEN_DESCRIPTION_LENGTH;
            unsigned char len = NLE_SCREEN_DESCRIPTION_LENGTH;

            while (len && !c[len - 1])
                len--;
            put(b, &len, 1);
            put(b, c, len);
        }
    if (nle->vterminal)
        n = tmt_save(nle->vterminal, 0);
    put(b, &n, sizeof n);
    if (n) {
        reserve(b, n);
        tmt_save(nle->vterminal, b->p + b->n);
        b->n += n;
    }
}

static int
take_display(struct crd *r, nle_ctx_t *nle, struct ckpt_hdr *h)
{
    unsigned char *map[3], *stale;
    const unsigned char *p;
    const size_t *np;
    char *sd;
    unsigned i;
    int w, rl = !nle_rl_ckpt_display(map, &sd, &stale);

    if (ttyDisplay) {
        ttyDisplay->curx = h->tty_cur[0], ttyDisplay->cury = h->tty_cur[1];
        ttyDisplay->color = h->tty_color, ttyDisplay->rawprint = h->tty_rawprint;
    }
    for (w = 0; w < MAXWIN; w++)
        if (wins[w] && h->win_set[w])
            wins[w]->curx = h->win_cur[w][0], wins[w]->cury = h->win_cur[w][1];
    nle_status_ckpt(&h->status, TRUE);
#ifdef STATUS_HILITES
    nle_tty_status_ckpt(&h->tty_status, TRUE);
#endif
    if (rl && h->rl_ok) {
        *stale = h->rl_stale;
        for (w = 0; w < 3; w++)
            memcpy(map[w], h->rl_map[w], SD_CELLS);
    }
    for (i = 0; h->rl_ok && i < SD_CELLS; i++) {
        const unsigned char *len = take(r, 1);

        if (!len || *len > NLE_SCREEN_DESCRIPTION_LENGTH || !(p = take(r, *len)))
            return -1;
        if (rl) {
            memcpy(sd + i * NLE_SCREEN_DESCRIPTION_LENGTH, p, *len);
            memset(sd + i * NLE_SCREEN_DESCRIPTION_LENGTH + *len, 0, NLE_SCREEN_DESCRIPTION_LENGTH - *len);
        }
    }
    if (!(np = (const size_t *) take(r, sizeof(size_t))) || !(p = take(r, *np)))
        return -1;
    nle->outbuf_write_ptr = nle->outbuf; /* nothing of the target's output is still to reach its terminal */
    if (nle->vterminal && !*np) /* the saved game had no terminal: a fresh one, as it would make */
        tmt_close(nle->vterminal), nle->vterminal = 0;
    if (!nle->vterminal && (*np || nle->tty_emit))
        nle->vterminal = tmt_open(NLE_TERM_LI, NLE_TERM_CO, nle_vt_callback, nle, NULL, true);
    if (*np && (!nle->vterminal || !tmt_load(nle->vterminal, p, *np)))
        return -1;
    return 0;
}

/* what NetHack's save code writes into the live game: savelev marks the level VISITED, savegamestate advances urealtime and
   links a swallowed hero's ball and chain, save_msghistory moves the message line into the tty's history ring over its oldest
   entry; and scratch (the ids nle_ckpt_heap sets, savelevl's nhUse, the date and file-name buffers) */
struct live_keep {
    struct u_realtime rt;
    struct obj *chain, *ball;
    unsigned use, stuck, steed;
    int ledger, nxtidx;
    unsigned char vis;
    char topl[TBUFSZ], date[sizeof nh_cur->g_hacklib_c_datestr], fqn[sizeof nh_cur->g_files_c_fqn_fname];
    struct WinDesc *cw;
    long row, maxcol;
    int wflags;
    char *slot;
    short len;
    char text[TBUFSZ + 8];
};

static void
live_get(struct live_keep *k, struct nh_ctx *nh, int ledger)
{
    struct WinDesc *cw = WIN_MESSAGE != WIN_ERR ? wins[WIN_MESSAGE] : 0;

    k->rt = urealtime, k->ledger = ledger, k->vis = level_info[ledger].linfo_flags;
    k->chain = uchain ? uchain->nobj : 0, k->ball = uball ? uball->nobj : 0;
    k->use = nhUse_dummy, k->stuck = nh->g_save_c_ustuck_id, k->steed = nh->g_save_c_usteed_id, k->nxtidx = nh->g_topl_c_nxtidx;
    memcpy(k->topl, toplines, sizeof k->topl);
    memcpy(k->date, nh->g_hacklib_c_datestr, sizeof k->date);
    memcpy(k->fqn, nh->g_files_c_fqn_fname, sizeof k->fqn);
    k->cw = cw && cw->data && cw->datlen && cw->datlen[cw->maxrow] <= (short) sizeof k->text ? cw : 0;
    if (!k->cw)
        return;
    k->row = cw->maxrow, k->maxcol = cw->maxcol, k->wflags = cw->wflags;
    k->slot = cw->data[k->row], k->len = cw->datlen[k->row];
    if (k->slot)
        memcpy(k->text, k->slot, k->len);
}

static void
live_put(const struct live_keep *k, struct nh_ctx *nh)
{
    struct WinDesc *cw = k->cw;

    urealtime = k->rt, level_info[k->ledger].linfo_flags = k->vis;
    if (uchain)
        uchain->nobj = k->chain;
    if (uball)
        uball->nobj = k->ball;
    nhUse_dummy = k->use, nh->g_save_c_ustuck_id = k->stuck, nh->g_save_c_usteed_id = k->steed, nh->g_topl_c_nxtidx = k->nxtidx;
    memcpy(toplines, k->topl, sizeof k->topl);
    memcpy(nh->g_hacklib_c_datestr, k->date, sizeof k->date);
    memcpy(nh->g_files_c_fqn_fname, k->fqn, sizeof k->fqn);
    if (!cw)
        return;
    if (cw->data[k->row] != k->slot) {
        free(cw->data[k->row]);
        cw->data[k->row] = k->slot ? (char *) alloc((unsigned) k->len) : 0;
    }
    if (k->slot)
        memcpy(cw->data[k->row], k->text, k->len);
    cw->datlen[k->row] = k->len;
    cw->maxrow = k->row, cw->maxcol = k->maxcol, cw->wflags = k->wflags;
}

/* into *buf (capacity *cap, grown with realloc and kept by the caller across saves); returns the length */
size_t
nle_ckpt_save_into(nle_ctx_t *nle, void **buf, size_t *cap)
{
    struct nh_ctx *nh;
    struct cbuf b;
    struct ckpt_hdr h;
    struct live_keep live;
    unsigned char *img, *cls;
    const unsigned char *data;
    size_t n;
    unsigned i, k;
    int lev, count = 0;

    b.p = (unsigned char *) *buf, b.cap = *cap, b.n = 0;
    if (nle->rl_in_yn_function || nle->rl_in_getlin || nle->done || ckpt_enter(nle) || !nle_at_command())
        return 0; /* only where the game waits for a command's first key (not inside getdir, a menu, --More--, a count) */
    nh = nh_cur;
    live_get(&live, nh, ledger_no(&u.uz));
    memset(&h, 0, sizeof h);
    h.magic = CKPT_MAGIC, h.version = CKPT_VERSION, h.ctx_size = sizeof *nh;
    h.nptr = nh_ckpt_nptr, h.nslot = nh_ckpt_nslot;
    h.ledger = ledger_no(&u.uz), h.underfoot = nle_rl_ckpt_get(h.rl_blstats, &h.rl_cond, h.rl_status_sig, &h.rl_inv);
    h.toplin = ttyDisplay ? ttyDisplay->toplin : 0;
    h.strs_gap = nle->obs_cache.strs_gap;
    h.time_seed = settings.time_seed, h.time_seed_is_set = settings.time_seed_is_set;
    h.fix_moon_phase = settings.fix_moon_phase;
    h.options = nle_options_id(nle);
    h.xlock_box = nh->g_lock_c_xlock.box ? nh->g_lock_c_xlock.box->o_id : 0;
    get_display(&h);
    put(&b, &h, sizeof h);

    put(&b, nh, sizeof *nh);
    img = b.p + b.n - sizeof *nh;
    nh_shop_bits_strip(img + offsetof(struct nh_ctx, g_level.locations));
    reserve(&b, nh_ckpt_nptr);
    img = b.p + b.n - sizeof *nh, cls = b.p + b.n;
    strip_kept(img + offsetof(struct nh_ctx, g_level.locations));
    for (i = 0; i < nh_ckpt_nptr; i++)
        cls[i] = canon(img + nh_ckpt_ptr[i], nh);
    b.n += nh_ckpt_nptr;

    for (k = 0; k < nh_ckpt_nslot; k++) {
        const struct nh_ckpt_slot *s = &nh_ckpt_slot[k];
        unsigned char present = nh->nh_lazy[s->slot] != 0;

        put(&b, &present, 1);
        if (!present)
            continue;
        reserve(&b, s->bytes + s->nptr);
        img = b.p + b.n, cls = img + s->bytes;
        memcpy(img, nh->nh_lazy[s->slot], s->bytes);
        for (i = 0; i < s->nptr; i++)
            cls[i] = canon(img + nh_ckpt_slot_ptr[s->first_ptr + i], nh);
        b.n += s->bytes + s->nptr;
    }

    nle_ckpt_heap(nh_memfd_create(SCRATCH), WRITE_SAVE);
    data = nh_memfd_data(SCRATCH, &n);
    put(&b, &n, sizeof n);
    put(&b, data, n);

    for (lev = 1; lev < MAXLINFO; lev++)
        if (lev != h.ledger && (level_info[lev].linfo_flags & LFILE_EXISTS))
            count++;
    put(&b, &count, sizeof count);
    for (lev = 1; lev < MAXLINFO; lev++)
        if (lev != h.ledger && (level_info[lev].linfo_flags & LFILE_EXISTS)) {
            data = nh_memfd_data(lev, &n);
            put(&b, &lev, sizeof lev);
            put(&b, &n, sizeof n);
            put(&b, data, n);
        }
    put_order(&b);
    put_record(&b);
    put_display(&b, nle);
    put_stale(&b);
    live_put(&live, nh);
    *buf = b.p, *cap = b.cap;
    return b.n;
}

void *
nle_ckpt_save(nle_ctx_t *nle, size_t *len)
{
    void *buf = 0;
    size_t cap = 0;

    *len = nle_ckpt_save_into(nle, &buf, &cap);
    if (!*len)
        free(buf), buf = 0;
    return buf;
}

int
nle_ckpt_load(nle_ctx_t *nle, const void *blob, size_t len)
{
    struct nh_ctx *nh, *keep;
    struct crd r;
    struct ckpt_hdr h;
    const unsigned char *img, *cls, *data, *slot_img[64], *slot_cls[64];
    const size_t *np;
    const int *ip;
    size_t n;
    unsigned i, k;
    int lev, count, rc;

    if (ckpt_enter(nle))
        return 1;
    if (nle->rl_in_yn_function || nle->rl_in_getlin || nle->done || !nle_at_command())
        return 5; /* the target resumes on its own stack: it too must wait for a command's first key */
    nh = nh_cur;
    r.p = (const unsigned char *) blob, r.n = len, r.pos = 0;
    if (!(data = take(&r, sizeof h)))
        return 2;
    memcpy(&h, data, sizeof h);
    if (h.magic != CKPT_MAGIC || h.version != CKPT_VERSION || h.ctx_size != sizeof *nh
        || h.nptr != nh_ckpt_nptr || h.nslot != nh_ckpt_nslot)
        return 3;
    if (h.options != nle_options_id(nle))
        return 8; /* the target keeps its own option structures (keep_fields): the image's would meet the target's */
    if (!(img = take(&r, sizeof *nh)) || !(cls = take(&r, nh_ckpt_nptr)))
        return 4;
    for (k = 0; k < nh_ckpt_nslot; k++) {
        const unsigned char *present = take(&r, 1);

        if (!present)
            return 4;
        slot_img[k] = slot_cls[k] = 0;
        if (*present && (!(slot_img[k] = take(&r, nh_ckpt_slot[k].bytes))
                         || !(slot_cls[k] = take(&r, nh_ckpt_slot[k].nptr))))
            return 4;
    }

    /* tear down this env's heap, then lay the image over the context */
    if (ttyDisplay)
        ttyDisplay->topl_ext = TBUFSZ; /* toplines takes the image's bytes */
    nle_ckpt_heap(nh_memfd_create(SCRATCH), FREE_SAVE);
    for (lev = 1; lev < MAXLINFO; lev++)
        (void) nh_memfd_create(lev);
    if (!scratch)
        scratch = (unsigned char *) malloc(sizeof *nh);
    keep = (struct nh_ctx *) scratch;
    memcpy(keep, nh, sizeof *nh);
    memcpy(nh, img, sizeof *nh);
    memcpy(nh->nh_lazy, keep->nh_lazy, sizeof nh->nh_lazy);
    for (i = 0; i < nh_ckpt_nfield; i++)
        if (keep_whole[i])
            memcpy((unsigned char *) nh + nh_ckpt_field[i].off, (unsigned char *) keep + nh_ckpt_field[i].off,
                   nh_ckpt_field[i].size);
    for (k = 0; k < nh_ckpt_nslot; k++) {
        void **slot = &nh->nh_lazy[nh_ckpt_slot[k].slot];

        if (!slot_img[k]) {
            free(*slot);
            *slot = 0;
        } else if (!*slot) {
            *slot = calloc(1, nh_ckpt_slot[k].bytes);
        }
    }
    for (i = 0; i < nh_ckpt_nptr; i++) {
        unsigned char *p = (unsigned char *) nh + nh_ckpt_ptr[i];

        if (ptr_act[i] == A_RELOC)
            reloc(p, cls[i], nh);
        else if (ptr_act[i] == A_KEEP)
            memcpy(p, (unsigned char *) keep + nh_ckpt_ptr[i], sizeof(void *));
    }
    if (nle->cmd_shared && memcmp(&nh->g_Cmd, &nh_cmd_shared, sizeof nh->g_Cmd))
        nle->cmd_shared = 0; /* the image's options built another Cmd */
    for (k = 0; k < nh_ckpt_nslot; k++) {
        const struct nh_ckpt_slot *s = &nh_ckpt_slot[k];
        unsigned char *blk = (unsigned char *) nh->nh_lazy[s->slot];

        if (!slot_img[k])
            continue;
        memcpy(blk, slot_img[k], s->bytes);
        for (i = 0; i < s->nptr; i++)
            reloc(blk + nh_ckpt_slot_ptr[s->first_ptr + i], slot_cls[k][i], nh);
    }

    /* the heap: current level and game state, then the other levels' files */
    if (!(np = (const size_t *) take(&r, sizeof n)) || !(data = take(&r, *np)))
        return 5;
    nh_memfd_put(SCRATCH, data, *np);
    nle_tty_reset_msghistory();
    rc = nle_ckpt_restore_heap(nh_memfd_open(SCRATCH));
    if (!(ip = (const int *) take(&r, sizeof count)))
        return 6;
    for (count = *ip; count > 0; count--) {
        const int *lp = (const int *) take(&r, sizeof lev);

        if (!lp || !(np = (const size_t *) take(&r, sizeof n)) || !(data = take(&r, *np)))
            return 6;
        nh_memfd_put(*lp, data, *np);
    }
    if (take_order(&r))
        rc = rc ? rc : 8;
    if (take_record(&r))
        rc = rc ? rc : 9;
    if (take_display(&r, nle, &h))
        rc = rc ? rc : 10;
    if (take_stale(&r))
        rc = rc ? rc : 11;
    relink_context();

    /* NetHack's restore recomputes some state (vision and display flags, unweapon, the message line);
       put every non-pointer byte back as saved, then drop the caches that hold heap pointers */
    for (i = 0; i < nrun; i++)
        memcpy((unsigned char *) nh + run_off[i], img + run_off[i], run_len[i]);
    for (k = 0; k < nh_ckpt_nslot; k++) {
        const struct nh_ckpt_slot *s = &nh_ckpt_slot[k];
        unsigned char *blk = (unsigned char *) nh->nh_lazy[s->slot];
        unsigned b, j;

        if (!slot_img[k])
            continue;
        for (b = 0, j = 0; b < s->bytes; b++) {
            while (j < s->nptr && nh_ckpt_slot_ptr[s->first_ptr + j] + sizeof(void *) <= b)
                j++;
            if (j < s->nptr && b >= nh_ckpt_slot_ptr[s->first_ptr + j])
                continue;
            blk[b] = slot_img[k][b];
        }
    }
    nh_fobj_invalidate();
    nh_kept_rebuild();
    nh_movers_valid = 0;
    nh_shop_bits_sync(TRUE); /* the image's cells carry none */
    nle->obs_cache.wt_valid = 0;
    nh_vis_cache()->vis_clean[0] = nh_vis_cache()->vis_clean[1] = 0; /* the image's could_see may hold bytes outside its bounds */
    settings.time_seed = h.time_seed;
    settings.time_seed_is_set = h.time_seed_is_set;
    settings.fix_moon_phase = h.fix_moon_phase;
    nle_rl_ckpt_set(h.underfoot, h.rl_blstats, h.rl_cond, h.rl_status_sig, &h.rl_inv);
    nle->obs_cache.strs_gap = (char) h.strs_gap;
    nh->g_lock_c_xlock.box = h.xlock_box ? find_obj(h.xlock_box) : 0;
    if (ttyDisplay)
        ttyDisplay->toplin = h.toplin, ttyDisplay->topl_ext = TBUFSZ;
    if (!rc) {
        nle_discoveries_changed();
        /* the quest pager's message lists are the role's the game started with (load_qtlist at game start); the loaded
           hero may have another */
        unload_qtlist();
        load_qtlist();
    }
    return rc ? 7 : 0;
}

/* fields whose saved bytes differ between two checkpoints (heap addresses ignored): a load
   followed by a save must reproduce the original */
int
nle_ckpt_diff(const void *a, size_t alen, const void *b, size_t blen)
{
    const unsigned char *pa = (const unsigned char *) a + sizeof(struct ckpt_hdr);
    const unsigned char *pb = (const unsigned char *) b + sizeof(struct ckpt_hdr);
    const unsigned char *ca = pa + nh_ckpt_ctx_size, *cb = pb + nh_ckpt_ctx_size;
    unsigned char *skip = (unsigned char *) calloc(nh_ckpt_ctx_size, 1);
    unsigned i, f, k, nd = 0;

    if (alen < sizeof(struct ckpt_hdr) + nh_ckpt_ctx_size + nh_ckpt_nptr
        || blen < sizeof(struct ckpt_hdr) + nh_ckpt_ctx_size + nh_ckpt_nptr || !keep_whole)
        return -1;
    for (i = 0; i < nh_ckpt_nptr; i++)
        if (ca[i] == P_HEAP && cb[i] == P_HEAP)
            memset(skip + nh_ckpt_ptr[i], 1, sizeof(void *));
    for (f = 0; f < nh_ckpt_nfield; f++) {
        unsigned off = nh_ckpt_field[f].off, nb = 0, first = 0;

        for (i = off; i < off + nh_ckpt_field[f].size; i++)
            if (!skip[i] && pa[i] != pb[i] && !nb++)
                first = i - off;
        if (nb && (keep_whole[f] || !strcmp(nh_ckpt_field[f].name, "nh_lazy")
                   || !strncmp(nh_ckpt_field[f].name, "g_nh_fobj_", 10) || !strncmp(nh_ckpt_field[f].name, "g_nh_movers", 11)
                   || !strcmp(nh_ckpt_field[f].name, "g_hack_c_tmp_anything_p")
                   || !strcmp(nh_ckpt_field[f].name, "g_nh_vis_last_fp")))
            continue; /* kept by the target, or a cache the load invalidates */
        if (nb)
            fprintf(stderr, "NH_CKPT diff %s: %u bytes, first +%u\n", nh_ckpt_field[f].name, nb, first), nd++;
    }
    free(skip);
    pa = ca + nh_ckpt_nptr, pb = cb + nh_ckpt_nptr;
    for (k = 0; k < nh_ckpt_nslot; k++) {
        const struct nh_ckpt_slot *s = &nh_ckpt_slot[k];
        unsigned nb = 0;

        if (*pa != *pb) {
            fprintf(stderr, "NH_CKPT diff slot %u presence\n", s->slot), nd++;
            break;
        }
        if (!*pa++) {
            pb++;
            continue;
        }
        pb++;
        for (i = 0; i < s->bytes; i++)
            if (pa[i] != pb[i])
                nb++;
        if (nb)
            fprintf(stderr, "NH_CKPT diff slot %u: %u bytes\n", s->slot, nb), nd++;
        pa += s->bytes + s->nptr, pb += s->bytes + s->nptr;
    }
    return (int) nd;
}

/* debug: a hash of the heap the struct diff cannot see (objects and monsters, by id and contents) */
static uint64_t
digest_objs(uint64_t h, struct obj *o)
{
    for (; o; o = o->nobj) {
        long v[] = { o->o_id, o->otyp, o->corpsenm, o->ox, o->oy, o->quan, o->spe, o->blessed, o->cursed, o->owt,
                     o->where, o->age };
        unsigned i;

        for (i = 0; i < sizeof v / sizeof *v; i++)
            h = (h ^ (uint64_t) v[i]) * 1099511628211ULL;
        if (Has_contents(o))
            h = digest_objs(h, o->cobj);
    }
    return h;
}

uint64_t
nle_ckpt_heapdigest(nle_ctx_t *nle)
{
    uint64_t h = 1469598103934665603ULL;
    struct monst *m;

    if (ckpt_enter(nle))
        return 0;
    h = digest_objs(digest_objs(digest_objs(h, fobj), invent), level.buriedobjlist);
    for (m = fmon; m; m = m->nmon) {
        long v[] = { m->m_id, m->mnum, m->mx, m->my, m->mhp, m->mpeaceful, m->mtame, m->mcanmove, m->movement };
        unsigned i;

        for (i = 0; i < sizeof v / sizeof *v; i++)
            h = (h ^ (uint64_t) v[i]) * 1099511628211ULL;
        h = digest_objs(h, m->minvent);
    }
    return h;
}

void
nle_ckpt_free(void *blob)
{
    free(blob);
}
