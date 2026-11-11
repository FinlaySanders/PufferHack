#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __AVX2__
#include <immintrin.h>
#endif
/* Copyright (c) Facebook, Inc. and its affiliates. */
#include <array>
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <stdio.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
#include "hack.h"
extern "C" {
#include "nle.h" /* current_nle_ctx for per-env window-port state */
}
}

extern "C" {
#include "wintty.h"
int nh_obuf_index(void);
void nh_obuf_set_index(int);
}

extern "C" {
#include "nletypes.h"
}

/* MSan boundary check: the C engine is instrumented but libstdc++ is not,
   so once a string enters std::string its shadow is untrustworthy. Check
   engine-produced strings here, at the C -> C++ crossing, where the shadow
   is still real. No-op outside MemorySanitizer builds. */
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define MSAN_CHECK_CSTR(s)                                       \
    do {                                                         \
        if (s)                                                   \
            __msan_check_mem_is_initialized((s), strlen(s) + 1); \
    } while (0)
#endif
#endif
#ifndef MSAN_CHECK_CSTR
#define MSAN_CHECK_CSTR(s) ((void) 0)
#endif

#define USE_DEBUG_API 0

#if USE_DEBUG_API
#define DEBUG_API(x)    \
    do {                \
        std::cerr << x; \
    } while (0)
#else
#define DEBUG_API(x)
#endif

/*
 * We had to change xwaitforspace() in getline.c to tell the agent in a
 * --More-- situation that enter/return (ironically not necessarily space)
 * is required to continue.
 */

/* some hack.h macros. Can be undefined here. */
#undef Invisible
#undef Warning
#undef index
#undef msleep
#undef rindex
#undef wizard
#undef yn

extern "C" {
extern void *nle_yield(boolean);
extern nle_obs *nle_get_obs();
extern int nle_underfoot_glyphs();
}

/* Initial value of glyph_ buffer. Cf. display.c. */
const int nul_glyph = cmap_to_glyph(S_stone);

namespace nethack_rl
{
/* The former win_proc_calls debug deque is gone: it was the last
 * process-global in the window port, its std::string churn perturbed the
 * heap on every window call (a suspect for layout-dependent behavior),
 * and it drowned MemorySanitizer in uninstrumented-libstdc++ shadow
 * noise. DEBUG_API logging remains for tracing. */
#define in_yn_function (current_nle_ctx->rl_in_yn_function)
#define in_getlin (current_nle_ctx->rl_in_getlin)

// Glyphs provide instructions for windows to render the game (see display.h).
// At the start of the game, descriptions and properties of the object classes
// are shuffled (see o_init.c) while the glyphs pointing to these classes are
// not. This means glyph observations would always identify a 'wand of
// wishing', regardless of whether it is 'metal', 'balsa', &c.
//
// In this function, we map a glyph to correspond to its shuffled equivalent,
// following the logic used by tiles that also need to generate images from
// glyphs (c.f. o_init.c).  In practice this means:
//   BEFORE: looking up objclass on a glyph gives CORRECT name INCORRECT descr
//   AFTER: looking up objclass on a glyph gives INCORRECT name CORRECT descr
// Gems and glass share descriptions ("violet": amethyst and worthless violet glass) and are not shuffled, so the
// appearance glyph of an unidentified gem would name the true type. Public form: the canonical twin of the description
// (the worthless glass if there is one, else the lowest index), which is what "a violet gem" tells the player.
// Keyed by the DESCRIPTION SLOT (oc_descr_idx), using the static description table obj_descr[]: gems are shuffled among
// themselves too, so the description currently attached to an object is not the object's own table entry.
// Every class: twins that share a description (bag of holding/tricks "bag", tin/magic whistle, tallow/wax candle, the gray
// stones, amethyst/violet glass) are not shuffled, so the description slot would name the true type. Canonical twin =
// the lowest index with that description in the class, the worthless glass for gems. Shuffled classes have unique
// descriptions within the class, so this is the identity there.
#include "appearance_canon.h"
static_assert(sizeof(nle_appearance_canon) / sizeof(nle_appearance_canon[0]) == NUM_OBJECTS + 1,
              "appearance table must match object definitions");
static int gem_canon(int slot)
{
    return nle_appearance_canon[slot];
}

int
shuffled_glyph(int glyph)
{
    if glyph_is_normal_object (glyph) {
        int obj = glyph_to_obj(glyph);
        unsigned g = nle_ocg()[obj];
        if (!(g & 1)) return GLYPH_OBJ_OFF + gem_canon(g >> 1);
        return GLYPH_OBJ_OFF + (g >> 1);
    }
    return glyph;
}
extern "C" int
nle_shuffled_glyph_c(int glyph) /* C entry for nle.c (public weight pricing) */
{
    return shuffled_glyph(glyph);
}

class alignas(64) NetHackRL
{
  public:
    NetHackRL(int &argc, char **argv);
    /* C++14 new does not honour alignas */
    static void *operator new(size_t n)
    {
        void *p;
        if (posix_memalign(&p, 64, n))
            throw std::bad_alloc();
        return p;
    }
    static void operator delete(void *p) { free(p); }

    static void rl_init_nhwindows(int *argc, char **argv);
    static void rl_player_selection();
    static void rl_askname();
    static void rl_get_nh_event();
    static void rl_exit_nhwindows(const char *);
    static void rl_suspend_nhwindows(const char *);
    static void rl_resume_nhwindows();
    static winid rl_create_nhwindow(int type);
    static void rl_clear_nhwindow(winid wid);
    static void rl_display_nhwindow(winid wid, BOOLEAN_P block);
    static void rl_destroy_nhwindow(winid wid);
    static void rl_curs(winid wid, int x, int y);
    static void rl_putstr(winid wid, int attr, const char *text);
    static void rl_display_file(const char *filename, BOOLEAN_P must_exist);
    static void rl_start_menu(winid wid);
    static void rl_add_menu(winid wid, int glyph, const ANY_P *identifier,
                            CHAR_P ch, CHAR_P gch, int attr, const char *str,
                            BOOLEAN_P presel);
    static void rl_end_menu(winid wid, const char *prompt);
    static int rl_select_menu(winid wid, int how, MENU_ITEM_P **menu_list);
    static void rl_update_inventory();
    static void rl_replay_export_ring();
    static void rl_mark_synch();
    static void rl_wait_synch();

    static void rl_cliparound(int x, int y);
    static void rl_print_glyph(winid wid, XCHAR_P x, XCHAR_P y, int glyph,
                               int bkglyph);
    static void rl_print_glyph_full(winid wid, XCHAR_P x, XCHAR_P y,
                                    int glyph, int bkglyph);
    static void rl_raw_print(const char *str);
    static void rl_raw_print_bold(const char *str);
    static int rl_nhgetch();
    static int getch_early();
    static int getch_key(int i);
    static int rl_nh_poskey(int *x, int *y, int *mod);
    static void rl_nhbell();
    static int rl_doprev_message();
    static char rl_yn_function(const char *question, const char *choices,
                               CHAR_P def);
    static void rl_getlin(const char *prompt, char *line);
    static int rl_get_ext_cmd();
    static void rl_number_pad(int);
    static void rl_delay_output();
    static void rl_start_screen();
    static void rl_end_screen();

    static char *rl_getmsghistory(BOOLEAN_P init);
    static void rl_putmsghistory(const char *msg, BOOLEAN_P is_restoring);

    static void rl_outrip(winid wid, int how, time_t when);
    static void rl_status_init();

    static void rl_status_update(int fldidx, genericptr_t ptr, int chg,
                                 int percent, int color,
                                 unsigned long *colormasks);

    /* !status_updates mode: blstats_ cache pumps from botl.c (see
       nle_rl_bot_direct / nle_rl_timebot_direct below). */
    static void rl_bot_direct();
    static void rl_timebot_direct();
    static void rl_fill_obs_direct(nle_obs *);
    static int rl_ckpt_get(long *, long *, long *, struct nle_rl_inv_ckpt *);
    static void rl_ckpt_set(int, const long *, long, const long *, const struct nle_rl_inv_ckpt *);
    static int rl_ckpt_display(unsigned char **, char **, uint8_t **);
    static void rl_map_resolve();

  private:
    struct rl_window {
        int type;
    };

    struct rl_inventory_item {
        int glyph;
        // TODO: Don't heap allocate this stuff.
        std::string str;
        char letter;
        char object_class;
        // TODO: Don't heap allocate this stuff.
        std::string object_class_name;
    };

    /* every-step members first, cold arrays last */
    /* Rows of the four display arrays changed since the last fill_obs;
       fill_obs copies only these into the obs buffers (obs rows not
       re-copied are unchanged from the previous fill, so the obs buffers
       must be stable per env — any buffer-pointer change forces a full
       copy). All-dirty on construction, map clears, not-in-game fills. */
    uint32_t dirty_rows_ = ~0u;
    /* chars_/colors_/specials_ skipped while no obs consumes them: bit 1 the map clear's fill is pending, bit 0 cells printed
       since are listed in cellf_ */
    uint8_t mapped_stale_ = 0;
    // opt-in (NLE_TRUE_GLYPHS): export true glyphs for discovered types
    bool true_glyphs_;
    uint8_t cell_n_ = 0; /* glyph stores listed in cell_at_ since the last full fill; CELLS + 1: more, or off the map */
    uint32_t glyph_export_rows_ = ~0u; /* rows rewritten by the last full fill */
    uint32_t glyph_export_acc_ = ~0u;  /* rows rewritten since the last nle_glyph_take_rows() */
    int underfoot_prev_ = -1; /* obs cell overwritten by the underfoot patch in the previous fill */
    unsigned disc_gen_ = 1, disc_gen_seen_ = 0; /* objects[].oc_name_known generation */
    unsigned inv_shown_gen_ = 0; /* disc_gen_ inv_glyphs_shown_ was mapped with; 0: the cache changed since */
    int inv_cache_n_ = 0; /* inv_glyphs_cache_ slots past this hold NO_GLYPH */
    int ring_before_ = -1; /* objnam's ring index before the last export that skipped doname's names; -1: none */
    int inv_memo_n_ = -1;
    int inv_memo_slots_ = 0; /* slots the last full fill wrote; the rest hold the cleared values */
    int inv_walk_n_, inv_walk_stale_; /* the fill's memo slots and the export rows it may have to clear */
    unsigned inv_memo_gen_ = 0; /* disc_gen_ at the last walk */
    unsigned long inv_walk_gen_ = 0; /* nh_invgen at the last walk */
    const signed char *inv_memo_ist_ = nullptr;
    const short *inv_memo_itg_ = nullptr;
    const int16_t *last_glyphs_buf_ = nullptr;
    const uint8_t *last_chars_buf_ = nullptr;
    const uint8_t *last_colors_buf_ = nullptr;
    const uint8_t *last_specials_buf_ = nullptr;
    long condition_bits_;
    /* last string put to the message window since its last clear: the only window text the port reads (yn prompts) */
    std::string msg_last_;

    long status_sig_[NLE_RL_STATUS_SIG] = {}; /* [0]: valid; then the status fields as stock evaluated them last */
    long blstats_[NLE_BLSTATS_SIZE];
    const unsigned char *msg_lo_ = nullptr; /* the bound message buffer while its upper half is all zero */
    std::array<short, NLE_INVENTORY_SIZE> inv_glyphs_shown_;
    std::array<unsigned char, NLE_INVENTORY_SIZE> inv_letters_cache_;
    std::array<unsigned char, NLE_INVENTORY_SIZE> inv_oclasses_cache_;
    /* the cells (y << 8 | x) of those stores: the hot block's tail, before the memo's line */
    static constexpr unsigned CELLS = 22;
    uint16_t cell_at_[CELLS];

