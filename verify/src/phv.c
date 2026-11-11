/* phv: the fast digest-first stock differential (PufferHack verify/, DESIGN section 13).
 *
 *   phv certify LIST OUTDIR [options]   both sides over every game of LIST, compare, rerun what differs, write the verdict
 *   phv game SIDE LOG [options]          one game: --perkey FILE (per-key records), --text-at K FILE (full text state at key K)
 *   phv cmpkey FORK.pk STOCK.pk LOG [options]   today's per-game verdict line from two per-key files
 *
 * Sides: "fork" = the engine library (--lib DIR/libnethack.so, which exports ph_digest), "stock" = untouched stock NLE 0.9.0
 * (--ref LIB) digested by phd_stock.so (--stock-digest, the same src/ph_digest.c compiled against stock's headers). Each worker
 * slot is one process: fork workers play games back to back in one process (a crash or timeout ends the worker, the game is
 * recorded with the shell's exit status, and a fresh worker takes the slot); stock workers load stock once and play each game
 * in a fork()ed child of that pristine image (stock keeps its game in globals). Per game and side the runner folds every key's
 * digest components into chains in shared memory; a game whose chains, lengths or exit statuses differ is rerun on both sides
 * in fresh processes with per-key records, compared key by key in C with today's rules (cmp.py, statecmp.py, abortcmp.py), and
 * then dumped as text at the first differing key to name the first differing field. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <dirent.h>
#include <x86intrin.h>
#include "nletypes.h"
#define PHD_STOCK 1 /* the enum only: phv never links a game library */
#include "ph_digest.h"
#include "keylog.h"

/* ---------------------------------------------------------------- records */
enum { RS_PENDING, RS_RUNNING, RS_DONE, RS_FAILED };
struct rec {
    int32_t state, rc;        /* rc: the shell's $? for the replay (0, exit code, 128 + signal, 124 timeout) */
    uint32_t lines, slines;   /* digest lines (keys played + 1), state lines */
    uint32_t logkeys, done;   /* keys in the log; the last digest line's done */
    uint64_t chain[PHD_NCOMP];
    uint64_t ns, tsc_step, tsc_dig;
    int32_t worker, setup;    /* setup: nonzero = the log or the hack directory could not be set up */
    int32_t poked, pad;       /* the state-perturbation control: the component of the section poked (+0x100: the fallback), -1 none */
};
struct pk { /* one per-key record */
    int32_t i, key;
    uint32_t st, pad;
    uint64_t v[PHD_NCOMP];
};

static inline uint64_t chainmix(uint64_t c, uint64_t v)
{
    c ^= v * 0x9E3779B185EBCA87ULL;
    c = (c << 27 | c >> 37) * 0xC2B2AE3D27D4EB4FULL + 0x165667B19E3779F9ULL;
    return c;
}
static void fold(struct rec *r, const uint64_t *v, int st)
{
    for (int c = 0; c < PHD_NDIG; c++)
        r->chain[c] = chainmix(r->chain[c], v[c]);
    r->lines++;
    r->done = (uint32_t) v[PHD_DONE];
    if (st) {
        for (int c = PHD_SEC0; c < PHD_NCOMP; c++)
            r->chain[c] = chainmix(r->chain[c], v[c]);
        r->slines++;
    }
}
static uint64_t keycomp(int i, int key)
{
    return (uint64_t) (uint32_t) i << 32 | (uint32_t) key;
}
static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000ULL + (uint64_t) t.tv_nsec;
}

/* ---------------------------------------------------------------- options */
static struct {
    const char *lib, *dat, *ref, *sdat, *sdig, *mode;
    int jobs, fjobs, timeout, keep, no_emu, bindall, rerun_cap;
    double fshare, fresh;
    unsigned long seed;
    const char *ignore;             /* state components statecmp ignores */
    const char *fault, *poke, *poke_lib; /* negative controls: --fault LOG:KEY:COMP,..., --poke LOG:KEY:NAME,... with --poke-lib */
} O = { .dat = NULL, .mode = "default", .jobs = 0, .fjobs = -1, .timeout = 900, .fshare = 0.25, .rerun_cap = 500, .fresh = 0.005 };

/* every child dies with its parent: no replay outlives a killed phv */
static pid_t cfork(void)
{
    pid_t parent = getpid();
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(1);
    }
    return p;
}

static const char *ph_comp_name(int c)
{
    static const char *const n[PHD_NCOMP] = {
        "core", "disp", "msg", "xyt", "done", "key",
        "u", "attr", "prop", "skill", "spell", "inv", "mon", "floor", "buried", "migr", "trap", "engr", "timer", "light", "region",
        "cell", "vis", "room", "oc", "disco", "ctx", "lflags", "mv", "linfo",
        "obs.gl", "obs.ch", "obs.co", "obs.sp", "obs.bl", "obs.msg", "obs.ps", "obs.in", "obs.ig", "obs.is", "obs.il", "obs.io",
        "obs.tc", "obs.tk", "obs.tu", "obs.mi"
    };
    return n[c];
}

/* ---------------------------------------------------------------- per-game setup shared by both sides */
static keylog_t K;
static struct hackdir H;
static char g_wd[4096];
static unsigned long g_seed;

static int write_rc(const char *rc, int stock_status_updates, char *opt, size_t n)
{
    char p[4200];
    snprintf(p, sizeof p, "%s/nhrc", g_wd);
    FILE *r = fopen(p, "w");
    if (!r)
        return -1;
    if (stock_status_updates) { /* stock fills blstats only through the status pipeline */
        static char rc2[sizeof K.rc];
        char *q;
        snprintf(rc2, sizeof rc2, "%s", rc);
        while ((q = strstr(rc2, "!status_updates")))
            memmove(q, q + 1, strlen(q));
        fputs(rc2, r);
    } else
        fputs(rc, r);
    fclose(r);
    snprintf(opt, n, "@%s", p);
    return 0;
}

/* ---------------------------------------------------------------- fork side */
typedef nle_ctx_t *(*f_start_t)(nle_obs *, FILE *, nle_settings *);
typedef nle_ctx_t *(*f_step_t)(nle_ctx_t *, nle_obs *);
typedef void (*f_end_t)(nle_ctx_t *);
typedef int (*f_dig_t)(nle_ctx_t *, const nle_obs *, unsigned, void *, unsigned long long *);
typedef int (*f_txt_t)(nle_ctx_t *, const nle_obs *, FILE *);
static f_start_t f_start;
static f_step_t f_step;
static f_end_t f_end;
static f_dig_t f_dig;
static f_txt_t f_txt;
static unsigned long long (*f_id)(void);
static unsigned long (*f_wsz)(void);
static void *f_ws;
static const char *(*f_poke)(void *, const char *);

static int fork_load(void)
{
    char p[4200];
    snprintf(p, sizeof p, "%s/libnethack.so", O.lib);
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "phv: dlopen %s: %s\n", p, dlerror());
        return -1;
    }
    f_start = (f_start_t) dlsym(h, "nle_start");
    f_step = (f_step_t) dlsym(h, "nle_step");
    f_end = (f_end_t) dlsym(h, "nle_end");
    f_dig = (f_dig_t) dlsym(h, "ph_digest");
    f_txt = (f_txt_t) dlsym(h, "ph_digest_text");
    f_id = (unsigned long long (*)(void)) dlsym(h, "ph_digest_id");
    f_wsz = (unsigned long (*)(void)) dlsym(h, "ph_digest_ws_size");
    if (!f_start || !f_step || !f_end || !f_dig || !f_txt || !f_id || !f_wsz) {
        fprintf(stderr, "phv: %s does not export nle_start/nle_step/nle_end/ph_digest/ph_digest_text/ph_digest_id/ph_digest_ws_size\n", p);
        return -1;
    }
    if (!getenv("PHV_NO_WS"))
        f_ws = calloc(1, f_wsz());
    if (O.poke_lib) { /* built against this library's headers; refused when the context layout differs */
        void *ph = dlopen(O.poke_lib, RTLD_NOW | RTLD_LOCAL);
        unsigned long (*psz)(void) = ph ? (unsigned long (*)(void)) dlsym(ph, "phv_poke_ctx_size") : NULL;
        const unsigned *lsz = (const unsigned *) dlsym(h, "nh_ckpt_ctx_size");
        f_poke = ph ? (const char *(*)(void *, const char *)) dlsym(ph, "phv_poke") : NULL;
        if (!ph || !psz || !f_poke || !lsz || psz() != *lsz) {
            fprintf(stderr, "phv: poke library %s: %s\n", O.poke_lib, !ph ? dlerror() : "missing symbols or built for another context layout");
            return -1;
        }
    }
    return 0;
}

static nle_obs FO;
static void *falloc(int on, void **slot, size_t n)
{
    if (!*slot)
        *slot = calloc(1, n);
    if (!on)
        return NULL;
    memset(*slot, 0, n);
    return *slot;
}

/* negative controls, both on the engine side and inherited by its reruns; each must be reported at exactly its key.
   --fault LOG:KEY:COMP[,...] flips the low bit of digest component COMP at key KEY of game LOG (the comparison's plumbing);
   --poke LOG:KEY:NAME[,...] changes one field of the live game after key KEY, before the digest (phv_poke.so, NAME from its
   table): the digest must read it. A poke whose record does not exist at that key falls back to ctx, which every game has. */
static int spec_entries(const char *spec, const char *log, int *keys, char (*names)[16], int max)
{
    int n = 0;
    if (!spec)
        return 0;
    char *d = strdup(spec);
    for (char *save = NULL, *t = strtok_r(d, ",", &save); t && n < max; t = strtok_r(NULL, ",", &save)) {
        char *c2 = strrchr(t, ':');
        if (!c2)
            continue;
        *c2 = 0;
        char *c1 = strrchr(t, ':');
        if (!c1)
            continue;
        *c1 = 0;
        if (strcmp(t, log))
            continue;
        keys[n] = atoi(c1 + 1);
        snprintf(names[n++], 16, "%s", c2 + 1);
    }
    free(d);
    return n;
}
static int comp_index(const char *name)
{
    for (int c = 0; c < PHD_NCOMP; c++)
        if (!strcmp(name, ph_comp_name(c)))
            return c;
    return -1;
}
static int nfault, fault_key[64], fault_comp[64], npoke, poke_key[64];
static char poke_name[64][16];
static void fault_setup(const char *log)
{
    char nm[64][16];
    int k[64], n = spec_entries(O.fault, log, k, nm, 64);
    nfault = 0;
    for (int i = 0; i < n; i++)
        if (comp_index(nm[i]) >= 0)
            fault_key[nfault] = k[i], fault_comp[nfault++] = comp_index(nm[i]);
    npoke = spec_entries(O.poke, log, poke_key, poke_name, 64);
}
static void poke_apply(nle_ctx_t *c, int i, struct rec *r)
{
    for (int q = 0; q < npoke; q++)
        if (poke_key[q] == i) {
            const char *sec = f_poke ? f_poke(c, poke_name[q]) : NULL;
            int fb = 0;
            if (!sec && f_poke)
                sec = f_poke(c, "ctx"), fb = 0x100;
            r->poked = sec && comp_index(sec) >= 0 ? comp_index(sec) | fb : -2;
        }
}
static void fault_apply(int i, uint64_t *v)
{
    for (int f = 0; f < nfault; f++)
        if (fault_key[f] == i)
            v[fault_comp[f]] ^= 1;
}

