#include "arena.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* Only for `GC_push_all` (ziman_arena_push_scanned_roots). A `make`/`make gc` build already
 * force-includes src/zgc.h into every translation unit (the Makefile's own `-include`, which is
 * what zgc.h's malloc/calloc/realloc/free redefinitions rely on -- see arena.h's comment on why
 * this file never calls any of them, mmap/munmap being untouched by that redirect either way) --
 * this explicit include is for gc.h's own declarations, not to pull in anything zgc.h wouldn't
 * already have made available transitively. */
#ifdef ZIMAN_GC
#include <gc.h>
#include <gc/gc_mark.h> /* GC_push_all, for ziman_arena_push_scanned_roots */
#include <pthread.h>
#endif

/* One `mmap`'d region, prepended to the arena's block list as it fills. `data` (a flexible array
 * member, so the block header and the bytes it hands out are one allocation) is bump-allocated
 * forward from `used`, which starts past the header itself; a block is never bump-allocated into
 * again once a later one has taken over as `current` -- only `arena_destroy` ever touches it again,
 * to `munmap` it. Blocks are never linked in any order but "most recently added first": nothing
 * here needs to walk them in allocation order, only to visit every one exactly once at destroy. */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t size; /* the whole mmap'd region, header included -- what munmap needs back */
    size_t used; /* bytes already claimed, header included */
    char data[];
} ArenaBlock;

struct Arena {
    ArenaBlock *current;
    size_t reserved;
    int scanned; /* see arena_create_scanned */
    /* g_scanned's own links, unused unless `scanned`. Doubly linked purely so that unlinking is
     * O(1): a `Channel` under load creates and destroys one scanned arena per message, so a
     * singly-linked list's own walk-to-find-the-predecessor makes the registry quadratic in how
     * many arenas are live at once -- and, since every walk happens under one global lock, it
     * serialises threads that have nothing to do with each other. Measured both ways on twelve
     * threads each streaming a 4000-row cursor: 6.5s singly-linked against 1.5s with these, and
     * on six threads 1.2-2.1s against 0.65s.
     * Only `next_scanned` is ever read outside the lock (by ziman_arena_push_scanned_roots). */
    struct Arena *next_scanned;
    struct Arena *prev_scanned;
};

/* C11's max_align_t is the portable way to ask for "however aligned the strictest C type needs to
 * be," but this codebase targets C11 without pulling in <stdalign.h> elsewhere; 16 is that
 * alignment on every platform this interpreter actually runs on (a `double`/int64_t/pointer all
 * need at most 8, and this leaves headroom for a SIMD type nothing here uses yet but might). */
#define ARENA_ALIGN ((size_t)16)

/* Where `data` actually starts, in bytes from the block's own address -- not assumed equal to
 * `sizeof(ArenaBlock)`, which the C standard does not promise matches a trailing flexible array
 * member's offset (in practice they agree on every platform this runs on, but `offsetof` is what
 * actually guarantees it, at zero cost). */
#define ARENA_HEADER offsetof(ArenaBlock, data)

/* The smallest block worth `mmap`ing on its own -- most allocations are far smaller than this and
 * share a block; a request bigger than it gets a dedicated block sized exactly to fit (see
 * `block_new`), so nothing here ever fails an allocation just because it's large. Page-sized
 * multiples, since that's mmap's own granularity -- anything smaller still costs a whole page. */
#define ARENA_BLOCK_SIZE ((size_t)(256 * 1024))

static size_t round_up(size_t v, size_t unit) {
    return (v + unit - 1) / unit * unit;
}

/* --- the per-block allocation-start bitmap -------------------------------------------------
 * One bit per `ARENA_ALIGN` bytes of the block, reserved inline between the block's own header and
 * the bytes it hands out, and set by `arena_alloc` for every allocation it returns. It answers, in
 * constant time and exactly, "did an allocation in this block start at this address" -- and, by
 * scanning backwards, "which allocation's span is this address inside".
 *
 * It lives here rather than in the collector because this is the only module that knows where an
 * allocation starts; it carries no interpretation of its own (`arena.h` says what the collector
 * uses it for). Inline rather than a side allocation on purpose: it needs no second `mmap`, no
 * registry mapping a block to its bitmap, and nothing has to free it -- it is unmapped with the
 * block it describes, by `arena_destroy` and `arena_reclaim_block` alike, with no code of their own.
 * It costs one bit per 16 bytes, 1/128th of every block (2KB of a 256KB one).
 *
 * Every `arena_alloc` result is `ARENA_ALIGN`-aligned *relative to its block's own base* (`used` is
 * kept a multiple of `ARENA_ALIGN` from `block_new` onward and every request is rounded up to one),
 * and a block's base is page-aligned by `mmap`, so an offset divided by `ARENA_ALIGN` is an exact,
 * dense bit index with nothing to round. */
