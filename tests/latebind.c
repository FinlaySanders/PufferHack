/* observation buffers bound after the start show what stock NLE shows there */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "ph_digest.h"

#define OPTS "name:Agent,role:Val,race:human,gender:female,align:neutral,!autopickup,nobones,nolegacy,playmode:debug"
#define TTY (NLE_TERM_LI * NLE_TERM_CO)
#define MAP (21 * 79)

/* a pile dropped and left in view, a blindfold, a potion of hallucination: the pile is not redrawn, so its cell keeps the
   object-pile mark it was printed with */
static const char pile_keys[] = "\rdbddkk\027blindfold\rPe\027potion of hallucination\rqf";

static const char walk[] = "hjklyubnsss";
static int misc[2][NLE_MISC_SIZE];

static nle_ctx_t *start(nle_obs *obs, const char *hackdir)
{
    static nle_settings s;

    memset(&s, 0, sizeof s);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", hackdir);
    snprintf(s.options, sizeof s.options, "%s", OPTS);
    s.spawn_monsters = 1;
    s.initial_seeds.seeds[0] = 7;
    s.initial_seeds.seeds[1] = 11;
    s.initial_seeds.use_init_seeds = 1;
    return nle_start(obs, NULL, &s);
}

static nle_ctx_t *key(nle_ctx_t *g, nle_obs *obs, int k)
{
    obs->action = k;
    return nle_step(g, obs);
}

/* n keys of a walk; a --More-- is answered with return */
static int w;
static nle_ctx_t *play(nle_ctx_t *g, nle_obs *obs, int n)
{
    for (; n > 0 && g && !obs->done; n--)
        g = key(g, obs, obs->misc[2] ? '\r' : walk[w++ % (sizeof walk - 1)]);
    return g;
}

static nle_ctx_t *keys(nle_ctx_t *g, nle_obs *obs, const char *k)
{
    for (; *k && g && !obs->done; k++)
        g = key(g, obs, *k);
    return g;
}

/* the mapped arrays bound after the start equal those of the same game bound from the start (stock keeps them whatever the
   bindings), key by key from the binding on */
static int map_late(const char *hackdir, int pile)
{
    static nle_obs a, b;
    static unsigned char ch[2][MAP], co[2][MAP], sp[2][MAP];
    static long bl[2][NLE_BLSTATS_SIZE];
    int i, j, n, diff = 0, cells = 0, marks = 0;
    long t;
    nle_ctx_t *ga, *gb;

    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.blstats = bl[0], b.blstats = bl[1];
    a.misc = misc[0], b.misc = misc[1];
    a.chars = ch[0], a.colors = co[0], a.specials = sp[0];
    w = 0;
    ga = start(&a, hackdir);
    ga = pile ? keys(ga, &a, pile_keys) : play(ga, &a, 60);
    w = 0;
    gb = start(&b, hackdir);
    gb = pile ? keys(gb, &b, pile_keys) : play(gb, &b, 60);
    b.chars = ch[1], b.colors = co[1], b.specials = sp[1];
    t = bl[1][NLE_BL_TIME];
    for (n = 0; n < 30 && !a.done && !b.done; n++) {
        int wa = w;

        ga = play(ga, &a, 1);
        w = wa;
        gb = play(gb, &b, 1);
        for (j = 0, i = 0; i < MAP; i++) {
            j += ch[0][i] != ch[1][i] || co[0][i] != co[1][i] || sp[0][i] != sp[1][i];
            marks += !n && (sp[0][i] & 0x40);
        }
        cells += j;
        diff += j > 0;
    }
    nle_end(ga);
    nle_end(gb);
    printf("%s: chars/colors/specials bound at turn %ld%s: %d of %d keys differ (%d cells)%s\n", diff ? "FAIL" : "ok",
           t, pile ? " (hallucinating, blind, a remembered pile)" : "", diff, n, cells,
           pile && !marks ? ", but no pile mark on the map" : "");
    return !diff && (!pile || marks);
}

/* the inventory glyphs, letters and classes bound after the start (here while hallucinating) equal those of the same game bound
   from the start and leave the game alone (stock keeps its item list whatever the bindings); the names are formatted only
   while inv_strs is bound, so binding it later is refused and the game goes on as before */
