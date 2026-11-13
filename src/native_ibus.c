/* native_ibus.c -- an ibus input context, so that composing text reaches `lib/ui/edit.zm`.
 *
 * F274's first two instalments built the model and the field that draws one, deliberately before
 * any platform: every protocol -- XIM, ibus, SDL3, Windows TSF, macOS's NSTextInputClient --
 * delivers the same two things, a pre-edit string with a caret and a marked run inside it, and a
 * commit string that replaces it. This is the platform half, and the user chose ibus over XIM and
 * over taking SDL3 as a second backend.
 *
 * **D-Bus directly, where the accessibility binding went through ATK -- and the difference is
 * which side of the protocol we are on.** AT-SPI made us a *server*: a reader asks arbitrary
 * questions about a tree, over the whole of an interface specification, and answering them by hand
 * would have been a protocol to maintain. Here we are the *client*, and a client of ibus uses six
 * methods and five signals. `libibus` would wrap exactly those, in exchange for a dependency; gio
 * is already linked into `make gui`, because at-spi2-atk brings it. So this costs the build
 * nothing, which is the whole reason it is not a second backend.
 *
 * **The route was proven with a standalone probe before any of it was written**, as F275's was,
 * and it answered five questions that would each have been a puzzle later:
 *
 *   1. **The bus address is named for the Wayland display even when the client is X11.** The file
 *      is `$XDG_CONFIG_HOME/ibus/bus/<machine-id>-unix-wayland-0` in a Wayland session, and a
 *      client that built the name from `DISPLAY=:0` -- which is what ziman's X11 backend has
 *      under XWayland -- looks for a `-unix-0` that does not exist. `WAYLAND_DISPLAY` first,
 *      `DISPLAY` second, which is what libibus does and what nothing else would have told us.
 *   2. The private bus wants EXTERNAL authentication and a `Hello`, which is
 *      `G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | ..._MESSAGE_BUS_CONNECTION`.
 *   3. `ProcessKeyEvent` answers a bool, and the answer is the whole routing decision: true with a
 *      composing engine, false with a plain `xkb:us::eng` layout, where the application inserts
 *      the character itself exactly as it did before.
 *   4. **The indices are characters, not bytes.** A pre-edit of "かな" is 6 bytes and reports a
 *      run of [0,2). `edit.Preedit`'s caret and marked run are code-point indices and `len` in
 *      Ziman counts characters, so the two agree with no conversion -- which is luck worth
 *      recording, because the bug it would otherwise cause is a `substring` inside a character.
 *   5. **The marked run arrives as `IBUS_ATTR_TYPE_HINT`, not as an underline or a background.**
 *      Composing "watasinonamae" and pressing space gives "私の名前" with two attributes: hint
 *      value 1 over [0,4), the whole composition, and hint value 2 over [0,2) -- 私の alone, the
 *      segment being converted. That second span is `Preedit.highlight_start/end`. Guessing from
 *      the enum would have picked UNDERLINE and found nothing, and the serialization was confirmed
 *      against an IBusText built with known values rather than against documentation.
 *
 * Built only into `make gui`, like the window backend and the accessibility binding: an ordinary
 * `ziman` should not need a session bus, and without the flag every entry point here answers an
 * error that says so. */
#define _POSIX_C_SOURCE 200809L
#include "native_ibus.h"

#include <stdint.h>
#include <string.h>

#include "error.h"
#include "native.h"
#include "value.h"
#include "xalloc.h"

static const Type *ibus_result_type(void) {
    return type_ref(type_result(type_scalar(TY_UNIT), type_scalar(TY_STRING)));
}

static Value ibus_err(const char *message) {
    return value_result(ibus_result_type(), result_obj_new(0, value_string(ziman_xstrdup(message))));
}

#ifdef ZIMAN_IBUS

#include <gio/gio.h>

static Value ibus_ok(void) {
    return value_result(ibus_result_type(), result_obj_new(1, value_unit()));
}

/* The three event kinds `lib/ui/desktop/ibus.zm` decodes. A composition that closes is its own
 * kind rather than an update to the empty string, because those are different things to a field:
 * one replaces what is shown, the other says there is nothing being composed any more. */
enum { EV_COMPOSING = 1, EV_COMMITTED = 2, EV_CLOSED = 3 };