static size_t bitmap_bytes_for(size_t block_size) {
    return round_up(block_size / (ARENA_ALIGN * 8), ARENA_ALIGN);
}

static size_t bitmap_offset(void) {
    return round_up(ARENA_HEADER, ARENA_ALIGN);
}

/* Where a block's own bump-allocated content begins: past the header and past the bitmap. The one
 * block that embeds the `Arena` struct (see arena_create_impl) has that in its content too, which
 * `arena_block_content` -- not this -- is what accounts for. */
static size_t content_offset(const ArenaBlock *b) {
    return bitmap_offset() + bitmap_bytes_for(b->size);
}

static unsigned char *bitmap_of(const ArenaBlock *b) {
    return (unsigned char *)((char *)b + bitmap_offset());
}

static void bitmap_set_start(ArenaBlock *b, size_t offset) {
    size_t bit = offset / ARENA_ALIGN;
    bitmap_of(b)[bit / 8] |= (unsigned char)(1u << (bit % 8));
}

static size_t page_size(void) {
    static size_t cached = 0;
    if (cached == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = ps > 0 ? (size_t)ps : 4096;
    }
    return cached;
}

/* A new block able to hand out at least `min_payload` bytes after its own header, `mmap`'d
 * directly (never `malloc`, see arena.h's own comment on why). Aborts the process on failure --
 * arena_alloc has no `Result` to report through and every other allocation failure in this
 * interpreter already does the same (this file has no more graceful option than malloc's own
 * callers already accept elsewhere). */
static ArenaBlock *block_new(size_t min_payload) {
    /* The bitmap's own size depends on the block's, which depends on the bitmap's -- resolved by
     * sizing the block without it, then growing a page at a time until it genuinely fits (a few
     * iterations at most, since each page added needs only another 32 bytes of bitmap to describe
     * it). Written as a loop rather than closed-form arithmetic on purpose: getting that algebra
     * subtly wrong would under-size a block by a few bytes, which is a corruption rather than a
     * shortfall, because arena_alloc trusts `size` to be the whole truth about the mapping. */
    size_t want = ARENA_HEADER + min_payload;
    size_t size = round_up(want > ARENA_BLOCK_SIZE ? want : ARENA_BLOCK_SIZE, page_size());
    while (bitmap_offset() + bitmap_bytes_for(size) + min_payload > size) {
        size = round_up(size + 1, page_size());
    }
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        abort();
    }
    ArenaBlock *b = mem;
    b->next = NULL;
    b->size = size;
    /* Rounded up to ARENA_ALIGN, not left at the header's own (unaligned) size: mmap's own base
     * address is always page-aligned, far stricter than ARENA_ALIGN needs, so keeping `used` a
     * multiple of ARENA_ALIGN from this point on is what keeps every arena_alloc result aligned --
     * ARENA_HEADER itself (three pointer/size_t-sized fields) has no reason to already be one. */
    b->used = content_offset(b);
    return b;
}

#ifdef ZIMAN_GC
/* Every live scanned arena, on an ordinary (non-`__thread`) global list, so that
 * `ziman_arena_push_scanned_roots` below can hand all of them to Boehm's own mark phase on every
 * collection -- see arena.h's own comment on `arena_create_scanned` for why these ranges have to be
 * scanned at all, and that same comment for why they are no longer registered one-by-one with
 * `GC_add_roots`. The lock is only ever taken by the (comparatively rare) link/unlink below; the collector
 * walks the list with every other thread already stopped and takes no lock at all, exactly as
 * gc_boehm_roots.c's own thread registry does.
 *
 * Both link and unlink are written so that a thread *suspended part-way through one* still leaves a
 * wholly valid list behind for the collector to walk, since unlike a thread registry these run
 * constantly during ordinary execution (every `Channel` send creates and destroys one) and so will
 * sometimes be interrupted by a collection. Each publishes its change with a single aligned pointer
 * store, so the list the collector sees either already includes the arena or does not yet -- never
 * something in between. */
static Arena *g_scanned = NULL;
static pthread_mutex_t g_scanned_lock = PTHREAD_MUTEX_INITIALIZER;

/* Links `a` in. Called before anything has been `arena_alloc`'d out of it, so the window where a
 * collection could run between `a`'s creation and its appearance on the list covers an arena with
 * nothing in it yet -- nothing unscanned that anything could already be pointing at. The release
 * store is what keeps `a->next_scanned` ordered before the publication of `a` itself. */
static void scanned_link(Arena *a) {
    pthread_mutex_lock(&g_scanned_lock);
    a->prev_scanned = NULL;
    a->next_scanned = g_scanned;
    if (g_scanned != NULL) g_scanned->prev_scanned = a;
    __atomic_store_n(&g_scanned, a, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_scanned_lock);
}

