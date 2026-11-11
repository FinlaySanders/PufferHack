/* keylog.h: key-log parsing (stock-diff src/common.h's keylog_read, the Q0/QX/P rules) and a reusable per-worker hack
   directory: the data directory's files symlinked once, the writable files reset before every game */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    unsigned long seed[2], lgen, time_seed;
    int reseed, use_init, use_lgen, fix_moon, time_set, spawn, underfoot;
    char bind[32], trueg[64], options[32768], rc[65536], record[65536];
    int nkeys, cap, *keys;
    int has_q, no_record, rec_fmt;
} keylog_t;

static int keylog_read(const char *path, keylog_t *k)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int *keys = k->keys, cap = k->cap;
    memset(k, 0, sizeof *k);
    k->keys = keys;
    k->cap = cap;
    static char line[40000];
    int in_keys = 0;
    while (fgets(line, sizeof line, f)) {
        if (in_keys) {
            if (line[0] == 'E')
                break;
            if (k->nkeys == k->cap)
                k->cap = k->cap ? k->cap * 2 : 65536, k->keys = realloc(k->keys, k->cap * sizeof(int));
            k->keys[k->nkeys++] = atoi(line);
            continue;
        }
        switch (line[0]) {
        case 'S': sscanf(line, "S %lx %lx %d %d %lx %d", &k->seed[0], &k->seed[1], &k->reseed, &k->use_init, &k->lgen, &k->use_lgen); break;
        case 'T': sscanf(line, "T %d %lx %d", &k->fix_moon, &k->time_seed, &k->time_set); break;
        case 'M': sscanf(line, "M %d %d", &k->spawn, &k->underfoot); break;
        case 'B': sscanf(line, "B %31s", k->bind); break;
        case 'G': sscanf(line, "G %63s", k->trueg); break;
        case 'O': line[strcspn(line, "\n")] = 0; snprintf(k->options, sizeof k->options, "%s", line + 2); break;
        case 'R': strncat(k->rc, line + 2, sizeof k->rc - strlen(k->rc) - 1); break;
        case 'P': k->rec_fmt = 1; break;
        case 'Q':
            k->has_q = 1;
            if (!strcmp(line, "Q0\n"))
                break;
            if (!strcmp(line, "QX\n")) {
                k->no_record = 1;
                break;
            }
            strncat(k->record, line + 2, sizeof k->record - strlen(k->record) - 1);
            break;
        case 'K': in_keys = 1; break;
        }
    }
    fclose(f);
    if (k->rec_fmt && !k->has_q)
        return -2; /* a keylog_rec log always says what record the game found */
    return 0;
}

/* the hack directory: every file of the data directory symlinked except record, perm, logfile, xlogfile and save */
struct hackdir {
    char dir[4096], src[4096];
    int nlinks;
    char links[256][256];
};

static int hd_skip(const char *b)
{
    return !strcmp(b, "record") || !strcmp(b, "perm") || !strcmp(b, "logfile") || !strcmp(b, "xlogfile") || !strcmp(b, "save");
}

static int hd_rmtree(const char *p)
{
    struct stat st;
    if (lstat(p, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *e;
        char q[4600];
        if (!d)
            return -1;
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                snprintf(q, sizeof q, "%s/%s", p, e->d_name);
                hd_rmtree(q);
            }
        closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}

static int hd_init(struct hackdir *h, const char *src, const char *dir)
{
    DIR *d = opendir(src);
    struct dirent *e;
    char a[4600], b[4600];
    snprintf(h->dir, sizeof h->dir, "%s", dir);
    snprintf(h->src, sizeof h->src, "%s", src);
    h->nlinks = 0;
    hd_rmtree(dir);
    if (!d || mkdir(dir, 0755) != 0)
        return -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2])))
            continue;
        if (e->d_name[0] == '.' || hd_skip(e->d_name)) /* the shell glob in make_hackdir skips dot files */
            continue;
        if (h->nlinks == 256 || strlen(e->d_name) >= 256)
            return -1;
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", dir, e->d_name);
        if (symlink(a, b) != 0)
            return -1;
        snprintf(h->links[h->nlinks++], 256, "%s", e->d_name);
    }
    closedir(d);
    return 0;
}

/* before every game: anything a game wrote (level files, bones, locks, saves) goes; perm, logfile, xlogfile empty; save/ empty;
   record as the log found it (none for QX) */
static int hd_reset(struct hackdir *h, const keylog_t *k)
{
    DIR *d = opendir(h->dir);
    struct dirent *e;
    char p[4600];
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        int keep = 0;
        for (int i = 0; i < h->nlinks && !keep; i++)
            keep = !strcmp(h->links[i], e->d_name);
        snprintf(p, sizeof p, "%s/%s", h->dir, e->d_name);
        if (keep) {
            struct stat st;
            if (lstat(p, &st) == 0 && S_ISLNK(st.st_mode))
                continue;
            hd_rmtree(p); /* a game replaced a link: restore it */
            char a[4600];
            snprintf(a, sizeof a, "%s/%s", h->src, e->d_name);
            if (symlink(a, p) != 0)
                return -1;
            continue;
        }
        hd_rmtree(p);
    }
    closedir(d);
    for (int i = 0; i < h->nlinks; i++) { /* a link the game removed */
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", h->dir, h->links[i]);
        if (lstat(p, &st) != 0) {
            char a[4600];
            snprintf(a, sizeof a, "%s/%s", h->src, h->links[i]);
            if (symlink(a, p) != 0)
                return -1;
        }
    }
    snprintf(p, sizeof p, "%s/save", h->dir);
    if (mkdir(p, 0755) != 0)
        return -1;
    const char *empty[3] = { "perm", "logfile", "xlogfile" };
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s", h->dir, empty[i]);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return -1;
        close(fd);
    }
    if (!k->no_record) {
        snprintf(p, sizeof p, "%s/record", h->dir);
        FILE *r = fopen(p, "w");
        if (!r)
            return -1;
        fputs(k->record, r);
        fclose(r);
    }
    return 0;
}