static int inv_late(const char *hackdir)
{
    static nle_obs a, b;
    static short ig[2][NLE_INVENTORY_SIZE];
    static unsigned char il[2][NLE_INVENTORY_SIZE], io[2][NLE_INVENTORY_SIZE], is[NLE_INVENTORY_SIZE * NLE_INVENTORY_STR_LENGTH];
    static long bl[2][NLE_BLSTATS_SIZE];
    unsigned long long da[PHD_NCOMP], db[PHD_NCOMP];
    int n, diff = 0, state = 0, refused = 1;
    long t;
    nle_ctx_t *ga, *gb, *r;

    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.blstats = bl[0], b.blstats = bl[1];
    a.misc = misc[0], b.misc = misc[1];
    a.inv_glyphs = ig[0], a.inv_letters = il[0], a.inv_oclasses = io[0];
    ga = keys(start(&a, hackdir), &a, pile_keys);
    gb = keys(start(&b, hackdir), &b, pile_keys);
    b.inv_glyphs = ig[1], b.inv_letters = il[1], b.inv_oclasses = io[1];
    t = bl[1][NLE_BL_TIME];
    w = 0;
    for (n = 0; n < 30 && !a.done && !b.done; n++) {
        int wa = w;

        ga = play(ga, &a, 1);
        w = wa;
        gb = play(gb, &b, 1);
        diff += memcmp(ig[0], ig[1], sizeof ig[0]) || memcmp(il[0], il[1], sizeof il[0]) || memcmp(io[0], io[1], sizeof io[0]);
        ph_digest(ga, &a, 0, NULL, da);
        ph_digest(gb, &b, 0, NULL, db);
        state += memcmp(da, db, sizeof(da[0]) * PHD_KEY) != 0;
        if (n == 10) {
            b.inv_strs = is;
            r = key(gb, &b, 's');
            refused &= !r && !nle_obs_refresh(gb, &b);
            b.inv_strs = NULL;
        }
    }
    nle_end(ga);
    nle_end(gb);
    printf("%s: inventory bound at turn %ld (hallucinating): %d of %d keys differ, %d with another game; inv_strs bound late %s\n",
           !diff && !state && refused ? "ok" : "FAIL", t, diff, n, state, refused ? "refused" : "accepted");
    return !diff && !state && refused;
}

/* the terminal opened at the start whatever the bindings: rows the late-bound step did not draw keep the caller's bytes */
static int tty_late(const char *hackdir)
{
    static nle_obs obs, scr;
    static unsigned char chars[TTY], screen[TTY];
    static signed char colors[TTY], scolors[TTY];
    static unsigned char cursor[2], scursor[2];
    static long blstats[NLE_BLSTATS_SIZE];
    int i, kept = 0, bad = 0;
    nle_ctx_t *g;

    memset(&obs, 0, sizeof obs);
    obs.blstats = blstats;
    obs.misc = misc[0];
    g = play(start(&obs, hackdir), &obs, 40);
    memset(chars, 1, sizeof chars);
    obs.tty_chars = chars;
    obs.tty_colors = colors;
    obs.tty_cursor = cursor;
    g = key(g, &obs, 's');
    memset(&scr, 0, sizeof scr);
    scr.tty_chars = screen;
    scr.tty_colors = scolors;
    scr.tty_cursor = scursor;
    g = nle_obs_refresh(g, &scr);
    for (i = 0; i < NLE_TERM_LI; i++) {
        const unsigned char *r = chars + i * NLE_TERM_CO;
        int j, ones = 0;

        for (j = 0; j < NLE_TERM_CO; j++)
            ones += r[j] == 1;
        if (ones == NLE_TERM_CO)
            kept++;
        else if (memcmp(r, screen + i * NLE_TERM_CO, NLE_TERM_CO))
            bad++;
    }
    nle_end(g);
    printf("%s: tty bound at turn %ld: %d rows kept the caller's bytes, %d drawn rows differ from the terminal\n",
           kept && !bad ? "ok" : "FAIL", blstats[NLE_BL_TIME], kept, bad);
    return kept && !bad;
}

int main(int argc, char **argv)
{
    int ok = 1;

    if (argc < 2) {
        fprintf(stderr, "usage: latebind HACKDIR\n");
        return 2;
    }
    ok &= tty_late(argv[1]);
    ok &= map_late(argv[1], 0);
    ok &= map_late(argv[1], 1);
    ok &= inv_late(argv[1]);
    return ok ? 0 : 1;
}