    /* input-keyed reuse of the inventory walk, per slot: an object's walked bytes (quan .. the flag bits, one line at either
       slot parity), its corpse species and its discovery entry determine its slot's outputs and its weight, so a slot whose
       key matches the previous full fill's still holds them. cnm NO_CNM: a species or weight that does not fit, never matches */
    static constexpr size_t KEYB = offsetof(struct obj, corpsenm) - offsetof(struct obj, quan);
    static constexpr short NO_CNM = 0x7FFF;
    struct inv_key { unsigned char blk[KEYB]; short cnm; unsigned short ocg; int w; };
    static_assert(sizeof(inv_key) == 32, "two memo slots per line");
    alignas(64) inv_key inv_memo_[64];
    /* ocg_same: no discovery entry changed since the memo was last walked, so a slot's stored entry is still the mirror's */
    bool inv_same(const unsigned short *ocgt, const struct obj *o, int j, bool ocg_same) const
    {
        const inv_key &k = inv_memo_[j];
        return !std::memcmp(reinterpret_cast<const char *>(o) + offsetof(struct obj, quan), k.blk, KEYB) && k.cnm != NO_CNM
               && (o->otyp != CORPSE || k.cnm == o->corpsenm) && (ocg_same || ocgt[o->otyp] == k.ocg);
    }
    void inv_shift(nle_obs *obs, int j, int by);
    long inv_export_item(nle_obs *obs, struct obj *otmp, int i);

    std::array<short, NLE_INVENTORY_SIZE> inv_glyphs_cache_;
    std::vector<rl_inventory_item> inventory_;
    // TODO: Don't heap allocate this stuff.
    std::vector<std::unique_ptr<rl_window> > windows_;
    std::array<std::string, MAXBLSTATS> status_;

    /* Output of mapglyph */
    std::array<uint8_t, (COLNO - 1) * ROWNO> chars_;
    std::array<uint8_t, (COLNO - 1) * ROWNO> colors_;
    std::array<uint8_t, (COLNO - 1) * ROWNO> specials_;

    /* a cell printed while the mapped arrays were skipped: 1, | 2 an object pile, | 4 the hero's square, | 8 Upolyd, as mapglyph
       would have read them then */
    uint8_t cellf_[ROWNO][COLNO] = {};

    std::array<char, (COLNO - 1) * ROWNO * NLE_SCREEN_DESCRIPTION_LENGTH>
        screen_descriptions_;

    void store_glyph(XCHAR_P x, XCHAR_P y, int glyph);
    void map_fill()
    {
        chars_.fill(' ');
        colors_.fill(0);
        specials_.fill(0);
    }
    void map_defer(int x, int y, unsigned f)
    {
        cellf_[y][x] = (uint8_t) f;
        mapped_stale_ |= 1;
    }
public:
    void map_resolve();
    void glyphs_cleared() { dirty_rows_ = ~0u; }
    void discoveries_changed() { disc_gen_++; }
    uint32_t glyph_export_rows() const { return glyph_export_rows_; }
    uint32_t glyph_take_rows() { uint32_t r = glyph_export_acc_; glyph_export_acc_ = 0; return r; }
private:
    void store_mapped_glyph(int ch, int color, int special, XCHAR_P x,
                            XCHAR_P y);
    void store_screen_description(XCHAR_P x, XCHAR_P y, int glyph);

    static void fill_obs_state(nle_obs *);
    static void fill_obs_internal(nle_obs *);
    static void fill_obs_blank(nle_obs *);
    void fill_obs(nle_obs *);
    void fill_obs_full(nle_obs *);
    void copy_message(nle_obs *);
    int getch_method();

    void status_signature(long *sig);
    void blstats_from_signature(const long *sig);
    bool status_changed(const long *prev, const long *cur);
    int maybe_true_glyph(int glyph);

    void update_blstats(bool quiet = false);

    void player_selection_method();
    void status_update_method(int fldidx, genericptr_t ptr, int, int percent,
                              int color, unsigned long *colormasks);

    void putstr_method(winid wid, int attr, const char *str);

    void start_menu_method(winid wid);
    void add_menu_method(winid wid, int glyph, const anything *identifier,
                         char ch, char gch, int attr, const char *str,
                         bool preselected);
    void update_inventory_method();

    winid create_nhwindow_method(int type);
    void clear_nhwindow_method(winid wid);
    void display_nhwindow_method(winid wid, BOOLEAN_P block);
    void destroy_nhwindow_method(winid wid);
};

/* Per-env window-port object, stored on nle_ctx_t (raw pointer: a C
 * struct can't hold a unique_ptr). Created in rl_init_nhwindows, deleted
 * in rl_exit_nhwindows / error paths below. */
#define instance (*(NetHackRL **) &current_nle_ctx->rl_instance)

NetHackRL::NetHackRL(int &argc, char **argv) : blstats_{}
{
    inv_glyphs_cache_.fill(NO_GLYPH);
    inv_letters_cache_.fill(0);
    inv_oclasses_cache_.fill(MAXOCLASSES);
    // create base window
    // (done in tty_init_nhwindows before this NetHackRL object got created).
    assert(BASE_WINDOW == 0);
    windows_.emplace_back(new rl_window({ NHW_BASE }));
    true_glyphs_ = settings.true_glyphs;
}

/* Discovered-type map export (opt-in): once a type is in the discoveries
 * list, farlook names every instance on the floor, so the true glyph is
 * interface-public. Undiscovered (and everything when the flag is off,
 * e.g. golden replays) stays appearance-shuffled. */
int
NetHackRL::maybe_true_glyph(int glyph)
{
    if (true_glyphs_ && glyph_is_normal_object(glyph)
        && nle_ocg_known(glyph_to_obj(glyph)))
        return glyph;
    return shuffled_glyph(glyph);
}

void
NetHackRL::player_selection_method()
{
}

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#define NLE_RL_MSAN 1
#endif
#endif
#ifndef NLE_RL_MSAN
#define NLE_RL_MSAN 0
#endif

/* the lower half (the trainer reads it every step) and, if hi, the upper half */
static inline void
zero_message(unsigned char *msg, bool hi)
{
#ifdef __AVX2__
    static_assert(NLE_MESSAGE_SIZE == 8 * 32, "message size");
    __m256i *m = (__m256i *) msg, z = _mm256_setzero_si256(); /* stores, not a rep stos */
    _mm256_storeu_si256(m, z), _mm256_storeu_si256(m + 1, z), _mm256_storeu_si256(m + 2, z), _mm256_storeu_si256(m + 3, z);
    if (hi)
        _mm256_storeu_si256(m + 4, z), _mm256_storeu_si256(m + 5, z), _mm256_storeu_si256(m + 6, z), _mm256_storeu_si256(m + 7, z);
#else
    std::memset(msg, 0, hi ? NLE_MESSAGE_SIZE : NLE_MESSAGE_SIZE / 2);
#endif
}

/* strncpy(msg, s, NLE_MESSAGE_SIZE) where msg's upper half is zero unless hi; returns whether it may be non-zero now */
static bool
put_message(unsigned char *msg, const char *s, bool hi)
{
    constexpr int H = NLE_MESSAGE_SIZE / 2;
    std::strncpy((char *) msg, s, hi ? NLE_MESSAGE_SIZE : H);
    if (!hi && msg[H - 1]) /* s is H bytes or longer */
        std::strncpy((char *) msg + H, s + H, H), hi = true;
    return hi && msg[H];
}

/* put_message from toplines a vector at a time: zeros first, then toplines up to its first NUL (toplines holds TBUFSZ bytes,
   so whole vectors past the NUL are readable) */
static bool __attribute__((noinline))
copy_toplines(unsigned char *msg, bool hi)
{
#if defined(__AVX2__) && !NLE_RL_MSAN
    static_assert(TBUFSZ >= NLE_MESSAGE_SIZE, "toplines read in vectors");
    const char *src = toplines;

    zero_message(msg, hi);
    for (int k = 0; k < NLE_MESSAGE_SIZE; k += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *) (src + k));
        unsigned nul = (unsigned) _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_setzero_si256()));
        if (nul) {
            const __m256i idx = _mm256_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                                 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);
            v = _mm256_and_si256(v, _mm256_cmpgt_epi8(_mm256_set1_epi8((char) __builtin_ctz(nul)), idx));
            _mm256_storeu_si256((__m256i *) (msg + k), v);
            return k >= NLE_MESSAGE_SIZE / 2;
        }
        _mm256_storeu_si256((__m256i *) (msg + k), v);
    }
    return true;
#else
    return put_message(msg, toplines, hi);
#endif
}


inline void
NetHackRL::copy_message(nle_obs *obs)
{
    unsigned char *m = obs->message;
    if (!m)
        return;
    // TODO: This doesn't show anything in situations where there's too
    // many items at one tile, which will get displayed in a new window.
    bool hi = m != msg_lo_;
    const char *src = nullptr;
    if (in_yn_function) {
        // Special case. See tty_putstr: yn_function doesn't add to
        // toplines until after that frame is over. Use last string on
        // NHW_MESSAGE instead.
        src = msg_last_.c_str();
        hi = put_message(m, src, hi);
        /* The source std::string was written by uninstrumented
           libstdc++, so the shadow strncpy propagated here is noise.
           The real content was vetted by MSAN_CHECK_CSTR at putstr;
           mark the copy clean so the obs funnel only reports truth. */
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
        __msan_unpoison(m, NLE_MESSAGE_SIZE);
#endif
#endif
    } else if (ttyDisplay->toplin) {
        // Copy toplines[], see topl.c.
        src = toplines;
        hi = copy_toplines(m, hi);
    } else {
        zero_message(m, hi);
        hi = false;
    }
    msg_lo_ = hi ? nullptr : m;
    (void) src;
}

inline __attribute__((always_inline)) void
NetHackRL::fill_obs_state(nle_obs *obs)
{
    if (obs->prog_state) {
        obs->prog_state[0] = program_state.gameover;
        obs->prog_state[1] = program_state.panicking;
        obs->prog_state[2] = program_state.exiting;
        obs->prog_state[3] = program_state.in_moveloop;
        obs->prog_state[4] = program_state.in_impossible;
        obs->prog_state[5] = program_state.something_worth_saving;
        // TODO: Consider adding something_worth_saving.
        // Also consider adding ttyDisplay->inmore ...
    }
    if (obs->misc) {
        obs->misc[0] = in_yn_function;
        obs->misc[1] = in_getlin;
        obs->misc[2] = xwaitingforspace;
    }
}

inline __attribute__((always_inline)) void
NetHackRL::fill_obs_internal(nle_obs *obs)
{
    const boolean nle_partial =
        obs->partial && program_state.in_moveloop && !program_state.gameover;
    if (obs->internal && !nle_partial) {
        // From do.c. sstairs is a potential "special" staircase.
        boolean stairs_down =
            ((u.ux == xdnstair && u.uy == ydnstair)
             || (u.ux == sstairs.sx && u.uy == sstairs.sy && !sstairs.up));

        obs->internal[0] = deepest_lev_reached(false);
        obs->internal[1] = in_yn_function;
        obs->internal[2] = in_getlin;
        obs->internal[3] = xwaitingforspace;
        obs->internal[4] = stairs_down;
        obs->internal[5] = 0; /* was prayer cooldown (u.ublesscnt): a hidden counter with no public signal,
                                 unread by the policy, removed from the export 2026-09-10 */
        /* engraving underfoot: 2 = active Elbereth, 1 = other (was: dead 0
         * since fork — the obs feature read constant zero). Bit 2 = engulfed,
         * consumed env-side for legality masking, stripped before the obs. */
        {
            int eb = sengr_at("Elbereth", u.ux, u.uy, TRUE) ? 2 : (engr_at(u.ux, u.uy) ? 1 : 0);
            // Blind: the flag is what the hero last knew about the engraving on this square (read, felt or written),
            // unread-since-written or wiped-since-read capped at 1, and nothing at all on a square it never read there.
            // Sighted, a look reads the floor at zero cost, so the truth is public.
            // Hallucination conceals exactly as blindness does. The "sighted" branch below rests on a look being free, but a
            // look while hallucinating names what it finds with random_obj_to_glyph and so DRAWS from the display RNG -- a
            // reconstruction cannot spend those draws without desyncing, so the truth is not publicly recoverable here.
            if (!youmonst.data) { // no hero yet (the name prompt after an ignored options file): nothing to remember
                eb = 0;
            } else if (Blind || Hallucination) {
                if (u.ux == u.nle_engr_bx && u.uy == u.nle_engr_by) { eb = u.nle_engr_blind ? 1 : u.nle_engr_last; if (u.nle_engr_wiped && eb > 1) eb = 1; }
                else eb = 0;
            } else { // sighted: the truth is public (a look reads the floor), so it is also what the hero will remember once blinded here
                u.nle_engr_bx = u.ux; u.nle_engr_by = u.uy; u.nle_engr_last = eb; u.nle_engr_blind = 0; u.nle_engr_wiped = 0;
            }
            obs->internal[6] = eb | (u.uswallow ? 4 : 0);
        }
        obs->internal[7] = u.uhunger;
        obs->internal[8] =
            u.urexp; /* score (careful! check botl_score() and end.c) */
    }
}