/* one game on the engine: perkey fd >= 0 writes per-key records; text_at >= -1 writes the text state at that key */
static int fork_game(const char *log, struct rec *r, int pkfd, long text_at, FILE *tf)
{
    static void *bufs[19];
    static nle_settings s;
    char opt[4200];
    int ok = keylog_read(log, &K);
    r->setup = ok;
    if (ok != 0)
        return -1;
    r->logkeys = (uint32_t) K.nkeys;
    r->poked = -1;
    fault_setup(log);
    if (hd_reset(&H, &K) != 0) {
        r->setup = -3;
        return -1;
    }
    memset(&s, 0, sizeof s);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", H.dir);
    if (K.options[0] == '@') {
        if (write_rc(K.rc, 0, opt, sizeof opt) != 0) {
            r->setup = -4;
            return -1;
        }
        snprintf(s.options, sizeof s.options, "%s", opt);
    } else
        snprintf(s.options, sizeof s.options, "%s", K.options);
    s.spawn_monsters = K.spawn;
    s.underfoot_glyphs = K.underfoot;
    s.initial_seeds.seeds[0] = K.seed[0];
    s.initial_seeds.seeds[1] = K.seed[1];
    s.initial_seeds.reseed = (char) K.reseed;
    s.initial_seeds.use_init_seeds = K.use_init;
    s.initial_seeds.lgen_seed = K.lgen;
    s.initial_seeds.use_lgen_seed = K.use_lgen;
    s.fix_moon_phase = K.fix_moon;
    s.time_seed = K.time_seed;
    s.time_seed_is_set = K.time_set;
    s.true_glyphs = strcmp(K.trueg, "-") != 0; /* logs record the trainer's NLE_TRUE_GLYPHS, now a setting */
    const char *b = K.bind;
    int A = O.bindall;
#define ON(i) (b[i] == '1')
    memset(&FO, 0, sizeof FO);
    FO.glyphs = falloc(ON(0) || A, &bufs[0], 21 * 79 * sizeof(short));
    FO.chars = falloc(ON(1) || A, &bufs[1], 21 * 79);
    FO.colors = falloc(ON(2) || A, &bufs[2], 21 * 79);
    FO.specials = falloc(ON(3) || A, &bufs[3], 21 * 79);
    FO.blstats = falloc(ON(4) || A, &bufs[4], NLE_BLSTATS_SIZE * sizeof(long));
    FO.message = falloc(ON(5) || A, &bufs[5], NLE_MESSAGE_SIZE);
    FO.prog_state = falloc(ON(6) || A, &bufs[6], NLE_PROGRAM_STATE_SIZE * sizeof(int));
    FO.internal = falloc(ON(7) || A, &bufs[7], 32 * sizeof(int));
    FO.inv_glyphs = falloc(ON(8) || A, &bufs[8], NLE_INVENTORY_SIZE * sizeof(short));
    FO.inv_strs = falloc(ON(9) || A, &bufs[9], NLE_INVENTORY_SIZE * NLE_INVENTORY_STR_LENGTH);
    FO.inv_letters = falloc(ON(10) || A, &bufs[10], NLE_INVENTORY_SIZE);
    FO.inv_oclasses = falloc(ON(11) || A, &bufs[11], NLE_INVENTORY_SIZE);
    FO.screen_descriptions = falloc(ON(12), &bufs[12], 21 * 79 * 80);
    FO.tty_chars = falloc(ON(13) || A, &bufs[13], NLE_TERM_LI * NLE_TERM_CO);
    FO.tty_colors = falloc(ON(14) || A, &bufs[14], NLE_TERM_LI * NLE_TERM_CO);
    FO.tty_cursor = falloc(ON(15) || A, &bufs[15], 2);
    FO.misc = falloc(ON(16) || A, &bufs[16], NLE_MISC_SIZE * sizeof(int));
    FO.inv_state = falloc(ON(17), &bufs[17], NLE_INVENTORY_SIZE * NLE_INV_STATE_FIELDS);
    FO.inv_true_glyphs = falloc(ON(18), &bufs[18], NLE_INVENTORY_SIZE * sizeof(short));
#undef ON
    uint64_t v[PHD_NCOMP], t0 = now_ns();
    struct pk pk;
    if (f_ws)
        memset(f_ws, 0, f_wsz());
    nle_ctx_t *c = f_start(&FO, NULL, &s);
    uint64_t a = __rdtsc();
    f_dig(c, &FO, PHD_STATE, f_ws, (unsigned long long *) v);
    r->tsc_dig += __rdtsc() - a;
    v[PHD_KEY] = keycomp(-1, -1);
    fault_apply(-1, v);
    fold(r, v, 1);
    if (pkfd >= 0) {
        pk.i = -1, pk.key = -1, pk.st = 1, pk.pad = 0;
        memcpy(pk.v, v, sizeof v);
        if (write(pkfd, &pk, sizeof pk) != sizeof pk)
            return -1;
    }
    if (text_at == -1 && tf)
        f_txt(c, &FO, tf);
    for (int i = 0; i < K.nkeys && !FO.done; i++) {
        FO.action = K.keys[i];
        a = __rdtsc();
        c = f_step(c, &FO);
        uint64_t m = __rdtsc();
        if (npoke)
            poke_apply(c, i, r);
        int st = !FO.done;
        f_dig(c, &FO, st ? PHD_STATE : 0, f_ws, (unsigned long long *) v);
        r->tsc_step += m - a;
        r->tsc_dig += __rdtsc() - m;
        v[PHD_KEY] = keycomp(i, K.keys[i]);
        fault_apply(i, v);
        fold(r, v, st);
        if (pkfd >= 0) {
            pk.i = i, pk.key = K.keys[i], pk.st = (uint32_t) st;
            memcpy(pk.v, v, sizeof v);
            if (write(pkfd, &pk, sizeof pk) != sizeof pk)
                return -1;
        }
        if (text_at == i && tf)
            f_txt(c, &FO, tf), fflush(tf);
    }
    r->ns = now_ns() - t0;
    if (tf)
        fflush(tf);
    f_end(c);
    return 0;
}

/* ---------------------------------------------------------------- stock side */
typedef struct {
    int action, done;
    char in_normal_game;
    int how_done;
    short *glyphs;
    unsigned char *chars, *colors, *specials;
    long *blstats;
    unsigned char *message;
    int *program_state, *internal;
    short *inv_glyphs;
    unsigned char *inv_strs, *inv_letters, *inv_oclasses, *screen_descriptions, *tty_chars;
    signed char *tty_colors;
    unsigned char *tty_cursor;
    int *misc;
} sobs_t; /* stock 0.9.0 nle_obs */
typedef struct {
    char hackdir[4096], scoreprefix[4096], options[32768], wizkit[4096];
    int spawn_monsters;
    char ttyrecname[4096];
} sset_t; /* stock 0.9.0 nle_settings */
typedef struct {
    unsigned long seeds[2];
    char reseed;
} sseed_t;
struct phs_in {
    const void *rng[2];
    void *st[7];
    const void *obs[PHD_NOBS];
    const unsigned char *msg;
    int done, in_normal_game, true_glyphs, underfoot, no_emu;
};
static void *g_dl;
static void *(*s_start)(sobs_t *, FILE *, sseed_t *, sset_t *);
static void *(*s_step)(void *, sobs_t *);
static void (*s_end)(void *);
static int (*s_dig)(const struct phs_in *, unsigned, unsigned long long *);
static int (*s_txt)(const struct phs_in *, FILE *);
static unsigned long long (*s_id)(void), (*s_aid)(void);
static void (*s_prof)(void);
static void *s_stat[7];
static const char *s_rng;

/* stock runs the game on a 64 KiB coroutine stack, which deep call chains overflow; the fork uses 256 KiB (as the old driver,
   exported so stock's call binds here) */
fcontext_stack_t create_fcontext_stack(size_t size)
{
    size_t page = (size_t) sysconf(_SC_PAGESIZE), sz = size < 262144 ? 262144 : size;
    fcontext_stack_t s = { 0, 0 };
    void *vp = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vp == MAP_FAILED)
        return s;
    mprotect(vp, page, PROT_NONE);
    s.sptr = (char *) vp + sz;
    s.ssize = sz;
    return s;
}

