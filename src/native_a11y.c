/* native_a11y.c -- publishing the accessibility tree over AT-SPI, so a screen reader can read a
 * window this interpreter drew.
 *
 * **`lib/ui/a11y.zm` builds a tree and, until this, nothing read it.** F275 split itself in two on
 * purpose: the expensive-to-retrofit half is the vocabulary -- a role and a state per widget,
 * declared where the widget is built -- and that half shipped first because fifty widgets without
 * it is the retrofit the ordering exists to avoid. This is the other half, and it is plumbing.
 *
 * **ATK and the bridge rather than D-Bus by hand.** `at-spi2-atk`'s bridge speaks the entire AT-SPI
 * protocol -- registration, the object interfaces, the event signals -- for an application that
 * hands it `AtkObject`s. Writing those interfaces directly would be a protocol implementation to
 * maintain against a spec that moves; this is a tree to keep in sync, which is the job we already
 * have. The route was proven with a standalone probe before any of it was wired in: a non-GTK
 * application that supplies its own root does appear on the bus, and an independent client reading
 * it sees the names and the roles.
 *
 * **Unix only, and that was the choice.** AccessKit's C API would have covered Windows too and
 * needs a Rust toolchain to build, where ziman needs a C compiler and nothing else. The user took
 * the narrower reach over the heavier build.
 *
 * Built only into `make gui`, like the window backend and for the same reason: an ordinary `ziman`
 * should not depend on glib, atk, or a session bus. Without the flag every entry point here
 * answers an error that says so. */
#define _POSIX_C_SOURCE 200809L
#include "native_a11y.h"

#include <stdint.h>
#include <string.h>

#include "error.h"
#include "native.h"
#include "value.h"
#include "xalloc.h"

#ifdef ZIMAN_ATSPI

#include <atk-bridge.h>
#include <atk/atk.h>
#include <glib.h>

/* One node of the published tree. The children are held here because **a parent must report them**:
 * `AtkObject`'s own `get_n_children` answers 0, so setting a child's parent and stopping leaves a
 * client seeing a lone application. That cost one round of the probe to find. */
typedef struct {
    AtkObject parent;
    GPtrArray *kids;
    char *description;
    char *value;
    int64_t states;
} ZimanNode;

/* The bits `lib/ui/desktop/atspi.zm` packs a node's state into. One int rather than eight booleans
 * crossing the boundary, because the alternative is eight parallel arrays for something a reader
 * consumes as a set. */
#define ZN_FOCUSED (1 << 0)
#define ZN_FOCUSABLE (1 << 1)
#define ZN_DISABLED (1 << 2)
#define ZN_SELECTED (1 << 3)
#define ZN_CHECKED (1 << 4)
#define ZN_MIXED (1 << 5)
#define ZN_EXPANDED (1 << 6)
#define ZN_EXPANDABLE (1 << 7)
#define ZN_READONLY (1 << 8)
#define ZN_REQUIRED (1 << 9)
#define ZN_INVALID (1 << 10)
#define ZN_MULTILINE (1 << 11)

typedef struct {
    AtkObjectClass parent;
} ZimanNodeClass;

static void ziman_text_iface_init(AtkTextIface *iface);

/* **`AtkText`, because a name is not a value.** A node's `value` -- what a field holds, what a
 * combo box has chosen -- crossed from Ziman and was stored and reached no client, because AT-SPI
 * exposes content through this interface rather than as part of the accessible name. Without it a
 * reader says "URL, entry" and stops, where a user needs to hear what is in it. */
G_DEFINE_TYPE_WITH_CODE(ZimanNode, ziman_node, ATK_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_TEXT, ziman_text_iface_init))

static gint ziman_node_n_children(AtkObject *o) {
    ZimanNode *n = (ZimanNode *)o;
    return n->kids == NULL ? 0 : (gint)n->kids->len;
}

static AtkObject *ziman_node_ref_child(AtkObject *o, gint i) {
    ZimanNode *n = (ZimanNode *)o;
    if (n->kids == NULL || i < 0 || (guint)i >= n->kids->len) return NULL;
    return g_object_ref(g_ptr_array_index(n->kids, i));
}

/* **What a reader checks before it says anything useful.** A tree of names and roles tells you what
 * is in a window; the states tell you which of it you are on, what is switched on, and what cannot
 * be used. Focus is the one that matters most -- a reader follows the focus, and without this it
 * has nothing to follow. */