/* a game not yet started (or windows gone): zero observations; the message is the caller's */
void
NetHackRL::fill_obs_blank(nle_obs *obs)
{
    obs->in_normal_game = false;
    if (obs->glyphs)
        std::fill_n(obs->glyphs, (COLNO - 1) * ROWNO, nul_glyph);
    if (obs->chars)
        std::memset(obs->chars, 0, sizeof chars_); /* Or fill with ' '? */
    if (obs->colors)
        std::memset(obs->colors, 0, sizeof colors_);
    if (obs->specials)
        std::memset(obs->specials, 0, sizeof specials_);
    if (obs->blstats)
        std::memset(obs->blstats, 0, sizeof(long) * NLE_BLSTATS_SIZE);
    if (obs->screen_descriptions)
        std::memset(obs->screen_descriptions, 0, sizeof screen_descriptions_);
}

void
NetHackRL::fill_obs(nle_obs *obs)
{
    fill_obs_state(obs);
    /* a partial fill in normal play (nle_partial below) adds only the message: it skips the full fill's frame */
    if (obs->partial && program_state.in_moveloop && !program_state.gameover && iflags.window_inited) {
        obs->in_normal_game = true;
        copy_message(obs);
        return;
    }
    fill_obs_full(obs);
}


/* fill_obs past the program state and misc */
void __attribute__((noinline))
NetHackRL::fill_obs_full(nle_obs *obs)
{
    uint32_t glyph_rows = ~0u; /* declared before any goto below */
    int ncell = -1; /* >= 0: the listed cells are every port store since the last full fill, into the same buffers under the same
                       discoveries, so the other cells of the dirty rows already hold their values */
    /* Opt-in cheap fill for the intermediate keystrokes of a multi-key
       wrapper step: only prog_state/misc/message are exported. Honored
       only for healthy in-game fills so boot and gameover fills (and any
       consumer that never sets obs->partial, e.g. golden replays) are
       byte-identical to stock. */
    const boolean nle_partial =
        obs->partial && program_state.in_moveloop && !program_state.gameover;
    fill_obs_internal(obs);

    if ((!program_state.something_worth_saving && !program_state.in_moveloop)
        || !iflags.window_inited) {
        // Game not yet started (!something_worth_saving && !in_moveloop -- we
        // need both as something_worth_saving also becomes false in
        // really_done(), but we still want to see the "Do you want..."
        // questions) or windows have already been destroyed. Return zero
        // observations.
        fill_obs_blank(obs);
        if (obs->message) {
            zero_message(obs->message, obs->message != msg_lo_);
            msg_lo_ = obs->message;
        }
        dirty_rows_ = ~0u; /* obs no longer mirrors the display arrays */
        glyph_export_rows_ = ~0u;
        glyph_export_acc_ = ~0u;
        return;
    }
    obs->in_normal_game = true;

    if (nle_partial)
        goto fill_message; /* dirty_rows_/last_*_buf_ keep accumulating */

    if (mapped_stale_ && (obs->chars || obs->colors || obs->specials))
        map_resolve();
    {
        bool same_bufs = obs->glyphs == last_glyphs_buf_
                         && obs->chars == last_chars_buf_
                         && obs->colors == last_colors_buf_
                         && obs->specials == last_specials_buf_;
        uint32_t dirty = same_bufs ? dirty_rows_ : ~0u;
        ncell = cell_n_ <= CELLS && !(dirty >> ROWNO) ? cell_n_ : -1;
        last_glyphs_buf_ = obs->glyphs;
        last_chars_buf_ = obs->chars;
        last_colors_buf_ = obs->colors;
        last_specials_buf_ = obs->specials;
        if (dirty) {
            constexpr size_t W = COLNO - 1;
            for (uint32_t m = (obs->chars || obs->colors || obs->specials) ? dirty & ((1u << ROWNO) - 1u) : 0; m; m &= m - 1) {
                size_t off = (size_t) __builtin_ctz(m) * W;
                if (obs->chars)
                    std::memcpy(obs->chars + off, chars_.data() + off, W);
                if (obs->colors)
                    std::memcpy(obs->colors + off, colors_.data() + off, W);
                if (obs->specials)
                    std::memcpy(obs->specials + off, specials_.data() + off,
                                W);
            }
            dirty_rows_ = 0;
            cell_n_ = 0;
        }
        /* Glyph rows to rewrite: rows the port printed since the last fill (dirty), never show_glyph() writes still
           pending a flush (stock's observation is its port array); every row when the buffers changed or the
           discoveries list did, because maybe_true_glyph depends on objects[].oc_name_known and the mapping is not
           idempotent. Untouched rows keep the previous fill's values, computed from the same port glyphs and the same
           discoveries. */
        glyph_rows = dirty;
        if (disc_gen_seen_ != disc_gen_) {
            glyph_rows = ~0u;
            disc_gen_seen_ = disc_gen_;
            ncell = -1;
        }
    }
    if (obs->glyphs) { // identity mapping on the rewritten rows: the port's map holds true glyphs;
        // maybe_true_glyph hides the ones the player has not identified.
        constexpr int W = COLNO - 1;
        glyph_rows &= (1u << ROWNO) - 1u;
        for (int k = 0; k < ncell; k++) {
            int x = cell_at_[k] & 0xff, y = cell_at_[k] >> 8;
            obs->glyphs[y * W + x - 1] = maybe_true_glyph(nle_port_glyph_at(x, y));
        }
        if (ncell < 0)
            nle_copy_glyph_rows(obs->glyphs, glyph_rows);
        for (uint32_t m = ncell < 0 ? glyph_rows : 0; m; m &= m - 1) {
            int i = __builtin_ctz(m) * W, end = i + W;
#ifdef __AVX2__
            const __m256i lo = _mm256_set1_epi16(GLYPH_OBJ_OFF - 1);
            const __m256i hi = _mm256_set1_epi16(GLYPH_OBJ_OFF + NUM_OBJECTS);
            auto map16 = [&](int b, unsigned int lanes) { /* the object glyphs among 16 cells from b, in lanes */
                const __m256i g = _mm256_loadu_si256((const __m256i *) (obs->glyphs + b));
                unsigned int mask = (unsigned int) _mm256_movemask_epi8(_mm256_and_si256(
                    _mm256_cmpgt_epi16(g, lo), _mm256_cmpgt_epi16(hi, g))) & lanes;
                while (mask) {
                    unsigned int bit = __builtin_ctz(mask);
                    int j = b + bit / 2;
                    obs->glyphs[j] = maybe_true_glyph(obs->glyphs[j]);
                    mask &= ~(3U << (bit & ~1U));
                }
            };
            static_assert(W >= 16, "row narrower than a vector");
            for (; i + 16 <= end; i += 16)
                map16(i, 0xFFFFFFFFu);
            if (i < end) { /* the row's last 16 cells, minus those mapped above */
                map16(end - 16, 0xFFFFFFFFu << 2 * (i - (end - 16)));
                i = end;
            }
#elif defined(__SSE2__)
            const __m128i lo = _mm_set1_epi16(GLYPH_OBJ_OFF - 1);
            const __m128i hi = _mm_set1_epi16(GLYPH_OBJ_OFF + NUM_OBJECTS);
            auto map8 = [&](int b, unsigned int lanes) { /* the object glyphs among 8 cells from b, in lanes */
                const __m128i g = _mm_loadu_si128((const __m128i *) (obs->glyphs + b));
                unsigned int mask = _mm_movemask_epi8(_mm_and_si128(
                    _mm_cmpgt_epi16(g, lo), _mm_cmpgt_epi16(hi, g))) & lanes;
                while (mask) {
                    unsigned int bit = __builtin_ctz(mask);
                    int j = b + bit / 2;
                    obs->glyphs[j] = maybe_true_glyph(obs->glyphs[j]);
                    mask &= ~(3U << (bit & ~1U));
                }
            };
            static_assert(W >= 8, "row narrower than a vector");
            for (; i + 8 <= end; i += 8)
                map8(i, 0xFFFFu);
            if (i < end) { /* the row's last 8 cells, minus those mapped above */
                map8(end - 8, 0xFFFFu << 2 * (i - (end - 8)));
                i = end;
            }
#endif
            for (; i < end; i++)
                obs->glyphs[i] = maybe_true_glyph(obs->glyphs[i]);
        }
        glyph_export_rows_ = glyph_rows;
        /* The underfoot patch below overwrites the hero's cell with terrain/object instead of the
           screen glyph. The old full copy repaired that cell once the hero moved on; with row copies
           the previous patched cell must be restored from the screen buffer explicitly unless its
           row was just recopied. Report its row so incremental consumers re-read it. */
        if (underfoot_prev_ >= 0) {
            int py = underfoot_prev_ / W, px = underfoot_prev_ % W + 1;
            if (ncell >= 0 || !(glyph_rows & (1u << py)))
                obs->glyphs[underfoot_prev_] = maybe_true_glyph(nle_port_glyph_at(px, py));
            glyph_export_rows_ |= 1u << py;
            underfoot_prev_ = -1;
        }
    }
    if (obs->glyphs && nle_underfoot_glyphs() && u.ux >= 1 && u.ux < COLNO
        && u.uy >= 0 && u.uy < ROWNO) {
        // Hero tile shows what the hero stands on (top object, else terrain)
        // instead of the hero glyph. Patched into obs->glyphs only, after the
        // dirty-row copy so it always wins; recomputed every fill.
        // Conceal-first, and note the ORDER matters: obj_to_glyph() resolves to random_obj_to_glyph() while hallucinating,
        // which DRAWS from the display RNG. Evaluating it before the conceal checks made this observation channel consume
        // randomness every frame a hallucinating hero stood on an object -- unreproducible from outside, so a replay drifts
        // for the rest of the game. Hallucination now conceals exactly as blindness does: a hallucinating hero cannot
        // identify what is underfoot any more than a blind one can, and the screen glyph needs no draw.
        int g;
        if (u.uswallow) {
            g = nle_port_glyph_at(u.ux, u.uy); // engulfed: nothing under the hero can be inspected from inside
        } else if (Blind || Hallucination) {
            g = nle_port_glyph_at(u.ux, u.uy); // the port's map at the hero's square, as stock shows it
        } else {
            struct obj *under = NH_OBJS_AT(u.ux, u.uy);
            g = under ? obj_to_glyph(under, rn2_on_display_rng) : back_to_glyph(u.ux, u.uy);
        }
        obs->glyphs[(size_t) u.uy * (COLNO - 1) + (u.ux - 1)] =
            maybe_true_glyph(g);
        glyph_export_rows_ |= 1u << u.uy;
        underfoot_prev_ = (int) ((size_t) u.uy * (COLNO - 1) + (u.ux - 1));
    }
    glyph_export_acc_ |= glyph_export_rows_;
fill_message:
    copy_message(obs);
    if (obs->blstats && !nle_partial) {
        /* Both modes read the blstats_ cache here; it is pumped at bot()/
           timebot() time — by the status pipeline when status_updates is
           on, by nle_rl_bot_direct (botl.c) when it is off — so staleness
           semantics (e.g. energy regen between the last bot() and the
           observation point) are identical in both modes. */
        if (!u.dz) {
            /* Tricky hack: On "You descend the stairs.--More--" we are
               technically on the next floor, but we don't see it yet.
               But x, y needs to be updated at every step (not just when
               blstats changes for other reasons). But if we update it
               on the descend message, it will be the new position.
               u.dz stays nonzero for the env step after, too, but there
               blstats will be updated. */
            blstats_[NLE_BL_X] = u.ux - 1; /* x coordinate, 1 <= ux <= cols */
            blstats_[NLE_BL_Y] = u.uy;     /* y coordinate, 0 <= uy < rows */
            blstats_[NLE_BL_TIME] = moves;
        }
        std::memcpy(obs->blstats, &blstats_[0], sizeof(blstats_));
    }
    if (obs->inv_glyphs && !nle_partial) {
        /* mapped with the current discoveries, as the map is: an item reads as identified once its type is, not at the
           next inventory update (none comes while the game is over) */
        if (inv_shown_gen_ != disc_gen_) {
            int i = 0;
            for (; i < inv_cache_n_; ++i)
                inv_glyphs_shown_[i] = (short) shuffled_glyph(inv_glyphs_cache_[i]);
            std::fill(inv_glyphs_shown_.begin() + i, inv_glyphs_shown_.end(), (short) shuffled_glyph(NO_GLYPH));
            inv_shown_gen_ = disc_gen_;
        }
        std::memcpy(obs->inv_glyphs, inv_glyphs_shown_.data(), sizeof(short) * NLE_INVENTORY_SIZE);
    }
    if (obs->inv_strs && !nle_partial) {
        int i = 0;
        for (const rl_inventory_item &item : inventory_) {
            int j = 0;
            for (int size = min(item.str.size(), NLE_INVENTORY_STR_LENGTH);
                 j < size; ++j) {
                obs->inv_strs[i++] = item.str[j];
            }
            for (; j < NLE_INVENTORY_STR_LENGTH; ++j) {
                obs->inv_strs[i++] = 0;
            }
        }
        for (; i < NLE_INVENTORY_SIZE * NLE_INVENTORY_STR_LENGTH; ++i) {
            obs->inv_strs[i] = 0;
        }
    }
    if (obs->inv_letters && !nle_partial) {
        std::memcpy(obs->inv_letters, inv_letters_cache_.data(),
                    NLE_INVENTORY_SIZE);
    }
    if (obs->inv_oclasses && !nle_partial) {
        std::memcpy(obs->inv_oclasses, inv_oclasses_cache_.data(),
                    NLE_INVENTORY_SIZE);
    }
    if (!nle_partial) {
        /* One walk of the inventory chain for three exports. Identification-gated per-slot state: exactly what doname would
           print (objnam.c) — BUC only if bknown (never for gold), spe or charges only if known on classes that display them,
           erosion, poison, grease and worn markers always visible, fooproof only if rknown, typeknown when the appearance's
           identity is discovered. True glyph only once discovered AND this instance was seen. Perceived weight, nle_weight()'s
           public pricing: the true type only while its name is displayed (doname's dknown && oc_name_known), otherwise the
           appearance's canonical type (shuffled_glyph), containers empty, partly eaten food half. Flag reads only, no RNG, so
           goldens (which leave these obs unbound) see an identical stream. Same walk order as inventory_. */
        int i, n;
        long w = 0;
        struct obj *otmp;
        struct nh_obs_cache *c = &current_nle_ctx->obs_cache;
        bool keep = inv_memo_n_ >= 0 && obs->inv_state == inv_memo_ist_ && obs->inv_true_glyphs == inv_memo_itg_;
        const bool ocg_same = inv_memo_gen_ == disc_gen_;
        /* clean: same chain (nh_invgen), no export-field write to a carried object since the last walk (nh_inv_touch), same
           discoveries and rows: the rows and the weight are still the last walk's */
        const bool clean = keep && ocg_same && !c->inv_dirty && inv_walk_gen_ == nh_invgen;
        if (!clean) {
        const unsigned short *ocgt = nle_ocg();
        inv_walk_n_ = keep ? inv_memo_n_ : 0;
        inv_walk_stale_ = keep ? inv_memo_slots_ : NLE_INVENTORY_SIZE;
        for (otmp = invent, i = 0; otmp; otmp = otmp->nobj, ++i)
            w += i < inv_walk_n_ && inv_same(ocgt, otmp, i, ocg_same) ? inv_memo_[i].w : inv_export_item(obs, otmp, i);
        n = i < NLE_INVENTORY_SIZE ? i : NLE_INVENTORY_SIZE;
        {
            int stale = inv_walk_stale_;
            if (obs->inv_state && stale > n)
                std::memset(obs->inv_state + n * NLE_INV_STATE_FIELDS, 0, (stale - n) * NLE_INV_STATE_FIELDS);
            if (obs->inv_true_glyphs)
                for (int j = n; j < stale; ++j)
                    obs->inv_true_glyphs[j] = NO_GLYPH;
        }
        inv_memo_n_ = i < 64 ? i : 64;
        inv_memo_slots_ = n;
        inv_memo_ist_ = obs->inv_state;
        inv_memo_itg_ = obs->inv_true_glyphs;
        inv_memo_gen_ = disc_gen_;
        c->wt = (int) w;
        }
        c->wt_valid = 1;
        c->inv_dirty = 0;
        inv_walk_gen_ = nh_invgen;
    }
    if (obs->screen_descriptions && !nle_partial) {
        memcpy(obs->screen_descriptions, &screen_descriptions_,
               screen_descriptions_.size());
    }
}


