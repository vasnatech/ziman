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

static void walk(AtspiAccessible *node, int depth, const char *indent) {
    char *name = atspi_accessible_get_name(node, NULL);
    AtspiRole role = atspi_accessible_get_role(node, NULL);
    char *role_name = atspi_accessible_get_role_name(node, NULL);
    printf("%*s%s: \"%s\"\n", depth * 2, indent, role_name ? role_name : "?", name ? name : "");
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
