/* NetHack 3.6	pline.c	$NHDT-Date: 1549327495 2019/02/05 00:44:55 $  $NHDT-Branch: NetHack-3.6.2-beta01 $:$NHDT-Revision: 1.73 $ */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Robert Patrick Rankin, 2018. */
/* NetHack may be freely redistributed.  See license for details. */

#define NEED_VARARGS /* Uses ... */ /* comment line for pre-compiled headers */
#ifdef __AVX2__
#include <immintrin.h>
#endif
#include "hack.h"

#define BIGBUFSZ (5 * BUFSZ) /* big enough to format a 4*BUFSZ string (from
                              * config file parsing) with modest decoration;
                              * result will then be truncated to BUFSZ-1 */

/* TLS — per-thread message state. */
/* Per-env (was __thread). pline modifier bitfield. */
#define pline_flags (nh_cur->g_pline_c_pline_flags)
/* prevmsg — per-env message-repeat suppression buffer. */
#define prevmsg (nh_cur->g_pline_c_prevmsg)

static void FDECL(putmesg, (const char *));

static char *FDECL(You_buf, (int));
#if defined(MSGHANDLER) && (defined(POSIX_TYPES) || defined(__GNUC__))
static void FDECL(execplinehandler, (const char *));
#endif

/* message buffers in 32-byte blocks: a write covers the blocks through its NUL and zeroes only the blocks the old text used */

/* the block copy reads a string's last block past its NUL */
#if defined(__SANITIZE_ADDRESS__)
#define NH_STRBLK_BYTES 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#define NH_STRBLK_BYTES 1
#endif
#endif

static unsigned
strblk_bytes(char *dst, const char *src, unsigned nblk)
{
    size_t n = strnlen(src, 32 * (size_t) nblk), b = n < 32 * (size_t) nblk ? n / 32 + 1 : nblk;

    memcpy(dst, src, n);
    memset(dst + n, 0, 32 * b - n);
    return (unsigned) b;
}

/* strncpy(dst, src, 32 * nblk) through src's NUL block only, later blocks untouched; returns the blocks written */
unsigned
nh_strblk(char *dst, const char *src, unsigned nblk)
{
#if defined(__AVX2__) && !defined(NH_STRBLK_BYTES)
    unsigned k, safe = (4096 - ((uintptr_t) src & 4095)) / 32; /* blocks of src before the next page */

    for (k = 0; k < nblk; k++) {
        __m256i v;
        unsigned nul;

        if (k == safe)
            return strblk_bytes(dst, src, nblk);
        v = _mm256_loadu_si256((const __m256i *) (src + 32 * k));
        nul = (unsigned) _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_setzero_si256()));
        if (nul) {
            const __m256i idx = _mm256_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                                 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);

            v = _mm256_and_si256(v, _mm256_cmpgt_epi8(_mm256_set1_epi8((char) __builtin_ctz(nul)), idx));
            _mm256_storeu_si256((__m256i *) (dst + 32 * k), v);
            return k + 1;
        }
        _mm256_storeu_si256((__m256i *) (dst + 32 * k), v);
    }
    return nblk;
#else
    return strblk_bytes(dst, src, nblk);
#endif
}

/* the blocks of s through its first NUL (nblk if none) */
unsigned
nh_nulblk(const char *s, unsigned nblk)
{
#if defined(__AVX2__) && !defined(NH_STRBLK_BYTES)
    unsigned k;

    for (k = 0; k < nblk; k++)
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *) (s + 32 * k)), _mm256_setzero_si256())))
            return k + 1;
    return nblk;
#else
    size_t n = strnlen(s, 32 * (size_t) nblk);

    return n < 32 * (size_t) nblk ? (unsigned) n / 32 + 1 : nblk;
#endif
}

void
nh_zeroblk(char *s, unsigned from, unsigned to)
{
    unsigned m = from < to ? (1u << to) - (1u << from) : 0;

    for (; m; m &= m - 1)
#ifdef __AVX2__
        _mm256_storeu_si256((__m256i *) (s + 32 * __builtin_ctz(m)), _mm256_setzero_si256());
#else
        memset(s + 32 * __builtin_ctz(m), 0, 32);
#endif
}

#ifdef DUMPLOG
/* also used in end.c */
unsigned saved_pline_index = 0; /* slot in saved_plines[] to use next */
char *saved_plines[DUMPLOG_MSG_COUNT] = { (char *) 0 };

