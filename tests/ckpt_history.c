/* a game loaded from a checkpoint plays on as the game that saved it, ^P history included: game A plays to a command prompt
   and saves; game B (another seed, other messages) plays, then loads A's checkpoint; both then pray, answer the prompt with ^P
   twice and n, and must show the same terminal after every key */
#include <stdio.h>
#include <string.h>
#include "engine.h"

static const int pre_a[] = { 's', 's', 's', 'i', 27, 's', 's' };
static const int pre_b[] = { 'l', 'l', 'k', 's', 'i', 27, 'j', 's', 's', 's' };
static const int tail[] = { '#', 'p', 'r', 'a', 'y', 13, 16, 16, 16, 'n', 27, 16, 's', 16 };

struct game {
    nle_settings s;
    nle_obs obs;
    long blstats[NLE_BLSTATS_SIZE];
    unsigned char msg[NLE_MESSAGE_SIZE], tty[NLE_TERM_LI * NLE_TERM_CO], cur[2];
    signed char ttyc[NLE_TERM_LI * NLE_TERM_CO];
    int misc[NLE_MISC_SIZE];
    nle_ctx_t *g;
};

static void start(struct game *x, const char *hackdir, unsigned long seed)
{
    memset(x, 0, sizeof *x);
    snprintf(x->s.hackdir, sizeof x->s.hackdir, "%s", hackdir);
    snprintf(x->s.options, sizeof x->s.options, "%s",
             "name:Agent,role:Wizard,race:human,gender:male,align:neutral,autopickup,color,!tombstone,nobones,nolegacy,"
             "nocmdassist,time,!status_updates");
    x->s.spawn_monsters = 1;
    x->s.initial_seeds.seeds[0] = seed;
    x->s.initial_seeds.seeds[1] = seed * 0x9e3779b97f4a7c15UL;
    x->s.initial_seeds.use_init_seeds = 1;
    x->s.fix_moon_phase = 1;
    x->s.time_seed = seed;
    x->s.time_seed_is_set = 1;
    x->obs.blstats = x->blstats, x->obs.message = x->msg, x->obs.tty_chars = x->tty, x->obs.tty_colors = x->ttyc;
    x->obs.tty_cursor = x->cur, x->obs.misc = x->misc;
    x->g = nle_start(&x->obs, NULL, &x->s);
}

static void play(struct game *x, const int *k, int n)
{
    for (int i = 0; i < n && !x->obs.done; i++) {
        x->obs.action = k[i];
        x->g = nle_step(x->g, &x->obs);
    }
}

int main(int argc, char **argv)
{
    static struct game a, b;
    void *buf = NULL;
    size_t cap = 0, len;
    int bad = 0;

    if (argc < 2)
        return fprintf(stderr, "usage: ckpt_history HACKDIR\n"), 2;
    start(&a, argv[1], 7);
    play(&a, pre_a, (int) (sizeof pre_a / sizeof *pre_a));
    if (!(len = nle_ckpt_save_into(a.g, &buf, &cap)))
        return fprintf(stderr, "FAIL: no save at A's prompt\n"), 1;
    start(&b, argv[1], 11);
    play(&b, pre_b, (int) (sizeof pre_b / sizeof *pre_b));
    int rc = nle_ckpt_load(b.g, buf, len);
    if (rc)
        return fprintf(stderr, "FAIL: load rc %d\n", rc), 1;
    nle_obs_refresh(b.g, &b.obs);
    for (int i = 0; i < (int) (sizeof tail / sizeof *tail); i++) {
        a.obs.action = b.obs.action = tail[i];
        a.g = nle_step(a.g, &a.obs);
        b.g = nle_step(b.g, &b.obs);
        if (memcmp(a.tty, b.tty, sizeof a.tty) && bad++ < 3) {
            for (int r = 0; r < NLE_TERM_LI; r++)
                if (memcmp(a.tty + r * NLE_TERM_CO, b.tty + r * NLE_TERM_CO, NLE_TERM_CO))
                    printf("key %d row %d\n  A |%.80s|\n  B |%.80s|\n", i, r, (char *) a.tty + r * NLE_TERM_CO,
                           (char *) b.tty + r * NLE_TERM_CO);
        }
    }
    nle_ckpt_free(buf);
    nle_end(a.g), nle_end(b.g);
    if (bad)
        return printf("FAIL: the loaded game's terminal differs from the saving game's at %d keys\n", bad), 1;
    printf("ok: a loaded game shows the saving game's ^P history\n");
    return 0;
}
