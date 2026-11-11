/* seedapi LIBDIR: the seeding API against stock NLE 0.9.0's semantics, several games in one process.
   T1 ph_get_seed/ph_set_seed act on the game passed in (any thread); T2 lgen in a game started without it changes nothing;
   T3 lgen in a game started with it reseeds the base and every dungeon stream from the new seed. Exit status = failures. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "nletypes.h"

struct fld { unsigned off, size; const char *name; };
typedef nle_ctx_t *(*start_t)(nle_obs *, FILE *, nle_settings *);
typedef nle_ctx_t *(*step_t)(nle_ctx_t *, nle_obs *);
typedef void (*gs_t)(nle_ctx_t *, unsigned long *, unsigned long *, int *, unsigned long *, int *);
typedef void (*ss_t)(nle_ctx_t *, unsigned long, unsigned long, int, unsigned long);
typedef int (*dig_t)(nle_ctx_t *, const nle_obs *, unsigned, void *, unsigned long long *);
static start_t st;
static step_t sp;
static gs_t gs;
static ss_t ss;
static dig_t dig;
static const struct fld *flds;
static unsigned nflds;
static const char *dat;
static int fails;
static char dirs[8][4200];
static int ndirs;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}
static const unsigned char *field(nle_ctx_t *c, const char *name, unsigned *size)
{
    for (unsigned i = 0; i < nflds; i++)
        if (!strcmp(flds[i].name, name)) {
            *size = flds[i].size;
            return (const unsigned char *) c->nh + flds[i].off;
        }
    fprintf(stderr, "no field %s\n", name);
    exit(100);
}
static nle_ctx_t *game(nle_obs *o, unsigned long core, unsigned long disp, int use_lgen, unsigned long lgen)
{
    static nle_settings s;
    char *dir = dirs[ndirs++], cmd[8500];
    snprintf(dir, sizeof dirs[0], "%s/seedapi.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir))
        exit(101);
    snprintf(cmd, sizeof cmd, "cp -a %s/. %s/", dat, dir);
    if (system(cmd))
        exit(102);
    memset(&s, 0, sizeof s);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", dir);
    snprintf(s.options, sizeof s.options, "name:Agent,role:valkyrie,race:human,gender:female,align:lawful,nobones,nolegacy");
    s.spawn_monsters = 1;
    s.initial_seeds.seeds[0] = core, s.initial_seeds.seeds[1] = disp, s.initial_seeds.use_init_seeds = 1;
    s.initial_seeds.lgen_seed = lgen, s.initial_seeds.use_lgen_seed = use_lgen;
    return st(o, NULL, &s);
}
static int seeds_are(nle_ctx_t *c, unsigned long core, unsigned long disp)
{
    unsigned long co = 0, di = 0, lg = 0;
    int rs = -1, lu = -1;
    gs(c, &co, &di, &rs, &lg, &lu);
    return co == core && di == disp;
}
static nle_ctx_t *A_;
static void *thr_get(void *p)
{
    *(int *) p = seeds_are(A_, 0x1111, 0x2222);
    return 0;
}
static void digest(nle_ctx_t *c, nle_obs *o, unsigned long long *d)
{
    memset(d, 0, 64 * sizeof *d);
    dig(c, o, 0, NULL, d);
}

int main(int argc, char **argv)
{
    char p[4200];
    if (argc < 2) {
        fprintf(stderr, "usage: seedapi LIBDIR (LIBDIR/libnethack.so, LIBDIR/dat)\n");
        return 2;
    }
    snprintf(p, sizeof p, "%s/libnethack.so", argv[1]);
    static char datp[4200];
    snprintf(datp, sizeof datp, "%s/dat", argv[1]);
    dat = datp;
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    st = (start_t) dlsym(h, "nle_start"), sp = (step_t) dlsym(h, "nle_step");
    gs = (gs_t) dlsym(h, "ph_get_seed"), ss = (ss_t) dlsym(h, "ph_set_seed"), dig = (dig_t) dlsym(h, "ph_digest");
    flds = (const struct fld *) dlsym(h, "nh_ckpt_field");
    const unsigned *nf = (const unsigned *) dlsym(h, "nh_ckpt_nfield");
    if (!st || !sp || !gs || !ss || !dig || !flds || !nf) {
        fprintf(stderr, "missing symbols\n");
        return 2;
    }
    nflds = *nf;
    static nle_obs oa, ob, og, oh;
    static short ga[21 * 79], gb[21 * 79], gg[21 * 79], gh[21 * 79];
    static long ba[NLE_BLSTATS_SIZE], bb[NLE_BLSTATS_SIZE];
    oa.glyphs = ga, ob.glyphs = gb, og.glyphs = gg, oh.glyphs = gh;
    oa.blstats = ba, ob.blstats = bb;
    unsigned long long d0[64], d1[64];
    unsigned n, n2;

    /* T1: the game passed in, not the calling thread's last game */
    A_ = game(&oa, 0x1111, 0x2222, 0, 0);
    nle_ctx_t *B = game(&ob, 0x3333, 0x4444, 0, 0);
    check(seeds_are(A_, 0x1111, 0x2222), "T1 get_seed(A) after start(A), start(B) is A's seeds");
    check(seeds_are(B, 0x3333, 0x4444), "T1 get_seed(B) after start(A), start(B) is B's seeds");
    oa.action = 27;
    A_ = sp(A_, &oa);
    check(seeds_are(B, 0x3333, 0x4444), "T1 get_seed(B) after step(A) is B's seeds");
    digest(A_, &oa, d0);
    ss(B, 0x5555, 0x6666, 0, 0);
    digest(A_, &oa, d1);
    check(seeds_are(A_, 0x1111, 0x2222), "T1 set_seed(B) leaves get_seed(A)");
    check(!memcmp(d0, d1, sizeof d0), "T1 set_seed(B) leaves A's random streams and state digest");
    check(seeds_are(B, 0x5555, 0x6666), "T1 set_seed(B) sets B's seeds");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int ok = 0;
        pthread_t t;
        pthread_create(&t, 0, thr_get, &ok);
        pthread_join(t, 0);
        _exit(ok ? 0 : 1);
    }
    int stt = 0;
    waitpid(pid, &stt, 0);
    check(WIFEXITED(stt) && WEXITSTATUS(stt) == 0, "T1 get_seed(A) from a thread that never ran a game");

    /* T2: lgen in a game started without it */
    unsigned char *z;
    const unsigned char *ls = field(B, "g_nlernd_c_lgen_state", &n);
    z = calloc(1, n);
    ss(B, 0x7777, 0x8888, 0, 0x4d);
    unsigned long co, di, lg;
    int rs, lu;
    gs(B, &co, &di, &rs, &lg, &lu);
    check(!lu && !*(const int *) field(B, "g_nlernd_c_lgen_initialised", &n2), "T2 set_seed(lgen) does not switch a game to lgen");
    check(!memcmp(ls, z, n), "T2 the lgen streams stay unused");

    /* T3: lgen in a game started with it: G started with lgen 42, H with 43 then set_seed(lgen 42) */
    nle_ctx_t *G = game(&og, 0x9, 0xa, 1, 42);
    nle_ctx_t *H = game(&oh, 0x9, 0xa, 1, 43);
    const unsigned char *gs_ = field(G, "g_nlernd_c_lgen_state", &n), *hs = field(H, "g_nlernd_c_lgen_state", &n);
    const unsigned char *gb_ = field(G, "g_nlernd_c_lgen_base", &n2), *hb = field(H, "g_nlernd_c_lgen_base", &n2);
    unsigned one = n / 8;
    unsigned char *h0 = malloc(one);
    memcpy(h0, hs, one);
    check(memcmp(gs_ + one, hs + one, n - one) != 0, "T3 before: different lgen seeds give different dungeon streams");
    ss(H, 0x9, 0xa, 0, 42);
    gs(H, &co, &di, &rs, &lg, &lu);
    check(lu && lg == 42, "T3 get_seed reports the new lgen");
    check(!memcmp(gb_, hb, n2), "T3 set_seed(lgen 42) gives the base state a game started with lgen 42 has");
    check(!memcmp(gs_ + one, hs + one, n - one), "T3 set_seed(lgen 42) gives dungeons 1-7 the streams a game started with lgen 42 has");
    check(memcmp(h0, hs, one) != 0, "T3 set_seed(lgen 42) reseeds dungeon 0's stream too");
    printf("%d failed\n", fails);
    for (int i = 0; i < ndirs; i++) {
        char cmd[sizeof dirs[0] + 16];
        snprintf(cmd, sizeof cmd, "rm -rf %s", dirs[i]);
        if (system(cmd))
            fprintf(stderr, "could not remove %s\n", dirs[i]);
    }
    return fails;
}