/* the fork's fixed clock for the logged time seed (stock_replay_je.c) */
static time_t fake_time(time_t *t)
{
    time_t v = (time_t) (1600000000L + (long) (g_seed % 100000UL) * 257L);
    if (t)
        *t = v;
    return v;
}
static void (*g_iinit)(void *, const unsigned char *, int);
static unsigned long (*g_inext)(void *, unsigned long);
static struct tm *fake_localtime(const time_t *t)
{
    static struct tm tmv;
    static unsigned char rng[16384];
    unsigned char sb[8];
    (void) t;
    memcpy(sb, &g_seed, 8);
    g_iinit(rng, sb, 8);
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = 100 + (int) g_inext(rng, 50);
    tmv.tm_mon = (int) g_inext(rng, 12);
    tmv.tm_mday = 1 + (int) g_inext(rng, 28);
    tmv.tm_hour = (int) g_inext(rng, 24);
    tmv.tm_wday = (int) g_inext(rng, 7);
    tmv.tm_yday = tmv.tm_mon * 30 + tmv.tm_mday;
    return &tmv;
}
static int patch_got(void)
{
    struct link_map *lm = NULL;
    if (dlinfo(g_dl, RTLD_DI_LINKMAP, &lm) != 0 || !lm)
        return -1;
    ElfW(Dyn) *d = lm->l_ld;
    const char *strtab = NULL;
    ElfW(Sym) *symtab = NULL;
    ElfW(Rela) *jr = NULL, *ra = NULL;
    size_t jsz = 0, rsz = 0;
    for (; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRTAB) strtab = (const char *) d->d_un.d_ptr;
        else if (d->d_tag == DT_SYMTAB) symtab = (ElfW(Sym) *) d->d_un.d_ptr;
        else if (d->d_tag == DT_JMPREL) jr = (ElfW(Rela) *) d->d_un.d_ptr;
        else if (d->d_tag == DT_PLTRELSZ) jsz = d->d_un.d_val;
        else if (d->d_tag == DT_RELA) ra = (ElfW(Rela) *) d->d_un.d_ptr;
        else if (d->d_tag == DT_RELASZ) rsz = d->d_un.d_val;
    }
    int n = 0;
    long page = sysconf(_SC_PAGESIZE);
    for (int pass = 0; pass < 2; pass++) {
        ElfW(Rela) *r = pass ? ra : jr;
        size_t sz = pass ? rsz : jsz;
        for (size_t i = 0; r && i < sz / sizeof(ElfW(Rela)); i++) {
            unsigned type = ELF64_R_TYPE(r[i].r_info);
            if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT)
                continue;
            const char *name = strtab + symtab[ELF64_R_SYM(r[i].r_info)].st_name;
            void *fn = !strcmp(name, "time") ? (void *) fake_time : !strcmp(name, "localtime") ? (void *) fake_localtime : NULL;
            if (!fn)
                continue;
            void **slot = (void **) (lm->l_addr + r[i].r_offset);
            mprotect((void *) ((unsigned long) slot & ~(page - 1)), page, PROT_READ | PROT_WRITE);
            *slot = fn;
            n++;
        }
    }
    return n;
}
static unsigned long local_symbol(const char *path, const char *want)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0)
        return 0;
    unsigned char *m = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    ElfW(Ehdr) *eh = (ElfW(Ehdr) *) m;
    ElfW(Shdr) *sh = (ElfW(Shdr) *) (m + eh->e_shoff);
    unsigned long val = 0;
    for (int i = 0; i < eh->e_shnum && !val; i++) {
        if (sh[i].sh_type != SHT_SYMTAB)
            continue;
        ElfW(Sym) *sym = (ElfW(Sym) *) (m + sh[i].sh_offset);
        const char *str = (const char *) (m + sh[sh[i].sh_link].sh_offset);
        for (size_t k = 0; k < sh[i].sh_size / sizeof(ElfW(Sym)); k++)
            if (ELF64_ST_TYPE(sym[k].st_info) == STT_OBJECT && !strcmp(str + sym[k].st_name, want)) {
                val = sym[k].st_value;
                break;
            }
    }
    munmap(m, (size_t) st.st_size);
    return val;
}

static int stock_load(void)
{
    g_dl = dlopen(O.ref, RTLD_NOW | RTLD_LOCAL);
    if (!g_dl) {
        fprintf(stderr, "phv: dlopen %s: %s\n", O.ref, dlerror());
        return -1;
    }
    g_iinit = dlsym(g_dl, "isaac64_init");
    g_inext = dlsym(g_dl, "isaac64_next_uint");
    if (patch_got() < 2) {
        fprintf(stderr, "phv: stock time/localtime GOT slots not found\n");
        return -1;
    }
    s_start = dlsym(g_dl, "nle_start");
    s_step = dlsym(g_dl, "nle_step");
    s_end = dlsym(g_dl, "nle_end");
    struct link_map *lm = NULL;
    dlinfo(g_dl, RTLD_DI_LINKMAP, &lm);
    unsigned long rl = local_symbol(O.ref, "rnglist");
    if (!rl || !s_start || !s_step || !s_end || !g_iinit || !g_inext) {
        fprintf(stderr, "phv: stock symbols missing (rnglist in .symtab, nle_*, isaac64_*)\n");
        return -1;
    }
    s_rng = (const char *) lm->l_addr + rl;
    const char *names[7] = { "head_engr", "timer_base", "disco", "light_base", "regions", "n_regions", "artiexist" };
    for (int j = 0; j < 7; j++) {
        unsigned long a = local_symbol(O.ref, names[j]);
        if (!a) {
            fprintf(stderr, "phv: stock static %s not found\n", names[j]);
            return -1;
        }
        s_stat[j] = (char *) lm->l_addr + a;
    }
    /* the digest library is linked against the stock library (soname libnethack.so), so its globals bind to this copy */
    void *dh = dlopen(O.sdig, RTLD_NOW | RTLD_LOCAL);
    if (!dh) {
        fprintf(stderr, "phv: dlopen %s: %s\n", O.sdig, dlerror());
        return -1;
    }
    s_dig = dlsym(dh, "phs_digest");
    s_txt = dlsym(dh, "phs_text");
    s_id = dlsym(dh, "phs_id");
    s_aid = dlsym(dh, "phs_adapter_id");
    s_prof = dlsym(dh, "phs_prof");
    if (!s_dig || !s_txt || !s_id) {
        fprintf(stderr, "phv: %s lacks phs_digest/phs_text/phs_id\n", O.sdig);
        return -1;
    }
    return 0;
}

static void *salloc(int on, size_t n)
{
    return on ? calloc(1, n) : NULL;
}

/* one game on stock, in a process of its own (the caller forks) */
static int stock_game(const char *log, struct rec *r, int pkfd, long text_at, FILE *tf)
{
    static sset_t s;
    static sobs_t o;
    char opt[4200];
    int ok = keylog_read(log, &K);
    r->setup = ok;
    if (ok != 0)
        return -1;
    r->logkeys = (uint32_t) K.nkeys;
    r->poked = -1;
    g_seed = K.time_set ? K.time_seed : K.seed[0];
    if (hd_reset(&H, &K) != 0) {
        r->setup = -3;
        return -1;
    }
    memset(&s, 0, sizeof s);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", H.dir);
    if (K.options[0] == '@') {
        if (write_rc(K.rc, !O.bindall, opt, sizeof opt) != 0) {
            r->setup = -4;
            return -1;
        }
        snprintf(s.options, sizeof s.options, "%s", opt);
    } else
        snprintf(s.options, sizeof s.options, "%s", K.options);
    s.spawn_monsters = K.spawn;
    sseed_t sd = { { K.seed[0], K.seed[1] }, (char) K.reseed };
    const char *b = K.bind;
    int A = O.bindall;
#define ON(i) (b[i] == '1' || A)
    memset(&o, 0, sizeof o);
    o.glyphs = salloc(ON(0), 21 * 79 * sizeof(short));
    o.chars = salloc(ON(1), 21 * 79);
    o.colors = salloc(ON(2), 21 * 79);
    o.specials = salloc(ON(3), 21 * 79);
    o.blstats = salloc(ON(4), 27 * sizeof(long));
    o.message = salloc(ON(5), 256);
    o.program_state = salloc(ON(6), 6 * sizeof(int));
    o.internal = salloc(ON(7), 32 * sizeof(int));
    o.inv_glyphs = salloc(ON(8), 55 * sizeof(short));
    o.inv_strs = salloc(ON(9), 55 * 80);
    o.inv_letters = salloc(ON(10), 55);
    o.inv_oclasses = salloc(ON(11), 55);
    o.screen_descriptions = NULL; /* binding it makes stock draw from the core RNG */
    o.tty_chars = salloc(ON(13), 24 * 80);
    o.tty_colors = salloc(ON(14), 24 * 80);
    o.tty_cursor = salloc(ON(15), 2);
    o.misc = salloc(ON(16), 3 * sizeof(int));
#undef ON
    struct phs_in in;
    memset(&in, 0, sizeof in);
    in.rng[0] = s_rng + 0x10;
    in.rng[1] = s_rng + 0x1040;
    memcpy(in.st, s_stat, sizeof in.st);
    const void *ob[PHD_NOBS] = { o.glyphs, o.chars, o.colors, o.specials, o.blstats, o.message, o.program_state, o.internal,
                                 o.inv_glyphs, o.inv_strs, o.inv_letters, o.inv_oclasses, o.tty_chars, o.tty_colors,
                                 o.tty_cursor, o.misc };
    memcpy(in.obs, ob, sizeof ob);
    in.msg = o.message;
    in.true_glyphs = strcmp(K.trueg, "-") != 0;
    in.underfoot = K.underfoot;
    in.no_emu = O.no_emu;
    uint64_t v[PHD_NCOMP], t0 = now_ns();
    struct pk pk;
    void *c = s_start(&o, NULL, &sd, &s);
    in.done = o.done, in.in_normal_game = o.in_normal_game;
    uint64_t a = __rdtsc();
    s_dig(&in, PHD_STATE, (unsigned long long *) v);
    r->tsc_dig += __rdtsc() - a;
    v[PHD_KEY] = keycomp(-1, -1);
    fold(r, v, 1);
    if (pkfd >= 0) {
        pk.i = -1, pk.key = -1, pk.st = 1, pk.pad = 0;
        memcpy(pk.v, v, sizeof v);
        if (write(pkfd, &pk, sizeof pk) != sizeof pk)
            return -1;
    }
    if (text_at == -1 && tf)
        s_txt(&in, tf), fflush(tf);
    for (int i = 0; i < K.nkeys && !o.done; i++) {
        o.action = K.keys[i];
        a = __rdtsc();
        c = s_step(c, &o);
        uint64_t m = __rdtsc();
        int st = !o.done;
        in.done = o.done, in.in_normal_game = o.in_normal_game;
        s_dig(&in, st ? PHD_STATE : 0, (unsigned long long *) v);
        r->tsc_step += m - a;
        r->tsc_dig += __rdtsc() - m;
        v[PHD_KEY] = keycomp(i, K.keys[i]);
        fold(r, v, st);
        if (pkfd >= 0) {
            pk.i = i, pk.key = K.keys[i], pk.st = (uint32_t) st;
            memcpy(pk.v, v, sizeof v);
            if (write(pkfd, &pk, sizeof pk) != sizeof pk)
                return -1;
        }
        if (text_at == i && tf)
            s_txt(&in, tf), fflush(tf);
    }
    r->ns = now_ns() - t0;
    s_end(c);
    return 0;
}

/* ---------------------------------------------------------------- the game list and shared state */
struct game {
    char *path, *name;
    long keys;
};
static struct game *G;
static int NG;
static int *ORDER;