/* What ibus calls an attribute type. Only the hint matters here, and only its value 2: see the
 * fifth finding above. */
#define ATTR_TYPE_HINT 4
#define ATTR_HINT_MARKED 2

/* The capabilities we claim. Deliberately **not** `LOOKUP_TABLE`: claiming it means the
 * application draws the candidate list, which is a widget this layer does not have yet, and
 * leaving it unclaimed means ibus's own panel draws it where `SetCursorLocation` says. */
#define CAP_PREEDIT_TEXT 1
#define CAP_FOCUS 8

typedef struct {
    int kind;
    char *text;
    int64_t caret;
    int64_t start;
    int64_t end;
} Event;

static GDBusConnection *g_bus;
static char *g_ctx_path;
static Event *g_queue;
static int g_queued, g_capacity;
/* The events `pump_raw` handed over, held until the next pump so the three accessors below all
 * read one snapshot. Separate from `g_queue`, which the signal callbacks keep filling. */
static Event *g_staged;
static int g_stagedn;
/* Whether a composition is open, which is **not** something the protocol asks us to track but is
 * what makes the stream honest. ibus sends an invisible pre-edit when a context is created and
 * another after every commit, so a binding that forwarded each one reported a composition closing
 * before any had opened, and twice per committed word. A field acting on that abandons a
 * composition it does not have. */
static int g_composing;

static void queue_push(int kind, const char *text, int64_t caret, int64_t start, int64_t end) {
    if (g_queued + 1 > g_capacity) {
        g_capacity = g_capacity == 0 ? 8 : g_capacity * 2;
        g_queue = ziman_xrealloc(g_queue, (size_t)g_capacity * sizeof *g_queue);
    }
    g_queue[g_queued].kind = kind;
    g_queue[g_queued].text = g_strdup(text == NULL ? "" : text);
    g_queue[g_queued].caret = caret;
    g_queue[g_queued].start = start;
    g_queue[g_queued].end = end;
    g_queued++;
}

/* An IBusText is `(sa{sv}sv)`: the serialized name, attachments, the text, and an attribute list
 * which is `(sa{sv}av)` of IBusAttribute `(sa{sv}uuuu)` -- type, value, start, end. The field
 * order was confirmed by serializing an attribute with values chosen here and reading them back,
 * rather than from the specification. */
static const char *text_of(GVariant *v, GVariant **attrs_out) {
    GVariant *inner = v;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) inner = g_variant_get_variant(v);
    if (!g_variant_is_of_type(inner, G_VARIANT_TYPE("(sa{sv}sv)"))) return NULL;
    const char *text = NULL;
    g_variant_get(inner, "(&sa{sv}&sv)", NULL, NULL, &text, attrs_out);
    return text;
}

/* The marked run, as character indices into the pre-edit, or an empty run when nothing is marked.
 * Nothing marked is the ordinary case: a dead key, a compose sequence, and every Japanese
 * composition before the first conversion all send one hint over the whole string. */
static void marked_run(GVariant *attrs, int64_t *start, int64_t *end) {
    *start = 0;
    *end = 0;
    if (attrs == NULL) return;
    GVariant *a = attrs;
    if (g_variant_is_of_type(a, G_VARIANT_TYPE_VARIANT)) a = g_variant_get_variant(a);
    if (!g_variant_is_of_type(a, G_VARIANT_TYPE("(sa{sv}av)"))) return;
    GVariant *list = NULL;
    g_variant_get(a, "(sa{sv}@av)", NULL, NULL, &list);
    gsize n = g_variant_n_children(list);
    for (gsize i = 0; i < n; i++) {
        GVariant *boxed = g_variant_get_child_value(list, i);
        GVariant *attr = g_variant_get_variant(boxed);
        if (g_variant_is_of_type(attr, G_VARIANT_TYPE("(sa{sv}uuuu)"))) {
            guint32 type = 0, value = 0, from = 0, to = 0;
            g_variant_get(attr, "(sa{sv}uuuu)", NULL, NULL, &type, &value, &from, &to);
            if (type == ATTR_TYPE_HINT && value == ATTR_HINT_MARKED) {
                *start = (int64_t)from;
                *end = (int64_t)to;
            }
        }
        g_variant_unref(attr);
        g_variant_unref(boxed);
    }
    g_variant_unref(list);
}