/* An item gone from slot j (by -1) or new at slot j (by 1): the memo slots above it and their export rows move with their
   items. Only while every memo slot has its export row. */
void
NetHackRL::inv_shift(nle_obs *obs, int j, int by)
{
    int from = by > 0 ? j : j + 1, cnt = inv_walk_n_ - from;

    std::memmove(&inv_memo_[from + by], &inv_memo_[from], cnt * sizeof inv_memo_[0]);
    if (obs->inv_state)
        std::memmove(obs->inv_state + (from + by) * NLE_INV_STATE_FIELDS, obs->inv_state + from * NLE_INV_STATE_FIELDS,
                     cnt * NLE_INV_STATE_FIELDS);
    if (obs->inv_true_glyphs)
        std::memmove(obs->inv_true_glyphs + from + by, obs->inv_true_glyphs + from, cnt * sizeof(short));
    inv_walk_n_ += by;
    if (inv_walk_n_ > inv_walk_stale_)
        inv_walk_stale_ = inv_walk_n_;
}

/* fill_obs's inventory export of the item at slot i when its memo slot does not hold it: the item after a removed one
   (the next memo slot holds it) shifts the memo down; an item before which one was inserted holds the next item's old
   slot, so the memo shifts up and the new item is computed. Returns the item's weight. */
long
NetHackRL::inv_export_item(nle_obs *obs, struct obj *otmp, int i)
{
    const unsigned short *ocgt = nle_ocg();
    const char *key = reinterpret_cast<const char *>(otmp) + offsetof(struct obj, quan);
    unsigned ocg = ocgt[otmp->otyp];

    if (i < inv_walk_n_ && inv_walk_n_ <= NLE_INVENTORY_SIZE) {
        if (i + 1 < inv_walk_n_ && inv_same(ocgt, otmp, i + 1, false)) {
            inv_shift(obs, i, -1);
            return inv_memo_[i].w;
        }
        if (inv_walk_n_ < NLE_INVENTORY_SIZE && otmp->nobj && inv_same(ocgt, otmp->nobj, i, false))
            inv_shift(obs, i, 1);
    }
    long wi;
    int oc = otmp->oclass;
    int nkn = (int) (ocg & 1); /* nle_ocg_known(): the bitset is rebuilt from this entry */
    int nk = (int) otmp->dknown & nkn;
    if (i < NLE_INVENTORY_SIZE && obs->inv_state) {
        signed char *st = obs->inv_state + i * NLE_INV_STATE_FIELDS;
        /* v2.3d: branch-free forms of the same fields (0/1 flags combined bitwise) */
        int weptool = (oc == TOOL_CLASS) & (object_defaults[otmp->otyp].oc_skill != P_NONE);
        int buc = 2 + (int) otmp->blessed - (int) otmp->cursed * (1 + (int) otmp->blessed); /* cursed 1, blessed 3, else 2 */
        st[0] = (signed char) (buc & -((int) otmp->bknown & (oc != COIN_CLASS)));
        int shows_spe = (oc == WEAPON_CLASS) | (oc == ARMOR_CLASS) | (oc == RING_CLASS) | (oc == WAND_CLASS) | weptool
                        | (object_defaults[otmp->otyp].oc_charged != 0);
        int spm = -((int) otmp->known & shows_spe);
        int s1 = ((int) otmp->spe & spm) | (-128 & ~spm);
        s1 &= -(int) !((oc == RING_CLASS) & !nk);
        st[1] = (signed char) s1;
        st[2] = otmp->quan > 127 ? 127 : (signed char) otmp->quan;
        /* erosion bits are overloaded storage on other classes
           (orotten food, odiluted potions, norevive corpses) and doname
           only calls add_erosion_words from its weapon/armor/weptool
           and ball/chain branches — exporting the bits outside those
           classes would leak hidden state (e.g. rotten food). */
        int erodible = (oc == WEAPON_CLASS) | (oc == ARMOR_CLASS) | weptool | (oc == BALL_CLASS) | (oc == CHAIN_CLASS);
        st[3] = (signed char) ((int) otmp->oeroded & -erodible);
        st[4] = (signed char) ((int) otmp->oeroded2 & -erodible);
        {
            long wm = otmp->owornmask;
            st[5] = (signed char) ((int) ((wm & (W_ARMOR | W_ACCESSORY | W_SADDLE)) != 0)
                                   | ((int) ((wm & W_WEP) != 0) << 1)
                                   | ((int) ((wm & W_SWAPWEP) != 0) << 2)
                                   | ((int) ((wm & W_QUIVER) != 0) << 3)
                                   | (((int) otmp->opoisoned & !Is_container(otmp)) << 4)
                                   | ((int) otmp->greased << 5)
                                   | ((erodible & (int) otmp->rknown & (int) otmp->oerodeproof) << 6));
        }
        st[6] = (signed char) nk;
        st[7] = 0;
    }
    if (i < NLE_INVENTORY_SIZE && obs->inv_true_glyphs) {
        int tm = -nk; /* v2.3d: select without a branch */
        obs->inv_true_glyphs[i] = ((GLYPH_OBJ_OFF + otmp->otyp) & tm) | (NO_GLYPH & ~tm);
    }
    if (oc == COIN_CLASS) {
        wi = (otmp->quan + 50L) / 100L;
    } else {
        long q = otmp->quan, base;
        if (otmp->otyp == CORPSE && otmp->corpsenm >= 0)
            base = mons[otmp->corpsenm].cwt;
        else {
            int typ = otmp->otyp;
            if (!nk) /* the mirror entry only for items whose true name is not on display */
                typ = nkn ? (int) (ocg >> 1) : nle_appearance_canon[ocg >> 1];
            base = object_defaults[typ].oc_weight;
        }
        base *= q;
        if (otmp->oeaten)
            base /= 2;
        wi = base;
    }
    if (i < 64) {
        inv_key &k = inv_memo_[i];
        int cn = otmp->otyp == CORPSE ? otmp->corpsenm : 0;
        std::memcpy(k.blk, key, KEYB);
        k.cnm = cn == (short) cn && cn != NO_CNM && wi == (int) wi ? (short) cn : NO_CNM;
        k.ocg = (unsigned short) ocg;
        k.w = (int) wi;
    }
    return wi;
}

inline __attribute__((always_inline)) int
NetHackRL::getch_key(int i)
{
    /* NOT calling tty_nhgetch() but instead getting the input from
       the context switch. No stdin required. The following code is from
       tty_nhgetch. */
    if (WIN_MESSAGE != WIN_ERR && wins[WIN_MESSAGE])
        wins[WIN_MESSAGE]->wflags &= ~WIN_STOP;
    if (!i)
        i = '\033'; /* map NUL to ESC since nethack doesn't expect NUL */
    else if (i == EOF)
        i = '\033'; /* same for EOF */
    if (ttyDisplay && ttyDisplay->toplin == 1)
        ttyDisplay->toplin = 2;
    DEBUG_API("getch_method: action=" << i << ", xwaitingforspace="
                                      << xwaitingforspace << std::endl);
    return i;
}

int
NetHackRL::getch_method()
{
    fill_obs(nle_get_obs());
    return getch_key(((nle_obs *) nle_yield(TRUE))->action);
}