/* keep the most recent DUMPLOG_MSG_COUNT messages */
void
dumplogmsg(line)
const char *line;
{
    /*
     * TODO:
     *  This essentially duplicates message history, which is
     *  currently implemented in an interface-specific manner.
     *  The core should take responsibility for that and have
     *  this share it.
     */
    unsigned indx = saved_pline_index; /* next slot to use */
    char *oldest = saved_plines[indx]; /* current content of that slot */

    if (oldest && strlen(oldest) >= strlen(line)) {
        /* this buffer will gradually shrink until the 'else' is needed;
           there's no pressing need to track allocation size instead */
        Strcpy(oldest, line);
    } else {
        if (oldest)
            free((genericptr_t) oldest);
        saved_plines[indx] = dupstr(line);
    }
    saved_pline_index = (indx + 1) % DUMPLOG_MSG_COUNT;
}

/* called during save (unlike the interface-specific message history,
   this data isn't saved and restored); end-of-game releases saved_pline[]
   while writing its contents to the final dump log */
void
dumplogfreemessages()
{
    unsigned indx;

    for (indx = 0; indx < DUMPLOG_MSG_COUNT; ++indx)
        if (saved_plines[indx])
            free((genericptr_t) saved_plines[indx]), saved_plines[indx] = 0;
    saved_pline_index = 0;
}
#endif

/* keeps windowprocs usage out of pline() */
static void
putmesg(line)
const char *line;
{
    int attr = ATR_NONE;

    if ((pline_flags & URGENT_MESSAGE) != 0
        && (windowprocs.wincap2 & WC2_URGENT_MESG) != 0)
        attr |= ATR_URGENT;
    if ((pline_flags & SUPPRESS_HISTORY) != 0
        && (windowprocs.wincap2 & WC2_SUPPRESS_HIST) != 0)
        attr |= ATR_NOHISTORY;

    putstr(WIN_MESSAGE, attr, line);
}

/* Note that these declarations rely on knowledge of the internals
 * of the variable argument handling stuff in "tradstdc.h"
 */

#if defined(USE_STDARG) || defined(USE_VARARGS)
static void FDECL(vpline, (const char *, va_list));

/* gs: the conversions pline formats use (%s %d %ld %u %lu %c %%, no flags, width or precision) written directly with
   vsnprintf's result and return value; any other conversion returns -1 with the caller's arguments unread (they are read
   from a copy), and the caller then calls vsnprintf. pct is fmt's first '%'; literal runs are copied whole */
static int
nh_fmt_simple(char *buf, size_t size, const char *fmt, const char *pct, va_list args)
{
    const char *p = fmt;
    size_t n = 0;
    va_list ap;

    va_copy(ap, args);
    for (;;) {
        const char *s;
        char num[24];
        size_t len = (size_t) (pct - p);

        if (len && n + 1 < size)
            memcpy(buf + n, p, n + len < size ? len : size - 1 - n);
        n += len;
        if (!*pct)
            break;
        p = pct + 1;
        if (*p == 's') {
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            len = strlen(s);
        } else if (*p == 'c' || *p == '%') {
            num[0] = *p == '%' ? '%' : (char) va_arg(ap, int);
            s = num, len = 1;
        } else {
            int is_long = *p == 'l', sgn = 0;
            unsigned long v;
            char *e = num + sizeof num;

            if (is_long)
                p++;
            if (*p == 'd') {
                long x = is_long ? va_arg(ap, long) : (long) va_arg(ap, int);
                sgn = x < 0;
                v = sgn ? 0UL - (unsigned long) x : (unsigned long) x;
            } else if (*p == 'u') {
                v = is_long ? va_arg(ap, unsigned long) : (unsigned long) va_arg(ap, unsigned);
            } else {
                va_end(ap);
                return -1;
            }
            do
                *--e = (char) ('0' + v % 10);
            while (v /= 10);
            if (sgn)
                *--e = '-';
            s = e, len = (size_t) (num + sizeof num - e);
        }
        if (n + 1 < size)
            memcpy(buf + n, s, n + len < size ? len : size - 1 - n);
        n += len;
        pct = strchrnul(++p, '%');
    }
    va_end(ap);
    if (size)
        buf[n < size ? n : size - 1] = '\0';
    return (int) n;
}


/*VARARGS1*/
void
pline
VA_DECL(const char *, line)
{
    VA_START(line);
    VA_INIT(line, char *);
    vpline(line, VA_ARGS);
    VA_END();
}

