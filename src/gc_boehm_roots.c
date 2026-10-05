#include "gc_boehm_roots.h"

#ifdef ZIMAN_GC
#include <gc/gc_mark.h>
#include <pthread.h>

#include "arena.h"
#include "error.h"
#include "interpreter.h"

/* One record per thread that has registered below, on a global (non-`__thread`) list for exactly
 * the reason native_thread.c's own ThreadInfo/g_threads is one -- see this module's header comment.
 * `frames`/`interp` are each just the *addresses* ziman_frames_root_addrs/ziman_interp_root_addrs
 * handed back at registration time; push_other_roots re-reads through them on every collection. */
typedef struct ThreadRootInfo {
    struct ThreadRootInfo *next;
    ZimanFramesRootAddrs frames;
    ZimanInterpRootAddrs interp;
} ThreadRootInfo;

static __thread ThreadRootInfo *t_info = NULL;
static ThreadRootInfo *g_roots = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* GC_set_push_other_roots replaces, not augments, GC_push_other_roots -- which defaults to
 * GC_default_push_other_roots, i.e. GC_push_all_stacks(), the thing that pushes every thread's own
 * stack and registers in the first place (vendor/bdwgc/os_dep.c). Installing our own without saving
 * and chaining to this would silently stop scanning every thread's stack -- found the hard way, as
 * `make fmt-check`'s very first real multi-file run (many allocations, one real collection) started
 * segfaulting deep inside GC_realloc/GC_free_internal: with no thread stacks scanned at all, nearly
 * everything looked unreachable, a far worse breakage than the one bug this module exists to close. */
static GC_push_other_roots_proc g_previous_push_other_roots = NULL;

/* GC_set_push_other_roots' own callback: runs on whichever thread is doing the collecting, with
 * every other thread already stopped, so g_roots cannot change underneath this walk and no lock is
 * needed here (only register/unregister, below, ever touch the lock). */
static void push_other_roots(void) {
    if (g_previous_push_other_roots != NULL) g_previous_push_other_roots();

    /* Every live `arena_create_scanned` arena's blocks. These used to register themselves with
     * `GC_add_roots` instead, which worked but capped the whole process at `MAX_ROOT_SETS` (2048)
     * simultaneously-registered ranges -- and `value_copy_send` makes one arena per value queued on
     * a `Channel`, so that was a hard ceiling on ordinary, non-leaking concurrent code (arena.h's
     * own comment on `arena_create_scanned`, and docs/DESIGN.md). Pushed from here instead, the
     * same uncapped way this hook already handles every other root source Boehm cannot find alone. */
    ziman_arena_push_scanned_roots();

    void **global_env = ziman_global_env_addr();
    if (*global_env != NULL) GC_push_all(global_env, global_env + 1);

    for (ThreadRootInfo *t = g_roots; t != NULL; t = t->next) {
        ZimanMemRange frame_ranges[1];
        int nf = ziman_frames_root_ranges(t->frames, frame_ranges, 1);
        for (int i = 0; i < nf; i++) GC_push_all(frame_ranges[i].start, frame_ranges[i].end);

        ZimanMemRange interp_ranges[4];
        int ni = ziman_interp_root_ranges(t->interp, interp_ranges, 4);
        for (int i = 0; i < ni; i++) GC_push_all(interp_ranges[i].start, interp_ranges[i].end);
    }
}
#endif

void ziman_gc_install_boehm_root_hook(void) {
#ifdef ZIMAN_GC
    g_previous_push_other_roots = GC_get_push_other_roots();
    GC_set_push_other_roots(push_other_roots);
#endif
}

void ziman_gc_register_thread_roots(void) {
#ifdef ZIMAN_GC
    ThreadRootInfo *t = calloc(1, sizeof(ThreadRootInfo));
    t->frames = ziman_frames_root_addrs();
    t->interp = ziman_interp_root_addrs();
    pthread_mutex_lock(&g_lock);
    t->next = g_roots;
    g_roots = t;
    pthread_mutex_unlock(&g_lock);
    t_info = t;
#endif
}

void ziman_gc_unregister_thread_roots(void) {
#ifdef ZIMAN_GC
    ThreadRootInfo *t = t_info;
    if (t == NULL) return;
    pthread_mutex_lock(&g_lock);
    for (ThreadRootInfo **p = &g_roots; *p != NULL; p = &(*p)->next) {
        if (*p == t) { *p = t->next; break; }
    }
    pthread_mutex_unlock(&g_lock);
    t_info = NULL;
#endif
}