/* Unlinks `a`. The caller must do this *before* unmapping any of `a`'s blocks: a collection that
 * runs in between finds `a` already off the list and simply doesn't scan it (correct -- the
 * receiving side of a crossing already holds its own independent copy by the time anything destroys
 * the arena), whereas the reverse order would leave the collector reading memory `munmap` has
 * handed back to the OS. */
static void scanned_unlink(Arena *a) {
    pthread_mutex_lock(&g_scanned_lock);
    Arena *pv = a->prev_scanned;
    Arena *nx = a->next_scanned;
    /* The forward chain the collector walks is repaired by this one store, before anything touches
     * a `prev_scanned` (which only this function and scanned_link ever read, both under the lock) --
     * so a collection interrupting here still finds a whole, valid list. */
    if (pv != NULL) {
        __atomic_store_n(&pv->next_scanned, nx, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&g_scanned, nx, __ATOMIC_RELEASE);
    }
    if (nx != NULL) nx->prev_scanned = pv;
    pthread_mutex_unlock(&g_scanned_lock);
}
#endif

void ziman_arena_push_scanned_roots(void) {
#ifdef ZIMAN_GC
    for (Arena *a = __atomic_load_n(&g_scanned, __ATOMIC_ACQUIRE); a != NULL; a = a->next_scanned) {
        for (ArenaBlock *b = __atomic_load_n(&a->current, __ATOMIC_ACQUIRE); b != NULL; b = b->next) {
            /* `b->used`, not `b->size`: only the bytes arena_alloc has actually handed out, never
             * the rest of the block. The untouched tail is freshly-`mmap`'d zeroes by construction
             * (bump allocation only ever moves forward, and nothing is reused until the whole arena
             * goes), so no pointer can be hiding in it and scanning it is pure cost -- and a large
             * one: a block is `ARENA_BLOCK_SIZE` (256KB) even when a crossing copy of one small
             * value is all it will ever hold, which for a `Channel` with a page of rows queued on
             * it is the difference between scanning tens of bytes per message and scanning a
             * quarter-megabyte. Measured both ways on twelve threads each streaming a 4000-row
             * cursor: 3.4-4.0s pushing whole blocks against 1.4-1.5s pushing only `used`, and a
             * smaller 0.8-0.9s against 0.65s on six. Reading `used` live on every collection is
             * exactly what pushing from a hook makes possible and a one-shot `GC_add_roots` range
             * could not do, since it was fixed at block-creation time, before anything was in it --
             * it is also safe, since the thread that owns this arena is stopped while this runs, and
             * `arena_alloc` advances `used` past a new allocation *before* it returns the pointer,
             * so nothing can be written into a region this has not yet been told about.
             *
             * From `content_offset`, not the block's own base, for the same reason: the header and
             * the allocation-start bitmap hold no Ziman pointer, and the bitmap in particular is
             * dense bit patterns that conservative scanning would be forced to consider. */
            GC_push_all((char *)b + content_offset(b), (char *)b + b->used);
        }
    }
#endif
}

static Arena *arena_create_impl(int scanned) {
    /* The Arena struct itself lives in the first block's own payload, so creating one is exactly
     * one mmap call, not two -- the same "no allocation outside this module's own mechanism"
     * discipline the rest of this file follows. Placed at `used` (the content start) rather than at
     * `data`, which is where the block's own bitmap begins, not its content. Deliberately *not*
     * recorded in that bitmap: it is not an `arena_alloc` result and no allocation here is ever
     * meant to resolve to it -- `arena_block_content` skips past it for the same reason. */
    ArenaBlock *first = block_new(sizeof(Arena));
    Arena *a = (Arena *)((char *)first + first->used);
    first->used += round_up(sizeof(Arena), ARENA_ALIGN);
    a->current = first;
    a->reserved = first->size;
    a->scanned = scanned;
    a->next_scanned = NULL;
    a->prev_scanned = NULL;
#ifdef ZIMAN_GC
    if (scanned) scanned_link(a);
#endif
    return a;
}

Arena *arena_create(void) {
    return arena_create_impl(0);
}

Arena *arena_create_scanned(void) {
    return arena_create_impl(1);
}

