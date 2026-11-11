/* Size-class pool for libnethack's own small allocations. The library links with -Wl,--wrap=malloc,free,calloc,realloc, so
   every malloc/free reference in its objects lands here (libc and libstdc++ keep the real allocator). Blocks of up to
   NH_POOL_MAX bytes come from 64 KB chunks of a single size class, carved from one reserved address range, so a pointer's
   owner is decided by its address; a thread keeps a bounded free list per class and trades batches with a global list, so
   blocks freed on another thread do not grow memory. Larger blocks and realloc(NULL, n) use the real allocator. Game state
   never depends on addresses (every gate replays exactly under a different allocator). */
#ifdef NH_POOL_ALLOC
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

void *__real_malloc(size_t);
void __real_free(void *);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t);
void __wrap_free(void *);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);

#define NH_POOL_MAX 1024
#define NH_POOL_CLS (NH_POOL_MAX / 16 + 1)
#define NH_POOL_CHUNK ((uintptr_t) 1 << 16)
#define NH_POOL_REGION ((size_t) 1 << 34) /* 16 GB of address space, pages materialise on touch */
#define NH_POOL_LOCAL_MAX 512
#define NH_POOL_BATCH 256

#if defined(NH_TLS_INITIAL_EXEC) && defined(__linux__)
#define NH_POOL_TLS __thread __attribute__((tls_model("initial-exec")))
#else
#define NH_POOL_TLS __thread
#endif

struct nh_pool_thread {
    void *free[NH_POOL_CLS];
    unsigned nfree[NH_POOL_CLS];
    char *bump[NH_POOL_CLS], *end[NH_POOL_CLS];
};

static char *nh_pool_lo;
static uintptr_t nh_pool_span; /* 0 until the range is reserved: every pointer then belongs to the real allocator */
static atomic_size_t nh_pool_used;
static pthread_once_t nh_pool_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t nh_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static void *nh_pool_global[NH_POOL_CLS];
static NH_POOL_TLS struct nh_pool_thread *nh_pool_tls;

static void
nh_pool_reserve(void)
{
    char *p = (char *) mmap(NULL, NH_POOL_REGION, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    char *lo;

    if (p == (char *) MAP_FAILED)
        return;
    lo = (char *) (((uintptr_t) p + NH_POOL_CHUNK - 1) & ~(NH_POOL_CHUNK - 1));
    nh_pool_lo = lo;
    __atomic_store_n(&nh_pool_span, (uintptr_t) (p + NH_POOL_REGION - lo) & ~(NH_POOL_CHUNK - 1), __ATOMIC_RELEASE);
}

static inline int
nh_pool_owns(const void *p)
{
    return (uintptr_t) ((const char *) p - nh_pool_lo) < __atomic_load_n(&nh_pool_span, __ATOMIC_ACQUIRE);
}

static inline unsigned
nh_pool_cls_of(const void *p)
{
    return *(const unsigned *) ((uintptr_t) p & ~(NH_POOL_CHUNK - 1)); /* the chunk header */
}

static struct nh_pool_thread *
nh_pool_self(void)
{
    struct nh_pool_thread *t = nh_pool_tls;

    if (t)
        return t;
    (void) pthread_once(&nh_pool_once, nh_pool_reserve);
    if (!__atomic_load_n(&nh_pool_span, __ATOMIC_ACQUIRE))
        return (struct nh_pool_thread *) 0;
    t = (struct nh_pool_thread *) __real_calloc(1, sizeof *t);
    nh_pool_tls = t;
    return t;
}

/* move NH_POOL_BATCH blocks from the thread's list to the global one */
static void
nh_pool_flush(struct nh_pool_thread *t, unsigned c)
{
    void *head = t->free[c], *last = head;
    unsigned i;

    for (i = 1; i < NH_POOL_BATCH; i++)
        last = *(void **) last;
    t->free[c] = *(void **) last;
    t->nfree[c] -= NH_POOL_BATCH;
    pthread_mutex_lock(&nh_pool_mu);
    *(void **) last = nh_pool_global[c];
    nh_pool_global[c] = head;
    pthread_mutex_unlock(&nh_pool_mu);
}

static void *
nh_pool_slow(struct nh_pool_thread *t, unsigned c)
{
    size_t sz = (size_t) c * 16, off;
    char *p;

    if (__atomic_load_n(&nh_pool_global[c], __ATOMIC_RELAXED)) { /* refill a batch from the global list */
        void *head, *last;
        unsigned n = 1;

        pthread_mutex_lock(&nh_pool_mu);
        head = last = nh_pool_global[c];
        if (head) {
            while (n < NH_POOL_BATCH && *(void **) last) {
                last = *(void **) last;
                n++;
            }
            nh_pool_global[c] = *(void **) last;
        }
        pthread_mutex_unlock(&nh_pool_mu);
        if (head) {
            *(void **) last = (void *) 0;
            t->free[c] = *(void **) head;
            t->nfree[c] = n - 1;
            return head;
        }
    }
    if (!t->bump[c] || t->bump[c] + sz > t->end[c]) { /* a new chunk of this class */
        off = atomic_fetch_add(&nh_pool_used, (size_t) NH_POOL_CHUNK);
        if (off + NH_POOL_CHUNK > nh_pool_span)
            return __real_malloc(sz); /* range exhausted: the real allocator from here on */
        p = nh_pool_lo + off;
        *(unsigned *) p = c;
        t->bump[c] = p + 64;
        t->end[c] = p + NH_POOL_CHUNK;
    }
    p = t->bump[c];
    t->bump[c] = p + sz;
    return p;
}

void *
__wrap_malloc(size_t n)
{
    struct nh_pool_thread *t;
    unsigned c;
    void *p;

    if (n > NH_POOL_MAX || !(t = nh_pool_self()))
        return __real_malloc(n);
    c = n ? (unsigned) ((n + 15) >> 4) : 1U;
    if ((p = t->free[c]) != 0) {
        t->free[c] = *(void **) p;
        t->nfree[c]--;
        return p;
    }
    return nh_pool_slow(t, c);
}

void
__wrap_free(void *p)
{
    struct nh_pool_thread *t;
    unsigned c;

    if (!nh_pool_owns(p)) {
        __real_free(p);
        return;
    }
    c = nh_pool_cls_of(p);
    t = nh_pool_self();
    *(void **) p = t->free[c];
    t->free[c] = p;
    if (++t->nfree[c] > NH_POOL_LOCAL_MAX)
        nh_pool_flush(t, c);
}

void *
__wrap_calloc(size_t a, size_t b)
{
    size_t n;
    void *p;

    if (__builtin_mul_overflow(a, b, &n) || n > NH_POOL_MAX)
        return __real_calloc(a, b);
    if ((p = __wrap_malloc(n)) != 0)
        memset(p, 0, n);
    return p;
}

void *
__wrap_realloc(void *p, size_t n)
{
    size_t old;
    void *q;

    if (!nh_pool_owns(p))
        return __real_realloc(p, n); /* includes realloc(NULL, n) */
    old = (size_t) nh_pool_cls_of(p) * 16;
    if (n <= old)
        return p;
    if ((q = __wrap_malloc(n)) == 0)
        return (void *) 0;
    memcpy(q, p, old);
    __wrap_free(p);
    return q;
}
#endif /* NH_POOL_ALLOC */
