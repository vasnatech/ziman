#include "gc_thread_heap.h"

static __thread GcHeap *t_heap = NULL;

GcHeap *ziman_thread_heap(void) {
    if (t_heap == NULL) t_heap = gc_heap_create();
    return t_heap;
}

size_t ziman_thread_heap_reserved(void) {
    return gc_heap_reserved(ziman_thread_heap());
}