struct slot {
    int32_t pid, side, cur;
    int32_t pad;
    uint64_t t0;
};
struct shared {
    int32_t next[2];  /* next ORDER index per side (0 fork, 1 stock) */
    int32_t nslots, pad;
    struct slot slot[256];
};
static struct shared *SH;
static struct rec *REC[2]; /* [side][game] */

static int load_list(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[16384];
    int cap = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        char *p = strtok(line, "\t"), *n = strtok(NULL, "\t"), *k = strtok(NULL, "\t");
        if (!p)
            continue;
        if (NG == cap)
            cap = cap ? cap * 2 : 4096, G = realloc(G, cap * sizeof *G);
        G[NG].path = strdup(p);
        if (n)
            G[NG].name = strdup(n);
        else { /* corpus/name as sjob.sh prints it */
            char *b = strdup(p), *s = strrchr(b, '/'), *d;
            if (s)
                *s = 0;
            d = strrchr(b, '/');
            char nm[8192];
            const char *base = s ? s + 1 : p;
            snprintf(nm, sizeof nm, "%s/%.*s", d ? d + 1 : b, (int) (strlen(base) - (strstr(base, ".keys") ? 5 : 0)), base);
            G[NG].name = strdup(nm);
            free(b);
        }
        G[NG].keys = k ? atol(k) : 0;
        NG++;
    }
    fclose(f);
    ORDER = malloc(NG * sizeof *ORDER);
    for (int i = 0; i < NG; i++)
        ORDER[i] = i;
    return 0;
}
static int by_keys(const void *a, const void *b)
{
    long x = G[*(const int *) a].keys, y = G[*(const int *) b].keys;
    return x < y ? 1 : x > y ? -1 : (*(const int *) a - *(const int *) b);
}

static int rc_of(int status)
{
    if (WIFSIGNALED(status))
        return WTERMSIG(status) == SIGALRM ? 124 : 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}

static void quiet_stdio(void)
{
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        if (!getenv("PHV_STDERR"))
            dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
}

/* ---------------------------------------------------------------- workers */
static void worker_dir(int slotno, const char *side, const char *data)
{
    snprintf(g_wd, sizeof g_wd, "/dev/shm/phv_%d_%s%d", (int) getppid(), side, slotno);
    hd_rmtree(g_wd);
    mkdir(g_wd, 0755);
    char hd[4200];
    snprintf(hd, sizeof hd, "%s/hack", g_wd);
    if (hd_init(&H, data, hd) != 0) {
        fprintf(stderr, "phv: hack directory %s from %s failed\n", hd, data);
        _exit(3);
    }
}

static void fork_worker(int slotno)
{
    struct slot *sl = &SH->slot[slotno];
    quiet_stdio();
    worker_dir(slotno, "f", O.dat);
    if (fork_load() != 0)
        _exit(2);
    for (;;) {
        int g = __atomic_fetch_add(&SH->next[0], 1, __ATOMIC_SEQ_CST);
        if (g >= NG)
            break;
        int gi = ORDER[g];
        struct rec *r = &REC[0][gi];
        sl->cur = gi;
        sl->t0 = now_ns();
        r->worker = slotno;
        r->state = RS_RUNNING;
        alarm((unsigned) O.timeout);
        int e = fork_game(G[gi].path, r, -1, -2, NULL);
        alarm(0);
        r->rc = 0;
        r->state = e == 0 ? RS_DONE : RS_FAILED;
        sl->cur = -1;
    }
    hd_rmtree(g_wd);
    _exit(0);
}

static void stock_worker(int slotno)
{
    struct slot *sl = &SH->slot[slotno];
    quiet_stdio();
    worker_dir(slotno, "s", O.sdat);
    if (stock_load() != 0)
        _exit(2);
    for (;;) {
        int g = __atomic_fetch_add(&SH->next[1], 1, __ATOMIC_SEQ_CST);
        if (g >= NG)
            break;
        int gi = ORDER[g];
        struct rec *r = &REC[1][gi];
        sl->cur = gi;
        sl->t0 = now_ns();
        r->worker = slotno;
        r->state = RS_RUNNING;
        pid_t p = cfork();
        if (p == 0) { /* exit(): stock's exit handlers and destructors run, as when today's driver returns from main */
            alarm((unsigned) O.timeout);
            int e = stock_game(G[gi].path, r, -1, -2, NULL);
            exit(e == 0 ? 0 : 3);
        }
        int status = 0;
        while (waitpid(p, &status, 0) < 0 && errno == EINTR)
            ;
        r->rc = rc_of(status);
        r->state = r->setup ? RS_FAILED : RS_DONE;
        sl->cur = -1;
    }
    hd_rmtree(g_wd);
    _exit(0);
}

/* ---------------------------------------------------------------- single game (fresh process): per-key records and text dump */
static int one_game_fd(int side, const char *log, int pkfd, long text_at, FILE *tf, struct rec *r)
{
    snprintf(g_wd, sizeof g_wd, "/dev/shm/phv1_%d", (int) getpid());
    hd_rmtree(g_wd);
    mkdir(g_wd, 0755);
    char hd[4200];
    snprintf(hd, sizeof hd, "%s/hack", g_wd);
    if (hd_init(&H, side ? O.sdat : O.dat, hd) != 0)
        return 3;
    if (side ? stock_load() : fork_load())
        return 2;
    int e = side ? stock_game(log, r, pkfd, text_at, tf) : fork_game(log, r, pkfd, text_at, tf);
    if (tf)
        fflush(tf);
    if (side && s_prof)
        s_prof();
    hd_rmtree(g_wd);
    return e == 0 ? 0 : 3;
}
static int one_game(int side, const char *log, const char *pkpath, long text_at, const char *txtpath, struct rec *r)
{
    int fd = pkpath ? open(pkpath, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    FILE *tf = txtpath ? fopen(txtpath, "w") : NULL;
    int e = one_game_fd(side, log, fd, text_at, tf, r);
    if (fd >= 0)
        close(fd);
    if (tf)
        fclose(tf);
    return e;
}

/* one game in a fresh child writing into a pipe: its per-key records, or (text_at >= -1) the text state at key text_at; *rfd
   is the read end. Nothing is stored: the comparer reads both sides as they play. */
static pid_t spawn_into(int side, const char *log, long text_at, struct rec *r, int *rfd)
{
    int p[2];
    if (pipe(p) != 0)
        return -1;
    fcntl(p[1], F_SETPIPE_SZ, 1 << 20);
    pid_t c = cfork();
    if (c == 0) {
        close(p[0]);
        quiet_stdio();
        alarm((unsigned) O.timeout);
        FILE *tf = text_at >= -1 ? fdopen(p[1], "w") : NULL;
        int e = one_game_fd(side, log, tf ? -1 : p[1], text_at, tf, r);
        if (tf)
            fclose(tf);
        if (side)
            exit(e); /* as the main run's stock child */
        _exit(e);
    }
    close(p[1]);
    *rfd = p[0];
    return c;
}

/* ---------------------------------------------------------------- the per-key comparator (cmp.py, statecmp.py, abortcmp.py) */
struct pkf {
    struct pk *r;
    long n;
};
static int pk_load(const char *p, struct pkf *f)
{
    f->r = NULL;
    f->n = 0;
    int fd = open(p, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
        return -1;
    f->n = st.st_size / (long) sizeof(struct pk);
    if (f->n) {
        f->r = malloc(f->n * sizeof(struct pk));
        if (read(fd, f->r, f->n * sizeof(struct pk)) != (ssize_t) (f->n * sizeof(struct pk)))
            f->n = 0;
    }
    close(fd);
    return 0;
}

static int ign[PHD_NCOMP];
#define cname ph_comp_name

struct mark {
    char name[16];
    long at;
};
static int mark_add(struct mark *m, int n, const char *name, long at)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(m[i].name, name))
            return n;
    snprintf(m[n].name, sizeof m[n].name, "%s", name);
    m[n].at = at;
    return n + 1;
}
static int by_at(const void *a, const void *b) /* stable by insertion order: qsort is not stable, so ties by address */
{
    const struct mark *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : (x < y ? -1 : 1);
}
static int by_at_name(const void *a, const void *b)
{
    const struct mark *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : strcmp(x->name, y->name);
}
static void sort_stable(struct mark *m, int n)
{ /* insertion sort: stable */
    for (int i = 1; i < n; i++) {
        struct mark t = m[i];
        int j = i - 1;
        while (j >= 0 && m[j].at > t.at)
            m[j + 1] = m[j], j--;
        m[j + 1] = t;
    }
    (void) by_at;
}

/* today's per-game verdict line from the two sides' per-key records as they arrive ("<cmp.py> | <statecmp.py>", or abortcmp.py's
   "ABORTED_BOTH ... | STATE_ABORTED_BOTH"): vc_pair for the records both sides have at one index, vc_tail for the rest of the
   longer side, vc_end for the line and the first key any component differs at (-2 none, -3 aborted on both). Only a side's last
   record (the done key) can lack a state line, so a side's state lines are a prefix of its records and state line j is record
   j: the streams need no buffering. */
