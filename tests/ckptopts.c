/* a checkpoint loads only into a game started with the same options: number_pad on one side and off on the other would read
   the keys of one game with the command table of the other */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "ph_digest.h"

#define OPTS "name:Agent,role:Val,race:human,gender:female,align:neutral,!autopickup,nobones,nolegacy"

static const char walk[] = "hjklyubnsss";

struct game {
    nle_obs obs;
    nle_ctx_t *g;
    long bl[NLE_BLSTATS_SIZE];
    int misc[NLE_MISC_SIZE];
    unsigned char msg[NLE_MESSAGE_SIZE];
};

static void start(struct game *s, const char *hackdir, const char *opts, unsigned long seed)
{
    static nle_settings st;

    memset(&st, 0, sizeof st);
    snprintf(st.hackdir, sizeof st.hackdir, "%s", hackdir);
    snprintf(st.options, sizeof st.options, "%s", opts);
    st.spawn_monsters = 1;
    st.initial_seeds.seeds[0] = seed;
    st.initial_seeds.seeds[1] = seed + 4;
    st.initial_seeds.use_init_seeds = 1;
    memset(&s->obs, 0, sizeof s->obs);
    s->obs.blstats = s->bl;
    s->obs.misc = s->misc;
    s->obs.message = s->msg;
    s->g = nle_start(&s->obs, NULL, &st);
}

static void key(struct game *s, int k)
{
    s->obs.action = k;
    s->g = nle_step(s->g, &s->obs);
}

static void play(struct game *s, int n)
{
    for (int i = 0; i < n && !s->obs.done; i++)
        key(s, s->misc[2] ? '\r' : walk[i % (sizeof walk - 1)]);
}

/* the random streams, message, position, turn and done of two games */
static int same(struct game *a, struct game *b)
{
    unsigned long long da[PHD_NCOMP], db[PHD_NCOMP];

    ph_digest(a->g, &a->obs, 0, NULL, da);
    ph_digest(b->g, &b->obs, 0, NULL, db);
    return !memcmp(da, db, sizeof(da[0]) * PHD_KEY);
}

/* source A saves after 30 keys; T (target) and its twin U start alike; the image goes into T; then every game gets the same keys */
static int run(const char *hackdir, const char *aopts, const char *topts, int want)
{
    static struct game a, t, u;
    const char *ks = "h4jk";
    void *ck = NULL;
    size_t len = 0;
    int rc, n, follows = 1, kept = 1;

    start(&a, hackdir, aopts, 3);
    play(&a, 30);
    for (n = 0; !ck && n < 20; n++)
        if (!(ck = nle_ckpt_save(a.g, &len)))
            key(&a, a.misc[2] ? '\r' : 27);
    start(&t, hackdir, topts, 9);
    start(&u, hackdir, topts, 9);
    play(&t, 5);
    play(&u, 5);
    rc = ck ? nle_ckpt_load(t.g, ck, len) : -1;
    for (n = 0; ks[n]; n++) {
        key(&a, ks[n]);
        key(&t, ks[n]);
        key(&u, ks[n]);
        follows &= same(&a, &t);
        kept &= same(&t, &u);
    }
    nle_ckpt_free(ck);
    nle_end(a.g);
    nle_end(t.g);
    nle_end(u.g);
    if (want == 0) {
        printf("%s: same options (%s, %s): load %d, the loaded game follows the saved one: %s\n", !rc && follows ? "ok" : "FAIL",
               aopts[0] == '@' ? "files" : "strings", aopts[0] == '@' ? "two paths" : "one string", rc, follows ? "yes" : "no");
        return !rc && follows;
    }
    printf("%s: number_pad differs (%s): load %d, the target plays on as its twin: %s%s\n", rc == want && kept ? "ok" : "FAIL",
           aopts[0] == '@' ? "files" : "strings", rc, kept ? "yes" : "no",
           rc ? "" : follows ? ", the loaded game follows the saved one" : ", the loaded game reads the keys otherwise");
    return rc == want && kept;
}

static void rc(char *opts, size_t n, const char *hackdir, const char *name, const char *text)
{
    char path[4200];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", hackdir, name);
    f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
    snprintf(opts, n, "@%s", path);
}

int main(int argc, char **argv)
{
    int ok = 1;

    if (argc < 2) {
        fprintf(stderr, "usage: ckptopts HACKDIR\n");
        return 2;
    }
    ok &= run(argv[1], OPTS ",number_pad:1", OPTS ",number_pad:1", 0);
    ok &= run(argv[1], OPTS ",number_pad:1", OPTS, 8);
    ok &= run(argv[1], OPTS, OPTS ",number_pad:1", 8);
    {
        static char np[4300], np2[4300], plain[4300];

        rc(np, sizeof np, argv[1], "rc_np", "OPTIONS=" OPTS "\nOPTIONS=number_pad:1\n");
        rc(np2, sizeof np2, argv[1], "rc_np2", "OPTIONS=" OPTS "\nOPTIONS=number_pad:1\n");
        rc(plain, sizeof plain, argv[1], "rc_plain", "OPTIONS=" OPTS "\n");
        ok &= run(argv[1], np, np2, 0);
        ok &= run(argv[1], np, plain, 8);
    }
    return ok ? 0 : 1;
}
