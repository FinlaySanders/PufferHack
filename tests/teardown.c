/* a game cut while a monster is dead but not yet detached (keys_both/g000354 ends inside a pet's bite) is torn down as stock
   NLE 0.9.0 tears it down: freedynamicdata's dmonsfree frees the monster and reports impossible("dmonsfree: 1 removed doesn't
   match 0 pending ..."), which reaches the terminal once the window port is closed */
#include <stdlib.h>
#include "engine.h"
#include "keylog.h"

static keylog_t K;
static struct hackdir H;

int main(int argc, char **argv)
{
    static nle_settings s;
    static nle_obs obs;
    static short glyphs[21 * 79], inv_glyphs[NLE_INVENTORY_SIZE];
    static unsigned char chars[21 * 79], colors[21 * 79], specials[21 * 79], msg[NLE_MESSAGE_SIZE];
    static unsigned char inv_strs[NLE_INVENTORY_SIZE * NLE_INVENTORY_STR_LENGTH], inv_letters[NLE_INVENTORY_SIZE];
    static unsigned char inv_oclasses[NLE_INVENTORY_SIZE], tty[NLE_TERM_LI * NLE_TERM_CO + 1], cursor[2];
    static signed char tty_colors[NLE_TERM_LI * NLE_TERM_CO];
    static long blstats[NLE_BLSTATS_SIZE];
    static int prog_state[NLE_PROGRAM_STATE_SIZE], internal[32], misc[NLE_MISC_SIZE];
    const char *want = "dmonsfree: 1 removed doesn't match 0 pending";
    char rc[4200];
    FILE *f;
    int i, found;

    if (argc < 4) {
        fprintf(stderr, "usage: teardown DATDIR WORKDIR KEYLOG\n");
        return 2;
    }
    if (keylog_read(argv[3], &K) != 0)
        return 2;
    snprintf(rc, sizeof rc, "%s/hack", argv[2]);
    if (hd_init(&H, argv[1], rc) != 0 || hd_reset(&H, &K) != 0)
        return 2;
    snprintf(rc, sizeof rc, "%s/nhrc", argv[2]);
    if (!(f = fopen(rc, "w")))
        return 2;
    fputs(K.rc, f);
    fclose(f);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", H.dir);
    snprintf(s.options, sizeof s.options, "@%s", rc);
    s.spawn_monsters = K.spawn;
    s.underfoot_glyphs = K.underfoot;
    s.initial_seeds.seeds[0] = K.seed[0];
    s.initial_seeds.seeds[1] = K.seed[1];
    s.initial_seeds.reseed = (char) K.reseed;
    s.initial_seeds.use_init_seeds = K.use_init;
    s.fix_moon_phase = K.fix_moon;
    s.time_seed = K.time_seed;
    s.time_seed_is_set = K.time_set;
    obs.glyphs = glyphs, obs.chars = chars, obs.colors = colors, obs.specials = specials, obs.blstats = blstats;
    obs.message = msg, obs.prog_state = prog_state, obs.internal = internal, obs.inv_glyphs = inv_glyphs;
    obs.inv_strs = inv_strs, obs.inv_letters = inv_letters, obs.inv_oclasses = inv_oclasses, obs.tty_chars = tty;
    obs.tty_colors = tty_colors, obs.tty_cursor = cursor, obs.misc = misc;
    nle_ctx_t *g = nle_start(&obs, NULL, &s);
    for (i = 0; i < K.nkeys && !obs.done; i++) {
        obs.action = K.keys[i];
        g = nle_step(g, &obs);
    }
    if (obs.done || i != K.nkeys) {
        printf("FAIL: teardown: the game ended at key %d of %d\n", i, K.nkeys);
        return 1;
    }
    nle_end(g);
    for (i = 0; i < NLE_TERM_LI * NLE_TERM_CO; i++)
        if (!tty[i])
            tty[i] = ' ';
    found = strstr((char *) tty, want) != NULL;
    printf("%s: teardown frees the dead jackal and reports \"%s\"\n", found ? "ok" : "FAIL", want);
    return !found;
}