struct vcmp {
    const keylog_t *k;
    struct mark m[64], d[64]; /* cmp.py's and statecmp.py's marks, in the order those scripts find them */
    int nm, nd, bad, lastdone;
    long j, n[2], s[2];
};
static void vc_init(struct vcmp *v, const keylog_t *k)
{
    memset(v, 0, sizeof *v);
    v->k = k;
}
static void vc_tail(struct vcmp *v, int side, const struct pk *x)
{
    v->n[side]++;
    v->s[side] += x->st != 0;
    if (!side)
        v->lastdone = x->v[PHD_DONE] != 0;
}
static void vc_pair(struct vcmp *v, const struct pk *x, const struct pk *y)
{
    static const int parts[5] = { PHD_CORE, PHD_DISP, PHD_MSG, PHD_XYT, PHD_DONE };
    long i = v->j++ - 1;
    for (int q = 0; q < 5; q++)
        if (x->v[parts[q]] != y->v[parts[q]])
            v->nm = mark_add(v->m, v->nm, cname(parts[q]), i);
    if (!(x->i == i && y->i == i && x->key == y->key))
        v->nm = mark_add(v->m, v->nm, "key", i);
    if (i >= 0 && (i >= v->k->nkeys || x->key != v->k->keys[i]))
        v->nm = mark_add(v->m, v->nm, "keylog", i);
    if (x->i != y->i || x->key != y->key) /* abortcmp.py: every line both wrote equal */
        v->bad = 1;
    for (int c = 0; c < PHD_NDIG; c++)
        if (c != PHD_KEY && x->v[c] != y->v[c])
            v->bad = 1;
    if (x->st && y->st) {
        if (x->i != y->i)
            v->nd = mark_add(v->d, v->nd, "index", x->i < y->i ? x->i : y->i), v->bad = 1;
        for (int c = PHD_SEC0; c < PHD_NCOMP; c++)
            if (!ign[c] && x->v[c] != y->v[c])
                v->nd = mark_add(v->d, v->nd, cname(c), x->i), v->bad = 1;
    }
    vc_tail(v, 0, x);
    vc_tail(v, 1, y);
}
static void vc_end(struct vcmp *v, int frc, int src, char *out, size_t on, long *first)
{
    char r[4096], s[4096];
    long na = v->n[0], nb = v->n[1], z = na < nb ? na : nb, sx = v->s[0], sy = v->s[1], sz = sx < sy ? sx : sy;
    *first = -2;
    /* cmp.py ("empty" comes first there; with an empty side no pair was compared, so adding it now keeps that order) */
    if (!na || !nb)
        v->nm = mark_add(v->m, v->nm, "empty", -1);
    if (na != nb)
        v->nm = mark_add(v->m, v->nm, "length", z - 1);
    long n = z - 1 > 0 ? z - 1 : 0;
    int done = na && v->lastdone;
    if (na && nb && na == nb && n < v->k->nkeys && !done)
        v->nm = mark_add(v->m, v->nm, "short", n);
    if (!v->nm)
        snprintf(r, sizeof r, "IDENTICAL keys %ld (fork %ld lines, stock %ld, log %d keys%s)", n, na, nb, v->k->nkeys,
                 done ? ", done" : "");
    else {
        sort_stable(v->m, v->nm);
        int p = snprintf(r, sizeof r, "DIVERGE first at key %ld of %ld: ", v->m[0].at, n);
        for (int i = 0; i < v->nm; i++)
            p += snprintf(r + p, sizeof r - p, "%s%s@%ld", i ? ", " : "", v->m[i].name, v->m[i].at);
        snprintf(r + p, sizeof r - p, " (fork %ld lines, stock %ld, log %d keys)", na, nb, v->k->nkeys);
        *first = v->m[0].at;
    }
    /* statecmp.py */
    if (sx != sy)
        v->nd = mark_add(v->d, v->nd, "length", sz);
    if (!sx || !sy)
        v->nd = mark_add(v->d, v->nd, "empty", -1);
    if (!v->nd)
        snprintf(s, sizeof s, "STATE_IDENTICAL keys %ld", sx);
    else {
        qsort(v->d, v->nd, sizeof *v->d, by_at_name);
        int p = snprintf(s, sizeof s, "STATE_DIVERGE first at key %ld:", v->d[0].at);
        for (int i = 0; i < v->nd; i++)
            p += snprintf(s + p, sizeof s - p, " %s@%ld", v->d[i].name, v->d[i].at);
        if (*first == -2 || v->d[0].at < *first)
            *first = v->d[0].at;
    }
    /* abortcmp.py: stock exited nonzero */
    if (src != 0 && !(v->bad || !nb || na < nb || sx < sy)) {
        snprintf(r, sizeof r, "ABORTED_BOTH keys %ld (stock rc %d, fork rc %d)", nb - 1, src, frc);
        snprintf(s, sizeof s, "STATE_ABORTED_BOTH");
        *first = -3;
    }
    snprintf(out, on, "%s | %s", r, s);
}

/* the same over two per-key files (phv cmpkey) */
static void verdict_pk(const struct pkf *a, const struct pkf *b, const keylog_t *k, int frc, int src, char *out, size_t on,
                       long *first)
{
    struct vcmp v;
    long z = a->n < b->n ? a->n : b->n;
    vc_init(&v, k);
    for (long j = 0; j < z; j++)
        vc_pair(&v, &a->r[j], &b->r[j]);
    for (long j = z; j < a->n; j++)
        vc_tail(&v, 0, &a->r[j]);
    for (long j = z; j < b->n; j++)
        vc_tail(&v, 1, &b->r[j]);
    vc_end(&v, frc, src, out, on, first);
}

/* ---------------------------------------------------------------- text-dump field diff */
struct lines {
    char **l;
    long n;
};
static void read_lines(FILE *f, struct lines *L)
{
    L->l = NULL;
    L->n = 0;
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0, lc = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n')
            line[len - 1] = 0;
        if ((size_t) L->n == lc)
            lc = lc ? lc * 2 : 1024, L->l = realloc(L->l, lc * sizeof *L->l);
        L->l[L->n++] = strdup(line);
    }
    free(line);
}
static void free_lines(struct lines *L)
{
    for (long q = 0; q < L->n; q++)
        free(L->l[q]);
    free(L->l);
    L->l = NULL;
    L->n = 0;
}
static void first_token_diff(const char *a, const char *b, char *out, size_t n)
{
    const char *p = a, *q = b;
    for (;;) {
        size_t la = strcspn(p, " "), lb = strcspn(q, " ");
        if (la != lb || strncmp(p, q, la)) {
            snprintf(out, n, "%.*s vs %.*s", (int) (la > 120 ? 120 : la), p, (int) (lb > 120 ? 120 : lb), q);
            return;
        }
        if (!p[la] || !q[lb]) {
            snprintf(out, n, "%s", p[la] || q[lb] ? "one line is longer" : "equal");
            return;
        }
        p += la + 1, q += lb + 1;
    }
}
/* per section (the first word of a line), in the dump's order: the first record where the two dumps differ and its first
   differing field, or which side has more records */
static void field_diff(const struct lines *pa, const struct lines *pb, FILE *out, const char *indent)
{
    const struct lines A = *pa, B = *pb;
    long i = 0, j = 0;
    int shown = 0;
    if (!A.n || !B.n)
        fprintf(out, "%sno text dump from the %s side (it ended before this key)\n", indent, !A.n ? "engine" : "stock");
    while (i < A.n && j < B.n && shown < 16) {
        char sa[64] = "", sb[64] = "";
        sscanf(A.l[i], "%63s", sa);
        sscanf(B.l[j], "%63s", sb);
        if (strcmp(sa, sb)) { /* one side has no records of this section: skip it there */
            long ii = i, jj = j;
            char t[64];
            while (ii < A.n && (sscanf(A.l[ii], "%63s", t), strcmp(t, sb)))
                ii++;
            while (jj < B.n && (sscanf(B.l[jj], "%63s", t), strcmp(t, sa)))
                jj++;
            if (jj < B.n && (ii >= A.n || jj - j <= ii - i)) {
                fprintf(out, "%s%s: stock has records the engine does not, first:\n%s  stock: %.300s\n", indent, sb, indent, B.l[j]);
                j = jj;
            } else {
                fprintf(out, "%s%s: the engine has records stock does not, first:\n%s  fork:  %.300s\n", indent, sa, indent, A.l[i]);
                i = ii;
            }
            shown++;
            continue;
        }
        long ei = i, ej = j; /* the section's extent on each side */
        char t[64];
        while (ei < A.n && (sscanf(A.l[ei], "%63s", t), !strcmp(t, sa)))
            ei++;
        while (ej < B.n && (sscanf(B.l[ej], "%63s", t), !strcmp(t, sa)))
            ej++;
        long k = 0;
        while (i + k < ei && j + k < ej && !strcmp(A.l[i + k], B.l[j + k]))
            k++;
        if (i + k < ei && j + k < ej) {
            char d[300];
            first_token_diff(A.l[i + k], B.l[j + k], d, sizeof d);
            fprintf(out, "%s%s: %s (record %ld of %ld / %ld)\n%s  fork:  %.300s\n%s  stock: %.300s\n", indent, sa, d, k + 1,
                    ei - i, ej - j, indent, A.l[i + k], indent, B.l[j + k]);
            shown++;
        } else if (ei - i != ej - j) {
            fprintf(out, "%s%s: %ld records on the engine side, %ld on stock; first unmatched:\n%s  %s %.300s\n", indent, sa,
                    ei - i, ej - j, indent, ei - i > ej - j ? "fork: " : "stock:", ei - i > ej - j ? A.l[i + k] : B.l[j + k]);
            shown++;
        }
        i = ei, j = ej;
    }
    if (!shown && A.n && B.n)
        fprintf(out, "%sthe text dumps are equal at this key (the difference is outside the dumped fields)\n", indent);
}

/* ---------------------------------------------------------------- certify */
static double tsc_hz(void)
{
    uint64_t t0 = now_ns(), c0 = __rdtsc();
    struct timespec ts = { 0, 50000000 };
    nanosleep(&ts, NULL);
    return (double) (__rdtsc() - c0) / ((double) (now_ns() - t0) / 1e9);
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f), fputc(*s, f);
        else if ((unsigned char) *s < 0x20)
            fprintf(f, "\\u%04x", *s);
        else
            fputc(*s, f);
    }
    fputc('"', f);
}

/* ---------------------------------------------------------------- reruns */
/* one rerun game, in shared memory: its comparer writes the verdict and the field report, its replays their records */
#define PHV_DIVSZ 16384
struct rr {
    struct rec r[2];
    int32_t rc[2];
    long first;
    char verdict[1024];
    char div[PHV_DIVSZ];
};

static void mkdirs(const char *path) /* mkdir -p of path's directory */
{
    char d[4400];
    snprintf(d, sizeof d, "%s", path);
    for (char *q = d + 1; (q = strchr(q, '/')); q++)
        *q = 0, mkdir(d, 0755), *q = '/';
}
static void reap(pid_t p, int *rc)
{
    int status = 0;
    while (waitpid(p, &status, 0) < 0 && errno == EINTR)
        ;
    if (rc)
        *rc = rc_of(status);
    char d[64]; /* a replay that crashed leaves its hack directory */
    snprintf(d, sizeof d, "/dev/shm/phv1_%d", (int) p);
    hd_rmtree(d);
}

/* the comparer (a child of certify): both sides of game gi in fresh processes, their per-key records compared as they arrive
   through two pipes (O(1) storage, nothing on disk); then, when they differ and dump is set, both replayed once more to the first
   differing key and their text states compared field by field, also through pipes */
