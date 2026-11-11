/* LD_PRELOAD: fopen("/dev/urandom") reads a fixed 8-byte seed, so sys_random_seed() is the same on both sides of a replay */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static const unsigned long seed = 0x5eed5eed12345678UL;

FILE *fopen(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (path && !strcmp(path, "/dev/urandom"))
        return fmemopen((void *) &seed, sizeof seed, "r");
    if (!real)
        real = (FILE * (*) (const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
    return real(path, mode);
}