# ifdef USE_STDARG
static void
vpline(const char *line, va_list the_args)
# else
static void
vpline(line, the_args)
const char *line;
va_list the_args;
# endif

#else /* USE_STDARG | USE_VARARG */

# define vpline pline

/*VARARGS1*/
void
pline
VA_DECL(const char *, line)
#endif /* USE_STDARG | USE_VARARG */
{       /* start of vpline() or of nested block in USE_OLDARG's pline() */
    /* Was process-shared function-local static — env A would
     * leak in_pline=1 across yields into env B's pline, suppressing
     * legitimate output. Per-env now. */
    #define in_pline (nh_cur->g_pline_c_in_pline)
    char pbuf[BIGBUFSZ]; /* will get chopped down to BUFSZ-1 if longer */
    int ln;
    int msgtyp;
#if !defined(NO_VSNPRINTF)
    int vlen = 0;
#endif
    const char *pct;
    boolean no_repeat;
    /* Do NOT use VA_START and VA_END in here... see above */

    if (!line || !*line)
        return;
#ifdef HANGUPHANDLING
    if (program_state.done_hup)
        return;
#endif
    if (program_state.wizkit_wishing)
        return;

    if ((pct = index(line, '%')) != 0) {
#if !defined(NO_VSNPRINTF)
        vlen = nh_fmt_simple(pbuf, sizeof pbuf, line, pct, VA_ARGS);
        if (vlen < 0)
            vlen = vsnprintf(pbuf, sizeof pbuf, line, VA_ARGS);
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED) && defined(DEBUG)
        if (vlen >= (int) sizeof pbuf)
            panic("%s: truncation of buffer at %zu of %d bytes",
                  "pline", sizeof pbuf, vlen);
#endif
#else
        Vsprintf(pbuf, line, VA_ARGS);
#endif
        line = pbuf;
    }
#if !defined(NO_VSNPRINTF)
    /* gs: a formatted line of at most BUFSZ - 1 characters cannot need the cut (strlen <= vlen); its strlen re-read pbuf
       before vsnprintf's byte stores had drained */
    if ((line != pbuf || (unsigned) vlen > BUFSZ - 1) && (ln = (int) strlen(line)) > BUFSZ - 1) {
#else
    if ((ln = (int) strlen(line)) > BUFSZ - 1) {
#endif
        if (line != pbuf)                          /* no '%' was present */
            (void) strncpy(pbuf, line, BUFSZ - 1); /* caveat: unterminated */
        /* truncate, preserving the final 3 characters:
           "___ extremely long text" -> "___ extremely l...ext"
           (this may be suboptimal if overflow is less than 3) */
        (void) strncpy(pbuf + BUFSZ - 1 - 6, "...", 3);
        /* avoid strncpy; buffers could overlap if excess is small */
        pbuf[BUFSZ - 1 - 3] = line[ln - 3];
        pbuf[BUFSZ - 1 - 2] = line[ln - 2];
        pbuf[BUFSZ - 1 - 1] = line[ln - 1];
        pbuf[BUFSZ - 1] = '\0';
        line = pbuf;
    }

#ifdef DUMPLOG
    /* We hook here early to have options-agnostic output.
     * Unfortunately, that means Norep() isn't honored (general issue) and
     * that short lines aren't combined into one longer one (tty behavior).
     */
    if ((pline_flags & SUPPRESS_HISTORY) == 0)
        dumplogmsg(line);
#endif
    /* use raw_print() if we're called too early (or perhaps too late
       during shutdown) or if we're being called recursively (probably
       via debugpline() in the interface code) */
    if (in_pline++ || !iflags.window_inited) {
        /* [we should probably be using raw_printf("\n%s", line) here] */
        raw_print(line);
        iflags.last_msg = PLNMSG_UNKNOWN;
        goto pline_done;
    }

    msgtyp = MSGTYP_NORMAL;
    no_repeat = (pline_flags & PLINE_NOREPEAT) ? TRUE : FALSE;
    if ((pline_flags & OVERRIDE_MSGTYPE) == 0) {
        msgtyp = msgtype_type(line, no_repeat);
        if ((pline_flags & URGENT_MESSAGE) == 0
            && (msgtyp == MSGTYP_NOSHOW
                || (msgtyp == MSGTYP_NOREP && !strcmp(line, prevmsg))))
            /* FIXME: we need a way to tell our caller that this message
             * was suppressed so that caller doesn't set iflags.last_msg
             * for something that hasn't been shown, otherwise a subsequent
             * message which uses alternate wording based on that would be
             * doing so out of context and probably end up seeming silly.
             * (Not an issue for no-repeat but matters for no-show.)
             */
            goto pline_done;
    }

    if (vision_full_recalc)
        vision_recalc(0);
    if (u.ux)
        flush_screen(1); /* %% */

    putmesg(line);

#if defined(MSGHANDLER) && (defined(POSIX_TYPES) || defined(__GNUC__))
    execplinehandler(line);
#endif

    /* this gets cleared after every pline message */
    iflags.last_msg = PLNMSG_UNKNOWN;
    /* strncpy(prevmsg, line, BUFSZ), prevmsg[BUFSZ - 1] = 0: prevmsg is zero past its NUL */
    {
        unsigned ob = nh_nulblk(prevmsg, BUFSZ / 32), nb = nh_strblk(prevmsg, line, BUFSZ / 32);

        if (nb == BUFSZ / 32)
            prevmsg[BUFSZ - 1] = '\0';
        else
            nh_zeroblk(prevmsg, nb, ob);
    }
    if (msgtyp == MSGTYP_STOP)
        display_nhwindow(WIN_MESSAGE, TRUE); /* --more-- */

 pline_done:
    --in_pline;
    return;

#if !(defined(USE_STDARG) || defined(USE_VARARGS))
    /* provide closing brace for the nested block
       which immediately follows USE_OLDARGS's VA_DECL() */
    VA_END();
#endif
}

