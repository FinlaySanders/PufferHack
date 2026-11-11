/* NetHack 3.6	rnd.c	$NHDT-Date: 1524689470 2018/04/25 20:51:10 $  $NHDT-Branch: NetHack-3.6.0 $:$NHDT-Revision: 1.18 $ */
/*      Copyright (c) 2004 by Robert Patrick Rankin               */
/* NetHack may be freely redistributed.  See license for details. */

#include "hack.h"

#ifdef USE_ISAAC64
#include "isaac64.h"

/* nle_state refactor stage 1: RNG state moved out of static storage and
 * into per-instance nle_ctx_t. The (constant) function-pointer side of
 * each entry stays static because it's the same across all instances. */

enum { CORE = 0, DISP = 1 };

static int FDECL((*rnglist_fn[2]), (int)) = {
    rn2,                  /* CORE */
    rn2_on_display_rng,   /* DISP */
};


/* Per-env RNG state accessors (state lives in struct nh_ctx; nlernd.c
 * also uses these to swap core/lgen states). */
isaac64_ctx *
nle_rng_state(idx)
int idx;
{
    return &nh_cur->g_rnd_c_rng_state[idx];
}

int *
nle_rng_init_flag(idx)
int idx;
{
    return &nh_cur->g_rnd_c_rng_init[idx];
}

int
whichrng(fn)
int FDECL((*fn), (int));
{
    int i;

    for (i = 0; i < 2; ++i)
        if (rnglist_fn[i] == fn)
            return i;
    return -1;
}

void
init_isaac64(seed, fn)
unsigned long seed;
int FDECL((*fn), (int));
{
    unsigned char new_rng_state[sizeof seed];
    unsigned i;
    int rngindx = whichrng(fn);

    if (rngindx < 0)
        panic("Bad rng function passed to init_isaac64().");

    for (i = 0; i < sizeof seed; i++) {
        new_rng_state[i] = (unsigned char) (seed & 0xFF);
        seed >>= 8;
    }
    isaac64_init(nle_rng_state(rngindx), new_rng_state,
                 (int) sizeof seed);
}

/* msan-hunt: trace each seeded draw — compile in with -DNLE_RNG_TRACE_HOOK
   and set NLE_RNG_TRACE at runtime; used to pinpoint call sites whose
   evaluation order differs across builds. Compile-time gated: RND is the
   hottest function in the engine, and an unconditional getenv here (linear
   scan of environ per draw) measured 45% of ALL engine instructions. */
#define nle_rng_trace(fn, x, r) (r)

static int
RND(int x)
{
    int r = (int) (isaac64_next_uint64(nle_rng_state(CORE)) % x);

    return nle_rng_trace("RND", x, r);
}

/* 0 <= rn2(x) < x, but on a different sequence from the "main" rn2;
   used in cases where the answer doesn't affect gameplay and we don't
   want to give users easy control over the main RNG sequence. */
int
rn2_on_display_rng(x)
register int x;
{
    return (isaac64_next_uint64(nle_rng_state(DISP)) % x);
}

#else   /* USE_ISAAC64 */

/* "Rand()"s definition is determined by [OS]conf.h */
#if defined(LINT) && defined(UNIX) /* rand() is long... */
extern int NDECL(rand);
#define RND(x) (rand() % x)
#else /* LINT */
#if defined(UNIX) || defined(RANDOM)
#define RND(x) ((int) (Rand() % (long) (x)))
#else
/* Good luck: the bottom order bits are cyclic. */
#define RND(x) ((int) ((Rand() >> 3) % (x)))
#endif
#endif /* LINT */
int
rn2_on_display_rng(x)
register int x;
{
    /* Function-local `static unsigned seed = 1;` migrated
     * to nh_cur->g_rnd_c_rn2disprng_seed (initialized to 1 in init_nle).
     * Direct access here rather than a #define seed macro because `seed`
     * collides with init_isaac64's parameter name above. */
    nh_cur->g_rnd_c_rn2disprng_seed *= 2739110765;
    return (int)((nh_cur->g_rnd_c_rn2disprng_seed >> 16) % (unsigned)x);
}
#endif  /* USE_ISAAC64 */

#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
/* stock's answer to a bad argument: impossible() and a fixed result, no draw */
STATIC_OVL int __attribute__((noinline, cold))
rn_bad(const char *fn, int x, int res)
{
    impossible("%s(%d) attempted", fn, x);
    return res;
}
#endif

