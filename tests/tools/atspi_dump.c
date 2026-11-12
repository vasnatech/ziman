/* Walks the accessibility bus the way a screen reader does and prints what it finds.
 *
 * **This is the gate for F275, and the distinction it draws is the whole point**: not "the program
 * called the bridge" but "something else can read it". Nothing inside ziman can check that -- the
 * publishing side would be marking its own homework -- so this is a separate process that connects
 * to the session's accessibility bus, finds the application by name, and prints the tree with the
 * roles a reader would announce.
 *
 * Compiled on demand by `tests/run_a11y_test.sh` rather than by the Makefile, for the reason
 * `x11_inject.c` is: it keeps libatspi a dependency of running the test and not of building ziman.
 *
 * Takes an application name and exits non-zero if it is not on the bus. */
#include <atspi/atspi.h>
#include <stdio.h>
#include <string.h>

/* The states worth printing: the ones a reader's behaviour actually turns on. Not every state --
 * a dump of all of them is unreadable and the ones left out (visible, showing, sensitive) are true
 * of everything here. */
static void print_states(AtspiAccessible *node) {
    AtspiStateSet *set = atspi_accessible_get_state_set(node);
    if (set == NULL) return;
    struct { AtspiStateType state; const char *word; } interesting[] = {
        {ATSPI_STATE_FOCUSED, "focused"},
        {ATSPI_STATE_SELECTED, "selected"},
        {ATSPI_STATE_CHECKED, "checked"},
        {ATSPI_STATE_INDETERMINATE, "partially checked"},
        {ATSPI_STATE_EXPANDABLE, "expandable"},
        {ATSPI_STATE_EXPANDED, "expanded"},
        {ATSPI_STATE_MULTI_LINE, "multi line"},
        {ATSPI_STATE_READ_ONLY, "read only"},
        {ATSPI_STATE_REQUIRED, "required"},
    };
    int first = 1;
    for (size_t i = 0; i < sizeof interesting / sizeof interesting[0]; i++) {
        if (atspi_state_set_contains(set, interesting[i].state)) {
            printf("%s%s", first ? " [" : ", ", interesting[i].word);
            first = 0;
        }
    }
    /* Said out loud rather than by omission, because "cannot be used" is a fact about a control and
     * not the absence of one. */
    if (!atspi_state_set_contains(set, ATSPI_STATE_ENABLED)) {
        printf("%s%s", first ? " [" : ", ", "disabled");
        first = 0;
    }
    if (!first) printf("]");
    g_object_unref(set);
}

static void walk(AtspiAccessible *node, int depth, const char *indent) {
    char *name = atspi_accessible_get_name(node, NULL);
    AtspiRole role = atspi_accessible_get_role(node, NULL);
    char *role_name = atspi_accessible_get_role_name(node, NULL);
    printf("%*s%s: \"%s\"", depth * 2, indent, role_name ? role_name : "?", name ? name : "");
    /* **The value, which is a different thing from the name.** A reader says the name to tell you
     * what a control is and the text to tell you what it holds, and a tree that published only the
     * first would announce "URL, entry" and stop. Read through `AtspiText`, which is where AT-SPI
     * keeps content. */
    AtspiText *text = atspi_accessible_get_text_iface(node);
    if (text != NULL) {
        gint n = atspi_text_get_character_count(text, NULL);
        if (n > 0) {
            char *content = atspi_text_get_text(text, 0, n, NULL);
            /* **The count is printed, not just the text.** A count in bytes rather than characters
             * returns the right string anyway -- `g_strndup` stops at the NUL -- so the only place
             * that error shows is the number itself, and a client that trusts it for iteration
             * walks off the end of the string. */
            if (content != NULL && content[0] != '\0') printf(" = \"%s\" (%d chars)", content, n);
            g_free(content);
        }
        g_object_unref(text);
    }
    print_states(node);
    printf("\n");
    g_free(name);
    g_free(role_name);
    (void)role;
    gint n = atspi_accessible_get_child_count(node, NULL);
    for (gint i = 0; i < n; i++) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(node, i, NULL);
        if (child != NULL) {
            walk(child, depth + 1, indent);
            g_object_unref(child);
        }
    }
}

int main(int argc, char **argv) {
    const char *wanted = argc > 1 ? argv[1] : "ziman probe";
    if (atspi_init() > 1) {
        fprintf(stderr, "dump: cannot reach the accessibility bus\n");
        return 2;
    }
    AtspiAccessible *desktop = atspi_get_desktop(0);
    gint n = atspi_accessible_get_child_count(desktop, NULL);
    int found = 0;
    for (gint i = 0; i < n; i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
        if (app == NULL) continue;
        char *name = atspi_accessible_get_name(app, NULL);
        if (name != NULL && strcmp(name, wanted) == 0) {
            found = 1;
            walk(app, 0, "");
        }
        g_free(name);
        g_object_unref(app);
    }
    if (!found) {
        fprintf(stderr, "dump: no application named \"%s\" on the bus (%d there)\n", wanted, n);
        return 1;
    }
    return 0;
}
