/* the public API end to end: pufferhack.h alone (no other include path), a config, two short seeded games on the shared,
   read-only data directory (make test checks that it is left unchanged and no private directory is left behind) */
#include <stdio.h>
#include "pufferhack.h"

int main(int argc, char **argv)
{
    static ph_obs obs;
    static long blstats[PH_BLSTATS_SIZE];
    static unsigned char msg[PH_MESSAGE_SIZE];
    ph_config cfg;

    if (argc < 3) {
        fprintf(stderr, "usage: smoke DATADIR OPTIONSFILE\n");
        return 2;
    }
    ph_config_default(&cfg);
    cfg.data_dir = argv[1];
    cfg.options_file = argv[2];
    cfg.seeded = 1;
    cfg.seed_core = 1;
    cfg.seed_disp = 2;
    obs.blstats = blstats;
    obs.message = msg;
    long turns[2];
    for (int game = 0; game < 2; game++) {
        ph_game *g = ph_start(&obs, &cfg);
        if (!g)
            return fprintf(stderr, "ph_start failed\n"), 1;
        const char keys[] = "hjklyubn\r\033";
        int n = 0;
        while (!obs.done && n < 2000) {
            obs.action = keys[n % 10];
            g = ph_step(g, &obs);
            n++;
        }
        int carried, capacity;
        ph_weight(g, &carried, &capacity);
        printf("game %d: steps %d turn %ld hp %ld done %d carried %d/%d\n", game, n, blstats[PH_BL_TIME], blstats[PH_BL_HP],
               obs.done, carried, capacity);
        turns[game] = blstats[PH_BL_TIME];
        ph_end(g);
    }
    /* a game that ends writes its record and log lines: into its private directory, not the data directory */
    ph_game *g = ph_start(&obs, &cfg);
    const char quit[] = "\033#quit\ry";
    for (int i = 0; quit[i] && !obs.done; i++) {
        obs.action = quit[i];
        g = ph_step(g, &obs);
    }
    for (int i = 0; i < 20 && !obs.done; i++) {
        obs.action = '\033';
        g = ph_step(g, &obs);
    }
    printf("game 2: quit, done %d\n", obs.done);
    int ended = obs.done;
    ph_end(g);
    return turns[0] > 1 && turns[0] == turns[1] && ended ? 0 : 1;
}