/* pline() variant which can override MSGTYPE handling or suppress
   message history (tty interface uses pline() to issue prompts and
   they shouldn't be blockable via MSGTYPE=hide) */
/*VARARGS2*/
void custompline
VA_DECL2(unsigned, pflags, const char *, line)
{
    VA_START(line);
    VA_INIT(line, const char *);
    pline_flags = pflags;
    vpline(line, VA_ARGS);
    pline_flags = 0;
    VA_END();
    return;
}

/*VARARGS1*/
void Norep
VA_DECL(const char *, line)
{
    VA_START(line);
    VA_INIT(line, const char *);
    pline_flags = PLINE_NOREPEAT;
    vpline(line, VA_ARGS);
    pline_flags = 0;
    VA_END();
    return;
}

/* Per-env work buffer for You(), &c and verbalize().
 * Was `static char *you_buf` + `int you_buf_siz`. Across envs
 * sharing a pthread, env A's heap pointer survived in TLS and env B's
 * You_buf() could free env A's buffer. Now stored per-env. */
struct nle_pline_state {
    char *_you_buf;
    int   _you_buf_siz;
};
static struct nle_pline_state *
nle_pline(void)
{
    if (!nh_cur)
        return NULL;
    struct nle_pline_state *s = (struct nle_pline_state *) nh_cur->nh_lazy[7];
    if (!s) {
        /* Arena-allocate (not libc calloc): this struct holds `_you_buf`, an
         * arena pointer. If the struct lived on the libc heap it would NOT be
         * captured by nle_fr_snapshot, so after a restore (which rewinds the
         * arena) `_you_buf` would dangle into a reused arena offset and the
         * next You_hear/pline would write its message over whatever now lives
         * there (e.g. a live monster's struct) -> corruption/SIGSEGV. */
        s = (struct nle_pline_state *) calloc(
            1, sizeof(struct nle_pline_state));
        nh_cur->nh_lazy[7] = s;
    }
    return s;
}
#define you_buf     (nle_pline()->_you_buf)
#define you_buf_siz (nle_pline()->_you_buf_siz)

static char *
You_buf(siz)
int siz;
{
    if (siz > you_buf_siz) {
        if (you_buf)
            free((genericptr_t) you_buf);
        you_buf_siz = siz + 10;
        you_buf = (char *) alloc((unsigned) you_buf_siz);
    }
    return you_buf;
}

void
free_youbuf()
{
    if (you_buf)
        free((genericptr_t) you_buf), you_buf = (char *) 0;
    you_buf_siz = 0;
}

/* `prefix' must be a string literal, not a pointer */
#define YouPrefix(pointer, prefix, text) \
    Strcpy((pointer = You_buf((int) (strlen(text) + sizeof prefix))), prefix)

