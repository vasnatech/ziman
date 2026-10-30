#include "gc_mark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_lookup.h"
#include "gc_object.h"
#include "mapobj.h"
#include "xalloc.h"

static void gc_mark_into(const Arena *a, Value v);
static void mark_trace(const Arena *a, Value v);

/* The explicit mark stack that replaces the C stack (A43).
 *
 * This phase used to be mutual recursion -- gc_mark_into dispatching to gc_mark_enum, which called
 * gc_mark_into on its payload -- so its depth was the depth of the value graph, and it overflowed
 * the C stack *inside a collection*, where no guard is possible: A1's stack-depth limit protects
 * the parser, the checker and the interpreter, and none of those is on the stack when the collector
 * runs. The result was a bare SIGSEGV. Reproduced at 300,000 links of `struct Node { next:
 * Option<Node> }` on a spawned thread (`--thread-stack-mb=1` and `=4`, core dumped both times),
 * with gc_mark_enum -> gc_mark_struct -> gc_mark_into repeating to the stack bottom. The main
 * thread's much larger stack takes millions of links, which is why this had not been seen.
 *
 * Depth-first with an explicit stack, so a *chain* of any length now costs O(1) entries: a link is
 * popped, marked, and its single child pushed in its place. Only breadth costs memory -- a wide
 * boxed array pushes all of its elements -- which is the normal trade of every iterative marker.
 *
 * Per-thread, because each thread collects its own heap (gc_thread_heap.c) and two of them can be
 * marking at once. `GC_MALLOC_UNCOLLECTABLE` rather than plain malloc for the same reason
 * gc_heap_create gives: a ZIMAN_GC build's own zgc.h redirects malloc to collectable GC_MALLOC, and
 * this codebase's established finding is that Boehm does not reliably scan thread-local storage, so
 * a collectable buffer reachable only from a `__thread` pointer could be reclaimed under the thread
 * still marking with it. It is never freed, deliberately: it is reused by every later collection on
 * this thread, which also keeps growth -- the one allocation this phase makes -- rare. */
#define GC_MARK_STACK_INITIAL 256

/* One entry: either a Value still to be traced, or a *cursor* over a container's slots.
 *
 * The cursor is what keeps this from trading one resource problem for another. A container that
 * pushed all of its children at once would make the stack as big as the widest array in the heap:
 * measured at 543 MB -> 825 MB peak RSS on a two-million-element `[Cell]`, a 52% ratchet on
 * exactly the shape a program holds a lot of. Pushing one cursor instead makes the stack O(nesting
 * depth) -- that same array costs a single slot, and the 2M-element measurement comes back level
 * with the recursive marker. */
typedef struct {
    const Value *slots; /* non-NULL: trace slots[next .. count). NULL: trace `v`. */
    int count;
    int next;
    Value v;
} MarkItem;

static __thread MarkItem *t_mark_stack;
static __thread size_t t_mark_count;
static __thread size_t t_mark_capacity;
static __thread int t_mark_draining;

static void mark_stack_grow(void) {
    size_t want = t_mark_capacity == 0 ? GC_MARK_STACK_INITIAL : t_mark_capacity * 2;
#ifdef ZIMAN_GC
    MarkItem *fresh = ziman_xcheck(GC_MALLOC_UNCOLLECTABLE(sizeof(MarkItem) * want));
#else
    MarkItem *fresh = ziman_xmalloc(sizeof(MarkItem) * want);
#endif
    if (t_mark_stack != NULL) {
        memcpy(fresh, t_mark_stack, sizeof(MarkItem) * t_mark_count);
#ifdef ZIMAN_GC
        GC_FREE(t_mark_stack);
#else
        free(t_mark_stack);
#endif
    }
    t_mark_stack = fresh;
    t_mark_capacity = want;
}

/* A run of adjacent Values -- a boxed array's elements, a struct's fields, a closure's captures.
 * Nothing moves during a mark, so holding the pointer across the drain is safe.
 *
 * A short run is pushed element by element and a long one as a cursor, because the two costs pull
 * in opposite directions and both were measured. Cursors everywhere cost a branch and a reread of
 * the top entry per element, which showed up as 8% on an allocation-heavy benchmark -- most runs
 * are a struct's two or three fields, where a cursor is pure overhead. Elements everywhere makes
 * the stack as wide as the widest array in the heap, which was +282 MB of peak RSS on a
 * two-million-element `[Cell]`. Sixteen keeps the struct and small-array cases on the cheap path
 * while every run big enough for its size to matter takes one slot. */
#define GC_MARK_INLINE_SLOTS 16