void *arena_alloc(Arena *a, size_t size) {
    /* At least one alignment unit even for size == 0, so two zero-size requests in a row still
     * get distinct addresses (arena.h documents this; a plain round_up(0, ARENA_ALIGN) is 0,
     * which would hand out the same pointer for both). */
    size_t need = size == 0 ? ARENA_ALIGN : round_up(size, ARENA_ALIGN);
    ArenaBlock *cur = a->current;
    if (cur->used + need > cur->size) {
        /* No per-block registration any more: the fresh block joins this arena's own block list,
         * and ziman_arena_push_scanned_roots walks that list on every collection, so a scanned
         * arena's new block is covered from the moment `a->current` points at it -- which is before
         * `used` grows past the header just below, so no allocation can exist in a block the
         * collector has not been told about yet.
         *
         * The release store is what makes that true for a *concurrent* collection rather than just
         * in program order: `fresh->next` must already be the rest of the list when `a->current`
         * becomes visible, or a collector walking from `fresh` would stop at the NULL `block_new`
         * left there and silently miss every older block in this arena. */
        ArenaBlock *fresh = block_new(need);
        fresh->next = cur;
        __atomic_store_n(&a->current, fresh, __ATOMIC_RELEASE);
        a->reserved += fresh->size;
        cur = fresh;
    }
    void *p = cur->data + (cur->used - ARENA_HEADER);
    /* Recorded before `used` grows, so a block never claims an allocation it has not yet marked
     * the start of -- the same ordering arena_alloc already owes the collector for `used` itself. */
    bitmap_set_start(cur, cur->used);
    cur->used += need;
    return p;
}

void arena_destroy(Arena *a) {
#ifdef ZIMAN_GC
    /* Off the list first, every block only after that -- see scanned_unlink's own comment. */
    if (a->scanned) scanned_unlink(a);
#endif
    ArenaBlock *b = a->current;
    while (b != NULL) {
        ArenaBlock *next = b->next;
        size_t size = b->size;
        munmap(b, size);
        b = next;
    }
    /* `a` itself lived inside the first block just unmapped above -- nothing left to free for it. */
}

size_t arena_reserved(const Arena *a) {
    return a->reserved;
}

void arena_reclaim_block(Arena *a, ArenaBlock *prev, ArenaBlock *b) {
    /* Unlinking `b` from the block list is all a scanned arena needs: that list *is* what
     * ziman_arena_push_scanned_roots walks, so `b` stops being scanned the moment this store lands,
     * and the single aligned pointer store means a collection interrupting this either still sees
     * `b` (mapped -- munmap is below) or already skips it. */
    __atomic_store_n(&prev->next, b->next, __ATOMIC_RELEASE);
    size_t size = b->size;
    a->reserved -= size;
    munmap(b, size);
}

const ArenaBlock *arena_first_block(const Arena *a) {
    return a->current;
}

const ArenaBlock *arena_next_block(const ArenaBlock *b) {
    return b->next;
}

int arena_block_is_alloc_start(const ArenaBlock *b, const void *addr) {
    size_t offset = (size_t)((const char *)addr - (const char *)b);
    if (offset >= b->used || offset % ARENA_ALIGN != 0) return 0;
    size_t bit = offset / ARENA_ALIGN;
    return (bitmap_of(b)[bit / 8] >> (bit % 8)) & 1u;
}

const void *arena_block_alloc_start_at_or_before(const ArenaBlock *b, const void *addr) {
    size_t offset = (size_t)((const char *)addr - (const char *)b);
    if (offset >= b->used) return NULL;
    size_t bit = offset / ARENA_ALIGN;
    const unsigned char *map = bitmap_of(b);
    size_t first_bit = content_offset(b) / ARENA_ALIGN;

    /* Backwards from `bit`, a byte at a time: the partial byte holding `bit` itself first (masked
     * so bits above it are ignored), then whole bytes. `first_bit` is where this block's content
     * starts, so the scan can never run back into the bitmap's own bytes or the block header. */
    size_t byte = bit / 8;
    unsigned int bits = map[byte] & (unsigned int)((1u << (bit % 8 + 1)) - 1u);
    for (;;) {
        if (bits != 0) {
            /* The highest set bit in this byte is the nearest start at or before `addr`. */
            int top = 7;
            while (((bits >> top) & 1u) == 0) top--;
            size_t found = byte * 8 + (size_t)top;
            if (found < first_bit) return NULL;
            return (const char *)b + found * ARENA_ALIGN;
        }
        if (byte == 0 || byte * 8 <= first_bit) return NULL;
        byte--;
        bits = map[byte];
    }
}

void arena_block_content(const ArenaBlock *b, const char **out_start, size_t *out_len) {
    /* `next == NULL` identifies the one block that embeds the `Arena` struct itself (see
     * arena_create_impl): every other block is only ever created by the "current is full" branch
     * of arena_alloc, which sets its `next` before anything else can run, and nothing ever touches
     * the original block's `next` again after arena_create_impl leaves it NULL. */
    size_t used_at_start = content_offset(b);
    if (b->next == NULL) used_at_start += round_up(sizeof(Arena), ARENA_ALIGN);
    *out_start = (const char *)b + used_at_start;
    *out_len = b->used - used_at_start;
}
