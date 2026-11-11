/* games back to back in one process see what stock NLE's fresh library per game sees: a wizard-mode refusal stays in its
   game, a rewritten options file is read again, an options string at a reused address is scanned again, and each game
   reads the nhdat of its own data directory */
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "engine.h"

/* no passwd entry for this user (as for a uid outside /etc/passwd): playmode:debug is refused */
struct passwd *getpwuid(uid_t u) { (void) u; return NULL; }
struct passwd *getpwnam(const char *n) { (void) n; return NULL; }
char *getlogin(void) { return NULL; }

static nle_obs obs;
static long blstats[NLE_BLSTATS_SIZE];
static unsigned char msg[NLE_MESSAGE_SIZE], tty[NLE_TERM_LI * NLE_TERM_CO];
static int misc[NLE_MISC_SIZE];
static char seen[1 << 16], bottom[2 * NLE_TERM_CO + 2];
static size_t nseen;
static nle_ctx_t *last_game;

static void note(void)
{
    int r;
    for (r = 0; r < NLE_TERM_LI && nseen + NLE_TERM_CO + 2 < sizeof seen; r++) {
        memcpy(seen + nseen, tty + r * NLE_TERM_CO, NLE_TERM_CO);
        nseen += NLE_TERM_CO;
        seen[nseen++] = '\n';
    }
    seen[nseen] = '\0';
    memcpy(bottom, tty + (NLE_TERM_LI - 2) * NLE_TERM_CO, 2 * NLE_TERM_CO);
}

/* one game: the screens after the start and after each key */
static void play(const char *hackdir, const char *options, const char *keys)
{
    static nle_settings s;
    nle_ctx_t *g;

    memset(&s, 0, sizeof s);
    snprintf(s.hackdir, sizeof s.hackdir, "%s", hackdir);
    snprintf(s.options, sizeof s.options, "%s", options);
    s.spawn_monsters = 1;
    s.initial_seeds.seeds[0] = 1;
    s.initial_seeds.seeds[1] = 2;
    s.initial_seeds.use_init_seeds = 1;
    s.fix_moon_phase = 1, s.time_seed = 5, s.time_seed_is_set = 1;
    memset(&obs, 0, sizeof obs);
    obs.blstats = blstats;
    obs.message = msg;
    obs.tty_chars = tty;
    obs.misc = misc;
    nseen = 0;
    g = nle_start(&obs, NULL, &s);
    note();
    for (; *keys && !obs.done; keys++) {
        obs.action = *keys;
        g = nle_step(g, &obs);
        note();
    }
    last_game = g;
    nle_end(g);
    if (getenv("CACHES_DUMP"))
        printf("---- %s %s\n%s", hackdir, options, seen);
}

static int check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    return ok;
}

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fputs(text, f);
    return fclose(f) == 0;
}

/* overwrite bytes at the first occurrence of from (same length), same inode and size; mtime put back */
static int rewrite(const char *path, const char *from, const char *to, int keep_mtime)
{
    static char buf[1 << 21];
    struct stat st;
    struct timespec t[2];
    ssize_t n;
    char *p;
    int fd = open(path, O_RDWR), ok = 0;

    if (fd < 0)
        return 0;
    if (fstat(fd, &st) == 0 && (n = pread(fd, buf, sizeof buf - 1, 0)) > 0) {
        buf[n] = '\0';
        for (p = buf; p + strlen(from) <= buf + n; p++)
            if (!memcmp(p, from, strlen(from)))
                break;
        if (p + strlen(from) <= buf + n && pwrite(fd, to, strlen(to), p - buf) == (ssize_t) strlen(to))
            ok = 1;
    }
    if (ok && keep_mtime) {
        t[0] = st.st_atim, t[1] = st.st_mtim;
        ok = futimens(fd, t) == 0;
    }
    close(fd);
    return ok;
}

