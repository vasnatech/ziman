/* Listens on the accessibility bus for announcements and prints what it hears.
 *
 * **The companion to atspi_dump.c, and it tests the other half.** That one walks the tree, which
 * is what a reader *pulls*; this one waits for what an application *pushes*. The distinction is
 * not academic: this bridge published a perfectly good tree for weeks while every event it emitted
 * died inside the process, because `AtkUtilClass.add_global_event_listener` was never implemented
 * and nothing pulled on that thread. A tree gate cannot see that. Only a listener can.
 *
 * Same principle either way: not "the program called the bridge" but "something else can hear it".
 * The publishing side marking its own homework proves nothing.
 *
 * Compiled on demand by `tests/run_a11y_test.sh`, as atspi_dump.c is, so libatspi stays a
 * dependency of running the test rather than of building ziman.
 *
 * Takes an application name to filter by (or "" for any) and how many seconds to listen. Exits
 * non-zero if it heard nothing. */
#include <atspi/atspi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int heard = 0;

static void on_event(AtspiEvent *e, void *user) {
    const char *want = (const char *)user;
    char *app = NULL;
    if (e->source != NULL) {
        AtspiAccessible *a = e->source;
        AtspiAccessible *top = g_object_ref(a);
        for (;;) {
            AtspiAccessible *p = atspi_accessible_get_parent(top, NULL);
            if (p == NULL) break;
            g_object_unref(top);
            top = p;
        }
        app = atspi_accessible_get_name(top, NULL);
        g_object_unref(top);
    }
    if (want == NULL || (app != NULL && strstr(app, want) != NULL)) {
        printf("%s announcement from [%s]: \"%s\"\n",
               e->detail1 == 2 ? "assertive" : "polite",
               app ? app : "?",
               e->any_data.g_type == G_TYPE_STRING ? g_value_get_string(&e->any_data) : "");
        fflush(stdout);
        heard++;
    }
    g_free(app);
}

int main(int argc, char **argv) {
    const char *want = argc > 1 ? argv[1] : NULL;
    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    if (atspi_init() > 1) { fprintf(stderr, "no accessibility bus\n"); return 2; }
    AtspiEventListener *l = atspi_event_listener_new(on_event, (void *)want, NULL);
    if (!atspi_event_listener_register(l, "object:announcement", NULL)) {
        fprintf(stderr, "could not register for object:announcement\n");
        return 2;
    }
    g_timeout_add_seconds(seconds, (GSourceFunc)atspi_event_quit, NULL);
    atspi_event_main();
    printf("heard %d\n", heard);
    return heard > 0 ? 0 : 1;
}