static void on_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *signal, GVariant *params, gpointer user) {
    (void)c; (void)sender; (void)path; (void)iface; (void)user;
    if (strcmp(signal, "UpdatePreeditText") == 0 ||
        strcmp(signal, "UpdatePreeditTextWithMode") == 0) {
        GVariant *boxed = g_variant_get_child_value(params, 0);
        guint32 caret = 0;
        gboolean visible = FALSE;
        g_variant_get_child(params, 1, "u", &caret);
        g_variant_get_child(params, 2, "b", &visible);
        GVariant *attrs = NULL;
        const char *text = text_of(boxed, &attrs);
        if (!visible) {
            if (g_composing) {
                queue_push(EV_CLOSED, "", 0, 0, 0);
                g_composing = 0;
            }
        } else {
            int64_t start = 0, end = 0;
            marked_run(attrs, &start, &end);
            queue_push(EV_COMPOSING, text, (int64_t)caret, start, end);
            g_composing = 1;
        }
        if (attrs != NULL) g_variant_unref(attrs);
        g_variant_unref(boxed);
    } else if (strcmp(signal, "CommitText") == 0) {
        GVariant *boxed = g_variant_get_child_value(params, 0);
        GVariant *attrs = NULL;
        const char *text = text_of(boxed, &attrs);
        queue_push(EV_COMMITTED, text, 0, 0, 0);
        /* A commit closes the composition, so the invisible pre-edit ibus sends next is not
         * another event -- the field already stopped composing when it inserted the text. */
        g_composing = 0;
        if (attrs != NULL) g_variant_unref(attrs);
        g_variant_unref(boxed);
    } else if (strcmp(signal, "HidePreeditText") == 0) {
        if (g_composing) {
            queue_push(EV_CLOSED, "", 0, 0, 0);
            g_composing = 0;
        }
    }
    /* ShowPreeditText needs nothing: the UpdatePreeditText that follows carries the text, and a
     * show with no text to show would make a field clear what it is drawing and draw it again.
     * ForwardKeyEvent is deliberately dropped -- it is the method handing a key back, and we only
     * ever send it keys the application was going to process anyway, so the application still has
     * it. Taking it would insert the character twice. */
}

/* The address, found the way libibus finds it. See the first finding at the top of this file: the
 * Wayland display names the file even for an X11 client under XWayland. */
static char *bus_address(void) {
    const char *env = g_getenv("IBUS_ADDRESS");
    if (env != NULL && *env != '\0') return g_strdup(env);

    const char *wayland = g_getenv("WAYLAND_DISPLAY");
    char *display;
    if (wayland != NULL && *wayland != '\0') {
        display = g_strdup_printf("unix-%s", wayland);
    } else {
        const char *d = g_getenv("DISPLAY");
        if (d == NULL || *d == '\0') d = ":0";
        const char *colon = strchr(d, ':');
        char *number = g_strdup(colon != NULL ? colon + 1 : "0");
        char *dot = strchr(number, '.');
        if (dot != NULL) *dot = '\0';
        display = g_strdup_printf("unix-%s", number);
        g_free(number);
    }

    char *machine = NULL;
    if (!g_file_get_contents("/etc/machine-id", &machine, NULL, NULL)) {
        g_free(display);
        return NULL;
    }
    g_strstrip(machine);
    char *path = g_strdup_printf("%s/ibus/bus/%s-%s", g_get_user_config_dir(), machine, display);
    g_free(machine);
    g_free(display);

    char *body = NULL;
    gboolean read = g_file_get_contents(path, &body, NULL, NULL);
    g_free(path);
    if (!read) return NULL;

    char *address = NULL;
    char **lines = g_strsplit(body, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix(lines[i], "IBUS_ADDRESS=")) {
            address = g_strdup(lines[i] + strlen("IBUS_ADDRESS="));
        }
    }
    g_strfreev(lines);
    g_free(body);
    return address;
}