static AtkStateSet *ziman_node_state_set(AtkObject *o) {
    ZimanNode *n = (ZimanNode *)o;
    AtkStateSet *set = atk_state_set_new();
    int64_t bits = n->states;
    /* Sensitive and enabled unless said otherwise: a reader treats their absence as "cannot be
     * used", so defaulting them off would make every control sound broken. */
    if (!(bits & ZN_DISABLED)) {
        atk_state_set_add_state(set, ATK_STATE_ENABLED);
        atk_state_set_add_state(set, ATK_STATE_SENSITIVE);
    }
    atk_state_set_add_state(set, ATK_STATE_VISIBLE);
    atk_state_set_add_state(set, ATK_STATE_SHOWING);
    if (bits & ZN_FOCUSABLE) atk_state_set_add_state(set, ATK_STATE_FOCUSABLE);
    if (bits & ZN_FOCUSED) atk_state_set_add_state(set, ATK_STATE_FOCUSED);
    if (bits & ZN_SELECTED) atk_state_set_add_state(set, ATK_STATE_SELECTED);
    if (bits & ZN_CHECKED) atk_state_set_add_state(set, ATK_STATE_CHECKED);
    /* A third state is its own thing and not "checked": a reader says "partially checked", which is
     * the distinction `widget.Toggle` exists to carry. */
    if (bits & ZN_MIXED) atk_state_set_add_state(set, ATK_STATE_INDETERMINATE);
    if (bits & ZN_EXPANDABLE) atk_state_set_add_state(set, ATK_STATE_EXPANDABLE);
    if (bits & ZN_EXPANDED) atk_state_set_add_state(set, ATK_STATE_EXPANDED);
    if (bits & ZN_READONLY) atk_state_set_add_state(set, ATK_STATE_READ_ONLY);
    else if (!(bits & ZN_DISABLED)) atk_state_set_add_state(set, ATK_STATE_EDITABLE);
    if (bits & ZN_REQUIRED) atk_state_set_add_state(set, ATK_STATE_REQUIRED);
    if (bits & ZN_INVALID) atk_state_set_add_state(set, ATK_STATE_INVALID_ENTRY);
    if (bits & ZN_MULTILINE) atk_state_set_add_state(set, ATK_STATE_MULTI_LINE);
    else atk_state_set_add_state(set, ATK_STATE_SINGLE_LINE);
    return set;
}

static const gchar *ziman_node_description(AtkObject *o) {
    ZimanNode *n = (ZimanNode *)o;
    return n->description == NULL ? "" : n->description;
}

static void ziman_node_init(ZimanNode *n) {
    n->kids = g_ptr_array_new();
    n->description = NULL;
    n->value = NULL;
    n->states = 0;
}

static void ziman_node_class_init(ZimanNodeClass *k) {
    ATK_OBJECT_CLASS(k)->get_n_children = ziman_node_n_children;
    ATK_OBJECT_CLASS(k)->ref_child = ziman_node_ref_child;
    ATK_OBJECT_CLASS(k)->get_description = ziman_node_description;
    ATK_OBJECT_CLASS(k)->ref_state_set = ziman_node_state_set;
}

/* The value as a counted string. **Characters, not bytes**: AT-SPI offsets are character offsets,
 * so a value with anything non-ASCII in it would otherwise be reported longer than it reads and a
 * client asking for the last character would be handed half of one. */
static const char *node_text(AtkText *text, glong *out_chars) {
    ZimanNode *n = (ZimanNode *)text;
    const char *s = n->value == NULL ? "" : n->value;
    if (out_chars != NULL) *out_chars = g_utf8_strlen(s, -1);
    return s;
}

static gchar *ziman_text_get_text(AtkText *text, gint start, gint end) {
    glong chars = 0;
    const char *s = node_text(text, &chars);
    if (start < 0) start = 0;
    if (end < 0 || end > chars) end = (gint)chars;
    if (start >= end) return g_strdup("");
    const char *from = g_utf8_offset_to_pointer(s, start);
    const char *to = g_utf8_offset_to_pointer(s, end);
    return g_strndup(from, (gsize)(to - from));
}

