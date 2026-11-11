/* options NetHack rejects: it reports them before its windows exist, waits for return, then plays on, as stock NLE does */
#include <stdio.h>
#include <string.h>
#include "engine.h"

static nle_obs obs;
static long blstats[NLE_BLSTATS_SIZE];
static unsigned char msg[NLE_MESSAGE_SIZE], tty[NLE_TERM_LI * NLE_TERM_CO + 1];
static int misc[NLE_MISC_SIZE];

static int play(const char *hackdir, const char *options, const char *shown, const char *keys)
{
    static nle_settings s;
    const char cycle[] = "hjklyubn\r\033";
    int n = 0;

    memset(&s, 0, sizeof s);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", hackdir);
    snprintf(s.options, sizeof s.options, "%s", options);
    s.spawn_monsters = 1;
    s.initial_seeds.seeds[0] = 1;
    s.initial_seeds.seeds[1] = 2;
    s.initial_seeds.use_init_seeds = 1;
    memset(&obs, 0, sizeof obs);
    obs.blstats = blstats;
    obs.message = msg;
    obs.tty_chars = tty;
    obs.misc = misc;
    nle_ctx_t *g = nle_start(&obs, NULL, &s);
    int ok = !obs.done && misc[2] && !obs.in_normal_game && strstr((char *) tty, shown);
    if (!keys) { /* ended while it waits */
        nle_end(g);
        printf("%s: %s, ended at the wait\n", ok ? "ok" : "FAIL", shown);
        return ok;
    }
    obs.action = ' ';
    g = nle_step(g, &obs);
    ok = ok && misc[2];
    for (; *keys; keys++) {
        obs.action = *keys;
        g = nle_step(g, &obs);
    }
    while (!obs.done && n < 2000) {
        obs.action = cycle[n++ % 10];
        g = nle_step(g, &obs);
    }
    ok = ok && blstats[NLE_BL_TIME] > 1;
    printf("%s: %s turn %ld\n", ok ? "ok" : "FAIL", shown, blstats[NLE_BL_TIME]);
    nle_end(g);
    return ok;
}

int main(int argc, char **argv)
{
    char rc[4200];
    int ok = 1;

    if (argc < 3) {
        fprintf(stderr, "usage: badoptions HACKDIR BADRCFILE\n");
        return 2;
    }
    snprintf(rc, sizeof rc, "@%s", argv[2]);
    ok &= play(argv[1], rc, "1 error in", NULL);
    ok &= play(argv[1], rc, "1 error in", "\r");
    ok &= play(argv[1], "name:Agent,role:Val,race:human,gender:female,align:neutral,nosuchoption", "1 error in NETHACKOPTIONS.",
               "\r");
    ok &= play(argv[1], "OPTIONS=name:Agent\n", "Unknown option 'OPTIONS=name:Agent'", "\rAgent\ryy");
    ok &= play(argv[1], "@/nonexistent/nethackrc", "Access to /nonexistent/nethackrc denied", "\rAgent\ryy");
    return ok ? 0 : 1;
}