/* a key with no window port (an options error waits for one before init_nhwindows): stock's fill_obs on its null instance
   reaches only the not-started branch, which reads no member */
int
NetHackRL::getch_early()
{
    nle_obs *obs = nle_get_obs();

    fill_obs_state(obs);
    fill_obs_internal(obs);
    fill_obs_blank(obs);
    if (obs->message)
        std::memset(obs->message, 0, NLE_MESSAGE_SIZE);
    return getch_key(((nle_obs *) nle_yield(TRUE))->action);
}


/* What stock's per-item doname() leaves behind besides the string: xname's knowledge flags, and RNG draws while
   hallucinating (currency() of unpaid goods, glow_color() of a warning weapon, the name of a leashed monster), so the
   real doname runs then and for a leash tied to a monster. The name-buffer rotation is not reproduced here: the one
   reached reader of a name held across an export replays it (rl_replay_export_ring). NH_DONAME_VERIFY runs doname()
   after it and reports any flag or RNG change it would still make. */

static void
doname_effects(struct obj *otmp)
{
    if (Hallucination || (otmp->otyp == LEASH && otmp->leashmon)) {
        (void) doname(otmp);
        return;
    }
    if (object_defaults[otmp->otyp].oc_uses_known && object_defaults[otmp->otyp].oc_unique
        && !objects[otmp->otyp].oc_name_known && otmp->known)
        otmp->known = 0, nh_inv_touch(otmp);
    if (!Blind && !otmp->dknown)
        otmp->dknown = 1, nh_inv_touch(otmp);
    if (Role_if(PM_PRIEST) && !otmp->bknown)
        otmp->bknown = 1, nh_inv_touch(otmp);
    /* xname names a known tin through tin_details, whose tin_variety draws rn2(TTSZ - 1) on the core RNG for an empty tin */
    if (otmp->otyp == TIN && otmp->known)
        tin_details(otmp, otmp->corpsenm, (char *) 0);
    /* doname quotes an unpaid item's price: unpaid_cost reaches the shopkeeper through shop_keeper(), which riles or pacifies
       it (the anger surcharge on every billed item) */
    if (!iflags.suppress_price && !restoring && is_unpaid(otmp))
        (void) unpaid_cost(otmp, TRUE);
}

void
NetHackRL::update_inventory_method()
{
    /* We cannot simply call display_inventory() as window.doc suggests,
       since we want to also use the tty window proc and we don't want the
       inventory to pop up whenever it changed. Instead, we keep our inventory
       list up to date via the following code adopted from display_pickinv
       in invent.c */

    struct obj *otmp;
    inventory_.clear();

    /* The glyph, letter and class caches are kept whatever the bindings, as stock keeps its item list, so a buffer bound
       later shows stock's; the doname/let_to_name string formatting (which drags the whole singplur/strncmpi naming chain
       behind it) is only paid when inv_strs itself is bound (nle_step refuses a later inv_strs binding). */
    const nle_obs *o = nle_get_obs();
    const bool want_strs = !o || o->inv_strs;

    ring_before_ = !want_strs && !Hallucination ? nh_obuf_index() : -1;
    if (!want_strs) {
        /* glyphs, letters and classes go straight into the caches: the item list (two strings per item, a cold heap
           vector per game) is only needed for inv_strs. Same walk, same obj_to_glyph draws. */
        inv_glyphs_cache_.fill(NO_GLYPH);
        inv_shown_gen_ = 0;
        inv_letters_cache_.fill(0);
        inv_oclasses_cache_.fill(MAXOCLASSES);
        size_t n = 0;
        for (otmp = invent; otmp; otmp = otmp->nobj) {
            auto glyph = obj_to_glyph(otmp, rn2_on_display_rng); /* true glyph: fill_obs maps it */
            doname_effects(otmp);
            if (n >= (size_t) NLE_INVENTORY_SIZE)
                continue;
            inv_glyphs_cache_[n] = glyph;
            inv_letters_cache_[n] = otmp->invlet;
            inv_oclasses_cache_[n] = otmp->oclass;
            ++n;
        }
        inv_cache_n_ = (int) n;
        return;
    }

    for (otmp = invent; otmp; otmp = otmp->nobj) {
        auto glyph = obj_to_glyph(otmp, rn2_on_display_rng); /* true glyph: fill_obs maps it */
        inventory_.emplace_back(rl_inventory_item{ glyph, std::string(doname(otmp)), otmp->invlet, otmp->oclass,
                                                   std::string(let_to_name(otmp->oclass, false, false)) });
    }
    inv_glyphs_cache_.fill(NO_GLYPH);
    inv_shown_gen_ = 0;
    inv_letters_cache_.fill(0);
    inv_oclasses_cache_.fill(MAXOCLASSES);
    size_t count = min(inventory_.size(), (size_t) NLE_INVENTORY_SIZE);
    for (size_t i = 0; i < count; ++i) {
        inv_glyphs_cache_[i] = inventory_[i].glyph;
        inv_letters_cache_[i] = inventory_[i].letter;
        inv_oclasses_cache_[i] = inventory_[i].object_class;
    }
    inv_cache_n_ = (int) count;
}

void
NetHackRL::store_glyph(XCHAR_P x, XCHAR_P y, int glyph)
{
    unsigned n = cell_n_;

    nle_port_glyph_store(x, y % ROWNO, glyph);
    dirty_rows_ |= 1u << (y % ROWNO);
    if (n < CELLS && (unsigned) (x - 1) < COLNO - 1 && (unsigned) y < ROWNO)
        cell_at_[n] = (uint16_t) (y << 8 | x), cell_n_ = n + 1;
    else
        cell_n_ = CELLS + 1;
}

void
NetHackRL::store_mapped_glyph(int ch, int color, int special, XCHAR_P x,
                              XCHAR_P y)
{
    // 1 <= x < cols, 0 <= y < rows (!)
    size_t i = (x - 1) % (COLNO - 1);
    size_t j = y % ROWNO;
    size_t offset = j * (COLNO - 1) + i;

    chars_[offset] = ch;
    colors_[offset] = color;
    specials_[offset] = special;
    dirty_rows_ |= 1u << j;
}

/* the glyphs whose mapglyph reads the floor for the object-pile mark */
static inline bool
pile_glyph(int glyph)
{
    return (glyph >= GLYPH_BODY_OFF && glyph < GLYPH_RIDDEN_OFF) || glyph >= GLYPH_STATUE_OFF
           || (glyph >= GLYPH_OBJ_OFF && glyph < GLYPH_CMAP_OFF && glyph != GLYPH_OBJ_OFF + BOULDER);
}

/* a print while the mapped arrays are skipped is mapped at the next bound fill (map_resolve) from its glyph and what mapglyph
   reads besides it now: the object-pile mark, the hero's square, Upolyd (cellf_) */
static inline unsigned
map_context(int x, int y, int glyph)
{
    unsigned f = 1;

    if (x == u.ux && y == u.uy)
        f |= Upolyd ? 12 : 4;
    if (pile_glyph(glyph) && !Hallucination && level.objs[x][y] && level.objs[x][y]->nexthere)
        f |= 2;
    return f;
}

/* the mapped arrays as stock's: the clear's blank screen, then every deferred cell as its print would have mapped it */
void
NetHackRL::map_resolve()
{
    if (mapped_stale_ & 2)
        map_fill();
    for (int y = 0; (mapped_stale_ & 1) && y < ROWNO; ++y)
        for (int x = 1; x < COLNO; ++x)
            if (unsigned f = cellf_[y][x]) {
                int ch, color, glyph = nle_port_glyph_at(x, y);
                unsigned special;

                (void) mapglyph_as(glyph, &ch, &color, &special, x, y, f >> 1 & 1, f >> 2 & 1, f >> 3 & 1);
                if (glyph != nul_glyph && color == CLR_BLACK)
                    color = iflags.wc2_darkgray ? 8 : CLR_BLUE;
                store_mapped_glyph(ch, color, special, x, y);
                cellf_[y][x] = 0;
            }
    mapped_stale_ = 0;
}

void
NetHackRL::store_screen_description(XCHAR_P x, XCHAR_P y, int glyph)
{
    // 1 <= x < cols, 0 <= y < rows (!)
    size_t i = (x - 1) % (COLNO - 1);
    size_t j = y % ROWNO;
    size_t offset = j * (COLNO - 1) + i;
    size_t start = offset * NLE_SCREEN_DESCRIPTION_LENGTH;

    // see code in src/do_name.c:538 auto_describe
    coord cc;
    int sym = 0;
    char tmpbuf[BUFSZ];
    const char *firstmatch = "unknown";

    cc.x = x;
    cc.y = y;

    if (do_screen_description(cc, TRUE, sym, tmpbuf, &firstmatch,
                              (struct permonst **) 0)) {
        strncpy((char *) &screen_descriptions_ + start, firstmatch,
                NLE_SCREEN_DESCRIPTION_LENGTH);
    } else {
        strncpy((char *) &screen_descriptions_ + start, "",
                NLE_SCREEN_DESCRIPTION_LENGTH);
    }
}

void
NetHackRL::update_blstats(bool quiet)
{
    int hitpoints;

    /* See botl.c. */
    int i = Upolyd ? u.mh : u.uhp;
    if (i < 0)
        i = 0;

    hitpoints = min(i, 9999);

    int max_hitpoints;
    i = Upolyd ? u.mhmax : u.uhpmax;
    max_hitpoints = min(i, 9999);

    /* Cf. botl.c. */
    blstats_[NLE_BL_X] = u.ux - 1;     /* x coordinate, 1 <= ux <= cols */
    blstats_[NLE_BL_Y] = u.uy;         /* y coordinate, 0 <= uy < rows */
    acurr_status(&blstats_[NLE_BL_STR125]); /* strength 3..125, dexterity, constitution, intelligence, wisdom, charisma */
    blstats_[NLE_BL_STR25] = acurrstr_of((int) blstats_[NLE_BL_STR125]); /* strength 3..25 */
    blstats_[NLE_BL_SCORE] = botl_score();         /* score             */
    blstats_[NLE_BL_HP] = hitpoints;               /* hitpoints         */
    blstats_[NLE_BL_HPMAX] = max_hitpoints;        /* max_hitpoints     */
    blstats_[NLE_BL_DEPTH] = depth(&u.uz);         /* depth             */
    blstats_[NLE_BL_GOLD] = nh_money_invent();     /* gold              */
    blstats_[NLE_BL_ENE] = min(u.uen, 9999);       /* energy            */
    blstats_[NLE_BL_ENEMAX] = min(u.uenmax, 9999); /* max_energy        */
    blstats_[NLE_BL_AC] = u.uac;                   /* armor_class       */
    blstats_[NLE_BL_HD] = Upolyd ? (int) mons[u.umonnum].mlevel
                                 : 0;       /* monster level, hit-dice */
    blstats_[NLE_BL_XP] = u.ulevel;         /* experience level  */
    blstats_[NLE_BL_EXP] = u.uexp;          /* experience points */
    blstats_[NLE_BL_TIME] = moves;          /* time              */
    blstats_[NLE_BL_HUNGER] = u.uhs;        /* hunger state      */
    blstats_[NLE_BL_CAP] = quiet ? near_capacity_quiet() : near_capacity_hot(); /* carrying capacity */
    blstats_[NLE_BL_DNUM] = u.uz.dnum;      /* dungeon number */
    blstats_[NLE_BL_DLEVEL] = u.uz.dlevel;  /* level number */
    blstats_[NLE_BL_CONDITION] = condition_bits_; /* condition bit mask */
    blstats_[NLE_BL_ALIGN] = u.ualign.type;       /* character alignment */
}

void
NetHackRL::status_update_method(int fldidx, genericptr_t ptr, int,
                                int percent, int color,
                                unsigned long *colormasks)
{
    if ((fldidx < BL_RESET) || (fldidx >= MAXBLSTATS))
        return;

    // Needs to be kept in sync with the switch statement in rl_status_update.
    if (fldidx == BL_FLUSH || fldidx == BL_RESET) {
        update_blstats();
        return;
    } else if (fldidx == BL_CONDITION) {
        long *condptr = (long *) ptr;
        condition_bits_ = *condptr;
        blstats_[NLE_BL_CONDITION] = condition_bits_;
        return;
    }

    char *text = (char *) ptr;
    MSAN_CHECK_CSTR(text);
    std::string status(text);
    if (fldidx == BL_GOLD) {
        // Handle gold glyph.
        char buf[BUFSZ];
        status = decode_mixed(buf, text);
    }
    status_[fldidx] = status;
}