static gint ziman_text_get_character_count(AtkText *text) {
    glong chars = 0;
    node_text(text, &chars);
    return (gint)chars;
}

static gunichar ziman_text_get_character_at_offset(AtkText *text, gint offset) {
    glong chars = 0;
    const char *s = node_text(text, &chars);
    if (offset < 0 || offset >= chars) return 0;
    return g_utf8_get_char(g_utf8_offset_to_pointer(s, offset));
}

/* **The whole value, whatever granularity was asked for.** A reader asks for a word, a line or a
 * paragraph; this layer knows none of those -- `edit` does, and it is on the other side of the
 * boundary. Answering the whole string is honest and useful where answering nothing is neither,
 * and a reader that wanted a word gets a value it can split. */
static gchar *ziman_text_get_string_at_offset(AtkText *text, gint offset, AtkTextGranularity granularity,
                                              gint *start, gint *end) {
    (void)offset;
    (void)granularity;
    glong chars = 0;
    node_text(text, &chars);
    if (start != NULL) *start = 0;
    if (end != NULL) *end = (gint)chars;
    return ziman_text_get_text(text, 0, (gint)chars);
}

static void ziman_text_iface_init(AtkTextIface *iface) {
    iface->get_text = ziman_text_get_text;
    iface->get_character_count = ziman_text_get_character_count;
    iface->get_character_at_offset = ziman_text_get_character_at_offset;
    iface->get_string_at_offset = ziman_text_get_string_at_offset;
}

static AtkObject *g_root = NULL;
static int g_started = 0;

static AtkObject *ziman_get_root(void) { return g_root; }
static const gchar *ziman_toolkit_name(void) { return "ziman"; }
static const gchar *ziman_toolkit_version(void) { return ZIMAN_VERSION; }

/* The roles `lib/ui/a11y.zm` sends, in the order `atspi_role_of` there writes them. **Both sides of
 * this number are in one commit and must stay that way**: `widget.Role` has no default arm anywhere
 * it is switched on, so adding a variant is a compile error in Ziman that leads here. */
static AtkRole role_for(int64_t code) {
    switch (code) {
        case 1: return ATK_ROLE_PUSH_BUTTON;
        case 2: return ATK_ROLE_CHECK_BOX;
        case 3: return ATK_ROLE_RADIO_BUTTON;
        case 4: return ATK_ROLE_ENTRY;
        case 5: return ATK_ROLE_SLIDER;
        case 6: return ATK_ROLE_LINK;
        case 7: return ATK_ROLE_LIST;
        case 8: return ATK_ROLE_LIST_ITEM;
        case 9: return ATK_ROLE_PAGE_TAB;
        case 10: return ATK_ROLE_PAGE_TAB_LIST;
        case 11: return ATK_ROLE_MENU;
        case 12: return ATK_ROLE_MENU_ITEM;
        case 13: return ATK_ROLE_COMBO_BOX;
        case 14: return ATK_ROLE_SCROLL_BAR;
        case 15: return ATK_ROLE_DIALOG;
        case 16: return ATK_ROLE_PANEL;
        case 17: return ATK_ROLE_LABEL;
        case 18: return ATK_ROLE_FRAME;
        default: return ATK_ROLE_UNKNOWN;
    }
}

static Value a11y_err(const char *message) {
    Type ok = type_scalar(TY_UNIT);
    return value_result(type_ref(type_result(ok, type_scalar(TY_STRING))),
                        result_obj_new(0, value_string(ziman_xstrdup(message))));
}

static Value a11y_ok(void) {
    Type ok = type_scalar(TY_UNIT);
    return value_result(type_ref(type_result(ok, type_scalar(TY_STRING))),
                        result_obj_new(1, value_unit()));
}

static const char *item_cstr(ArrayObj *a, int i) {
    if (a == NULL || a->boxed_items == NULL || i >= a->count) return "";
    const char *s = value_cstr(a->boxed_items[i]);
    return s == NULL ? "" : s;
}

/* a11y.publish_raw(app, names, roles, descriptions, values, parents): the whole tree, replacing
 * whatever was published before.
 *
 * **Replacing rather than patching**, which is the same decision `a11y.zm` makes one layer up: the
 * tree is rebuilt from a laid-out widget tree every frame, so there is no identity to diff against
 * and a patch would need one. A published tree is small -- tens of nodes -- where the pixels it
 * describes are millions, so the cost is not where it looks.
 *
 * `parents` holds an index into these same arrays, or -1 for a node hanging off the application.
 * `a11y.Tree` emits parents before children, so a forward reference cannot happen. */
