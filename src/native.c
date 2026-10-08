#define _POSIX_C_SOURCE 200809L
#include "native.h"
#include "xalloc.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *qualified_name;
    NativeFn fn;
} NativeEntry;

static NativeEntry *entries = NULL;
static int count = 0;
static int capacity = 0;

void native_register(const char *qualified_name, NativeFn fn) {
    if (count == capacity) {
        capacity = capacity == 0 ? 8 : capacity * 2;
        entries = ziman_xrealloc(entries, (size_t)capacity * sizeof(NativeEntry));
    }
    entries[count].qualified_name = ziman_xstrdup(qualified_name);
    entries[count].fn = fn;
    count++;
}

NativeFn native_lookup(const char *qualified_name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].qualified_name, qualified_name) == 0) {
            return entries[i].fn;
        }
    }
    return NULL;
}