static void call_context(const char *method, GVariant *params) {
    if (g_bus == NULL || g_ctx_path == NULL) return;
    g_dbus_connection_call(g_bus, NULL, g_ctx_path, "org.freedesktop.IBus.InputContext", method,
                           params, NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
}

/* ibus.open_raw(client): connect, make a context, and start listening.
 *
 * Asynchronous for everything but the key, which is the shape the protocol wants: `FocusIn`,
 * `SetCursorLocation` and `Reset` are told-not-asked, so waiting for them would put a round trip
 * in a frame for an answer that is always the same. `ProcessKeyEvent` is the one call whose answer
 * decides what the application does next, so that one blocks. */
static Value native_ibus_open_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    if (g_bus != NULL) return ibus_ok();

    char *address = bus_address();
    if (address == NULL) {
        return ibus_err("ibus: no bus address (is ibus-daemon running? set IBUS_ADDRESS to point "
                        "at one, or start it with `ibus-daemon --daemonize`)");
    }

    GError *err = NULL;
    g_bus = g_dbus_connection_new_for_address_sync(
        address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    g_free(address);
    if (g_bus == NULL) {
        Value v = ibus_err(err->message);
        g_error_free(err);
        return v;
    }

    const char *client = value_cstr(args[0]);
    GVariant *made = g_dbus_connection_call_sync(
        g_bus, "org.freedesktop.IBus", "/org/freedesktop/IBus", "org.freedesktop.IBus",
        "CreateInputContext", g_variant_new("(s)", client == NULL ? "ziman" : client),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    if (made == NULL) {
        Value v = ibus_err(err->message);
        g_error_free(err);
        g_object_unref(g_bus);
        g_bus = NULL;
        return v;
    }
    g_variant_get(made, "(o)", &g_ctx_path);
    g_variant_unref(made);

    g_dbus_connection_signal_subscribe(g_bus, NULL, "org.freedesktop.IBus.InputContext", NULL,
                                       g_ctx_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_signal, NULL,
                                       NULL);
    call_context("SetCapabilities", g_variant_new("(u)", (guint32)(CAP_PREEDIT_TEXT | CAP_FOCUS)));
    return ibus_ok();
}

static Value native_ibus_close_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    if (g_bus != NULL) {
        call_context("Destroy", NULL);
        g_dbus_connection_close_sync(g_bus, NULL, NULL);
        g_object_unref(g_bus);
        g_bus = NULL;
    }
    g_free(g_ctx_path);
    g_ctx_path = NULL;
    g_composing = 0;
    return value_unit();
}

static Value native_ibus_focus_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    call_context(args[0].as.b ? "FocusIn" : "FocusOut", NULL);
    return value_unit();
}

/* ibus.key_raw(keyval, keycode, state) -> bool: did the input method take this key?
 *
 * **The one synchronous call**, because the answer is the routing decision and the application
 * cannot decide without it: true and the key belongs to the composition, false and the
 * application inserts the character exactly as it did before any of this existed.
 *
 * A timeout rather than no timeout: a wedged engine must not freeze a window, and a key the method
 * never answered for is a key the application should handle itself, which is what a failed call
 * answers below. */
static Value native_ibus_key_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    if (g_bus == NULL || g_ctx_path == NULL) return value_bool(0);
    GVariant *answer = g_dbus_connection_call_sync(
        g_bus, NULL, g_ctx_path, "org.freedesktop.IBus.InputContext", "ProcessKeyEvent",
        g_variant_new("(uuu)", (guint32)args[0].as.i, (guint32)args[1].as.i, (guint32)args[2].as.i),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 250, NULL, NULL);
    if (answer == NULL) return value_bool(0);
    gboolean handled = FALSE;
    g_variant_get(answer, "(b)", &handled);
    g_variant_unref(answer);
    return value_bool(handled ? 1 : 0);
}

/* Where the caret is on screen, so the candidate list appears under it rather than at the corner
 * of the display. Told, not asked: a window that moved tells it again. */
static Value native_ibus_where_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    call_context("SetCursorLocation",
                 g_variant_new("(iiii)", (gint32)args[0].as.i, (gint32)args[1].as.i,
                               (gint32)args[2].as.i, (gint32)args[3].as.i));
    return value_unit();
}

static Value native_ibus_reset_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    call_context("Reset", NULL);
    return value_unit();
}