void
NetHackRL::putstr_method(winid wid, int attr, const char *str)
{
    DEBUG_API("About to set strings on " << wid << std::endl);
    MSAN_CHECK_CSTR(str);
    if (wid == WIN_MESSAGE)
        msg_last_.assign(str);
}

winid
NetHackRL::create_nhwindow_method(int type)
{
    std::string window_type;
    switch (type) {
    case NHW_MAP:
        window_type = "map";
        break;
    case NHW_MESSAGE:
        window_type = "message";
        break;
    case NHW_STATUS:
        window_type = "status";
        break;
    case NHW_MENU:
        window_type = "menu";
        break;
    case NHW_TEXT:
        window_type = "text";
        break;
    }

    DEBUG_API("rl_create_nhwindow(type=" << window_type << ")");

    winid wid = tty_create_nhwindow(type);
    DEBUG_API(": wid == " << wid << std::endl);

    windows_.resize(wid + 1);
    assert(!windows_[wid]);

    DEBUG_API("ABOUT TO RESET " << wid << std::endl;);

    windows_[wid].reset(new rl_window{ type });
    return wid;
}

void
NetHackRL::clear_nhwindow_method(winid wid)
{
    if (wid == WIN_MESSAGE)
        msg_last_.clear();
    if (wid == WIN_MAP) {
        const nle_obs *o = nle_get_obs();

        nle_port_glyph_clear();
        if (mapped_stale_ & 1)
            std::memset(cellf_, 0, sizeof cellf_);
        if (o->chars || o->colors || o->specials) {
            map_fill();
            mapped_stale_ = 0;
        } else
            mapped_stale_ = 2;
        dirty_rows_ = ~0u;
        if (o->screen_descriptions) {
            screen_descriptions_.fill(0);
        }
    }

    DEBUG_API("rl_clear_nhwindow(wid=" << wid << ")" << std::endl);
    tty_clear_nhwindow(wid);
}

void
NetHackRL::display_nhwindow_method(winid wid, BOOLEAN_P block)
{
    DEBUG_API("rl_display_nhwindow(wid=" << wid << ", block=" << block << ")"
                                         << std::endl);

    tty_display_nhwindow(wid, block);
}

void
NetHackRL::destroy_nhwindow_method(winid wid)
{
    DEBUG_API("rl_destroy_nhwindow(wid=" << wid << ")" << std::endl);
    windows_[wid].reset(nullptr);
    tty_destroy_nhwindow(wid);
}

void
NetHackRL::start_menu_method(winid wid)
{
    DEBUG_API("rl_start_menu(wid=" << wid << ")" << std::endl);
    tty_start_menu(wid);
}

void
NetHackRL::add_menu_method(
    winid wid,                  /* window to use, must be of type NHW_MENU */
    int glyph,                  /* glyph to display with item (not used) */
    const anything *identifier, /* what to return if selected */
    char ch,                    /* keyboard accelerator (0 = pick our own) */
    char gch,                   /* group accelerator (0 = no group) */
    int attr,                   /* attribute for string (like putstr()) */
    const char *str,            /* menu string */
    bool preselected            /* item is marked as selected */
)
{
    DEBUG_API("rl_add_menu" << std::endl);
    MSAN_CHECK_CSTR(str);
    tty_add_menu(wid, glyph, identifier, ch, gch, attr, str, preselected);
}

void
NetHackRL::rl_init_nhwindows(int *argc, char **argv)
{
    DEBUG_API("rl_init_nhwindows" << std::endl);
    tty_init_nhwindows(argc, argv);
    instance = new NetHackRL(*argc, argv);
}

void
NetHackRL::rl_player_selection()
{
    DEBUG_API("rl_player_selection" << std::endl);
    tty_player_selection();
    instance->player_selection_method();
}

void
NetHackRL::rl_askname()
{
    DEBUG_API("rl_askname" << std::endl);
    tty_askname();
}

void
NetHackRL::rl_get_nh_event()
{
    DEBUG_API("rl_get_nh_event" << std::endl);
    tty_get_nh_event();
}

void
NetHackRL::rl_exit_nhwindows(const char *c)
{
    DEBUG_API("rl_exit_nhwindows" << std::endl);
    delete instance;
    instance = nullptr;
    tty_exit_nhwindows(c);
}

void
NetHackRL::rl_suspend_nhwindows(const char *c)
{
    DEBUG_API("rl_suspend_nhwindows" << std::endl);
    tty_suspend_nhwindows(c);
}

void
NetHackRL::rl_resume_nhwindows()
{
    DEBUG_API("rl_resume_nhwindows" << std::endl);
    tty_resume_nhwindows();
}

winid
NetHackRL::rl_create_nhwindow(int type)
{
    // win_proc_calls code happens in method.
    return instance->create_nhwindow_method(type);
}

void
NetHackRL::rl_clear_nhwindow(winid wid)
{
    instance->clear_nhwindow_method(wid);
}

/* display_nhwindow(window, boolean blocking)
                -- Display the window on the screen.  If there is data
                   pending for output in that window, it should be sent.
                   If blocking is TRUE, display_nhwindow() will not
                   return until the data has been displayed on the screen,
                   and acknowledged by the user where appropriate.
                -- All calls are blocking in the tty window-port.
                -- Calling display_nhwindow(WIN_MESSAGE,???) will do a
                   --more--, if necessary, in the tty window-port. */
void
NetHackRL::rl_display_nhwindow(winid wid, BOOLEAN_P block)
{
    instance->display_nhwindow_method(wid, block);
}

void
NetHackRL::rl_destroy_nhwindow(winid wid)
{
    instance->destroy_nhwindow_method(wid);
}

void
NetHackRL::rl_curs(winid wid, int x, int y)
{
    DEBUG_API("rl_curs(wid=" << wid << ", x=" << x << ", y=" << y << ")"
                             << std::endl);
    DEBUG_API("rl_curs for window id " << wid << std::endl);
    tty_curs_hot(wid, x, y);
}

void
NetHackRL::rl_putstr(winid wid, int attr, const char *text)
{
    DEBUG_API("rl_putstr(wid=" << wid << ", attr=" << attr
                               << ", text=" << text << ")" << std::endl);
    instance->putstr_method(wid, attr, text);
    tty_putstr(wid, attr, text);
}

void
NetHackRL::rl_display_file(const char *filename, BOOLEAN_P must_exist)
{
    DEBUG_API("rl_display_file" << std::endl);
    tty_display_file(filename, must_exist);
}

void
NetHackRL::rl_start_menu(winid wid)
{
    instance->start_menu_method(wid);
}

void
NetHackRL::rl_add_menu(winid wid, int glyph, const ANY_P *identifier,
                       CHAR_P ch, CHAR_P gch, int attr, const char *str,
                       BOOLEAN_P presel)
{
    instance->add_menu_method(wid, glyph, identifier, ch, gch, attr, str,
                              presel);
}

void
NetHackRL::rl_end_menu(winid wid, const char *prompt)
{
    DEBUG_API("rl_end_menu" << std::endl);
    tty_end_menu(wid, prompt);
}

int
NetHackRL::rl_select_menu(winid wid, int how, MENU_ITEM_P **menu_list)
{
    DEBUG_API("rl_select_menu");
    int response = tty_select_menu(wid, how, menu_list);
    DEBUG_API(" : " << response << std::endl);
    return response;
}

void
NetHackRL::rl_update_inventory()
{
    DEBUG_API("rl_update_inventory" << std::endl);
    instance->update_inventory_method();
}

/* Stock's export names every item with doname, rotating objnam's ring; the export without inv_strs skips the names. A caller
   that reads a name it took before the export sees stock's text only if the ring is rotated as stock's was: rewind to where
   the export started and name every item again (no flag, RNG or shop change: doname_effects made them already). */
void
NetHackRL::rl_replay_export_ring()
{
    if (!current_nle_ctx || !current_nle_ctx->rl_instance || instance->ring_before_ < 0)
        return;
    nh_obuf_set_index(instance->ring_before_);
    instance->ring_before_ = -1;
    for (struct obj *otmp = invent; otmp; otmp = otmp->nobj)
        (void) doname(otmp);
}

void
NetHackRL::rl_mark_synch()
{
    DEBUG_API("rl_mark_synch" << std::endl);
    tty_mark_synch();
}

void
NetHackRL::rl_wait_synch()
{
    DEBUG_API("rl_wait_synch" << std::endl);
    tty_wait_synch();
}

void
NetHackRL::rl_cliparound(int x, int y)
{
#ifdef CLIPPING
    tty_cliparound(x, y);
#endif
}

/* print_glyph(window, x, y, glyph, bkglyph)
                -- Print the glyph at (x,y) on the given window.  Glyphs are
                   integers at the interface, mapped to whatever the window-
                   port wants (symbol, font, color, attributes, ...there's
                   a 1-1 map between glyphs and distinct things on the map).
                -- bkglyph is a background glyph for potential use by some
                   graphical or tiled environments to allow the depiction
                   to fall against a background consistent with the grid
                   around x,y. If bkglyph is NO_GLYPH, then the parameter
                   should be ignored (do nothing with it). */
__attribute__((hot)) void
NetHackRL::rl_print_glyph(winid wid, XCHAR_P x, XCHAR_P y, int glyph,
                          int bkglyph)
{
    /* the glyph plane alone: no mapped arrays, descriptions or tty bytes to produce */
    if (!USE_DEBUG_API && wid == WIN_MAP && (unsigned) y < ROWNO && ttyDisplay && ttyDisplay->nle_glyph_only) {
        NetHackRL *r = instance;
        unsigned f = map_context(x, y, glyph);

        r->store_glyph(x, y, glyph);
        r->map_defer(x, y, f);
        return;
    }
    rl_print_glyph_full(wid, x, y, glyph, bkglyph);
}

__attribute__((noinline)) void
NetHackRL::rl_print_glyph_full(winid wid, XCHAR_P x, XCHAR_P y, int glyph,
                               int bkglyph)
{
    int ch;
    int color;
    unsigned special;
    const nle_obs *o = nle_get_obs();
    const bool mapped = !o || o->chars || o->colors || o->specials;

    if (mapped)
        (void) mapglyph(glyph, &ch, &color, &special, x, y, 0);
#if USE_DEBUG_API
    DEBUG_API("rl_print_glyph(wid=" << wid << ", x=" << x << ", y=" << y
                                    << ", glyph=(ch='" << (char) ch
                                    << "', color=" << color
                                    << ", special=" << special);
    int bch;
    int bcolor;
    unsigned bspecial;
    (void) mapglyph(bkglyph, &bch, &bcolor, &bspecial, x, y, 0);
    DEBUG_API("), bkglyph=(ch='" << (char) bch << "', color=" << bcolor
                                 << ", special=" << bspecial << ")"
                                 << std::endl);
#endif

    // No win_proc_calls entry here.
    if (wid == WIN_MAP) {
        instance->store_glyph(x, y, glyph);
        if (!mapped) {
            instance->map_defer(x, y, map_context(x, y, glyph));
        } else {
            if (instance->mapped_stale_ & 2) {
                instance->map_fill();
                instance->mapped_stale_ &= ~2;
            }
            if (instance->mapped_stale_ & 1)
                instance->cellf_[y][x] = 0;
            if (glyph != nul_glyph && color == CLR_BLACK) {
                /* This will be 'bright black' (or blue) on tty so we change it to
                 * make NLE's colors and tty_colors stay compatible. */
                color = iflags.wc2_darkgray ? 8 : CLR_BLUE;
            }
            instance->store_mapped_glyph(ch, color, special, x, y);
        }
        if (nle_get_obs()->screen_descriptions) {
            instance->store_screen_description(x, y, glyph);
        }
    } else {
        DEBUG_API("Window id is " << wid << ". This shouldn't happen."
                                  << std::endl);
    }

    /* The tty map mirror (cursor walk + attr churn per cell) matters only
       when tty bytes are consumed (tty_* obs bound or ttyrec). The NLE
       obs above come from the store_* arrays either way. */
    if (!ttyDisplay || ttyDisplay->nle_emit)
        tty_print_glyph(wid, x, y, glyph, bkglyph);
}
void
NetHackRL::rl_raw_print(const char *str)
{
    DEBUG_API("rl_raw_print" << std::endl);
    MSAN_CHECK_CSTR(str);
    /* Not calling tty_raw_print(str); here or below as that
       uses puts/fputs. */
    xputs(str);
    putchar('\n');
    fflush(stdout);
}

