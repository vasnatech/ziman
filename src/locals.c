#define _POSIX_C_SOURCE 200809L
#include "locals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "error.h"
#include "value.h"
#include "xalloc.h"

/* A single value is truncated to this many characters. Long enough that an ordinary array, string
 * or struct is shown whole, short enough that one variable holding a hundred thousand elements
 * cannot bury the rest of the trace -- which is the case that matters, since the variable you need
 * to see is rarely the enormous one. */
#define MAX_VALUE_CHARS 160

typedef struct {
    char *text;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const char *s) {
    size_t add = strlen(s);
    if (b->len + add + 1 > b->cap) {
        while (b->len + add + 1 > b->cap) b->cap = b->cap == 0 ? 256 : b->cap * 2;
        b->text = ziman_xrealloc(b->text, b->cap);
    }
    memcpy(b->text + b->len, s, add + 1);
    b->len += add;
}

/* `value_display` renders arrays and structs recursively, so a big one produces a big string: it is
 * truncated here rather than by asking for less, because nothing in that interface takes a limit
 * and an error path is the wrong place to add one. The marker says the value continued, so a reader
 * is never shown a prefix that looks like the whole thing. */
static void buf_add_value(Buf *b, Value v, Type t) {
    /* A string is quoted here, which `print` deliberately does not do: in a dump the difference
     * between the number 5 and the string "5" is exactly the kind of thing being looked for, and
     * leading or trailing whitespace is invisible without delimiters. Only at the top level --
     * value_display renders the insides of an array or struct itself, so a string nested in one is
     * still shown bare, which is a limitation of that shared renderer rather than of this. */
    int quote = t.kind == TY_STRING;
    char *shown = value_display(v);
    if (shown == NULL) {
        buf_add(b, "<unprintable>");
        return;
    }
    if (quote) buf_add(b, "\"");
    if (strlen(shown) > MAX_VALUE_CHARS) {
        shown[MAX_VALUE_CHARS] = '\0';
        buf_add(b, shown);
        if (quote) buf_add(b, "\"");
        buf_add(b, "... (truncated)");
    } else {
        buf_add(b, shown);
        if (quote) buf_add(b, "\"");
    }
    free(shown);
}

/* Innermost scope first, out to but not including the global one. Returns NULL when the frame has
 * nothing to show, which the caller treats as "print no locals for this frame" rather than as an
 * empty section. */
static char *render(void *env_void) {
    const Env *env = env_void;
    if (env == NULL) return NULL;

    Buf b = {0};
    /* Names already printed for this frame, so a binding shadowed by an inner block is shown once,
     * as the inner one. A frame has a handful of variables, so a linear scan is the right tool and
     * the array is sized as it goes. */
    const char **seen = NULL;
    int seen_count = 0, seen_cap = 0;

    for (const Env *e = env; e != NULL && !e->is_global; e = e->parent) {
        for (int i = 0; i < e->count; i++) {
            const Binding *bind = &e->items[i];
            if (bind->name == NULL) continue;
            /* Compiler-generated bindings, not anything the user wrote: `$` cannot occur in an
             * identifier (lexer.c accepts only alphanumerics and `_`), so it is the marker for a
             * name this implementation invented -- parser.c's `name$default$N` placeholders for
             * default arguments being the ones that otherwise show up in every single frame. */
            if (strchr(bind->name, '$') != NULL) continue;
            int shadowed = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen[s], bind->name) == 0) { shadowed = 1; break; }
            }
            if (shadowed) continue;
            if (seen_count == seen_cap) {
                seen_cap = seen_cap == 0 ? 8 : seen_cap * 2;
                seen = ziman_xrealloc(seen, (size_t)seen_cap * sizeof(*seen));
            }
            seen[seen_count++] = bind->name;

            char *tname = type_name(bind->type);
            buf_add(&b, "\n        ");
            buf_add(&b, bind->name);
            buf_add(&b, ": ");
            buf_add(&b, tname != NULL ? tname : "?");
            buf_add(&b, " = ");
            free(tname);
            buf_add_value(&b, bind->value, bind->type);
        }
    }
    free(seen);
    return b.text; /* NULL when nothing was added, which is exactly the "nothing to show" signal */
}

void ziman_locals_enable(void) {
    ziman_set_locals_renderer(render);
}