static Value native_a11y_publish_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *app = value_cstr(args[0]);
    ArrayObj *names = args[1].as.arr;
    ArrayObj *roles = args[2].as.arr;
    ArrayObj *descriptions = args[3].as.arr;
    ArrayObj *values = args[4].as.arr;
    ArrayObj *parents = args[5].as.arr;
    ArrayObj *states = args[6].as.arr;

    int count = names == NULL ? 0 : names->count;
    if (roles == NULL || roles->count != count || parents == NULL || parents->count != count) {
        return a11y_err("a11y: the published arrays must all be the same length");
    }
    if (roles->ints == NULL || parents->ints == NULL) {
        if (count > 0) return a11y_err("a11y: roles and parents must be arrays of int");
    }

    if (!g_started) {
        AtkUtilClass *util = g_type_class_ref(ATK_TYPE_UTIL);
        util->get_root = ziman_get_root;
        util->get_toolkit_name = ziman_toolkit_name;
        util->get_toolkit_version = ziman_toolkit_version;
        g_root = g_object_new(ziman_node_get_type(), NULL);
        atk_object_set_role(g_root, ATK_ROLE_APPLICATION);
        if (atk_bridge_adaptor_init(NULL, NULL) < 0) {
            ziman_error("runtime", line, col,
                        "a11y.publish: could not reach the accessibility bus (is at-spi2-registryd "
                        "running? try `gsettings set org.gnome.desktop.interface "
                        "toolkit-accessibility true`)");
        }
        g_started = 1;
    }
    atk_object_set_name(g_root, app == NULL ? "ziman" : app);

    /* The previous tree goes before the new one is built: each node holds a reference to its
     * children, so dropping the root's list is what releases them. */
    ZimanNode *root = (ZimanNode *)g_root;
    for (guint i = 0; i < root->kids->len; i++) {
        g_object_unref(g_ptr_array_index(root->kids, i));
    }
    g_ptr_array_set_size(root->kids, 0);

    AtkObject **made = count == 0 ? NULL : ziman_xcalloc((size_t)count, sizeof(AtkObject *));
    for (int i = 0; i < count; i++) {
        AtkObject *node = g_object_new(ziman_node_get_type(), NULL);
        atk_object_set_name(node, item_cstr(names, i));
        atk_object_set_role(node, role_for(roles->ints[i]));
        ZimanNode *z = (ZimanNode *)node;
        z->description = g_strdup(item_cstr(descriptions, i));
        z->value = g_strdup(item_cstr(values, i));
        z->states = (states != NULL && states->ints != NULL && i < states->count) ? states->ints[i] : 0;
        made[i] = node;
    }
    for (int i = 0; i < count; i++) {
        int64_t at = parents->ints[i];
        AtkObject *parent = (at >= 0 && at < count) ? made[at] : g_root;
        atk_object_set_parent(made[i], parent);
        g_ptr_array_add(((ZimanNode *)parent)->kids, made[i]);
    }
    free(made);
    return a11y_ok();
}

/* a11y.pump_raw(): lets the bridge answer whatever the bus has asked.
 *
 * **A window loop is not a glib main loop**, and the bridge dispatches on glib's. So the
 * application's loop lends it a turn each frame: non-blocking, so a frame costs nothing when
 * nothing is asking. Without this the tree is published and never answers a question. */
static Value native_a11y_pump_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    if (g_started) {
        int turns = 0;
        while (g_main_context_iteration(NULL, FALSE) && turns < 64) {
            turns++;
        }
    }
    return value_unit();
}

#else /* no ZIMAN_ATSPI */

static Value native_a11y_publish_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_UNIT);
    return value_result(type_ref(type_result(ok, type_scalar(TY_STRING))),
                        result_obj_new(0, value_string(ziman_xstrdup(
                            "this build of ziman has no accessibility backend (build with `make gui`)"))));
}

static Value native_a11y_pump_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_unit();
}

#endif

void native_a11y_register(void) {
    native_register("atspi::publish_raw", native_a11y_publish_raw);
    native_register("atspi::pump_raw", native_a11y_pump_raw);
}
