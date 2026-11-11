#include "hack.h"
#include "nle.h"
#include "nlernd.h"
#include "pufferhack.h"

int nle_in_getpos(nle_ctx_t *);
long nle_shop_price(nle_ctx_t *);
int nle_inside_shop(nle_ctx_t *);
int nle_terrain_underfoot(nle_ctx_t *);
int nle_container_at(nle_ctx_t *);
int nle_food_underfoot(nle_ctx_t *);
int nle_discoveries(nle_ctx_t *);
int nle_peaceful_at(nle_ctx_t *, int, int);
int nle_lnc_bits(nle_ctx_t *);
void nle_weight(nle_ctx_t *, int *, int *);
int nle_spells(nle_ctx_t *, short *, signed char *, signed char *, int *, int);
int nle_cast_blocked(nle_ctx_t *);
int nle_intrinsics(nle_ctx_t *);
void nle_identity(nle_ctx_t *, int *, int *, int *, int *);
int nle_path_drain(nle_ctx_t *, short *, int);

#define SAME(f) _Static_assert(offsetof(ph_obs, f) == offsetof(nle_obs, f), "ph_obs." #f)
SAME(action); SAME(done); SAME(in_normal_game); SAME(how_done); SAME(glyphs); SAME(chars); SAME(colors); SAME(specials);
SAME(blstats); SAME(message); SAME(prog_state); SAME(internal); SAME(inv_glyphs); SAME(inv_strs); SAME(inv_letters);
SAME(inv_oclasses); SAME(screen_descriptions); SAME(tty_chars); SAME(tty_colors); SAME(tty_cursor); SAME(misc);
SAME(inv_state); SAME(inv_true_glyphs); SAME(partial);
_Static_assert(sizeof(ph_obs) == sizeof(nle_obs), "ph_obs size");
_Static_assert(PH_BLSTATS_SIZE == NLE_BLSTATS_SIZE && PH_INTERNAL_SIZE == NLE_INTERNAL_SIZE
               && PH_INVENTORY_SIZE == NLE_INVENTORY_SIZE && PH_TERM_CO == NLE_TERM_CO && PH_TERM_LI == NLE_TERM_LI
               && PH_ROWS == ROWNO && PH_COLS == COLNO - 1, "sizes");

#define G(g) ((nle_ctx_t *) (g))

void
ph_config_default(ph_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->spawn_monsters = 1;
}

static int
copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n >= cap)
        return 0;
    memcpy(dst, src, n + 1);
    return 1;
}

ph_game *
ph_start(ph_obs *obs, FILE *ttyrec, const ph_config *cfg)
{
    nle_settings *s = (nle_settings *) calloc(1, sizeof *s);
    nle_ctx_t *g = 0;

    if (!s)
        return 0;
    if (cfg->data_dir && copy(s->hackdir, sizeof s->hackdir, cfg->data_dir)
        && (!cfg->wizkit || copy(s->wizkit, sizeof s->wizkit, cfg->wizkit))
        && (cfg->options ? copy(s->options, sizeof s->options, cfg->options)
                         : !cfg->options_file
                               || snprintf(s->options, sizeof s->options, "@%s", cfg->options_file)
                                      < (int) sizeof s->options)) {
        s->initial_seeds.seeds[0] = cfg->seed_core;
        s->initial_seeds.seeds[1] = cfg->seed_disp;
        s->initial_seeds.reseed = cfg->reseed != 0;
        s->initial_seeds.use_init_seeds = cfg->seeded != 0;
        s->fix_moon_phase = cfg->seeded_clock != 0;
        s->time_seed = cfg->clock_seed;
        s->time_seed_is_set = cfg->seeded_clock != 0;
        s->spawn_monsters = cfg->spawn_monsters;
        s->underfoot_glyphs = cfg->underfoot_glyphs;
        s->true_glyphs = cfg->true_glyphs != 0;
        s->skip_gamelogs = cfg->skip_gamelogs != 0;
        g = nle_start((nle_obs *) obs, ttyrec, s);
    }
    free(s);
    return (ph_game *) g;
}

ph_game *ph_step(ph_game *game, ph_obs *obs) { return (ph_game *) nle_step(G(game), (nle_obs *) obs); }
ph_game *ph_refresh(ph_game *game, ph_obs *obs) { return (ph_game *) nle_obs_refresh(G(game), (nle_obs *) obs); }
void ph_end(ph_game *game) { nle_end(G(game)); }
int ph_in_getpos(ph_game *game) { return nle_in_getpos(G(game)); }

void ph_set_seed(ph_game *game, unsigned long core, unsigned long disp, int reseed, unsigned long lgen)
{
    nle_set_seed(G(game), core, disp, reseed ? TRUE : FALSE, lgen);
}

void ph_get_seed(ph_game *game, unsigned long *core, unsigned long *disp, int *reseed, unsigned long *lgen, int *use_lgen)
{
    boolean r = FALSE;
    bool lg = false;
    nle_get_seed(G(game), core, disp, &r, lgen, &lg);
    if (reseed) *reseed = r;
    if (use_lgen) *use_lgen = lg;
}

void *ph_ckpt_save(ph_game *game, size_t *len) { return nle_ckpt_save(G(game), len); }
size_t ph_ckpt_save_into(ph_game *game, void **buf, size_t *cap) { return nle_ckpt_save_into(G(game), buf, cap); }
int ph_ckpt_load(ph_game *game, const void *buf, size_t len) { return nle_ckpt_load(G(game), buf, len); }
void ph_ckpt_free(void *buf) { nle_ckpt_free(buf); }
int ph_ckpt_diff(const void *a, size_t alen, const void *b, size_t blen) { return nle_ckpt_diff(a, alen, b, blen); }

long ph_shop_price(ph_game *game) { return nle_shop_price(G(game)); }
int ph_inside_shop(ph_game *game) { return nle_inside_shop(G(game)); }
int ph_terrain_underfoot(ph_game *game) { return nle_terrain_underfoot(G(game)); }
int ph_container_at(ph_game *game) { return nle_container_at(G(game)); }
int ph_food_underfoot(ph_game *game) { return nle_food_underfoot(G(game)); }
int ph_discoveries(ph_game *game) { return nle_discoveries(G(game)); }
int ph_peaceful_at(ph_game *game, int x, int y) { return nle_peaceful_at(G(game), x, y); }
int ph_lnc_bits(ph_game *game) { return nle_lnc_bits(G(game)); }
void ph_weight(ph_game *game, int *carried, int *capacity) { nle_weight(G(game), carried, capacity); }
int ph_spells(ph_game *game, short *ids, signed char *levels, signed char *fails, int *known, int cap)
{
    return nle_spells(G(game), ids, levels, fails, known, cap);
}
int ph_cast_blocked(ph_game *game) { return nle_cast_blocked(G(game)); }
int ph_intrinsics(ph_game *game) { return nle_intrinsics(G(game)); }
void ph_identity(ph_game *game, int *role, int *race, int *gender, int *alignment)
{
    nle_identity(G(game), role, race, gender, alignment);
}
int ph_path_drain(ph_game *game, short *out, int cap) { return nle_path_drain(G(game), out, cap); }
