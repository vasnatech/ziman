#include "arena.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* Only for GC_add_roots/GC_remove_roots (arena_create_scanned). A `make`/`make gc` build already
 * force-includes src/zgc.h into every translation unit (the Makefile's own `-include`, which is
 * what zgc.h's malloc/calloc/realloc/free redefinitions rely on -- see arena.h's comment on why
 * this file never calls any of them, mmap/munmap being untouched by that redirect either way) --
 * this explicit include is for gc.h's own declarations, not to pull in anything zgc.h wouldn't
 * already have made available transitively. */
#ifdef ZIMAN_GC
#include <gc.h>
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
    size_t want = ARENA_HEADER + min_payload;
    size_t size = round_up(want > ARENA_BLOCK_SIZE ? want : ARENA_BLOCK_SIZE, page_size());
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
    b->used = round_up(ARENA_HEADER, ARENA_ALIGN);
    return b;
}

/* Registers `b` with the collector as an additional conservatively-scanned range, in a `ZIMAN_GC`
 * build with `scanned` set -- a no-op otherwise (arena.h's own comment on `arena_create_scanned`
 * explains what this is for and why it's needed at all). */
static void maybe_scan(const ArenaBlock *b, int scanned) {
#ifdef ZIMAN_GC
    if (scanned) GC_add_roots((void *)b, (char *)b + b->size);
#else
    (void)b;
    (void)scanned;
#endif
}

static Arena *arena_create_impl(int scanned) {
    /* The Arena struct itself lives in the first block's own payload, so creating one is exactly
     * one mmap call, not two -- the same "no allocation outside this module's own mechanism"
     * discipline the rest of this file follows. */
    ArenaBlock *first = block_new(sizeof(Arena));
    maybe_scan(first, scanned);
    Arena *a = (Arena *)(first->data);
    first->used += round_up(sizeof(Arena), ARENA_ALIGN);
    a->current = first;
    a->reserved = first->size;
    a->scanned = scanned;
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
        ArenaBlock *fresh = block_new(need);
        maybe_scan(fresh, a->scanned);
        fresh->next = cur;
        a->current = fresh;
        a->reserved += fresh->size;
        cur = fresh;
    }
    void *p = cur->data + (cur->used - ARENA_HEADER);
    cur->used += need;
    return p;
}

void arena_destroy(Arena *a) {
    int scanned = a->scanned;
    ArenaBlock *b = a->current;
    while (b != NULL) {
        ArenaBlock *next = b->next;
        size_t size = b->size;
#ifdef ZIMAN_GC
        if (scanned) GC_remove_roots((void *)b, (char *)b + size);
#else
        (void)scanned;
#endif
        munmap(b, size);
        b = next;
    }
    /* `a` itself lived inside the first block just unmapped above -- nothing left to free for it. */
}

size_t arena_reserved(const Arena *a) {
    return a->reserved;
}
