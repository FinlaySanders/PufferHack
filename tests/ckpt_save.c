/* a checkpoint save leaves the game it saves as it was: no context byte (pointers aside) and no part of the state digest moves,
   and a game saved at every command prompt plays on as the unsaved one (both refresh their observations at every key). The keys
   annotate dlvl 1, then a confused read of teleportation answers the level prompt with that name: stock asks again, since
   dlvl 1 is not yet VISITED; then ^P history */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "nh_ckpt.h"
#include "ph_digest.h"

static const int keys[] = { 35, 97, 110, 110, 111, 116, 97, 116, 101, 13, 102, 111, 111, 13, 80, 101, 114, 113, 102, 114, 105, 13, 13,
                            102, 111, 111, 13, 27, 115, 115, 115, 115, 115, 16, 16, 16, 16, 27, 115, 16, 16, 16 };
#define NKEYS ((int) (sizeof keys / sizeof *keys))

static nle_obs obs;
static short glyphs[21 * 79];
static long blstats[NLE_BLSTATS_SIZE];
static unsigned char msg[NLE_MESSAGE_SIZE], tty[NLE_TERM_LI * NLE_TERM_CO], ttyc[NLE_TERM_LI * NLE_TERM_CO], cur[2];
static int misc[NLE_MISC_SIZE];
static unsigned long long dig[2][NKEYS + 1][PHD_NCOMP];
static unsigned char before[1 << 20], isptr[1 << 20];

static const char *field(unsigned off)
{
    for (unsigned i = 0; i < nh_ckpt_nfield; i++)
        if (off >= nh_ckpt_field[i].off && off < nh_ckpt_field[i].off + nh_ckpt_field[i].size)
            return nh_ckpt_field[i].name;
    return "(padding)";
}

static int play(const char *hackdir, int save)
{
    static nle_settings s;
    void *buf = NULL;
    size_t cap = 0;
    int bad = 0, saves = 0;

    memset(&s, 0, sizeof s);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", hackdir);
    snprintf(s.options, sizeof s.options, "%s",
             "name:Agent,role:Wizard,race:human,gender:male,align:neutral,autopickup,color,disclose:-i -a -v -g -c -o,"
             "!tombstone,mention_walls,nobones,nocmdassist,nolegacy,nosparkle,pickup_burden:unencumbered,pickup_types:$?!/,"
             "runmode:teleport,showexp,showscore,time,!status_updates");
    s.spawn_monsters = 1;
    s.underfoot_glyphs = 1;
    s.initial_seeds.seeds[0] = 0xa84;
    s.initial_seeds.seeds[1] = 0x9e3779b97f4a7691UL;
    s.initial_seeds.use_init_seeds = 1;
    s.fix_moon_phase = 1;
    s.time_seed = 0xa84;
    s.time_seed_is_set = 1;
    memset(&obs, 0, sizeof obs);
    memset(glyphs, 0, sizeof glyphs), memset(blstats, 0, sizeof blstats), memset(msg, 0, sizeof msg);
    memset(tty, 0, sizeof tty), memset(ttyc, 0, sizeof ttyc), memset(cur, 0, sizeof cur), memset(misc, 0, sizeof misc);
    obs.glyphs = glyphs, obs.blstats = blstats, obs.message = msg, obs.tty_chars = tty, obs.tty_colors = ttyc;
    obs.tty_cursor = cur, obs.misc = misc;
    nle_ctx_t *g = nle_start(&obs, NULL, &s);
    for (int i = 0; i <= NKEYS && !obs.done; i++) {
        if (i) {
            obs.action = keys[i - 1];
            g = nle_step(g, &obs);
        }
        if (obs.done) {
            ph_digest(g, &obs, 0, NULL, dig[save][i]);
            break;
        }
        if (save) {
            unsigned long long pre[PHD_NCOMP], post[PHD_NCOMP];
            ph_digest(g, &obs, PHD_STATE, NULL, pre);
            memcpy(before, g->nh, nh_ckpt_ctx_size);
            if (nle_ckpt_save_into(g, &buf, &cap)) {
                saves++;
                ph_digest(g, &obs, PHD_STATE, NULL, post);
                for (int c = 0; c < PHD_NCOMP; c++)
                    if (post[c] != pre[c] && bad++ < 5)
                        printf("FAIL: the save after key %d changed %s\n", i, ph_digest_name(c));
                for (unsigned b = 0; b < nh_ckpt_ctx_size; b++)
                    if (!isptr[b] && ((unsigned char *) g->nh)[b] != before[b]) {
                        if (bad++ < 5)
                            printf("FAIL: the save after key %d changed context byte %u (%s)\n", i, b, field(b));
                        break;
                    }
            }
        }
        g = nle_obs_refresh(g, &obs);
        ph_digest(g, &obs, PHD_STATE, NULL, dig[save][i]);
    }
    nle_end(g);
    nle_ckpt_free(buf);
    if (save && !saves)
        printf("FAIL: no save succeeded\n"), bad++;
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ckpt_save HACKDIR\n");
        return 2;
    }
    if (nh_ckpt_ctx_size > sizeof before)
        return 2;
    for (unsigned i = 0; i < nh_ckpt_nptr; i++)
        memset(isptr + nh_ckpt_ptr[i], 1, sizeof(void *));
    int bad = play(argv[1], 0) + play(argv[1], 1);
    for (int i = 0; i <= NKEYS; i++)
        if (memcmp(dig[0][i], dig[1][i], sizeof dig[0][i])) {
            for (int c = 0; c < PHD_NCOMP; c++)
                if (dig[0][i][c] != dig[1][i][c]) {
                    printf("FAIL: saved and unsaved games differ at key %d in %s\n", i, ph_digest_name(c));
                    break;
                }
            bad++;
            break;
        }
    printf("%s: checkpoint saves leave the game unchanged\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