int main(int argc, char **argv)
{
    char rc[4096], rcw[4096], opt[4200], datb[4200], s1[256], s2[256];
    const char *hack, *hackb, *dir;
    nle_ctx_t *g;
    int ok = 1;

    if (argc < 4) {
        fprintf(stderr, "usage: caches HACKDIR HACKDIR_COPY WORKDIR\n");
        return 2;
    }
    hack = argv[1], hackb = argv[2], dir = argv[3];

    /* wizard mode refused in one game; the next game asks for normal play */
    snprintf(rcw, sizeof rcw, "%s/rc_wiz", dir);
    snprintf(rc, sizeof rc, "%s/rc_plain", dir);
    write_file(rcw, "OPTIONS=name:Agent,role:Val,race:human,gender:female,align:neutral,nolegacy,playmode:debug\n");
    write_file(rc, "OPTIONS=name:Agent,role:Val,race:human,gender:female,align:neutral,nolegacy\n");
    snprintf(opt, sizeof opt, "@%s", rcw);
    play(hack, opt, "\r\r");
    ok &= check(strstr(seen, "may access debug (wizard) mode") != NULL, "playmode:debug refused");
    snprintf(opt, sizeof opt, "@%s", rc);
    play(hack, opt, "\r\r");
    ok &= check(!strstr(seen, "debug (wizard) mode") && !strstr(seen, "explore"), "next game in normal mode");

    /* an options file rewritten in place, same size and mtime: the new role */
    snprintf(rc, sizeof rc, "%s/rc_swap", dir);
    write_file(rc, "OPTIONS=name:Agent,role:Priest,race:human,gender:male,align:lawful,nolegacy\n");
    snprintf(opt, sizeof opt, "@%s", rc);
    play(hack, opt, "");
    ok &= check(strstr(seen, "Priest") != NULL, "options file: Priest");
    ok &= check(rewrite(rc, "Priest", "Knight", 1), "rewritten in place, same size and mtime");
    play(hack, opt, "");
    ok &= check(strstr(seen, "Knight") && !strstr(seen, "Priest"), "options file rewritten: Knight");
    rewrite(rc, "Knight", "Priest", 1);
    play(hack, opt, "");
    ok &= check(strstr(seen, "Priest") && !strstr(seen, "Knight"), "options file back: Priest");

    /* an options string whose last statement sits where the previous game's last-parsed statement sat */
    memset(s1, ' ', 68);
    snprintf(s1 + 68, sizeof s1 - 68, "time,name:Agent,role:Val,race:human,gender:female,align:neutral");
    snprintf(s2, sizeof s2, "name:Agent,role:Val,race:human,gender:female,align:neutral,showexp,");
    memset(s2 + strlen(s2), ' ', 68 - strlen(s2));
    snprintf(s2 + 68, sizeof s2 - 68, "timed_delay");
    play(hack, s1, "");
    play(hack, s1, "");
    ok &= check(strstr(bottom, "T:1") != NULL, "options string with time: T shown");
    g = last_game;
    play(hack, s2, "");
    printf("     (game context %s)\n", last_game == g ? "reused" : "not reused");
    ok &= check(!strstr(bottom, "T:"), "options string with timed_delay: no T");

    /* two data directories whose nhdat differ in the help text */
    snprintf(datb, sizeof datb, "%s/nhdat", hackb);
    ok &= check(rewrite(datb, "Welcome to NetHack!", "WELCOME TO NETHACK!", 0), "second data dir's help text edited");
    snprintf(opt, sizeof opt, "@%s/rc_plain", dir);
    play(hack, opt, "\033?b");
    ok &= check(strstr(seen, "Welcome to NetHack!") != NULL, "first data dir: its help text");
    play(hackb, opt, "\033?b");
    ok &= check(strstr(seen, "WELCOME TO NETHACK!") && !strstr(seen, "Welcome to NetHack!"), "second data dir: its help text");
    play(hack, opt, "\033?b");
    ok &= check(strstr(seen, "Welcome to NetHack!") != NULL, "first data dir again: its help text");
    /* same size, directory changed in place: help is no longer a member, so the plain file beside nhdat is read */
    ok &= check(rewrite(datb, "nhelp ", "nhxlp ", 0), "second data dir's directory edited");
    play(hackb, opt, "\033?b");
    ok &= check(strstr(seen, "Welcome to NetHack!") && !strstr(seen, "WELCOME TO NETHACK!"),
                "second data dir, help not in the archive: the plain help file");
    return ok ? 0 : 1;
}