static void rerun_game(int gi, struct rr *R, int dump, const char *outdir)
{
    char path[4400];
    R->first = -2;
    if (keylog_read(G[gi].path, &K) != 0) {
        snprintf(R->verdict, sizeof R->verdict, "UNREADABLE_LOG | STATE_NOT_RUN");
        return;
    }
    int fa = -1, fb = -1;
    pid_t A = spawn_into(0, G[gi].path, -2, &R->r[0], &fa);
    pid_t B = A > 0 ? spawn_into(1, G[gi].path, -2, &R->r[1], &fb) : -1;
    if (A <= 0 || B <= 0) {
        snprintf(R->verdict, sizeof R->verdict, "RERUN_NOT_STARTED | STATE_NOT_RUN");
        return;
    }
    FILE *xa = fdopen(fa, "r"), *xb = fdopen(fb, "r"), *ka = NULL, *kb = NULL;
    setvbuf(xa, NULL, _IOFBF, 1 << 18);
    setvbuf(xb, NULL, _IOFBF, 1 << 18);
    if (O.keep) {
        snprintf(path, sizeof path, "%s/perkey/%s.fork.pk", outdir, G[gi].name);
        mkdirs(path);
        ka = fopen(path, "w");
        snprintf(path, sizeof path, "%s/perkey/%s.stock.pk", outdir, G[gi].name);
        kb = fopen(path, "w");
    }
    struct vcmp v;
    struct pk x, y;
    int ea = 0, eb = 0;
    vc_init(&v, &K);
    for (;;) {
        int ga = !ea && fread(&x, sizeof x, 1, xa) == 1, gb = !eb && fread(&y, sizeof y, 1, xb) == 1;
        ea |= !ga, eb |= !gb;
        if (ga && ka)
            fwrite(&x, sizeof x, 1, ka);
        if (gb && kb)
            fwrite(&y, sizeof y, 1, kb);
        if (ga && gb)
            vc_pair(&v, &x, &y);
        else if (ga)
            vc_tail(&v, 0, &x);
        else if (gb)
            vc_tail(&v, 1, &y);
        else
            break;
    }
    fclose(xa);
    fclose(xb);
    if (ka)
        fclose(ka);
    if (kb)
        fclose(kb);
    reap(A, &R->rc[0]);
    reap(B, &R->rc[1]);
    vc_end(&v, R->rc[0], R->rc[1], R->verdict, sizeof R->verdict, &R->first);
    if (!dump || R->first < -1)
        return;
    static struct rec dummy[2];
    A = spawn_into(0, G[gi].path, R->first, &dummy[0], &fa);
    B = A > 0 ? spawn_into(1, G[gi].path, R->first, &dummy[1], &fb) : -1;
    if (A <= 0 || B <= 0)
        return;
    struct lines LA, LB;
    xa = fdopen(fa, "r");
    read_lines(xa, &LA); /* the stock replay waits on its full pipe meanwhile: it depends on nothing of ours */
    fclose(xa);
    xb = fdopen(fb, "r");
    read_lines(xb, &LB);
    fclose(xb);
    reap(A, NULL);
    reap(B, NULL);
    char *buf = NULL;
    size_t n = 0;
    FILE *ms = open_memstream(&buf, &n);
    field_diff(&LA, &LB, ms, "    ");
    fclose(ms);
    if (n >= PHV_DIVSZ)
        snprintf(buf + PHV_DIVSZ - 64, 64, "    ... (report cut at %d bytes)\n", PHV_DIVSZ - 64);
    snprintf(R->div, PHV_DIVSZ, "%s", buf ? buf : "");
    free(buf);
    if (O.keep)
        for (int side = 0; side < 2; side++) {
            struct lines *L = side ? &LB : &LA;
            snprintf(path, sizeof path, "%s/text/%s.%s.txt", outdir, G[gi].name, side ? "stock" : "fork");
            mkdirs(path);
            FILE *f = fopen(path, "w");
            for (long q = 0; f && q < L->n; q++)
                fprintf(f, "%s\n", L->l[q]);
            if (f)
                fclose(f);
        }
    free_lines(&LA);
    free_lines(&LB);
}

/* hack and rerun directories of phv processes that no longer exist (killed runs) */
static int sweep_stale(void)
{
    DIR *d = opendir("/dev/shm");
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        int pid = 0;
        if (sscanf(e->d_name, "phv1_%d", &pid) != 1 && sscanf(e->d_name, "phv_rerun_%d", &pid) != 1
            && sscanf(e->d_name, "phv_%d_", &pid) != 1)
            continue;
        if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) {
            char p[512];
            snprintf(p, sizeof p, "/dev/shm/%s", e->d_name);
            hd_rmtree(p);
            n++;
        }
    }
    closedir(d);
    return n;
}

/* a game over the rerun cap: its line from the main run's chains alone; cls: its class for summary.json */
static void not_rerun_line(int g, char *out, size_t on, char *cls, size_t cn)
{
    const struct rec *a = &REC[0][g], *b = &REC[1][g];
    char dg[512] = "", ds[1024] = "";
    for (int c = 0; c < PHD_NCOMP; c++)
        if (!ign[c] && a->chain[c] != b->chain[c]) {
            char *t = c < PHD_SEC0 ? dg : ds;
            size_t tn = c < PHD_SEC0 ? sizeof dg : sizeof ds, l = strlen(t);
            snprintf(t + l, tn - l, "%s%s", l ? "," : "", cname(c));
        }
    snprintf(out, on, "NOT_RERUN (over the rerun cap of %d): chains differ in %s (fork %u lines rc %d, stock %u lines rc %d, log %u keys)"
                      " | STATE_NOT_RERUN chains differ in %s", O.rerun_cap, dg[0] ? dg : "none", a->lines, a->rc, b->lines, b->rc,
             a->logkeys, ds[0] ? ds : "none");
    snprintf(cls, cn, "%s|%s%s%s", dg, ds, a->lines != b->lines ? "|lines" : "", a->rc || b->rc || a->setup || b->setup ? "|rc" : "");
}
static uint64_t chain_fold(const struct rec *r)
{
    uint64_t h = chainmix(r->lines, r->slines);
    for (int c = 0; c < PHD_NCOMP; c++)
        h = chainmix(h, r->chain[c]);
    return chainmix(h, (uint64_t) (uint32_t) r->rc);
}