/* rn2()'s body, inlined at the hot draw sites; every other caller shares rn2's one copy */
__attribute__((always_inline)) inline int
rn2_hot(int x)
{
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (__builtin_expect(x <= 0, 0))
        return rn_bad("rn2", x, 0);
#endif
    return RND(x);
}

#ifdef USE_ISAAC64
/* the shared bodies' refill draw, out of line so their common path needs no stack frame */
STATIC_OVL int __attribute__((noinline))
rn2_refill(int x)
{
    return rn2_hot(x);
}

#define RNG_EMPTY() __builtin_expect(!nle_rng_state(CORE)->n, 0)
#else
#define RNG_EMPTY() 0
#define rn2_refill(x) rn2_hot(x)
#define rnd_refill(x) rnd_hot(x)
#endif

/* 0 <= rn2(x) < x */
int __attribute__((noinline))
rn2(x)
register int x;
{
    if (RNG_EMPTY())
        return rn2_refill(x);
    return rn2_hot(x);
}

/* 0 <= rnl(x) < x; sometimes subtracting Luck;
   good luck approaches 0, bad luck approaches (x-1) */
int __attribute__((noinline))
rnl(x)
register int x;
{
    register int i, adjustment;

#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (__builtin_expect(x <= 0, 0))
        return rn_bad("rnl", x, 0);
#endif

    adjustment = Luck;
    if (x <= 15) {
        /* for small ranges, use Luck/3 (rounded away from 0);
           also guard against architecture-specific differences
           of integer division involving negative values */
        adjustment = (abs(adjustment) + 1) / 3 * sgn(adjustment);
        /*
         *       11..13 ->  4
         *        8..10 ->  3
         *        5.. 7 ->  2
         *        2.. 4 ->  1
         *       -1,0,1 ->  0 (no adjustment)
         *       -4..-2 -> -1
         *       -7..-5 -> -2
         *      -10..-8 -> -3
         *      -13..-11-> -4
         */
    }

    i = RND(x);
    if (adjustment && rn2(37 + abs(adjustment))) {
        i -= adjustment;
        if (i < 0)
            i = 0;
        else if (i >= x)
            i = x - 1;
    }
    return i;
}

/* rnd()'s body, inlined at the hot sites */
__attribute__((always_inline)) inline int
rnd_hot(int x)
{
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (__builtin_expect(x <= 0, 0))
        return rn_bad("rnd", x, 1);
#endif
    x = RND(x) + 1;
    return x;
}

#ifdef USE_ISAAC64
STATIC_OVL int __attribute__((noinline))
rnd_refill(int x)
{
    return rnd_hot(x);
}
#endif

/* 1 <= rnd(x) <= x */
int __attribute__((noinline))
rnd(x)
register int x;
{
    if (RNG_EMPTY())
        return rnd_refill(x);
    return rnd_hot(x);
}

/* d(N,X) == NdX == dX+dX+...+dX N times; n <= d(n,x) <= (n*x) */
int __attribute__((noinline))
d(n, x)
register int n, x;
{
    register int tmp = n;

#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (__builtin_expect(x < 0 || n < 0 || (x == 0 && n != 0), 0)) {
        impossible("d(%d,%d) attempted", n, x);
        return 1;
    }
#endif
    while (n--)
        tmp += RND(x);
    return tmp; /* Alea iacta est. -- J.C. */
}

/* 1 <= rne(x) <= max(u.ulevel/3,5) */
int
rne(x)
register int x;
{
    register int tmp, utmp;

    utmp = (u.ulevel < 15) ? 5 : u.ulevel / 3;
    tmp = 1;
    while (tmp < utmp && !rn2(x))
        tmp++;
    return tmp;

    /* was:
     *  tmp = 1;
     *  while (!rn2(x))
     *    tmp++;
     *  return min(tmp, (u.ulevel < 15) ? 5 : u.ulevel / 3);
     * which is clearer but less efficient and stands a vanishingly
     * small chance of overflowing tmp
     */
}

/* rnz: everyone's favorite! */
int
rnz(i)
int i;
{
#ifdef LINT
    int x = i;
    int tmp = 1000;
#else
    register long x = (long) i;
    register long tmp = 1000L;
#endif

    tmp += rn2(1000);
    tmp *= rne(4);
    if (rn2(2)) {
        x *= tmp;
        x /= 1000;
    } else {
        x *= 1000;
        x /= tmp;
    }
    return (int) x;
}

/*rnd.c*/