static void mark_push_slots(const Arena *a, const Value *slots, int count) {
    if (slots == NULL || count <= 0) return;
    if (count <= GC_MARK_INLINE_SLOTS) {
        for (int i = 0; i < count; i++) gc_mark_into(a, slots[i]);
        return;
    }
    if (t_mark_count == t_mark_capacity) mark_stack_grow();
    t_mark_stack[t_mark_count].slots = slots;
    t_mark_stack[t_mark_count].count = count;
    t_mark_stack[t_mark_count].next = 0;
    t_mark_count++;
}

/* Drains the stack. Entered only from the outermost gc_mark_value/gc_mark_object, so a registered
 * struct-content marker calling back into this file pushes onto the same stack rather than starting
 * a second drain -- which is what keeps the whole phase one level deep however the graph is shaped. */
static void mark_drain(const Arena *a) {
    if (t_mark_draining) return;
    t_mark_draining = 1;
    while (t_mark_count > 0) {
        MarkItem *top = &t_mark_stack[t_mark_count - 1];
        if (top->slots == NULL) {
            Value v = top->v;
            t_mark_count--;
            mark_trace(a, v);
        } else if (top->next >= top->count) {
            t_mark_count--;
        } else {
            /* Read the element and advance the cursor *before* gc_mark_into, which may grow the
             * stack and leave `top` dangling. */
            Value v = top->slots[top->next++];
            gc_mark_into(a, v);
        }
    }
    t_mark_draining = 0;
}

/* See gc_mark_register_struct_content_marker's own comment (gc_mark.h) for what this is and why it
 * exists. A handful of fixed slots, not a dynamic list: only MutableArray/MutableMap/MutableSet ever
 * register one today, and this is filled once at startup, never resized concurrently with a lookup. */
#define GC_MAX_STRUCT_CONTENT_MARKERS 8
static struct {
    const char *type_name;
    void (*mark_contents)(const Arena *a, StructObj *st);
} g_struct_content_markers[GC_MAX_STRUCT_CONTENT_MARKERS];
static int g_struct_content_marker_count = 0;

void gc_mark_register_struct_content_marker(const char *type_name,
                                             void (*mark_contents)(const Arena *a, StructObj *st)) {
    /* The ninth registration would write a `type_name` *and a function pointer* past the end of a
     * file-scope array -- into whatever follows it, which the collector then calls. Three are
     * registered today and the comment above says so, which is the only thing that was keeping
     * this in bounds; an argument about today's call sites is not a bound (F126). Startup-only, so
     * failing hard here is right: there is no sensible way to run with a marker dropped. */
    if (g_struct_content_marker_count >= GC_MAX_STRUCT_CONTENT_MARKERS) {
        fprintf(stderr,
                "ziman: more than %d struct content markers registered ('%s' did not fit) -- raise "
                "GC_MAX_STRUCT_CONTENT_MARKERS in src/gc_mark.c\n",
                GC_MAX_STRUCT_CONTENT_MARKERS, type_name);
        exit(1);
    }
    g_struct_content_markers[g_struct_content_marker_count].type_name = type_name;
    g_struct_content_markers[g_struct_content_marker_count].mark_contents = mark_contents;
    g_struct_content_marker_count++;
}

static void gc_mark_registered_contents(const Arena *a, const Type *t, StructObj *st) {
    /* Gated on the kind, not only on the name being non-NULL. Today's single caller only ever
     * passes a TY_STRUCT, so the name check alone happens to be enough -- an implicit contract
     * worth stating rather than relying on, since what this does on a match is call a registered
     * marker that assumes `st` really is the struct the name says. */
    if (t->kind != TY_STRUCT || t->struct_name == NULL) return;
    for (int i = 0; i < g_struct_content_marker_count; i++) {
        if (strcmp(g_struct_content_markers[i].type_name, t->struct_name) == 0) {
            g_struct_content_markers[i].mark_contents(a, st);
            return;
        }
    }
}

/* Resolves `ptr` (which may be an interior pointer -- a string view, an `array_obj_view` slice)
 * through `gc_lookup` to the enclosing object's own exact start, and marks *that* -- `gc_set_marked`
 * cannot be called on an interior address directly (see gc_object.h's own contract). A `NULL`
 * result would mean `ptr` doesn't resolve to anything `a` actually holds at all; harmless to just
 * skip, since there is nothing to mark either way -- this also already makes this function safe to
 * call on a pointer that lives in a *different* arena entirely (see is_gc_heap_object's own comment
 * below for why that matters), since gc_lookup naturally answers "not found" for it too. */