/* ibus.pump_raw() -> int: lend glib a turn, and answer how many events it produced.
 *
 * The same bargain `atspi.pump_raw` makes, for the same reason -- a window loop is not a glib main
 * loop, and the signals arrive on glib's. It answers a count so that the three accessors below are
 * only called in the frames where something is being composed, which is almost none of them. */
static Value native_ibus_pump_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    for (int i = 0; i < g_stagedn; i++) g_free(g_staged[i].text);
    free(g_staged);
    g_staged = NULL;
    g_stagedn = 0;

    if (g_bus != NULL) {
        int turns = 0;
        while (g_main_context_iteration(NULL, FALSE) && turns < 64) turns++;
    }
    g_staged = g_queue;
    g_stagedn = g_queued;
    g_queue = NULL;
    g_queued = 0;
    g_capacity = 0;
    return value_int(g_stagedn);
}

static Value native_ibus_kinds_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    int64_t *kinds = ziman_xmalloc((size_t)(g_stagedn > 0 ? g_stagedn : 1) * sizeof *kinds);
    for (int i = 0; i < g_stagedn; i++) kinds[i] = g_staged[i].kind;
    return value_array(type_ref(type_array(type_scalar(TY_INT))),
                       array_obj_new_packed(PK_INT, kinds, g_stagedn));
}

static Value native_ibus_texts_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Value *items = ziman_xmalloc((size_t)(g_stagedn > 0 ? g_stagedn : 1) * sizeof *items);
    for (int i = 0; i < g_stagedn; i++) items[i] = value_string(ziman_xstrdup(g_staged[i].text));
    return value_array(type_ref(type_array(type_scalar(TY_STRING))),
                       array_obj_new(items, g_stagedn));
}

/* Three numbers per event -- caret, marked start, marked end -- in one array, the stride the
 * window backend's event queue already uses and for the same reason: the Ziman decoder is a loop
 * over threes rather than a parser. */
static Value native_ibus_numbers_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    int n = g_stagedn * 3;
    int64_t *out = ziman_xmalloc((size_t)(n > 0 ? n : 1) * sizeof *out);
    for (int i = 0; i < g_stagedn; i++) {
        out[i * 3] = g_staged[i].caret;
        out[i * 3 + 1] = g_staged[i].start;
        out[i * 3 + 2] = g_staged[i].end;
    }
    return value_array(type_ref(type_array(type_scalar(TY_INT))),
                       array_obj_new_packed(PK_INT, out, n));
}

#else /* no ZIMAN_IBUS */

static const char *no_backend =
    "this build of ziman has no input-method backend (build with `make gui`)";

static Value native_ibus_open_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return ibus_err(no_backend);
}

static Value native_ibus_close_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_unit();
}

static Value native_ibus_focus_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_unit();
}

static Value native_ibus_key_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_bool(0);
}

static Value native_ibus_where_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_unit();
}

static Value native_ibus_reset_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_unit();
}

static Value native_ibus_pump_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_int(0);
}

static Value native_ibus_kinds_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_array(type_ref(type_array(type_scalar(TY_INT))),
                       array_obj_new_packed(PK_INT, ziman_xmalloc(sizeof(int64_t)), 0));
}

static Value native_ibus_texts_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_array(type_ref(type_array(type_scalar(TY_STRING))),
                       array_obj_new(ziman_xmalloc(sizeof(Value)), 0));
}

static Value native_ibus_numbers_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_array(type_ref(type_array(type_scalar(TY_INT))),
                       array_obj_new_packed(PK_INT, ziman_xmalloc(sizeof(int64_t)), 0));
}

#endif

void native_ibus_register(void) {
    native_register("ibus::open_raw", native_ibus_open_raw);
    native_register("ibus::close_raw", native_ibus_close_raw);
    native_register("ibus::focus_raw", native_ibus_focus_raw);
    native_register("ibus::key_raw", native_ibus_key_raw);
    native_register("ibus::where_raw", native_ibus_where_raw);
    native_register("ibus::reset_raw", native_ibus_reset_raw);
    native_register("ibus::pump_raw", native_ibus_pump_raw);
    native_register("ibus::kinds_raw", native_ibus_kinds_raw);
    native_register("ibus::texts_raw", native_ibus_texts_raw);
    native_register("ibus::numbers_raw", native_ibus_numbers_raw);
}