void
NetHackRL::rl_raw_print_bold(const char *str)
{
    DEBUG_API("rl_raw_print_bold" << std::endl);
    MSAN_CHECK_CSTR(str);
    /* Not calling tty_raw_print_bold(str);, so above. */
    xputs(str);
    putchar('\n');
    fflush(stdout);
}

int
NetHackRL::rl_nhgetch()
{
    DEBUG_API("rl_nhgetch" << std::endl);
    if (!instance)
        return getch_early();
    int i = instance->getch_method();
    return i;
}

int
NetHackRL::rl_nh_poskey(int *x, int *y, int *mod)
{
    (void) x;
    (void) y;
    (void) mod;

    int action = rl_nhgetch();
    DEBUG_API("rl_nh_poskey: " << action << std::endl);
    return action;
    // Not calling nh_poskey, but no extra logic necessary here.
}

void
NetHackRL::rl_nhbell()
{
    DEBUG_API("rl_nhbell" << std::endl);
    return tty_nhbell();
}

int
NetHackRL::rl_doprev_message()
{
    DEBUG_API("rl_doprev_message" << std::endl);
    int result = tty_doprev_message();
    return result;
}

char
NetHackRL::rl_yn_function(const char *question_, const char *choices,
                          CHAR_P def)
{
    DEBUG_API("rl_yn_function" << std::endl);
    in_yn_function = true;
    char result = tty_yn_function(question_, choices, def);
    in_yn_function = false;
    return result;
}

void
NetHackRL::rl_getlin(const char *prompt, char *line)
{
    DEBUG_API("rl_getlin" << std::endl);
    in_getlin = true;
    tty_getlin(prompt, line);
    in_getlin = false;
}

int
NetHackRL::rl_get_ext_cmd()
{
    DEBUG_API("rl_get_ext_cmd" << std::endl);
    return tty_get_ext_cmd();
}

void
NetHackRL::rl_number_pad(int i)
{
    DEBUG_API("rl_number_pad" << std::endl);
    tty_number_pad(i);
}

void
NetHackRL::rl_delay_output()
{
    DEBUG_API("rl_delay_output" << std::endl);
    // No call to tty_delay_output() as we don't actually want delays.
}

void
NetHackRL::rl_start_screen()
{
    DEBUG_API("rl_start_screen" << std::endl);
    tty_start_screen();
}

void
NetHackRL::rl_end_screen()
{
    DEBUG_API("rl_end_screen" << std::endl);
    tty_end_screen();

    if (instance)
        // The only way instance can still be around is in an error situation.
        // Unfortunately, ZQM doesn't close properly when destructed via
        // global objects. So we do it here.
        delete instance;
    instance = nullptr;
}

void
NetHackRL::rl_outrip(winid wid, int how, time_t when)
{
    DEBUG_API("rl_outrip" << std::endl);
    genl_outrip(wid, how, when);
}

char *
NetHackRL::rl_getmsghistory(BOOLEAN_P init)
{
    DEBUG_API("rl_getmsghistory" << std::endl);
    return tty_getmsghistory(init);
}

void
NetHackRL::rl_putmsghistory(const char *msg, BOOLEAN_P is_restoring)
{
    DEBUG_API("rl_putmsghistory" << std::endl);
    MSAN_CHECK_CSTR(msg);
    tty_putmsghistory(msg, is_restoring);
}

void
NetHackRL::rl_status_init()
{
    DEBUG_API("rl_status_init" << std::endl);
    tty_status_init();
}

void
NetHackRL::rl_status_update(int fldidx, genericptr_t ptr, int chg,
                            int percent, int color, unsigned long *colormasks)
{
    DEBUG_API("rl_status_update" << std::endl);

    instance->status_update_method(fldidx, ptr, chg, percent, color,
                                   colormasks);
#ifdef STATUS_HILITES
    tty_status_update(fldidx, ptr, chg, percent, color, colormasks);
#endif
}

static void
rl_update_positionbar(char *chrs)
{
    DEBUG_API("rl_update_positionbar" << std::endl);
#ifdef POSITIONBAR
    tty_update_positionbar(chrs);
#endif
}

/* The values bot_via_windowport compares (botl.c), as it stores them: title and level as text keys, caps applied */
enum { SS_VALID, SS_TITLE, SS_STR, SS_DX, SS_CO, SS_IN, SS_WI, SS_CH, SS_ALIGN, SS_SCORE, SS_HP, SS_HPMAX, SS_LEVEL, SS_GOLD,
       SS_ENE, SS_ENEMAX, SS_AC, SS_HD, SS_XP, SS_EXP, SS_TIME, SS_HUNGER, SS_CAP, SS_COND, SS_RNDENC, SS_GOLDSYM };
static_assert(SS_GOLDSYM < NLE_RL_STATUS_SIG, "status signature size");

static const char *
status_title(long key) /* BL_TITLE's varying part: the rank, or the monster while polymorphed */
{
    return key < 0 ? mons[-1 - key].mname : rank_of((int) (key >> 1), Role_switch, (boolean) (key & 1));
}

void
NetHackRL::status_signature(long *s)
{
    long money = nh_money_invent();
    int hp = Upolyd ? u.mh : u.uhp;

    s[SS_VALID] = 1;
    s[SS_TITLE] = Upolyd ? -1L - u.umonnum : ((long) u.ulevel << 1 | (flags.female ? 1 : 0));
    acurr_status(&s[SS_STR]); /* SS_STR, SS_DX, SS_CO, SS_IN, SS_WI, SS_CH */
    s[SS_ALIGN] = u.ualign.type == A_CHAOTIC ? 0 : u.ualign.type == A_NEUTRAL ? 1 : 2;
    s[SS_SCORE] = flags.showscore ? botl_score() : 0L;
    s[SS_HP] = min(max(hp, 0), 9999), s[SS_HPMAX] = min(Upolyd ? u.mhmax : u.uhpmax, 9999);
    s[SS_LEVEL] = Is_knox(&u.uz) ? (1L << 24 | u.uz.dnum) : In_quest(&u.uz) ? (2L << 24 | dunlev(&u.uz))
                  : In_endgame(&u.uz) ? (3L << 24 | Is_astralevel(&u.uz)) : (long) depth(&u.uz);
    s[SS_GOLD] = min(max(money, 0L), 999999L);
    s[SS_ENE] = min(u.uen, 9999), s[SS_ENEMAX] = min(u.uenmax, 9999);
    s[SS_AC] = u.uac;
    s[SS_HD] = Upolyd ? (long) mons[u.umonnum].mlevel : 0L;
    s[SS_XP] = u.ulevel, s[SS_EXP] = u.uexp, s[SS_TIME] = moves;
    s[SS_HUNGER] = (long) u.uhs, s[SS_CAP] = near_capacity_quiet();
    {
        long cond = 0L; /* BL_CONDITION, as bot_via_windowport assembles it */
        if (Stoned) cond |= BL_MASK_STONE;
        if (Slimed) cond |= BL_MASK_SLIME;
        if (Strangled) cond |= BL_MASK_STRNGL;
        if (Sick && (u.usick_type & SICK_VOMITABLE) != 0) cond |= BL_MASK_FOODPOIS;
        if (Sick && (u.usick_type & SICK_NONVOMITABLE) != 0) cond |= BL_MASK_TERMILL;
        if (Blind) cond |= BL_MASK_BLIND;
        if (Deaf) cond |= BL_MASK_DEAF;
        if (Stunned) cond |= BL_MASK_STUN;
        if (Confusion) cond |= BL_MASK_CONF;
        if (Hallucination) cond |= BL_MASK_HALLU;
        if (Levitation) cond |= BL_MASK_LEV;
        if (Flying) cond |= BL_MASK_FLY;
        if (u.usteed) cond |= BL_MASK_RIDE;
        s[SS_COND] = cond;
    }
    s[SS_RNDENC] = context.rndencode, s[SS_GOLDSYM] = (long) showsyms[COIN_CLASS + SYM_OFF_O];
}

/* evaluate_and_notify_windowport's test: a field the status line shows differs from the previous evaluation (a new gold
   encoding forces every field) */
bool
NetHackRL::status_changed(const long *p, const long *c)
{
    unsigned long diff = 0;
    int i = 0;
#ifdef __AVX2__
    for (; i + 4 <= NLE_RL_STATUS_SIG; i += 4) {
        __m256i eq = _mm256_cmpeq_epi64(_mm256_loadu_si256((const __m256i *) (p + i)),
                                        _mm256_loadu_si256((const __m256i *) (c + i)));
        diff |= (unsigned long) (~_mm256_movemask_pd(_mm256_castsi256_pd(eq)) & 0xF) << i;
    }
#endif
    for (; i < NLE_RL_STATUS_SIG; i++)
        diff |= (unsigned long) (p[i] != c[i]) << i;
    diff &= ~((1UL << SS_STR) - 1);
    if (!flags.showscore)
        diff &= ~(1UL << SS_SCORE);
    if (!flags.showexp || Upolyd)
        diff &= ~(1UL << SS_EXP);
    if (!flags.time)
        diff &= ~(1UL << SS_TIME);
    diff &= ~(1UL << (Upolyd ? SS_XP : SS_HD));
    return diff || (p[SS_TITLE] != c[SS_TITLE] && std::strcmp(status_title(p[SS_TITLE]), status_title(c[SS_TITLE])) != 0);
}

/* The blstats refresh of the !status_updates (RL perf) mode, at the status pipeline's flush points (rl_bot_direct): what
   update_blstats() and the BL_CONDITION delivery would store, from the evaluation just made */
void
NetHackRL::blstats_from_signature(const long *s)
{
    blstats_[NLE_BL_X] = u.ux - 1;
    blstats_[NLE_BL_Y] = u.uy;
    blstats_[NLE_BL_STR25] = acurrstr_of((int) s[SS_STR]);
    blstats_[NLE_BL_STR125] = s[SS_STR];
    blstats_[NLE_BL_DEX] = s[SS_DX];
    blstats_[NLE_BL_CON] = s[SS_CO];
    blstats_[NLE_BL_INT] = s[SS_IN];
    blstats_[NLE_BL_WIS] = s[SS_WI];
    blstats_[NLE_BL_CHA] = s[SS_CH];
    blstats_[NLE_BL_SCORE] = flags.showscore ? s[SS_SCORE] : botl_score();
    blstats_[NLE_BL_HP] = s[SS_HP];
    blstats_[NLE_BL_HPMAX] = s[SS_HPMAX];
    blstats_[NLE_BL_DEPTH] = depth(&u.uz);
    blstats_[NLE_BL_GOLD] = nh_money_invent();
    blstats_[NLE_BL_ENE] = s[SS_ENE];
    blstats_[NLE_BL_ENEMAX] = s[SS_ENEMAX];
    blstats_[NLE_BL_AC] = s[SS_AC];
    blstats_[NLE_BL_HD] = s[SS_HD];
    blstats_[NLE_BL_XP] = s[SS_XP];
    blstats_[NLE_BL_EXP] = s[SS_EXP];
    blstats_[NLE_BL_TIME] = s[SS_TIME];
    blstats_[NLE_BL_HUNGER] = s[SS_HUNGER];
    blstats_[NLE_BL_CAP] = s[SS_CAP];
    blstats_[NLE_BL_DNUM] = u.uz.dnum;
    blstats_[NLE_BL_DLEVEL] = u.uz.dlevel;
    blstats_[NLE_BL_CONDITION] = condition_bits_ = s[SS_COND];
    blstats_[NLE_BL_ALIGN] = u.ualign.type;
}