static void mark_interior_capable(const Arena *a, const void *ptr) {
    if (ptr == NULL) return;
    const void *obj = gc_lookup(a, ptr);
    if (obj != NULL) gc_set_marked((void *)obj, 1);
}

/* Whether `ptr` is actually a `gc_alloc`'d object's own exact start within `a` -- `gc_lookup`
 * resolves both exact and interior addresses, so an exact match against its own result confirms
 * `ptr` itself is a real object here, not merely inside one.
 *
 * Exists because not every ArrayObj/StructObj/ResultObj/EnumObj/ClosureObj/boxed BigNum/Decimal
 * this interpreter ever builds lives in a `GcHeap` arena at all -- most don't yet (only
 * `value_string_copy` has been migrated so far, docs/design/journal/'s "narrowest possible slice"), and
 * a value that crossed a `spawn`/`Channel` boundary (`value_copy.c`'s own outbound arena) never
 * will, by design. Those objects have no `gc_alloc` header -- `gc_set_marked`/`gc_is_marked` read
 * and write the bytes immediately before an object's own address, and calling them on a pointer
 * with no real header there corrupts whatever those bytes actually belong to (found the hard way:
 * a struct's own `bool` field, sitting where a non-existent header's `marked` bit landed, flipped
 * from `false` to `true` the moment a collection first ran against a program crossing it through
 * `spawn` -- see docs/design/journal/'s own entry on this incident for the full story). Every per-kind
 * marking function below checks this before touching an object's own mark bit, but still recurses
 * into its fields regardless: a plain, not-yet-migrated struct can perfectly well hold a field that
 * *is* `gc_heap`-allocated (an ordinary struct with a string field, once any string constructor
 * beyond `value_string_copy` migrates), and that field still needs protecting.
 *
 * This is a second `gc_lookup` call per structural object during marking, on top of the one
 * `mark_interior_capable` already needed for interior-pointer-capable buffers. It was landed as a
 * knowingly-unmeasured cost ("correctness first, measured before optimized") and, when finally
 * measured, turned out to be the single largest one in the interpreter under concurrent load --
 * 37% of all CPU time in a cursor-streaming benchmark, because answering it meant walking the
 * containing block's objects from the start, 827 of them per answer. It is now a single bit test
 * against the allocation-start bitmap each arena block keeps (`arena.h`, `gc_lookup.c`), so the
 * two calls per object cost essentially nothing and the gate can stay exactly where correctness
 * wants it. */
static int is_gc_heap_object(const Arena *a, const void *ptr) {
    return gc_lookup(a, ptr) == ptr;
}

static void gc_mark_bignum_digits(const Arena *a, BigNum n) {
    /* Always a whole, own-start allocation -- every BigNum operation builds a fresh one (see
     * bignum.h), never a view -- so no interior-pointer resolution is ever needed here, unlike
     * mark_interior_capable's own cases. Still needs the same is_gc_heap_object gate as everything
     * else in this file, though: the digit array can just as easily be a plain (not yet migrated)
     * or permanently outbound-arena allocation as the boxed BigNum/Decimal wrapping it can. */
    if (n.count > 0 && is_gc_heap_object(a, n.digits)) gc_set_marked(n.digits, 1);
}

static void gc_mark_array(const Arena *a, ArrayObj *arr) {
    /* Only an object gc_alloc actually built here has a real header to read or set -- see
     * is_gc_heap_object's own comment. A plain (not yet migrated, or permanently outbound-arena)
     * ArrayObj has no mark bit to check or set, and so no "already visited" guard either; fields
     * are still walked every time this reaches it, which cannot loop (the graph is a DAG) and only
     * costs redundant work on a shared, not-yet-migrated substructure -- an accepted, documented
     * cost, not a new correctness issue. */
    if (is_gc_heap_object(a, arr)) {
        if (gc_is_marked(arr)) return;
        gc_set_marked(arr, 1);
    }
    /* At most one of these is non-NULL (see ArrayObj's own doc comment); each is interior-pointer-
     * capable (array_obj_view slices whichever one is in use at an offset), so each goes through
     * mark_interior_capable, never a direct gc_set_marked. */
    mark_interior_capable(a, arr->boxed_items);
    mark_interior_capable(a, arr->bytes);
    mark_interior_capable(a, arr->bools);
    mark_interior_capable(a, arr->ints);
    mark_interior_capable(a, arr->floats);
    /* Packed elements (bytes/bools/ints/floats) are scalars -- nothing further to trace into them,
     * so only a boxed array's own elements need walking, each by its own Value.type.kind. */
    mark_push_slots(a, arr->boxed_items, arr->count);
}

