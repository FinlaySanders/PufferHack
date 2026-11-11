/* phd_stock.c -> phd_stock.so: the stock side of the digest. Compiles the engine's own src/ph_digest.c (PHD_SRC, the one
   definition) against stock NLE 0.9.0's headers and links against the stock library (soname libnethack.so), so every game
   global binds to the stock copy the driver loaded. Adds the stock driver's observation emulation of the fork's deliberate
   glyph behaviours (stock-diff src/state_dump.c sd_obs_emulate, with the gem-twin fix): the emulated copies are digested, never
   written back into stock's buffers. */
#define PHD_STOCK 1
#include PHD_SRC
#include "appearance_canon.h"

static int sd_shuffled(int glyph, int true_glyphs)
{
    if (glyph_is_normal_object(glyph)) {
        int o = glyph_to_obj(glyph), nk = objects[o].oc_name_known, d = objects[o].oc_descr_idx;
        if (true_glyphs && nk)
            return glyph;
        return GLYPH_OBJ_OFF + (nk ? d : nle_appearance_canon[d]);
    }
    return glyph;
}

static void sd_obs_emulate(short *glyphs, short *inv_glyphs, int ninv, int true_glyphs, int underfoot)
{
    static short slot_obj[NUM_OBJECTS];
    int i;
    for (i = 0; i < NUM_OBJECTS; i++)
        slot_obj[objects[i].oc_descr_idx] = (short) i;
#define SD_TRUE(g) (glyph_is_normal_object(g) ? GLYPH_OBJ_OFF + slot_obj[(g) - GLYPH_OBJ_OFF] : (g))
    if (inv_glyphs) {
        static short shadow[64];
        static int shadow_init;
        struct obj *io = invent;
        if (!shadow_init)
            for (i = 0; i < 64; i++)
                shadow[i] = -1;
        shadow_init = 1;
        for (i = 0; i < ninv; i++, io = io ? io->nobj : io) {
            int g = inv_glyphs[i];
            if (!glyph_is_normal_object(g))
                g = SD_TRUE(g);
            else if (io && g - GLYPH_OBJ_OFF == objects[io->otyp].oc_descr_idx)
                shadow[i] = io->otyp, g = GLYPH_OBJ_OFF + io->otyp;
            else if (i < 64 && shadow[i] >= 0 && g - GLYPH_OBJ_OFF == objects[shadow[i]].oc_descr_idx)
                g = GLYPH_OBJ_OFF + shadow[i];
            else
                g = SD_TRUE(g);
            inv_glyphs[i] = (short) sd_shuffled(g, 0);
        }
    }
    if (!glyphs)
        return;
    for (i = 0; i < ROWNO * (COLNO - 1); i++) {
        if ((i & 15) == 0 && i + 16 <= ROWNO * (COLNO - 1)) { /* no object glyph among the next 16: each maps to itself */
#ifdef __AVX2__
            __m256i v = _mm256_loadu_si256((const __m256i *) (glyphs + i));
            __m256i in = _mm256_and_si256(_mm256_cmpgt_epi16(v, _mm256_set1_epi16(GLYPH_OBJ_OFF - 1)),
                                          _mm256_cmpgt_epi16(_mm256_set1_epi16(GLYPH_OBJ_OFF + NUM_OBJECTS), v));
            int any = !_mm256_testz_si256(in, in);
#else
            int any = 0;
            for (int j = 0; j < 16; j++)
                any |= (unsigned) (glyphs[i + j] - GLYPH_OBJ_OFF) < (unsigned) NUM_OBJECTS;
#endif
            if (!any) {
                i += 15;
                continue;
            }
        }
        if (!glyph_is_normal_object(glyphs[i]))
            continue;
        int g = glyphs[i], lg = levl[i % (COLNO - 1) + 1][i / (COLNO - 1)].glyph;
        if (glyph_is_normal_object(g) && glyph_is_normal_object(lg) && g - GLYPH_OBJ_OFF == objects[glyph_to_obj(lg)].oc_descr_idx)
            g = lg;
        else
            g = SD_TRUE(g);
        glyphs[i] = (short) sd_shuffled(g, true_glyphs);
    }
    if ((!program_state.something_worth_saving && !program_state.in_moveloop) || !iflags.window_inited)
        return;
    if (underfoot && u.ux >= 1 && u.ux < COLNO && u.uy >= 0 && u.uy < ROWNO) {
        if (!(u.uswallow || Blind || Hallucination)) {
            struct obj *under = vobj_at(u.ux, u.uy);
            int g = under ? obj_to_glyph(under, rn2_on_display_rng) : back_to_glyph(u.ux, u.uy);
            glyphs[u.uy * (COLNO - 1) + (u.ux - 1)] = (short) sd_shuffled(g, true_glyphs);
        }
    }
}