/* Mirrors bot(): bot_via_windowport compares every status field with its previous evaluation and flushes (BL_FLUSH ->
   update_blstats()) only when one changed or context.botlx asks for a full redraw. Between flushes the port's cache keeps
   the values of the last one: the status fields are unchanged, while x/y (not status fields) may be stale where fill_obs
   does not patch them (u.dz set). */
void
NetHackRL::rl_bot_direct()
{
    if (!current_nle_ctx || !current_nle_ctx->rl_instance)
        return;
    long cur[NLE_RL_STATUS_SIG];
    instance->status_signature(cur);
    bool flush = !instance->status_sig_[SS_VALID] || context.botlx || instance->status_changed(instance->status_sig_, cur);
    std::memcpy(instance->status_sig_, cur, sizeof cur);
    if (flush)
        instance->blstats_from_signature(cur);
}

/* Mirrors timebot(): stat_update_time delivers BL_TIME (status_ string
   only) then BL_FLUSH -> update_blstats() with the CACHED condition mask
   (no BL_CONDITION delivery), so no condition recompute here. */
void
NetHackRL::rl_timebot_direct()
{
    if (current_nle_ctx && current_nle_ctx->rl_instance) {
        instance->update_blstats(true);
        instance->status_sig_[SS_TIME] = moves; /* stat_update_time stores BL_TIME in the current evaluation */
    }
}

/* Out-of-band observation export for nle_obs_refresh (nle.c): the same
   fill the next getch would run, without stepping the game. */
void
NetHackRL::rl_fill_obs_direct(nle_obs *obs)
{
    if (current_nle_ctx && current_nle_ctx->rl_instance)
        instance->fill_obs(obs);
}

/* checkpoint: state kept from fill to fill (the underfoot patch, and the status cache that
   !status_updates refreshes only when the status pipeline fires); inventory caches resync */
int
NetHackRL::rl_ckpt_get(long *blstats, long *cond, long *status_sig, struct nle_rl_inv_ckpt *inv)
{
    if (!current_nle_ctx || !current_nle_ctx->rl_instance)
        return -1;
    std::memcpy(blstats, instance->blstats_, sizeof instance->blstats_);
    *cond = instance->condition_bits_;
    std::memcpy(status_sig, instance->status_sig_, sizeof instance->status_sig_);
    std::memset(inv, 0, sizeof *inv);
    std::memcpy(inv->glyph, instance->inv_glyphs_cache_.data(), sizeof inv->glyph);
    std::memcpy(inv->letter, instance->inv_letters_cache_.data(), sizeof inv->letter);
    std::memcpy(inv->oclass, instance->inv_oclasses_cache_.data(), sizeof inv->oclass);
    for (const rl_inventory_item &item : instance->inventory_) {
        if (inv->n >= NLE_INVENTORY_SIZE)
            break;
        std::strncpy(inv->str[inv->n], item.str.c_str(), NLE_INVENTORY_STR_LENGTH); /* the export keeps 80 bytes */
        inv->n++;
    }
    return instance->underfoot_prev_;
}

void
NetHackRL::rl_ckpt_set(int underfoot_prev, const long *blstats, long cond, const long *status_sig,
                       const struct nle_rl_inv_ckpt *inv)
{
    if (!current_nle_ctx || !current_nle_ctx->rl_instance)
        return;
    instance->underfoot_prev_ = underfoot_prev;
    instance->msg_lo_ = nullptr; /* rewrite the whole message buffer once */
    std::memcpy(instance->blstats_, blstats, sizeof instance->blstats_);
    instance->condition_bits_ = cond;
    std::memcpy(instance->status_sig_, status_sig, sizeof instance->status_sig_);
    std::memcpy(instance->inv_glyphs_cache_.data(), inv->glyph, sizeof inv->glyph);
    instance->inv_shown_gen_ = 0;
    instance->inv_cache_n_ = NLE_INVENTORY_SIZE;
    std::memcpy(instance->inv_letters_cache_.data(), inv->letter, sizeof inv->letter);
    std::memcpy(instance->inv_oclasses_cache_.data(), inv->oclass, sizeof inv->oclass);
    instance->inventory_.clear();
    for (int i = 0; i < inv->n; i++)
        instance->inventory_.emplace_back(rl_inventory_item{ inv->glyph[i], std::string(inv->str[i], strnlen(inv->str[i], NLE_INVENTORY_STR_LENGTH)),
                                                             (char) inv->letter[i], (char) inv->oclass[i], std::string() });
    instance->inv_memo_n_ = -1;
    instance->disc_gen_seen_ = 0;
    std::memset(instance->cellf_, 0, sizeof instance->cellf_); /* the image's map arrays are resolved (rl_map_resolve) */
    instance->mapped_stale_ &= 2;
    instance->dirty_rows_ = ~0u; /* the display arrays now hold another game: copy every row once */
    instance->glyph_export_rows_ = ~0u;
    instance->glyph_export_acc_ = ~0u;
}

/* checkpoint: the mapped arrays brought up to date, as a bound fill would */
void
NetHackRL::rl_map_resolve()
{
    if (current_nle_ctx && current_nle_ctx->rl_instance && instance->mapped_stale_)
        instance->map_resolve();
}

/* checkpoint: the rendered map as the port wrote it at print time (map[0..2]: chars, colors, specials) */
int
NetHackRL::rl_ckpt_display(unsigned char **map, char **sd, uint8_t **stale)
{
    if (!current_nle_ctx || !current_nle_ctx->rl_instance)
        return -1;
    map[0] = instance->chars_.data();
    map[1] = instance->colors_.data();
    map[2] = instance->specials_.data();
    *sd = instance->screen_descriptions_.data();
    *stale = &instance->mapped_stale_;
    return 0;
}

} // namespace nethack_rl

extern "C" int
nle_rl_ckpt_get(long *blstats, long *cond, long *status_sig, struct nle_rl_inv_ckpt *inv)
{
    return nethack_rl::NetHackRL::rl_ckpt_get(blstats, cond, status_sig, inv);
}

extern "C" void
nle_rl_map_resolve(void)
{
    nethack_rl::NetHackRL::rl_map_resolve();
}

extern "C" int
nle_rl_ckpt_display(unsigned char **map, char **sd, uint8_t **stale)
{
    return nethack_rl::NetHackRL::rl_ckpt_display(map, sd, stale);
}

extern "C" void
nle_rl_ckpt_set(int underfoot_prev, const long *blstats, long cond, const long *status_sig, const struct nle_rl_inv_ckpt *inv)
{
    nethack_rl::NetHackRL::rl_ckpt_set(underfoot_prev, blstats, cond, status_sig, inv);
}

/* botl.c hooks for the !status_updates (RL perf) mode: pump the blstats_
   cache at exactly the status pipeline's trigger points (bot / timebot)
   so observation-time staleness matches the pipeline bit-for-bit. */
extern "C" void
nle_rl_replay_export_ring(void)
{
    nethack_rl::NetHackRL::rl_replay_export_ring();
}

extern "C" void
nle_rl_bot_direct(void)
{
    nethack_rl::NetHackRL::rl_bot_direct();
}

extern "C" void
nle_rl_timebot_direct(void)
{
    nethack_rl::NetHackRL::rl_timebot_direct();
}

extern "C" void
nle_rl_fill_obs(nle_obs *obs)
{
    nethack_rl::NetHackRL::rl_fill_obs_direct(obs);
}

extern const struct window_procs
    rl_procs; /* C++ const needs explicit extern linkage */
const struct window_procs rl_procs = {
    "rl",
    (WC_COLOR | WC_HILITE_PET | WC_INVERSE | WC_EIGHT_BIT_IN
     | WC_PERM_INVENT),
    (0
#if defined(SELECTSAVED)
     | WC2_SELECTSAVED
#endif
#if defined(STATUS_HILITES)
     | WC2_HILITE_STATUS | WC2_HITPOINTBAR | WC2_FLUSH_STATUS
     | WC2_RESET_STATUS
#endif
     | WC2_DARKGRAY | WC2_SUPPRESS_HIST | WC2_STATUSLINES),
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1 }, /* color availability */
    nethack_rl::NetHackRL::rl_init_nhwindows,
    nethack_rl::NetHackRL::rl_player_selection,
    nethack_rl::NetHackRL::rl_askname,
    nethack_rl::NetHackRL::rl_get_nh_event,
    nethack_rl::NetHackRL::rl_exit_nhwindows,
    nethack_rl::NetHackRL::rl_suspend_nhwindows,
    nethack_rl::NetHackRL::rl_resume_nhwindows,
    nethack_rl::NetHackRL::rl_create_nhwindow,
    nethack_rl::NetHackRL::rl_clear_nhwindow,
    nethack_rl::NetHackRL::rl_display_nhwindow,
    nethack_rl::NetHackRL::rl_destroy_nhwindow,
    nethack_rl::NetHackRL::rl_curs,
    nethack_rl::NetHackRL::rl_putstr,
    genl_putmixed,
    nethack_rl::NetHackRL::rl_display_file,
    nethack_rl::NetHackRL::rl_start_menu,
    nethack_rl::NetHackRL::rl_add_menu,
    nethack_rl::NetHackRL::rl_end_menu,
    nethack_rl::NetHackRL::rl_select_menu,
    genl_message_menu, /* no need for X-specific handling */
    nethack_rl::NetHackRL::rl_update_inventory,
    nethack_rl::NetHackRL::rl_mark_synch,
    nethack_rl::NetHackRL::rl_wait_synch,
#ifdef CLIPPING
    nethack_rl::NetHackRL::rl_cliparound,
#endif
#ifdef POSITIONBAR
    nethack_rl::rl_update_positionbar,
#endif
    nethack_rl::NetHackRL::rl_print_glyph,
    // NetHackRL::rl_print_glyph_compose,
    nethack_rl::NetHackRL::rl_raw_print,
    nethack_rl::NetHackRL::rl_raw_print_bold,
    nethack_rl::NetHackRL::rl_nhgetch,
    nethack_rl::NetHackRL::rl_nh_poskey,
    nethack_rl::NetHackRL::rl_nhbell,
    nethack_rl::NetHackRL::rl_doprev_message,
    nethack_rl::NetHackRL::rl_yn_function,
    nethack_rl::NetHackRL::rl_getlin,
    nethack_rl::NetHackRL::rl_get_ext_cmd,
    nethack_rl::NetHackRL::rl_number_pad,
    nethack_rl::NetHackRL::rl_delay_output,
#ifdef CHANGE_COLOR /* only a Mac option currently */
    donull,
    donull,
    donull,
    donull,
#endif
    /* other defs that really should go away (they're tty specific) */
    nethack_rl::NetHackRL::rl_start_screen,
    nethack_rl::NetHackRL::rl_end_screen,
#ifdef GRAPHIC_TOMBSTONE
    nethack_rl::NetHackRL::rl_outrip,
#else
    genl_outrip,
#endif
    tty_preference_update,
    nethack_rl::NetHackRL::rl_getmsghistory,
    nethack_rl::NetHackRL::rl_putmsghistory,
    nethack_rl::NetHackRL::rl_status_init,
    genl_status_finish,
    tty_status_enablefield,
    nethack_rl::NetHackRL::rl_status_update,
    genl_can_suspend_yes,
};

using nethack_rl::NetHackRL;

/* C-callable hooks (declared in extern.h). display.c and o_init.c/read.c call
   these; the env reads the last fill's rewritten glyph rows. */
extern "C" void
nle_glyphs_cleared(void)
{
    if (current_nle_ctx && instance)
        instance->glyphs_cleared();
}
extern "C" void
nle_discoveries_changed(void)
{
    nle_ocg_sync();
    if (current_nle_ctx && instance)
        instance->discoveries_changed();
}
extern "C" void
nle_discovery_one(int oindx)
{
    nle_ocg_one(oindx);
    if (current_nle_ctx && instance)
        instance->discoveries_changed();
}
extern "C" unsigned
nle_glyph_dirty_rows(void)
{
    return (current_nle_ctx && instance) ? instance->glyph_export_rows() : ~0u;
}

/* map rows the observation's glyph plane changed in since the previous call (all rows the first time) */
extern "C" unsigned
nle_glyph_take_rows(void)
{
    return (current_nle_ctx && instance) ? instance->glyph_take_rows() : ~0u;
}