/* YouPrefix then strcat: the same bytes, the text measured once */
static inline char *
you_message(const char *prefix, size_t pn, const char *text)
{
    size_t tn = strlen(text);
    char *p = You_buf((int) (tn + pn + 1));

    memcpy(p, prefix, pn);
    memcpy(p + pn, text, tn + 1);
    return p;
}

#define YouMessage(pointer, prefix, text) \
    (pointer = you_message(prefix, sizeof prefix - 1, text))

/*VARARGS1*/
void You
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    vpline(YouMessage(tmp, "You ", line), VA_ARGS);
    VA_END();
}

/*VARARGS1*/
void Your
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    vpline(YouMessage(tmp, "Your ", line), VA_ARGS);
    VA_END();
}

/*VARARGS1*/
void You_feel
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    if (Unaware)
        YouPrefix(tmp, "You dream that you feel ", line);
    else
        YouPrefix(tmp, "You feel ", line);
    vpline(strcat(tmp, line), VA_ARGS);
    VA_END();
}

/*VARARGS1*/
void You_cant
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    vpline(YouMessage(tmp, "You can't ", line), VA_ARGS);
    VA_END();
}

/*VARARGS1*/
void pline_The
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    vpline(YouMessage(tmp, "The ", line), VA_ARGS);
    VA_END();
}

/*VARARGS1*/
void There
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    vpline(YouMessage(tmp, "There ", line), VA_ARGS);
    VA_END();
}

/*VARARGS1*/
void You_hear
VA_DECL(const char *, line)
{
    char *tmp;

    if (Deaf || !flags.acoustics)
        return;
    VA_START(line);
    VA_INIT(line, const char *);
    if (Underwater)
        YouPrefix(tmp, "You barely hear ", line);
    else if (Unaware)
        YouPrefix(tmp, "You dream that you hear ", line);
    else
        YouPrefix(tmp, "You hear ", line);  /* Deaf-aware */
    vpline(strcat(tmp, line), VA_ARGS);
    VA_END();
}

/*VARARGS1*/
void You_see
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    if (Unaware)
        YouPrefix(tmp, "You dream that you see ", line);
    else if (Blind) /* caller should have caught this... */
        YouPrefix(tmp, "You sense ", line);
    else
        YouPrefix(tmp, "You see ", line);
    vpline(strcat(tmp, line), VA_ARGS);
    VA_END();
}

/* Print a message inside double-quotes.
 * The caller is responsible for checking deafness.
 * Gods can speak directly to you in spite of deafness.
 */
/*VARARGS1*/
void verbalize
VA_DECL(const char *, line)
{
    char *tmp;

    VA_START(line);
    VA_INIT(line, const char *);
    tmp = You_buf((int) strlen(line) + sizeof "\"\"");
    Strcpy(tmp, "\"");
    Strcat(tmp, line);
    Strcat(tmp, "\"");
    vpline(tmp, VA_ARGS);
    VA_END();
}

/*VARARGS1*/
/* Note that these declarations rely on knowledge of the internals
 * of the variable argument handling stuff in "tradstdc.h"
 */

#if defined(USE_STDARG) || defined(USE_VARARGS)
static void FDECL(vraw_printf, (const char *, va_list));

void raw_printf
VA_DECL(const char *, line)
{
    VA_START(line);
    VA_INIT(line, char *);
    vraw_printf(line, VA_ARGS);
    VA_END();
}

# ifdef USE_STDARG
static void
vraw_printf(const char *line, va_list the_args)
# else
static void
vraw_printf(line, the_args)
const char *line;
va_list the_args;
# endif

#else /* USE_STDARG | USE_VARARG */

void raw_printf
VA_DECL(const char *, line)
#endif
{
    char pbuf[BIGBUFSZ]; /* will be chopped down to BUFSZ-1 if longer */
    /* Do NOT use VA_START and VA_END in here... see above */

    if (index(line, '%')) {
#if !defined(NO_VSNPRINTF)
        (void) vsnprintf(pbuf, sizeof pbuf, line, VA_ARGS);
#else
        Vsprintf(pbuf, line, VA_ARGS);
#endif
        line = pbuf;
    }
    if ((int) strlen(line) > BUFSZ - 1) {
        if (line != pbuf)
            line = strncpy(pbuf, line, BUFSZ - 1);
        /* unlike pline, we don't futz around to keep last few chars */
        pbuf[BUFSZ - 1] = '\0'; /* terminate strncpy or truncate vsprintf */
    }
    raw_print(line);
#if defined(MSGHANDLER) && (defined(POSIX_TYPES) || defined(__GNUC__))
    execplinehandler(line);
#endif
#if !(defined(USE_STDARG) || defined(USE_VARARGS))
    VA_END(); /* (see vpline) */
#endif
}