static void gc_mark_struct(const Arena *a, StructObj *st) {
    /* See gc_mark_array's own comment on is_gc_heap_object -- identical reasoning applies here. */
    if (is_gc_heap_object(a, st)) {
        if (gc_is_marked(st)) return;
        gc_set_marked(st, 1);
    }
    /* field_names is borrowed from the StructDecl (permanent AST, never gc_alloc'd) -- nothing to
     * mark there. fields is a Value[] buffer, always its own whole allocation for a StructObj
     * (never sliced the way ArrayObj.boxed_items can be -- see gc_copy.h's own reasoning for why
     * the same uniform rule is still applied there anyway; marking tolerates either choice equally
     * well, since mark_interior_capable resolves an exact-start pointer to itself with no extra
     * cost beyond the lookup call itself). */
    mark_interior_capable(a, st->fields);
    mark_push_slots(a, st->fields, st->count);
}

static void gc_mark_result(const Arena *a, ResultObj *res) {
    /* See gc_mark_array's own comment on is_gc_heap_object. */
    if (is_gc_heap_object(a, res)) {
        if (gc_is_marked(res)) return;
        gc_set_marked(res, 1);
    }
    gc_mark_into(a, res->value);
}

static void gc_mark_enum(const Arena *a, EnumObj *en) {
    /* See gc_mark_array's own comment on is_gc_heap_object. */
    if (is_gc_heap_object(a, en)) {
        if (gc_is_marked(en)) return;
        gc_set_marked(en, 1);
    }
    /* enum_name/variant_name borrowed from the EnumDecl (permanent AST) -- nothing to mark. */
    if (en->has_payload) gc_mark_into(a, en->payload);
}

static void gc_mark_closure(const Arena *a, ClosureObj *clo) {
    /* See gc_mark_array's own comment on is_gc_heap_object. */
    if (is_gc_heap_object(a, clo)) {
        if (gc_is_marked(clo)) return;
        gc_set_marked(clo, 1);
    }
    /* fn is a FunctionDecl* -- permanent AST, nothing to mark. */
    mark_interior_capable(a, clo->captured);
    mark_push_slots(a, clo->captured, clo->ncaptured);
}

/* mapobj_mark_leaves's own callback (mapobj.h): marks one Leaf's own header (if it has one -- see
 * gc_mark_map's own comment on mapobj_mut_new's builder-private MapObj, the one case that never
 * reaches here with a real one) and its key/val Values. `leaf` is opaque to this file (Leaf is
 * private to mapobj.c) -- is_gc_heap_object/gc_set_marked need nothing more than the bare address. */
static void mark_leaf(const void *leaf, Value key, Value val, void *ctx) {
    const Arena *a = ctx;
    if (is_gc_heap_object(a, leaf)) gc_set_marked((void *)leaf, 1);
    gc_mark_into(a, key);
    gc_mark_into(a, val);
}

static void gc_mark_map(const Arena *a, MapObj *m) {
    /* See gc_mark_array's own comment on is_gc_heap_object -- identical reasoning for the wrapper
     * itself: MapObj is gc_heap-migrated (mapobj_empty/set/remove/snapshot), but mapobj_mut_new's own
     * builder-private MapObj is never wrapped in a TY_MAP/TY_SET Value directly (only a
     * mapobj_snapshot of it is), so it never reaches this function with a real header at all.
     * mapobj_mark_leaves (not mapobj_entries, as this used to be) walks every entry once, regardless
     * of whether `m` itself has a real header -- same reasoning, since a plain not-yet-migrated
     * object can still hold gc_heap-allocated fields. */
    if (is_gc_heap_object(a, m)) {
        if (gc_is_marked(m)) return;
        gc_set_marked(m, 1);
    }
    mapobj_mark_leaves(m, mark_leaf, (void *)a);
}

/* Every call that used to recurse into a child now lands here, and this only ever *pushes* -- the
 * name is kept because all seven of its callers mean the same thing by it as before. A kind with no
 * children is finished on the spot rather than taking a slot. */
