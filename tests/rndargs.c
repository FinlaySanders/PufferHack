/* rn2/rnl/rnd/d with a bad argument answer as stock NLE 0.9.0's post-release build does: impossible() with stock's text and
   a fixed result, no draw; good arguments draw as before. Built from src/rnd.c and src/isaac64.c with impossible() caught
   here. Each case runs in a child: before the fix a bad argument divides by zero, loops for 2^32 draws or returns garbage */
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hack.h"
#include "isaac64.h"

extern isaac64_ctx *nle_rng_state(int);
NH_THREAD_LOCAL struct nh_ctx *nh_cur;
static char said[BUFSZ];
static int nsaid;

void impossible(const char *s, ...)
{
    va_list ap;

    va_start(ap, s);
    vsnprintf(said, sizeof said, s, ap);
    va_end(ap);
    nsaid++;
}

void panic(const char *s, ...)
{
    (void) s;
    _exit(3);
}

int sgn(int n)
{
    return (n < 0) ? -1 : (n != 0);
}

enum { RN2, RNL, RND_, D };

static int call(int f, int n, int x)
{
    return f == RN2 ? rn2(x) : f == RNL ? rnl(x) : f == RND_ ? rnd(x) : d(n, x);
}

/* bad: want (result, impossible text, no draw); good: want a draw, no impossible and a result in 0..want */
static int check(const char *name, int f, int n, int x, int want, const char *text)
{
    struct { int r, n, drew; char said[BUFSZ]; } got = { -999, 0, 0, "" };
    isaac64_ctx before;
    int st, ok, p[2];
    pid_t pid;

    if (pipe(p))
        return 0;
    if (!(pid = fork())) {
        alarm(5);
        before = *nle_rng_state(0);
        got.r = call(f, n, x);
        got.n = nsaid;
        got.drew = !!memcmp(&before, nle_rng_state(0), sizeof before);
        snprintf(got.said, sizeof got.said, "%s", said);
        _exit(write(p[1], &got, sizeof got) != sizeof got);
    }
    close(p[1]);
    if (read(p[0], &got, sizeof got) != sizeof got)
        got.r = -999;
    close(p[0]);
    waitpid(pid, &st, 0);
    ok = WIFEXITED(st) && !WEXITSTATUS(st)
         && (text ? got.r == want && got.n == 1 && !strcmp(got.said, text) && !got.drew
                  : !got.n && got.drew && got.r >= 0 && got.r <= want);
    if (WIFSIGNALED(st))
        printf("FAIL: %s: %s\n", name, strsignal(WTERMSIG(st)));
    else
        printf("%s: %s = %d, %s, impossible: %s\n", ok ? "ok" : "FAIL", name, got.r, got.drew ? "drew" : "no draw",
               got.n ? got.said : "none");
    return ok;
}

int main(void)
{
    int ok = 1;

    nh_cur = calloc(1, sizeof *nh_cur);
    init_isaac64(42UL, rn2);
    ok &= check("rn2(0)", RN2, 0, 0, 0, "rn2(0) attempted");
    ok &= check("rn2(-3)", RN2, 0, -3, 0, "rn2(-3) attempted");
    ok &= check("rnl(0)", RNL, 0, 0, 0, "rnl(0) attempted");
    ok &= check("rnd(0)", RND_, 0, 0, 1, "rnd(0) attempted");
    ok &= check("rnd(-1)", RND_, 0, -1, 1, "rnd(-1) attempted");
    ok &= check("d(1,0)", D, 1, 0, 1, "d(1,0) attempted");
    ok &= check("d(-1,6)", D, -1, 6, 1, "d(-1,6) attempted");
    ok &= check("d(2,-3)", D, 2, -3, 1, "d(2,-3) attempted");
    ok &= check("rn2(1)", RN2, 0, 1, 0, NULL);
    ok &= check("rnl(5)", RNL, 0, 5, 4, NULL);
    ok &= check("rnd(6)", RND_, 0, 6, 6, NULL);
    ok &= check("d(3,6)", D, 3, 6, 18, NULL);
    printf("%s\n", ok ? "rndargs: ok" : "rndargs: FAIL");
    return !ok;
}