/*VARARGS1*/
void impossible
VA_DECL(const char *, s)
{
    char pbuf[BIGBUFSZ]; /* will be chopped down to BUFSZ-1 if longer */

    VA_START(s);
    VA_INIT(s, const char *);
    if (program_state.in_impossible)
        panic("impossible called impossible");

    program_state.in_impossible = 1;
#if !defined(NO_VSNPRINTF)
    (void) vsnprintf(pbuf, sizeof pbuf, s, VA_ARGS);
#else
    Vsprintf(pbuf, s, VA_ARGS);
#endif
    pbuf[BUFSZ - 1] = '\0'; /* sanity */
    paniclog("impossible", pbuf);
    if (iflags.debug_fuzzer)
        panic("%s", pbuf);
    pline("%s", VA_PASS1(pbuf));
    /* reuse pbuf[] */
    Strcpy(pbuf, "Program in disorder!");
    if (program_state.something_worth_saving)
        Strcat(pbuf, "  (Saving and reloading may fix this problem.)");
    pline("%s", VA_PASS1(pbuf));

    program_state.in_impossible = 0;
    VA_END();
}

#if defined(MSGHANDLER) && (defined(POSIX_TYPES) || defined(__GNUC__))
static boolean use_pline_handler = TRUE;

static void
execplinehandler(line)
const char *line;
{
    int f;
    const char *args[3];
    char *env;

    if (!use_pline_handler)
        return;

    if (!(env = nh_getenv("NETHACK_MSGHANDLER"))) {
        use_pline_handler = FALSE;
        return;
    }

    f = fork();
    if (f == 0) { /* child */
        args[0] = env;
        args[1] = line;
        args[2] = NULL;
        (void) setgid(getgid());
        (void) setuid(getuid());
        (void) execv(args[0], (char *const *) args);
        perror((char *) 0);
        (void) fprintf(stderr, "Exec to message handler %s failed.\n", env);
        nh_terminate(EXIT_FAILURE);
    } else if (f > 0) {
        int status;

        waitpid(f, &status, 0);
    } else if (f == -1) {
        perror((char *) 0);
        use_pline_handler = FALSE;
        pline("%s", VA_PASS1("Fork to message handler failed."));
    }
}
#endif /* MSGHANDLER && (POSIX_TYPES || __GNUC__) */

/*
 * varargs handling for files.c
 */
#if defined(USE_STDARG) || defined(USE_VARARGS)
static void FDECL(vconfig_error_add, (const char *, va_list));

/*VARARGS1*/
void
config_error_add
VA_DECL(const char *, str)
{
    VA_START(str);
    VA_INIT(str, char *);
    vconfig_error_add(str, VA_ARGS);
    VA_END();
}

# ifdef USE_STDARG
static void
vconfig_error_add(const char *str, va_list the_args)
# else
static void
vconfig_error_add(str, the_args)
const char *str;
va_list the_args;
# endif

#else /* !(USE_STDARG || USE_VARARG) => USE_OLDARGS */

/*VARARGS1*/
void
config_error_add
VA_DECL(const char *, str)
#endif /* ?(USE_STDARG || USE_VARARG) */
{       /* start of vconf...() or of nested block in USE_OLDARG's conf...() */
#if !defined(NO_VSNPRINTF)
    int vlen = 0;
#endif
    char buf[BIGBUFSZ]; /* will be chopped down to BUFSZ-1 if longer */

#if !defined(NO_VSNPRINTF)
    vlen = vsnprintf(buf, sizeof buf, str, VA_ARGS);
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED) && defined(DEBUG)
    if (vlen >= (int) sizeof buf)
        panic("%s: truncation of buffer at %zu of %d bytes",
              "config_error_add", sizeof buf, vlen);
#endif
#else
    Vsprintf(buf, str, VA_ARGS);
#endif
    buf[BUFSZ - 1] = '\0';
    config_erradd(buf);

#if !(defined(USE_STDARG) || defined(USE_VARARGS))
    VA_END(); /* (see pline/vpline -- ends nested block for USE_OLDARGS) */
#endif
}

/*pline.c*/