static void gc_mark_into(const Arena *a, Value v) {
    switch (v.type->kind) {
        case TY_STRING:
            mark_interior_capable(a, v.as.str.ptr);
            return;
        case TY_BIGINT:
            /* See gc_mark_array's own comment on is_gc_heap_object: a boxed BigNum not yet
             * migrated to gc_heap allocation (or permanently outbound-arena) has no real header. */
            if (is_gc_heap_object(a, v.as.bi)) {
                if (gc_is_marked(v.as.bi)) return;
                gc_set_marked(v.as.bi, 1);
            }
            gc_mark_bignum_digits(a, *v.as.bi);
            return;
        case TY_DECIMAL:
            if (is_gc_heap_object(a, v.as.dec)) {
                if (gc_is_marked(v.as.dec)) return;
                gc_set_marked(v.as.dec, 1);
            }
            gc_mark_bignum_digits(a, v.as.dec->unscaled);
            return;
        case TY_ARRAY:
        case TY_STRUCT:
        case TY_RESULT:
        case TY_ENUM:
        case TY_FUNC:
        case TY_MAP:
        case TY_SET:
            break; /* has children: traced from the stack, so the depth is the stack's, not the C one's */
        case TY_CHANNEL:
            /* Never traced into -- see this file's own header comment. */
            return;
        default:
            /* int, float, bool, byte, unit: embedded in the Value itself, nothing to mark. */
            return;
    }
    if (t_mark_count == t_mark_capacity) mark_stack_grow();
    t_mark_stack[t_mark_count].slots = NULL;
    t_mark_stack[t_mark_count].v = v;
    t_mark_count++;
}

/* One popped Value: marks what it names and pushes its children. Only the kinds gc_mark_into
 * pushes can arrive here. */
static void mark_trace(const Arena *a, Value v) {
    switch (v.type->kind) {
        case TY_ARRAY:
            gc_mark_array(a, v.as.arr);
            return;
        case TY_STRUCT:
            gc_mark_struct(a, v.as.st);
            gc_mark_registered_contents(a, v.type, v.as.st);
            return;
        case TY_RESULT:
            gc_mark_result(a, v.as.res);
            return;
        case TY_ENUM:
            gc_mark_enum(a, v.as.en);
            return;
        case TY_FUNC:
            gc_mark_closure(a, v.as.clo);
            return;
        case TY_MAP:
        case TY_SET:
            gc_mark_map(a, v.as.map);
            return;
        default:
            return;
    }
}

void gc_mark_value(const Arena *a, Value v) {
    gc_mark_into(a, v);
    mark_drain(a);
}

static void mark_object_shallow(const Arena *a, const void *obj);

void gc_mark_object(const Arena *a, const void *obj) {
    mark_object_shallow(a, obj);
    mark_drain(a);
}

static void mark_object_shallow(const Arena *a, const void *obj) {
    if (obj == NULL) return;
    switch (gc_kind_of(obj)) {
        case GC_ATOMIC:
            /* A leaf: mark it and stop. Idempotent regardless (setting an already-set bit is a
             * no-op), so no gc_is_marked check is needed before it, unlike the structural kinds
             * below, which need one to avoid retracing an already-visited object's own children. */
            gc_set_marked((void *)obj, 1);
            return;
        case GC_VALUES: {
            if (gc_is_marked(obj)) return;
            gc_set_marked((void *)obj, 1);
            size_t count = gc_size_of(obj) / sizeof(Value);
            mark_push_slots(a, (const Value *)obj, (int)count);
            return;
        }
        case GC_ARRAY:
            gc_mark_array(a, (ArrayObj *)obj);
            return;
        case GC_STRUCT:
            gc_mark_struct(a, (StructObj *)obj);
            return;
        case GC_RESULT:
            gc_mark_result(a, (ResultObj *)obj);
            return;
        case GC_ENUM:
            gc_mark_enum(a, (EnumObj *)obj);
            return;
        case GC_CLOSURE:
            gc_mark_closure(a, (ClosureObj *)obj);
            return;
        case GC_MAP:
            gc_mark_map(a, (MapObj *)obj);
            return;
        case GC_MAP_LEAF:
            /* A stray stack word resolving to a Leaf's own exact start -- the real, complete
             * marking (its own key/val Values too) happens through gc_mark_map's own
             * mapobj_mark_leaves walk, reached via the MapObj that actually owns it; this type-less
             * path has no way to recover Leaf's own private layout (it's private to mapobj.c) to
             * trace further, so it only protects the header itself here, matching GC_ATOMIC's own
             * "nothing further to do without type info" shape. Expected to be rare to unreachable in
             * practice: a freshly built Leaf is always linked into its own tree with no re-entrant
             * Ziman call (and so no possible collection point) in between -- the same class of
             * harmless, conservative-scanning false positive GC_FREE's own comment already accepts. */
            gc_set_marked((void *)obj, 1);
            return;
        case GC_FREE:
            /* A stray stack word resolved to an already-reclaimed chunk -- not live, nothing to
             * mark (see this function's own header comment). */
            return;
    }
}
