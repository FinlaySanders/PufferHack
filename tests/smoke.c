/* the public API end to end: pufferhack.h alone (no other include path), a config, a short seeded game */
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
    ph_game *g = ph_start(&obs, NULL, &cfg);
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
    printf("steps %d turn %ld hp %ld done %d carried %d/%d\n", n, blstats[PH_BL_TIME], blstats[PH_BL_HP], obs.done, carried,
           capacity);
    ph_end(g);
    return blstats[PH_BL_TIME] > 1 ? 0 : 1;
}