/* one key's input from the driver: stock's raw buffers in canonical order (internal: stock's whole array) */
struct phs_in {
    const void *rng[2];
    void *st[7]; /* head_engr timer_base disco light_base regions n_regions artiexist */
    const void *obs[PHD_NOBS];
    const unsigned char *msg;
    int done, in_normal_game, true_glyphs, underfoot, no_emu;
};

unsigned long long phs_id(void)
{
    return phd_id();
}

/* this adapter's own id: the sha256 of phd_stock.c and appearance_canon.h (PHS_ADAPTER_SHA, from build.sh; the verifier pins it) */
#ifndef PHS_ADAPTER_SHA
#define PHS_ADAPTER_SHA 0
#endif
unsigned long long phs_adapter_id(void)
{
    return (unsigned long long) PHS_ADAPTER_SHA;
}

const char *phs_name(int c)
{
    return c >= 0 && c < PHD_NCOMP ? phd_names[c] : NULL;
}

static void phs_prepare(const struct phs_in *in, struct phd_statics *st, const void **b)
{
    static short emu_gl[21 * 79], emu_ig[55]; /* emu_ig: outside normal play neither port refills the inventory */
    static int in9[9];
    st->head_engr = (struct engr **) in->st[0];
    st->timer_base = (timer_element **) in->st[1];
    st->disco = (short *) in->st[2];
    st->light_base = (light_source **) in->st[3];
    st->regions = (NhRegion ***) in->st[4];
    st->n_regions = (int *) in->st[5];
    st->artiexist = (boolean *) in->st[6];
    st->rng[0] = (const struct isaac64_ctx *) in->rng[0];
    st->rng[1] = (const struct isaac64_ctx *) in->rng[1];
    for (int i = 0; i < PHD_NOBS; i++)
        b[i] = in->obs[i];
    if (in->obs[7]) {
        memcpy(in9, in->obs[7], sizeof in9);
        in9[6] = 0;
        b[7] = in9;
    }
    if (in->obs[0]) {
        memcpy(emu_gl, in->obs[0], sizeof emu_gl);
        b[0] = emu_gl;
    }
    if (in->obs[8]) {
        if (in->in_normal_game)
            memcpy(emu_ig, in->obs[8], sizeof emu_ig);
        b[8] = emu_ig;
    }
    if (!in->no_emu)
        sd_obs_emulate(in->obs[0] ? emu_gl : NULL, in->obs[8] && in->in_normal_game ? emu_ig : NULL, 55, in->true_glyphs,
                       in->underfoot);
}

/* out[PHD_NCOMP]; mode PHD_STATE: also the state line (the emulation runs only then, as in the old driver) */
static struct phd_ws phs_ws; /* this process's one game */
static int phs_nows = -1;
int phs_digest(const struct phs_in *in, unsigned mode, unsigned long long *out)
{
    struct phd_statics st;
    const void *b[PHD_NOBS];
    uint64_t o[PHD_NCOMP];
    if (mode & PHD_STATE)
        phs_prepare(in, &st, b);
    else {
        st.rng[0] = (const struct isaac64_ctx *) in->rng[0];
        st.rng[1] = (const struct isaac64_ctx *) in->rng[1];
    }
    if (phs_nows < 0)
        phs_nows = getenv("PHV_NO_WS") != NULL;
    phd_compute(&st, b, in->msg, in->done, mode, phs_nows ? NULL : &phs_ws, o);
    for (int i = 0; i < PHD_NCOMP; i++)
        out[i] = o[i];
    out[PHD_KEY] = 0;
    return PHD_NCOMP;
}

/* the text snapshot; call it only where the digest's state line was computed (it re-runs the emulation on the same buffers) */
int phs_text(const struct phs_in *in, FILE *f)
{
    struct phd_statics st;
    const void *b[PHD_NOBS];
    phs_prepare(in, &st, b);
    phd_text(&st, b, f);
    return 0;
}