static int certify(const char *list, const char *outdir)
{
    if (getenv("PHV_FAULT") || getenv("PHV_POKE")) {
        fprintf(stderr, "phv: PHV_FAULT / PHV_POKE are not read: negative controls are planted with --fault / --poke only\n");
        return 2;
    }
    int nswept = sweep_stale();
    if (nswept)
        fprintf(stderr, "phv: removed %d stale /dev/shm directories of phv processes that no longer exist\n", nswept);
    if (load_list(list) != 0 || !NG) {
        fprintf(stderr, "phv: no games in %s\n", list);
        return 2;
    }
    qsort(ORDER, NG, sizeof *ORDER, by_keys);
    int jobs = O.jobs > 0 ? O.jobs : (int) sysconf(_SC_NPROCESSORS_ONLN);
    if (jobs > 256)
        jobs = 256;
    int fj = O.fjobs >= 0 ? O.fjobs : (int) (jobs * O.fshare + 0.5);
    if (fj < 1)
        fj = 1;
    if (fj > jobs - 1 && jobs > 1)
        fj = jobs - 1;
    size_t shsz = sizeof(struct shared) + 2 * (size_t) NG * sizeof(struct rec);
    void *mem = mmap(NULL, shsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return 2;
    SH = mem;
    REC[0] = (struct rec *) (SH + 1);
    REC[1] = REC[0] + NG;
    mkdir(outdir, 0755);
    double hz = tsc_hz();
    uint64_t T0 = now_ns(), Tside[2] = { 0, 0 };
    int live[2] = { 0, 0 }, restarts = 0;
    SH->nslots = jobs;
    for (int s = 0; s < jobs; s++) {
        SH->slot[s].pid = 0;
        SH->slot[s].cur = -1;
    }
    fprintf(stderr, "phv: %d games, %d slots (%d fork, %d stock first)\n", NG, jobs, fj, jobs - fj);
    /* start */
    for (int s = 0; s < jobs; s++) {
        int side = s < fj ? 0 : 1;
        SH->slot[s].side = side;
        pid_t p = cfork();
        if (p == 0) {
            if (side == 0)
                fork_worker(s);
            else
                stock_worker(s);
        }
        SH->slot[s].pid = p;
        live[side]++;
    }
    uint64_t lastp = now_ns();
    while (live[0] + live[1] > 0) {
        int status;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if (p == 0) {
            struct timespec ts = { 0, 100000000 };
            nanosleep(&ts, NULL);
            if (now_ns() - lastp > 30000000000ULL) {
                lastp = now_ns();
                long dk[2] = { 0, 0 }, dg[2] = { 0, 0 };
                for (int side = 0; side < 2; side++)
                    for (int g = 0; g < NG; g++)
                        if (REC[side][g].state >= RS_DONE)
                            dg[side]++, dk[side] += REC[side][g].lines ? REC[side][g].lines - 1 : 0;
                fprintf(stderr, "phv: %.0f s: fork %ld/%d games %ld keys (%d workers), stock %ld/%d games %ld keys (%d workers)\n",
                        (now_ns() - T0) / 1e9, dg[0], NG, dk[0], live[0], dg[1], NG, dk[1], live[1]);
            }
            continue;
        }
        if (p < 0)
            break;
        int s;
        for (s = 0; s < jobs && SH->slot[s].pid != p; s++)
            ;
        if (s == jobs)
            continue;
        int side = SH->slot[s].side;
        live[side]--;
        if (SH->slot[s].cur >= 0) { /* the worker died in a game: only fork workers play games in their own process */
            struct rec *r = &REC[side][SH->slot[s].cur];
            if (r->state == RS_RUNNING) {
                r->rc = rc_of(status);
                r->state = RS_DONE;
            }
            SH->slot[s].cur = -1;
        } else if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            fprintf(stderr, "phv: %s worker %d failed at start (rc %d)\n", side ? "stock" : "fork", s, rc_of(status));
            if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
                return 2;
        }
        if (SH->next[side] >= NG && live[side] == 0 && !Tside[side])
            Tside[side] = now_ns() - T0;
        /* the slot goes to whichever side still has games queued, its own first */
        int ns = SH->next[side] < NG ? side : SH->next[!side] < NG ? !side : -1;
        if (ns >= 0) {
            restarts += ns == side && !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            SH->slot[s].side = ns;
            pid_t q = cfork();
            if (q == 0) {
                if (ns == 0)
                    fork_worker(s);
                else
                    stock_worker(s);
            }
            SH->slot[s].pid = q;
            live[ns]++;
        } else
            SH->slot[s].pid = 0;
    }
    for (int side = 0; side < 2; side++)
        if (!Tside[side])
            Tside[side] = now_ns() - T0;
    uint64_t Tmain = now_ns() - T0;
    for (int s = 0; s < jobs; s++) { /* a worker that crashed or changed sides leaves its hack directory */
        char wd[64];
        snprintf(wd, sizeof wd, "/dev/shm/phv_%d_f%d", (int) getpid(), s);
        hd_rmtree(wd);
        snprintf(wd, sizeof wd, "/dev/shm/phv_%d_s%d", (int) getpid(), s);
        hd_rmtree(wd);
    }
    fprintf(stderr, "phv: both sides done in %.1f s (fork %.1f s, stock %.1f s), comparing\n", Tmain / 1e9, Tside[0] / 1e9,
            Tside[1] / 1e9);

    /* fast compare; everything else is rerun key by key */
    char (*V)[1024] = calloc(NG, sizeof *V);
    int *need = calloc(NG, sizeof *need), nneed = 0;
    for (int g = 0; g < NG; g++) {
        const struct rec *a = &REC[0][g], *b = &REC[1][g];
        int same = a->state == RS_DONE && b->state == RS_DONE && a->rc == 0 && b->rc == 0 && !a->setup && !b->setup
                   && a->lines == b->lines && a->slines == b->slines && a->lines > 0 && a->slines > 0;
        for (int c = 0; c < PHD_NCOMP && same; c++)
            if (!ign[c] && a->chain[c] != b->chain[c])
                same = 0;
        long n = (long) a->lines - 1;
        if (same && !a->done && n < (long) a->logkeys)
            same = 0; /* short: stops before the log's last key without done */
        if (same)
            snprintf(V[g], sizeof V[g], "IDENTICAL keys %ld (fork %u lines, stock %u, log %u keys%s) | STATE_IDENTICAL keys %u", n,
                     a->lines, b->lines, a->logkeys, a->done ? ", done" : "", a->slines);
        else
            need[nneed++] = g;
    }
    fprintf(stderr, "phv: %d of %d games identical by chains; %d differ\n", NG - nneed, NG, nneed);

    /* reruns: the differing games (crashes and nonzero exits first, then list order) up to the cap, and a random sample of the
       identical ones (--fresh: both sides in fresh processes must reproduce the back-to-back main run); one comparer per game,
       jobs / 2 at a time (each plays both sides) */
    uint64_t T1 = now_ns();
    int *task = calloc(NG + 1, sizeof *task), nt = 0, nrr = 0, nover = 0, *over = calloc(nneed + 1, sizeof *over);
    for (int pass = 0; pass < 2; pass++)
        for (int t = 0; t < nneed; t++) {
            const struct rec *a = &REC[0][need[t]], *b = &REC[1][need[t]];
            int crash = a->rc || b->rc || a->setup || b->setup || a->state != RS_DONE || b->state != RS_DONE;
            if (crash == !pass) {
                if (nrr < O.rerun_cap)
                    task[nt++] = need[t], nrr++;
                else
                    over[nover++] = need[t];
            }
        }
    int nfresh = 0;
    unsigned long seed = O.seed ? O.seed : (unsigned long) now_ns() ^ (unsigned long) getpid() << 32;
    {
        int nid = 0, *idg = malloc((NG + 1) * sizeof *idg);
        for (int g = 0; g < NG; g++)
            if (!strncmp(V[g], "IDENTICAL keys", 14))
                idg[nid++] = g;
        int want = (int) (O.fresh * nid + 0.999);
        if (want > 1000)
            want = 1000;
        unsigned long x = seed | 1;
        for (int i = 0; i < want && i < nid; i++) { /* partial Fisher-Yates, xorshift */
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            int j = i + (int) (x % (unsigned long) (nid - i)), tmp = idg[i];
            idg[i] = idg[j], idg[j] = tmp;
            task[nt++] = idg[i], nfresh++;
        }
        free(idg);
    }
    struct rr *RR = mmap(NULL, (size_t) (nt + 1) * sizeof(struct rr), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (RR == MAP_FAILED)
        return 2;
    fprintf(stderr, "phv: rerunning %d differing games (%d over the cap of %d, not rerun) and %d identical games as a fresh-process "
                    "sample\n", nrr, nover, O.rerun_cap, nfresh);
    {
        int ncmp = jobs / 2 > 0 ? jobs / 2 : 1, next = 0, running = 0;
        while (next < nt || running) {
            while (running < ncmp && next < nt) {
                int t = next++;
                pid_t p = cfork();
                if (p == 0) {
                    quiet_stdio();
                    rerun_game(task[t], &RR[t], 1, outdir);
                    _exit(0);
                }
                running += p > 0;
            }
            int status;
            pid_t p = wait(&status);
            if (p < 0)
                break;
            running--;
        }
    }
    /* verdicts, the rerun-against-main check, the field reports */
    int nconsist = 0, nfresh_bad = 0;
    FILE *dv = NULL;
    char p[4400];
    snprintf(p, sizeof p, "%s/divergences.txt", outdir);
    dv = fopen(p, "w");
    for (int t = 0; t < nt; t++) {
        int gi = task[t];
        struct rr *R = &RR[t];
        if (t >= nrr && strcmp(V[gi], R->verdict)) {
            nfresh_bad++;
            if (dv)
                fprintf(dv, "%s FRESH_DIFFERS: back to back %s; fresh processes %s\n", G[gi].name, V[gi], R->verdict);
        }
        snprintf(V[gi], sizeof V[gi], "%s", R->verdict[0] ? R->verdict : "RERUN_FAILED | STATE_NOT_RUN");
        /* the rerun must reproduce the main run's chains on both sides (a back-to-back leak or nondeterminism otherwise) */
        for (int side = 0; side < 2; side++) {
            const struct rec *m = &REC[side][gi], *q = &R->r[side];
            int same = m->lines == q->lines && m->slines == q->slines && !memcmp(m->chain, q->chain, sizeof m->chain);
            if (!same || m->rc != R->rc[side]) {
                nconsist++;
                if (dv)
                    fprintf(dv, "%s RERUN_DIFFERS %s side: main run %u lines rc %d, fresh rerun %u lines rc %d\n", G[gi].name,
                            side ? "stock" : "fork", m->lines, m->rc, q->lines, R->rc[side]);
            }
        }
    }
    for (int t = 0; t < nt && dv; t++)
        if (RR[t].first >= -1) {
            int gi = task[t];
            fprintf(dv, "%s %s\n  first differing key %ld (fork rc %d, stock rc %d); text state at that key:\n%s", G[gi].name, V[gi],
                    RR[t].first, RR[t].rc[0], RR[t].rc[1], RR[t].div);
        }
    /* over the cap: classes from the chains */
    char (*cls)[1600] = calloc(nover + 1, sizeof *cls);
    int *ccount = calloc(nover + 1, sizeof *ccount), ncls = 0;
    for (int t = 0; t < nover; t++) {
        char c[1600];
        not_rerun_line(over[t], V[over[t]], sizeof V[over[t]], c, sizeof c);
        int q;
        for (q = 0; q < ncls && strcmp(cls[q], c); q++)
            ;
        if (q == ncls)
            snprintf(cls[ncls++], sizeof cls[0], "%s", c);
        ccount[q]++;
    }
    if (dv)
        fclose(dv);
    uint64_t Trerun = now_ns() - T1, Ttot = now_ns() - T0;

    /* verdict.txt (one line per game, list order) and summary.json */
    snprintf(p, sizeof p, "%s/verdict.txt", outdir);
    FILE *vf = fopen(p, "w");
    long nid = 0, nab = 0, ndiff = 0;
    long keys_played = 0, keys_side[2] = { 0, 0 }, keys_log = 0;
    double busy[2] = { 0, 0 }, step[2] = { 0, 0 }, dig[2] = { 0, 0 };
    for (int g = 0; g < NG; g++) {
        fprintf(vf, "%s %s\n", G[g].name, V[g]);
        if (!strncmp(V[g], "IDENTICAL keys", 14) && strstr(V[g], "| STATE_IDENTICAL keys"))
            nid++;
        else if (strstr(V[g], "STATE_ABORTED_BOTH"))
            nab++;
        else
            ndiff++;
        long n = 0;
        if (sscanf(V[g], "IDENTICAL keys %ld", &n) == 1 || sscanf(V[g], "ABORTED_BOTH keys %ld", &n) == 1)
            keys_played += n;
        else {
            const char *q = strstr(V[g], " of ");
            if (q)
                keys_played += atol(q + 4);
        }
        keys_log += REC[0][g].logkeys;
        for (int side = 0; side < 2; side++) {
            keys_side[side] += REC[side][g].lines ? REC[side][g].lines - 1 : 0;
            busy[side] += REC[side][g].ns / 1e9;
            step[side] += REC[side][g].tsc_step / hz;
            dig[side] += REC[side][g].tsc_dig / hz;
        }
    }
    fclose(vf);
    /* timing.tsv: per game and side, keys, wall, step and digest seconds, exit status */
    snprintf(p, sizeof p, "%s/timing.tsv", outdir);
    FILE *tf = fopen(p, "w");
    if (tf) {
        fprintf(tf, "name\tkeys\tfork_s\tfork_step_s\tfork_digest_s\tfork_rc\tstock_s\tstock_step_s\tstock_digest_s\tstock_rc"
                    "\tfork_chains\tstock_chains\n");
        for (int g = 0; g < NG; g++) { /* *_chains: every component's chain, the line counts and the exit status folded */
            const struct rec *a = &REC[0][g], *b = &REC[1][g];
            fprintf(tf, "%s\t%u\t%.6f\t%.6f\t%.6f\t%d\t%.6f\t%.6f\t%.6f\t%d\t%016llx\t%016llx\n", G[g].name,
                    a->lines ? a->lines - 1 : 0, a->ns / 1e9, a->tsc_step / hz, a->tsc_dig / hz, a->rc, b->ns / 1e9, b->tsc_step / hz,
                    b->tsc_dig / hz, b->rc, (unsigned long long) chain_fold(a), (unsigned long long) chain_fold(b));
        }
        fclose(tf);
    }
    /* poked.tsv: the state-perturbation controls as applied (section; fallback 1 = the planned record was absent, ctx poked) */
    if (O.poke) {
        snprintf(p, sizeof p, "%s/poked.tsv", outdir);
        FILE *pf = fopen(p, "w");
        for (int g = 0; pf && g < NG; g++) {
            fault_setup(G[g].path);
            for (int q = 0; q < npoke; q++) {
                int pk = REC[0][g].poked;
                fprintf(pf, "%s\t%d\t%s\t%s\t%d\n", G[g].name, poke_key[q], poke_name[q],
                        pk >= 0 ? cname(pk & 0xff) : pk == -2 ? "FAILED" : "NOT_REACHED", pk >= 0 && (pk & 0x100));
            }
        }
        if (pf)
            fclose(pf);
    }
    snprintf(p, sizeof p, "%s/summary.json", outdir);
    FILE *j = fopen(p, "w");
    fprintf(j, "{\n \"games\": %d,\n \"identical\": %ld,\n \"aborted_both\": %ld,\n \"differ\": %ld,\n \"rerun\": %d,\n"
               " \"rerun_inconsistent\": %d,\n \"keys_logged\": %ld,\n \"keys_played\": %ld,\n",
            NG, nid, nab, ndiff, nrr, nconsist, keys_log, keys_played);
    fprintf(j, " \"differ_by_chains\": %d,\n \"rerun_cap\": %d,\n \"not_rerun\": %d,\n \"not_rerun_classes\": {", nneed,
            O.rerun_cap, nover);
    for (int q = 0; q < ncls; q++) {
        fprintf(j, "%s", q ? ", " : "");
        json_str(j, cls[q]);
        fprintf(j, ": %d", ccount[q]);
    }
    fprintf(j, "},\n \"fresh_sample\": %d,\n \"fresh_seed\": %lu,\n \"fresh_differs\": %d,\n", nfresh, seed, nfresh_bad);
    int nfrc = 0, nsrc = 0;
    fprintf(j, " \"fork_rc_nonzero\": [");
    for (int g = 0; g < NG; g++)
        if (REC[0][g].rc) {
            if (nfrc++ < 100) {
                fprintf(j, "%s[", nfrc > 1 ? ", " : "");
                json_str(j, G[g].name);
                fprintf(j, ", %d]", REC[0][g].rc);
            }
        }
    for (int g = 0; g < NG; g++)
        nsrc += REC[1][g].rc != 0;
    fprintf(j, "],\n \"fork_rc_nonzero_n\": %d,\n \"stock_rc_nonzero_n\": %d,\n \"stale_dirs_removed\": %d,\n", nfrc, nsrc, nswept);
    fprintf(j, " \"slots\": %d,\n \"fork_slots_first\": %d,\n \"worker_restarts\": %d,\n \"wall_s\": %.3f,\n \"main_s\": %.3f,\n"
               " \"rerun_s\": %.3f,\n",
            jobs, fj, restarts, Ttot / 1e9, Tmain / 1e9, Trerun / 1e9);
    const char *sn[2] = { "fork", "stock" };
    for (int side = 0; side < 2; side++)
        fprintf(j, " \"%s\": {\"keys\": %ld, \"side_wall_s\": %.3f, \"keys_per_s_wall\": %.0f, \"core_s\": %.3f, "
                   "\"keys_per_core_s\": %.0f, \"step_s\": %.3f, \"digest_s\": %.3f, \"digest_share\": %.4f},\n",
                sn[side], keys_side[side], Tside[side] / 1e9, keys_side[side] / (Tside[side] / 1e9), busy[side],
                busy[side] > 0 ? keys_side[side] / busy[side] : 0, step[side], dig[side],
                step[side] + dig[side] > 0 ? dig[side] / (step[side] + dig[side]) : 0);
    fprintf(j, " \"ignore\": ");
    json_str(j, O.ignore ? O.ignore : "");
    fprintf(j, ",\n \"fault\": ");
    json_str(j, O.fault ? O.fault : "");
    fprintf(j, ",\n \"poke\": ");
    json_str(j, O.poke ? O.poke : "");
    fprintf(j, ",\n \"poke_lib\": ");
    json_str(j, O.poke_lib ? O.poke_lib : "");
    fprintf(j, ",\n \"workspace\": %s", getenv("PHV_NO_WS") ? "false" : "true");
    fprintf(j, ",\n \"mode\": ");
    json_str(j, O.mode);
    fprintf(j, "\n}\n");
    fclose(j);
    printf("games %d identical %ld aborted_both %ld differ %ld rerun %d not_rerun %d fresh %d rerun_inconsistent %d keys_played %ld "
           "wall %.1f s\n", NG, nid, nab, ndiff, nrr, nover, nfresh, nconsist, keys_played, Ttot / 1e9);
    return 0;
}

/* ---------------------------------------------------------------- main */
static void usage(void)
{
    fprintf(stderr,
            "usage: phv certify LIST OUTDIR --lib DIR --dat DIR --ref LIB --stock-dat DIR --stock-digest SO [--jobs N]\n"
            "           [--fork-jobs N] [--bind-all] [--ignore STATE,...] [--timeout S] [--keep] [--rerun-cap N] [--fresh FRACTION]\n"
            "           [--seed N] [--fault LOG:KEY:COMP,...] [--poke LOG:KEY:NAME,... --poke-lib SO]\n"
            "       phv game fork|stock LOG [--perkey FILE] [--text-at K FILE] (same library options)\n"
            "       phv cmpkey FORK.pk STOCK.pk LOG [--fork-rc N] [--stock-rc N] [--ignore a,b]\n"
            "       phv id (the digest ids of --lib and --stock-digest)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        usage();
    const char *cmd = argv[1], *pos[4] = { 0 };
    int np = 0, frc = 0, src = 0;
    long text_at = -2;
    const char *perkey = NULL, *textf = NULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
#define ARG() (i + 1 < argc ? argv[++i] : (usage(), ""))
        if (!strcmp(a, "--lib")) O.lib = ARG();
        else if (!strcmp(a, "--dat")) O.dat = ARG();
        else if (!strcmp(a, "--ref")) O.ref = ARG();
        else if (!strcmp(a, "--stock-dat")) O.sdat = ARG();
        else if (!strcmp(a, "--stock-digest")) O.sdig = ARG();
        else if (!strcmp(a, "--jobs")) O.jobs = atoi(ARG());
        else if (!strcmp(a, "--fork-jobs")) O.fjobs = atoi(ARG());
        else if (!strcmp(a, "--fork-share")) O.fshare = atof(ARG());
        else if (!strcmp(a, "--timeout")) O.timeout = atoi(ARG());
        else if (!strcmp(a, "--keep")) O.keep = 1;
        else if (!strcmp(a, "--no-emu")) O.no_emu = 1;
        else if (!strcmp(a, "--bind-all")) O.bindall = 1, O.mode = "bind-all";
        else if (!strcmp(a, "--ignore")) O.ignore = ARG();
        else if (!strcmp(a, "--rerun-cap")) O.rerun_cap = atoi(ARG());
        else if (!strcmp(a, "--fresh")) O.fresh = atof(ARG());
        else if (!strcmp(a, "--seed")) O.seed = strtoul(ARG(), NULL, 0);
        else if (!strcmp(a, "--fault")) O.fault = ARG();
        else if (!strcmp(a, "--poke")) O.poke = ARG();
        else if (!strcmp(a, "--poke-lib")) O.poke_lib = ARG();
        else if (!strcmp(a, "--perkey")) perkey = ARG();
        else if (!strcmp(a, "--text-at")) { text_at = atol(ARG()); textf = ARG(); }
        else if (!strcmp(a, "--fork-rc")) frc = atoi(ARG());
        else if (!strcmp(a, "--stock-rc")) src = atoi(ARG());
        else if (a[0] == '-' && a[1] == '-') usage();
        else if (np < 4) pos[np++] = a;
        else usage();
    }
    if (O.ignore) { /* statecmp.py's STATE_IGNORE: state components only (the digest line is compared whole, as cmp.py does) */
        char *s = strdup(O.ignore);
        for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) {
            int c = comp_index(t);
            if (c < PHD_SEC0) {
                fprintf(stderr, "phv: --ignore %s: not a state component\n", t);
                return 2;
            }
            ign[c] = 1;
        }
    }
    if (!strcmp(cmd, "certify")) {
        if (np != 2 || !O.lib || !O.dat || !O.ref || !O.sdat || !O.sdig)
            usage();
        return certify(pos[0], pos[1]);
    }
    if (!strcmp(cmd, "game")) {
        if (np != 2)
            usage();
        int side = !strcmp(pos[0], "stock");
        if (side ? !O.ref || !O.sdat || !O.sdig : !O.lib || !O.dat)
            usage();
        struct rec r;
        memset(&r, 0, sizeof r);
        int e = one_game(side, pos[1], perkey, text_at, textf, &r);
        double hz = tsc_hz();
        printf("%s %s rc %d lines %u slines %u done %u logkeys %u wall %.3f s step %.3f s digest %.3f s (%.2f us/key step, "
               "%.2f us/key digest) setup %d\n",
               pos[0], pos[1], e, r.lines, r.slines, r.done, r.logkeys, r.ns / 1e9, r.tsc_step / hz, r.tsc_dig / hz,
               r.lines > 1 ? r.tsc_step / hz * 1e6 / (r.lines - 1) : 0, r.lines ? r.tsc_dig / hz * 1e6 / r.lines : 0, r.setup);
        for (int c = 0; c < PHD_NCOMP; c++)
            printf("%s%s=%016llx", c ? " " : "chain ", cname(c), (unsigned long long) r.chain[c]);
        printf("\n");
        return e;
    }
    if (!strcmp(cmd, "cmpkey")) {
        if (np != 3)
            usage();
        struct pkf A, B;
        if (pk_load(pos[0], &A) || pk_load(pos[1], &B) || keylog_read(pos[2], &K))
            return 2;
        char out[8192];
        long first;
        verdict_pk(&A, &B, &K, frc, src, out, sizeof out, &first);
        printf("%s\nfirst %ld\n", out, first);
        return 0;
    }
    if (!strcmp(cmd, "id")) {
        fflush(stdout);
        for (int side = 0; side < 2; side++) {
            pid_t p = fork();
            if (p == 0) {
                if (side == 0 && O.lib && fork_load() == 0)
                    printf("fork %016llx\n", f_id());
                if (side == 1 && O.ref && O.sdig && stock_load() == 0) {
                    printf("stock %016llx\n", s_id());
                    if (s_aid)
                        printf("adapter %016llx\n", s_aid());
                }
                fflush(stdout);
                _exit(0);
            }
            waitpid(p, NULL, 0);
        }
        return 0;
    }
    usage();
    return 2;
}
