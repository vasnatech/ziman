/* The platform floor under a desktop GUI: a window the OS will show, a block of pixels to put in
 * it, and what the user did. docs/reviews/tasks/f269-f277.md F269.
 *
 * **Three decisions are visible in the shape of this file.**
 *
 * `present` takes `width * height * 4` bytes of RGBA and nothing else. It does not know whether
 * they came from `raster.render`, from F270's C renderer, or from a test, and that is the seam the
 * whole of route B hangs on -- one function wide, so the slow pipeline that works today and the
 * fast one that replaces it are the same pipeline.
 *
 * Events leave here as a flat array of ints, five per event, and `lib/core/window.zm` turns them
 * into an `Event`. That is `terminal`'s decision 3.2 again and for the same reason: a native cannot
 * build a user-declared enum, and everything a test would want to reach -- the decoding, the
 * key names, the modifier bits -- belongs where `ziman test` can reach it with no display attached.
 *
 * **There is no signal or atexit machinery here, unlike native_terminal.c, and that is deliberate
 * rather than an omission.** `terminal` must restore `termios` from a signal handler because those
 * settings outlive the process: a program that dies in raw mode leaves the user's shell broken.
 * A window does not outlive its connection -- when the process dies the X server drops it and the
 * window disappears -- so there is nothing to put back. The one thing that would outlive us is a
 * pointer or keyboard grab, and this module never grabs. `with_window` in the Ziman half is still
 * a scope rather than an open/close pair, because releasing deterministically while the program
 * runs is worth having; it is not guarding against a crash.
 *
 * X11 rather than SDL because it needs nothing installed that a Linux desktop does not already
 * have, and the backend seam means F274 can add SDL3 when input methods make it worth a
 * dependency.
 *
 * xalloc-exempt: the window's pixel buffer is allocated with libc's own malloc and checked for NULL
 * here, because `XDestroyImage` calls libc `free()` on it -- an XImage owns the data it was handed.
 * A buffer from the collector would be freed by the X library through the wrong allocator, which is
 * the one thing worse than checking NULL by hand. **`malloc` alone is not libc's malloc in this
 * build** -- see `libc_malloc` below, which is the correction to the version of this note that
 * thought it was. Every other allocation in this file uses xalloc as usual. */
#define _POSIX_C_SOURCE 200809L
#include "native_window.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "native.h"
#include "prelude.h"
#include "value.h"
#include "xalloc.h"

/* One slot per open window. A small fixed table rather than a growing one: a program with more
 * than this many windows open at once is not a case worth carrying allocation for, and a handle
 * that is an index into a fixed table cannot dangle -- it can only be stale, which `slot_of`
 * reports. */
#define WINDOW_MAX 16

/* Five ints per event, which is the widest any of them needs (`kind` plus four fields). A fixed
 * stride rather than a tagged length so the Ziman decoder is a loop over fives and not a parser. */
#define EVENT_FIELDS 5

/* How long a paste waits for the owner to answer, and the most text it will take in one go.
 * Both are judgements rather than protocol: see `native_window_clipboard_get_raw`. */
#define CLIPBOARD_WAIT_MS 500
#define CLIPBOARD_MAX (4u * 1024u * 1024u)
/* How many unrelated events a paste will hold on to before giving up on the paste
 * rather than on them: losing a keystroke is the worse of the two failures. */
#define CLIPBOARD_HELD_MAX 128

/* The appearance event's code, matching `ev_scheme` in lib/ui/desktop/window.zm. **13, and the only
 * code in this file that is not an X11 event** -- see the portal section below. */
#define EV_SCHEME 13

/* The event kinds, which lib/core/window.zm's decoder mirrors. Numbered rather than named across
 * the boundary because an int array is what crosses it; the names live in Ziman. */
enum {
    EV_KEY_DOWN = 1,  /* keysym, modifiers */
    EV_KEY_UP = 2,    /* keysym, modifiers */
    EV_CHAR = 3,      /* codepoint -- text the server composed, not a key */
    EV_POINTER_DOWN = 4, /* x, y, button */
    EV_POINTER_UP = 5,   /* x, y, button */
    EV_POINTER_MOVE = 6, /* x, y */
    EV_SCROLL = 7,       /* x, y, dx, dy */
    EV_RESIZE = 8,       /* width, height */
    EV_CLOSE = 9,
    EV_FOCUS_IN = 10,
    EV_FOCUS_OUT = 11,
    EV_EXPOSED = 12
};

/* The same two-line helper native_terminal.c has, and for the same reason: every native here
 * answers `Result<T, string>`, and spelling the type out at each return reads as if the shape
 * varied. */
static const Type *result_of(Type ok_type) {
    return type_ref(type_result(ok_type, type_scalar(TY_STRING)));
}

static Value window_err(Type ok, const char *message) {
    return value_result(result_of(ok), result_obj_new(0, value_string(ziman_xstrdup(message))));
}

/* Guarded, because without a backend every native above returns an error and nothing ever
 * succeeds -- so an unguarded helper is an unused-function warning in the default build. */
#ifdef ZIMAN_X11
static Value window_ok(Type ok, Value v) {
    return value_result(result_of(ok), result_obj_new(1, v));
}
#endif

#ifdef ZIMAN_X11

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/keysym.h>

typedef struct {
    int used;
    Window win;
    GC gc;
    XImage *image;      /* owns `pixels` below unless `shm_active`; recreated on resize */
    char *pixels;
    XShmSegmentInfo shm;
    int shm_active;     /* `pixels` is a shared-memory segment, not a malloc'd buffer */
    int image_width;
    int image_height;
    /* The back buffer: a server-side pixmap the size of the window, composed into and then copied
     * to the window in one batch per frame.
     *
     * **Why a pixmap and not the Double Buffer extension.** DBE's swap is defined over the whole
     * drawable, and this renderer paints *damage* -- a caret, a hovered row -- so it needs a back
     * buffer whose contents survive a frame. `XdbeCopied` promises that and is the swap action
     * servers implement least well; a pixmap promises it by being one. It also costs nothing on a
     * frame that changes nothing, because nothing is copied.
     *
     * Contents are undefined when it is created, which is correct here only because every path
     * that makes a new one also forces a full repaint: `desktop.painter_fitting` drops the
     * remembered scene on a resize, and the first frame of a window has no remembered scene at
     * all. A partial first frame would show whatever the server had in that memory. */
    Pixmap back;
    int back_width;
    int back_height;
    int width;
    int height;
    /* The cursor shape this window is currently showing, so that setting it again on every
     * pointer motion is free: X round-trips on `XDefineCursor`, and a window that called it sixty
     * times a second would be asking the server to do nothing, over and over, at the one moment
     * the user is dragging something. */
    int cursor_kind;
} Slot;

/* As many damage rectangles as one frame may present. `raster.damage_regions` suggests four
 * and `desktop.repaint` asks for four; the cap is here so the clipped bounds can live on the
 * stack, and anything past it is simply not drawn, which a caller would see at once. */
#define WINDOW_MAX_REGIONS 16

static Display *g_display = NULL;
static Atom g_wm_delete = None;
static Slot g_slots[WINDOW_MAX];

/* The visual's channel shifts, worked out once from its masks rather than assumed.
 *
 * **This is the part that would otherwise be a colour bug on somebody else's machine.** A 32-bit
 * TrueColor visual on a little-endian box is almost always 0x00RR0000/0x0000GG00/0x000000BB, which
 * means the bytes in memory are B, G, R, X -- so writing RGBA straight into an XImage shows blue
 * where red should be. Reading the masks and computing shifts gets the common case right and the
 * uncommon one right too, and costs one loop at startup. */
static int g_red_shift, g_green_shift, g_blue_shift;

/* Whether this display can take a shared-memory image: -1 not yet asked, 0 no, 1 yes.
 *
 * **Asked by trying, not by asking.** `XShmQueryExtension` answers whether the *server* has the
 * extension, which a server reached over the network also does -- and there the segment is in this
 * machine's memory, which that server cannot map. An attach to such a display fails, so the probe
 * is an attach: X errors arrive asynchronously, so it takes an error handler and an `XSync` to see
 * one, and that is the whole reason this is a probe rather than a predicate. */
static int g_shm_usable = -1;
static int g_shm_failed = 0;

static int shm_error_handler(Display *d, XErrorEvent *e) {
    (void)d;
    (void)e;
    g_shm_failed = 1;
    return 0;
}

static int shift_of(unsigned long mask) {
    int shift = 0;
    if (mask == 0) return 0;
    while ((mask & 1UL) == 0UL) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static const char *ensure_display(void) {
    if (g_display != NULL) return NULL;
    g_display = XOpenDisplay(NULL);
    if (g_display == NULL) {
        return "cannot open a display (is DISPLAY or WAYLAND_DISPLAY set, and an X server or "
               "XWayland running?)";
    }
    g_wm_delete = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    Visual *visual = DefaultVisual(g_display, DefaultScreen(g_display));
    g_red_shift = shift_of(visual->red_mask);
    g_green_shift = shift_of(visual->green_mask);
    g_blue_shift = shift_of(visual->blue_mask);
    return NULL;
}

/* The X cursor for each kind this layer names, created once and kept.
 *
 * **Created on demand and never freed**, which is right for a handful of shapes: `XCreateFontCursor`
 * allocates a server-side resource, and a program that made one per motion event would leak one per
 * motion event. There are five, they live as long as the display does, and the display outlives
 * every window that could be using them. */
#define CURSOR_KINDS 5
static Cursor g_cursors[CURSOR_KINDS];
static int g_cursor_made[CURSOR_KINDS];

/* The numbers in `X11/cursorfont.h`, in the order `window.Cursor` declares them. `XC_xterm` is the
 * I-beam, `XC_sb_h_double_arrow` the left-right arrows every toolkit shows over a column boundary,
 * and `XC_sb_v_double_arrow` its vertical twin. */
static unsigned int cursor_shape_for(int kind) {
    switch (kind) {
        case 1: return XC_sb_h_double_arrow;
        case 2: return XC_sb_v_double_arrow;
        case 3: return XC_xterm;
        case 4: return XC_hand2;
        default: return XC_left_ptr;
    }
}

static Cursor cursor_for(int kind) {
    if (kind < 0 || kind >= CURSOR_KINDS) kind = 0;
    if (!g_cursor_made[kind]) {
        g_cursors[kind] = XCreateFontCursor(g_display, cursor_shape_for(kind));
        g_cursor_made[kind] = 1;
    }
    return g_cursors[kind];
}

static Slot *slot_of(int64_t handle) {
    if (handle < 0 || handle >= WINDOW_MAX) return NULL;
    Slot *s = &g_slots[handle];
    return s->used ? s : NULL;
}

/* **The pixel buffer has to come from the allocator the X library will hand it back to.**
 * `XDestroyImage` calls libc `free()` on `image->data` -- that code is inside libX11 and so sees
 * nothing of src/zgc.h -- while in this build `malloc` *is* `GC_MALLOC`, because zgc.h is
 * force-included and `$(GC_FLAGS)` is part of the default `CFLAGS`. So the plain `malloc(bytes)`
 * this used to call gave the collector's memory to libc's free, and closing a window aborted the
 * process with `free(): invalid size`. Found by pressing a key at a real window (F269), not by
 * reading the code: the file's own header already said what the rule was.
 *
 * The idiom is src/iter.c:154 and src/native_http.c:15 -- suspended rather than undefined, so the
 * rest of the file still allocates through the collector like everything else. Keeping these pixels
 * outside the collector hides nothing from the mark phase: they are bytes, never pointers. */
#pragma push_macro("malloc")
#pragma push_macro("free")
#undef malloc
#undef free
static void *libc_malloc(size_t n) { return malloc(n); } /* xalloc-ok: libc's own malloc, reached on purpose; `ensure_image` checks the NULL */
static void libc_free(void *p) { free(p); }
#pragma pop_macro("free")
#pragma pop_macro("malloc")

static void free_image(Slot *s) {
    if (s->image == NULL) {
        s->shm_active = 0;
        return;
    }
    if (s->shm_active) {
        /* **`data` is cleared before `XDestroyImage` on purpose.** An XImage owns the buffer it was
         * handed and `XDestroyImage` releases it through `XFree`, which is libc's `free` -- and this
         * buffer came from `shmat`, so freeing it that way is exactly the allocator mismatch this
         * file's header is about, one allocator further out. The segment is released by `shmdt`,
         * after the image is gone and after the server has been told to detach. */
        XShmDetach(g_display, &s->shm);
        s->image->data = NULL;
        XDestroyImage(s->image);
        if (s->shm.shmaddr != NULL) shmdt(s->shm.shmaddr);
        s->shm.shmaddr = NULL;
        s->shm.shmid = -1;
        s->shm_active = 0;
    } else {
        /* XDestroyImage frees the data pointer too, with libc's free, which is why `pixels` comes
         * from `libc_malloc` and not from the collector. */
        XDestroyImage(s->image);
    }
    s->image = NULL;
    s->pixels = NULL;
    s->image_width = 0;
    s->image_height = 0;
}

/* The image as a shared-memory segment: 1 if `s` now has one, 0 to use the ordinary path.
 *
 * **Why this is worth the complexity.** `XPutImage` puts the pixels *in the request*, so a frame is
 * copied into the socket, through the kernel, and out again in the server -- 4 MB per frame at
 * 1280x800, measured at about a third of the cost of presenting one. A shared segment is mapped by
 * both processes, so `XShmPutImage` sends a few dozen bytes saying "draw what is already there".
 * The pixels are written exactly once, by the loop that converts them.
 *
 * Every failure here returns 0 and leaves the slot untouched, because the caller has a working path
 * to fall back to. Nothing about a missing extension, a full segment table or a remote display is
 * an error a Ziman program should see. */
static int try_shm_image(Slot *s, int width, int height, Visual *visual, int depth) {
    if (g_shm_usable == 0) return 0;
    /* **`ZIMAN_WINDOW_NO_SHM=1` forces the copying path.** Without it the fallback is unreachable on
     * any machine where shared memory works, which is every local display -- so the branch that
     * exists for remote displays and exhausted segment tables would never run here, and a defect in
     * it would wait for someone running over SSH. `tests/run_window_test.sh` presents through both.
     * Read once: a program does not change its mind about this mid-run. */
    if (g_shm_usable < 0) {
        const char *off = getenv("ZIMAN_WINDOW_NO_SHM");
        if (off != NULL && off[0] != '\0' && off[0] != '0') {
            g_shm_usable = 0;
            return 0;
        }
    }
    if (g_shm_usable < 0 && !XShmQueryExtension(g_display)) {
        g_shm_usable = 0;
        return 0;
    }

    /* **The segment info lives in the slot, and `XShmCreateImage` is handed that**, not a local.
     * This cost a `BadShmSeg` on the first present: `XShmCreateImage` keeps the pointer it is given
     * in the image's private `obdata`, and `XShmPutImage` reads the segment id back out of it every
     * frame -- so a local here is a dangling pointer into a dead stack frame the moment this
     * function returns, and the id the server is asked about is whatever is left on the stack. It
     * attaches fine and fails at the put, which is why the window gate caught it and the build did
     * not. A standalone probe of the same sequence *worked*, because there the struct was `main`'s
     * and outlived the call by accident. */
    memset(&s->shm, 0, sizeof s->shm);
    s->shm.shmid = -1;
    s->shm.shmaddr = NULL;
    s->shm.readOnly = False;

    /* Created with no data, so the server tells us the stride it wants before anything is sized. */
    XImage *image = XShmCreateImage(g_display, visual, (unsigned)depth, ZPixmap, NULL, &s->shm,
                                    (unsigned)width, (unsigned)height);
    if (image == NULL) return 0;
    if (image->bytes_per_line <= 0 || image->height <= 0) {
        XDestroyImage(image);
        return 0;
    }

    size_t bytes = (size_t)image->bytes_per_line * (size_t)image->height;
    s->shm.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (s->shm.shmid < 0) {
        XDestroyImage(image);
        return 0;
    }
    void *addr = shmat(s->shm.shmid, NULL, 0);
    if (addr == (void *)-1) {
        shmctl(s->shm.shmid, IPC_RMID, NULL);
        s->shm.shmid = -1;
        XDestroyImage(image);
        return 0;
    }
    s->shm.shmaddr = (char *)addr;
    image->data = s->shm.shmaddr;

    /* An attach failure is reported asynchronously, so the handler is installed and the sync forces
     * the answer before this function can claim success. */
    g_shm_failed = 0;
    int (*previous)(Display *, XErrorEvent *) = XSetErrorHandler(shm_error_handler);
    Bool attached = XShmAttach(g_display, &s->shm);
    XSync(g_display, False);
    XSetErrorHandler(previous);

    /* **Marked for destruction the moment it is attached**, which is standard and is about crashes
     * rather than tidiness: a System V segment outlives the process that made it, so a window that
     * died without detaching would leave 4 MB of kernel memory behind until a reboot. Removed-but-
     * attached keeps working for everyone already mapped and disappears when the last one detaches. */
    shmctl(s->shm.shmid, IPC_RMID, NULL);

    if (!attached || g_shm_failed) {
        shmdt(s->shm.shmaddr);
        s->shm.shmaddr = NULL;
        s->shm.shmid = -1;
        image->data = NULL;
        XDestroyImage(image);
        g_shm_usable = 0;
        return 0;
    }

    g_shm_usable = 1;
    s->image = image;
    s->pixels = s->shm.shmaddr;
    s->shm_active = 1;
    s->image_width = width;
    s->image_height = height;
    return 1;
}

static void put_into(Slot *s, Drawable into, int x, int y, int width, int height);
static void finish_frame(Slot *s);

/* The frame on screen, by whichever route this slot has.
 *
 * **The shared-memory route needs the sync and the copying route does not**, which is the one thing
 * about it that is easy to get wrong. `XPutImage` copies the pixels into the request, so the buffer
 * is the caller's again the instant it returns and `XFlush` is enough. With a shared segment the
 * server reads the buffer *we are still holding*, so presenting the next frame into it before the
 * server has finished with this one tears -- and `send_event False` asks for no completion event to
 * wait on. `XSync` is the wait: it returns once the server has processed the request, which costs a
 * round trip and saves copying 4 MB. One shared helper so the two `present` natives cannot disagree
 * about that. */
static void put_image(Slot *s, int x, int y, int width, int height) {
    /* The back buffer only when it is the size of the frame being presented. It always is, because
     * `ensure_image` makes them together -- and this costs two comparisons to make a door that
     * forgot to say so draw straight to the window, which is merely the old behaviour, rather than
     * into a pixmap of the wrong shape. */
    const int buffered = s->back != None && s->back_width == s->image_width &&
                         s->back_height == s->image_height;
    put_into(s, buffered ? s->back : s->win, x, y, width, height);
    if (buffered) {
        XCopyArea(g_display, s->back, s->win, s->gc, x, y, (unsigned)width, (unsigned)height, x, y);
    }
    finish_frame(s);
}

/* One rectangle of `s->image` onto a drawable -- the back buffer when there is one, the window
 * itself otherwise. */
static void put_into(Slot *s, Drawable into, int x, int y, int width, int height) {
    if (s->shm_active) {
        XShmPutImage(g_display, into, s->gc, s->image, x, y, x, y, (unsigned)width,
                     (unsigned)height, False);
        return;
    }
    XPutImage(g_display, into, s->gc, s->image, x, y, x, y, (unsigned)width, (unsigned)height);
}

/* **The one round trip a frame, and why it cannot be dropped.** With a shared segment the server
 * reads the buffer this process is still holding, so packing the next frame into it before the
 * server has finished tears -- and `send_event False` asks for no completion event to wait on.
 * `XSync` is the wait. Without the segment `XPutImage` has already copied the pixels into the
 * request, so a flush is enough.
 *
 * It is once per *frame* rather than once per rectangle, which is what composing into a back
 * buffer makes possible: a frame that changed four distant places used to cost four round trips
 * and showed its four rectangles one at a time. */
static void finish_frame(Slot *s) {
    if (s->shm_active) {
        XSync(g_display, False);
        return;
    }
    XFlush(g_display);
}

/* The back buffer at this size, made again when the window changes shape. */
static const char *ensure_back(Slot *s, int width, int height) {
    if (s->back != None && s->back_width == width && s->back_height == height) return NULL;
    if (s->back != None) {
        XFreePixmap(g_display, s->back);
        s->back = None;
        s->back_width = 0;
        s->back_height = 0;
    }
    int screen = DefaultScreen(g_display);
    Pixmap made = XCreatePixmap(g_display, s->win, (unsigned)width, (unsigned)height,
                                (unsigned)DefaultDepth(g_display, screen));
    if (made == None) return "cannot create the window's back buffer";
    s->back = made;
    s->back_width = width;
    s->back_height = height;
    return NULL;
}

static const char *ensure_image(Slot *s, int width, int height) {
    /* **The back buffer is made here and nowhere else**, because it is the same size as the image
     * and is wanted whenever the image is. It was `present_premul_raw`'s job for an afternoon, and
     * `present_raw` -- the bytes door, which calls this and did not call that -- would then have
     * drawn a resized frame into a pixmap still the old size: a window that presented floats,
     * resized, and then presented bytes would have shown the wrong pixels with nothing to say so.
     * A rule kept in two places is kept in neither. */
    const char *failed = ensure_back(s, width, height);
    if (failed != NULL) return failed;
    if (s->image != NULL && s->image_width == width && s->image_height == height) return NULL;
    free_image(s);
    int screen = DefaultScreen(g_display);
    Visual *visual = DefaultVisual(g_display, screen);
    int depth = DefaultDepth(g_display, screen);
    if (try_shm_image(s, width, height, visual, depth)) return NULL;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    char *data = libc_malloc(bytes); /* xalloc-ok: NULL checked below; XDestroyImage frees it */
    if (data == NULL) return "out of memory for the window's pixel buffer";
    memset(data, 0, bytes);
    XImage *image = XCreateImage(g_display, visual, (unsigned)depth, ZPixmap, 0, data,
                                 (unsigned)width, (unsigned)height, 32, width * 4);
    if (image == NULL) {
        libc_free(data);
        return "cannot create an image for the window (unsupported visual?)";
    }
    s->image = image;
    s->pixels = data;
    s->image_width = width;
    s->image_height = height;
    return NULL;
}

/* Appends one event's five fields, growing `out`. Returns the (possibly moved) buffer.
 *
 * A function because `poll_raw` appends from two places -- an X KeyPress produces both a `KeyDown`
 * and the `Char` it composed -- and the second copy of this arithmetic is what put them in the
 * wrong order. */
#ifdef ZIMAN_PORTAL
#include <gio/gio.h>

/* The desktop's appearance preference, read from the freedesktop portal.
 *
 * **GIO is already linked here and that is the whole reason this is cheap.** `src/native_ibus.c`
 * has been calling `g_dbus_connection_call_sync` and `g_dbus_connection_signal_subscribe` since it
 * was written, and the Makefile says why: "it costs nothing, because at-spi2-atk already brought
 * it." This is the same three calls against a different service.
 *
 * **It must never hang.** A desktop that does not run the portal, or runs one that is wedged, is an
 * ordinary machine -- so every call has a short timeout and a bus that fails once is remembered as
 * absent rather than retried sixty times a second. A frame budget is 16ms; a D-Bus round trip that
 * blocks for a second has already ruined the frame it was called from. */
static GDBusConnection *g_portal = NULL;
static int g_portal_tried = 0;   /* asked for the bus at least once */
static int g_portal_absent = 0;  /* asked, and there is none: do not ask again */
static int g_scheme = 0;         /* last value read, in the portal's own numbering */
static int g_scheme_fresh = 0;   /* a change arrived that nobody has been told about yet */

static int scheme_from_variant(GVariant *v) {
    if (v == NULL) return 0;
    /* `ReadOne` answers `v` and `Read` answers `(v)` wrapping another `v`, so the value arrives
     * inside one or two layers of variant depending on which method answered. Unwrapping until it
     * is no longer a variant handles both without the caller knowing which it called. */
    GVariant *inner = g_variant_ref(v);
    while (g_variant_is_of_type(inner, G_VARIANT_TYPE_VARIANT)) {
        GVariant *next = g_variant_get_variant(inner);
        g_variant_unref(inner);
        inner = next;
    }
    int out = 0;
    if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32)) {
        guint32 raw = g_variant_get_uint32(inner);
        /* **Anything unrecognised is 0**, which is what the spec asks for: a portal that one day
         * answers 3 reads as "no preference" rather than as an error or as dark. */
        if (raw == 1 || raw == 2) out = (int)raw;
    }
    g_variant_unref(inner);
    return out;
}

static void on_setting_changed(GDBusConnection *c, const char *sender, const char *path,
                               const char *iface, const char *signal, GVariant *params,
                               gpointer user) {
    (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)user;
    const char *space = NULL;
    const char *key = NULL;
    GVariant *value = NULL;
    g_variant_get(params, "(&s&sv)", &space, &key, &value);
    if (space != NULL && key != NULL &&
        strcmp(space, "org.freedesktop.appearance") == 0 && strcmp(key, "color-scheme") == 0) {
        int now = scheme_from_variant(value);
        if (now != g_scheme) {
            g_scheme = now;
            g_scheme_fresh = 1;
        }
    }
    if (value != NULL) g_variant_unref(value);
}

/* Reads the setting once, over whichever method this portal has.
 *
 * **`ReadOne` is current and `Read` is deprecated, and the fallback is required rather than
 * tidy**: older portals answer `ReadOne` with "No such method", which is an error and not an empty
 * answer, so a client that only calls the new one gets nothing from them. Ghostty hit exactly this
 * and it is the kind of thing that works on the machine it was written on. */
static int portal_read_now(void) {
    GError *err = NULL;
    GVariant *answer = g_dbus_connection_call_sync(
        g_portal, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "ReadOne",
        g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
        NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &err);
    if (answer == NULL) {
        g_clear_error(&err);
        answer = g_dbus_connection_call_sync(
            g_portal, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Settings", "Read",
            g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
            NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &err);
    }
    if (answer == NULL) {
        g_clear_error(&err);
        return 0;
    }
    GVariant *first = g_variant_n_children(answer) > 0 ? g_variant_get_child_value(answer, 0) : NULL;
    int out = scheme_from_variant(first);
    if (first != NULL) g_variant_unref(first);
    g_variant_unref(answer);
    return out;
}

/* Opens the session bus once, reads the setting, and subscribes to changes.
 *
 * **Lazily, and remembered either way.** A machine with no portal should pay for one failed
 * connection and never think about it again -- `g_portal_absent` is what stops this being asked
 * again on the next frame, which is the difference between a feature and a stall. */
static void portal_open(void) {
    if (g_portal_tried) return;
    g_portal_tried = 1;
    GError *err = NULL;
    g_portal = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (g_portal == NULL) {
        g_clear_error(&err);
        g_portal_absent = 1;
        return;
    }
    g_scheme = portal_read_now();
    /* **Some desktops historically never emitted this** -- a KDE bug fixed around Plasma 5.24 --
     * which is why the value read above stands on its own and a change is an improvement on it
     * rather than the only way to learn anything. A window on such a desktop shows what the setting
     * was when it opened, which is what it would have done anyway. */
    g_dbus_connection_signal_subscribe(
        g_portal, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings",
        "SettingChanged", "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_setting_changed, NULL, NULL);
}

/* Drains glib's loop so a subscription can fire, and says whether the value moved.
 *
 * **The signal arrives on glib's loop while the window runs X11's**, which is the same situation
 * `ibus.pump_raw` is in and the reason that function iterates the context by hand. Bounded, and
 * non-blocking, so an idle window pays a handful of empty iterations. */
static int portal_poll_changed(void) {
    if (g_portal_absent || g_portal == NULL) return 0;
    int turns = 0;
    while (g_main_context_iteration(NULL, FALSE) && turns < 64) turns++;
    if (!g_scheme_fresh) return 0;
    g_scheme_fresh = 0;
    return 1;
}
#endif /* ZIMAN_PORTAL */

/* ------------------------------------------------------------------ the clipboard
 *
 * X11 has no clipboard. It has *selections*: a name, an owner window, and a protocol by which a
 * would-be reader asks the owner to convert its data into a format and write it onto a property of
 * the reader's own window. Nothing is stored anywhere central, which is why a selection dies with
 * the program that owned it, and why this file -- not `lib/ui/controls.zm` -- is where the work
 * belongs. F273 built the seam for exactly this reason: `Input.Paste` carries text in and
 * `EntryStep.clipboard` carries text out, so a text field never learns what a selection is.
 */
#ifdef ZIMAN_X11
static Atom g_a_clipboard = None;
static Atom g_a_utf8 = None;
static Atom g_a_targets = None;
static Atom g_a_text = None;
static Atom g_a_prop = None;

/* What we have put on the clipboard, or NULL when we own nothing.
 *
 * Allocated through the collector and held in a static, which Boehm scans, so this is a root and
 * the block cannot be collected while we are the owner. Dropping it is an assignment: `free` is a
 * no-op in the default build, and the pointer is the collector's rather than libc's. */
static char *g_clip_text = NULL;

static void ensure_clip_atoms(void) {
    if (g_a_clipboard != None) return;
    g_a_clipboard = XInternAtom(g_display, "CLIPBOARD", False);
    g_a_utf8 = XInternAtom(g_display, "UTF8_STRING", False);
    g_a_targets = XInternAtom(g_display, "TARGETS", False);
    g_a_text = XInternAtom(g_display, "TEXT", False);
    /* Our own property, which a conversion is asked to land on. Named rather than `XA_STRING` so
     * two ziman windows talking to each other cannot collide on a well-known one. */
    g_a_prop = XInternAtom(g_display, "ZIMAN_SELECTION", False);
}

/* Answer somebody else's request for what we own.
 *
 * **TARGETS has to be answered or well-behaved readers never ask for the text.** GTK and Qt both
 * ask what formats are on offer before asking for one; a program that answers only UTF8_STRING
 * looks, to them, like a selection holding nothing. */
static void answer_selection_request(XSelectionRequestEvent *rq) {
    XSelectionEvent said;
    said.type = SelectionNotify;
    said.display = rq->display;
    said.requestor = rq->requestor;
    said.selection = rq->selection;
    said.target = rq->target;
    said.time = rq->time;
    /* `None` is the refusal: the protocol's way of saying "not in that format". */
    said.property = None;

    if (g_clip_text != NULL && rq->property != None) {
        if (rq->target == g_a_targets) {
            Atom offered[3];
            offered[0] = g_a_targets;
            offered[1] = g_a_utf8;
            offered[2] = g_a_text;
            XChangeProperty(g_display, rq->requestor, rq->property, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)offered, 3);
            said.property = rq->property;
        } else if (rq->target == g_a_utf8 || rq->target == g_a_text || rq->target == XA_STRING) {
            XChangeProperty(g_display, rq->requestor, rq->property, rq->target, 8,
                            PropModeReplace, (const unsigned char *)g_clip_text,
                            (int)strlen(g_clip_text));
            said.property = rq->property;
        }
    }
    XSendEvent(g_display, rq->requestor, False, 0, (XEvent *)&said);
    XFlush(g_display);
}

/* Put text on the clipboard: take the selection and remember what we promised.
 *
 * Nothing is copied anywhere. We are now a server, and the text goes out one request at a time
 * from `answer_selection_request` -- which is why a program that exits has taken the clipboard
 * with it, on X11, with or without this code. */
static Value native_window_clipboard_put_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_UNIT);
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    ensure_clip_atoms();
    const char *text = value_cstr(args[1]);
    g_clip_text = ziman_xstrdup(text == NULL ? "" : text);
    XSetSelectionOwner(g_display, g_a_clipboard, s->win, CurrentTime);
    if (XGetSelectionOwner(g_display, g_a_clipboard) != s->win) {
        g_clip_text = NULL;
        return window_err(ok, "the X server would not give us the clipboard");
    }
    XFlush(g_display);
    return window_ok(ok, value_unit());
}

/* Read the clipboard: ask the owner to write it onto a property of ours, and wait.
 *
 * **If we are the owner, answer from our own copy and do not ask.** The reply to a conversion
 * request comes from the owner's event loop -- and when the owner is us, that loop is this
 * function, which is blocked waiting for the reply it is itself supposed to send. Asking politely
 * would hang the window until the timeout, every time a user copied and pasted inside one program,
 * which is the common case rather than an edge one.
 *
 * **While waiting, keep answering requests.** Two ziman windows pasting from each other deadlock
 * otherwise, each blocked waiting on the other's loop -- which is precisely the two-window case
 * this task is finished by. */
/* Put back what we held while waiting, in the order it arrived.
 *
 * Backwards, because `XPutBackEvent` pushes onto the head: putting them back in order would hand
 * the next `poll` a reversed keyboard. */
static void give_back(XEvent *held, int count) {
    for (int i = count - 1; i >= 0; i--) XPutBackEvent(g_display, &held[i]);
}

static Value native_window_clipboard_get_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_STRING);
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    ensure_clip_atoms();

    if (XGetSelectionOwner(g_display, g_a_clipboard) == s->win) {
        return window_ok(ok, value_string(ziman_xstrdup(g_clip_text == NULL ? "" : g_clip_text)));
    }

    XConvertSelection(g_display, g_a_clipboard, g_a_utf8, g_a_prop, s->win, CurrentTime);
    XFlush(g_display);

    /* **A bounded wait and not an endless one.** The owner may be a program that has stopped
     * answering, and a text field that froze the window because another application was wedged
     * would be a worse bug than an empty paste. */
    /* Events that arrived while we waited and are nothing to do with the clipboard. */
    XEvent held[CLIPBOARD_HELD_MAX];
    int held_count = 0;

    int waited_ms = 0;
    while (waited_ms < CLIPBOARD_WAIT_MS) {
        while (XPending(g_display) > 0) {
            XEvent e;
            XNextEvent(g_display, &e);
            if (e.type == SelectionRequest) {
                answer_selection_request(&e.xselectionrequest);
                continue;
            }
            if (e.type == SelectionClear) {
                g_clip_text = NULL;
                continue;
            }
            if (e.type == SelectionNotify && e.xselection.requestor == s->win) {
                if (e.xselection.property == None) {
                    /* The owner holds nothing it can give us as text. */
                    give_back(held, held_count);
                    return window_ok(ok, value_string(ziman_xstrdup("")));
                }
                Atom kind = None;
                int format = 0;
                unsigned long items = 0, left = 0;
                unsigned char *data = NULL;
                if (XGetWindowProperty(g_display, s->win, g_a_prop, 0, (long)(CLIPBOARD_MAX / 4),
                                       True, AnyPropertyType, &kind, &format, &items, &left,
                                       &data) != Success || data == NULL) {
                    give_back(held, held_count);
                    return window_ok(ok, value_string(ziman_xstrdup("")));
                }
                /* Copied out before `XFree`, and copied through the collector: the block X gave us
                 * is libc's and must go back to X, while the string we return is the collector's.
                 * Handing either one to the other's allocator is the heap corruption
                 * `tools/check_libc_free.py` exists to catch. */
                size_t n = (size_t)items * (size_t)(format / 8);
                char *out = ziman_xmalloc(n + 1);
                memcpy(out, data, n);
                out[n] = '\0';
                XFree(data);
                give_back(held, held_count);
                return window_ok(ok, value_string(out));
            }
            /* Anything else that arrived while we waited is a real event -- a key, a resize --
             * and dropping it would lose a keystroke, so it is held and given back below.
             *
             * **It cannot be pushed back here.** `XPutBackEvent` puts an event at the *head* of
             * the queue, so an `Expose` sitting in front of the reply would be read, put back,
             * read again, and the answer behind it never reached: a paste that spins for the whole
             * timeout and comes back empty. The first paste in a window works, because the queue
             * is empty; the second one does not, which is what made this worth two programs and a
             * second read in the test rather than one of each. */
            if (held_count >= CLIPBOARD_HELD_MAX) {
                give_back(held, held_count);
                return window_ok(ok, value_string(ziman_xstrdup("")));
            }
            held[held_count++] = e;
        }
        struct timespec nap;
        nap.tv_sec = 0;
        nap.tv_nsec = 2 * 1000 * 1000;
        nanosleep(&nap, NULL);
        waited_ms += 2;
    }
    give_back(held, held_count);
    return window_ok(ok, value_string(ziman_xstrdup("")));
}
#endif /* ZIMAN_X11 */

static int64_t *push_event(int64_t *out, int *count, int *capacity, const int64_t *fields) {
    if (*count + 1 > *capacity) {
        *capacity = *capacity == 0 ? 32 : *capacity * 2;
        out = ziman_xrealloc(out, (size_t)*capacity * EVENT_FIELDS * sizeof *out);
    }
    memcpy(out + (size_t)*count * EVENT_FIELDS, fields, (size_t)EVENT_FIELDS * sizeof *out);
    (*count)++;
    return out;
}

#endif /* ZIMAN_X11 */

/* window.open_raw(title, width, height) -> Result<int, string>: a mapped window, and the handle
 * everything else takes. */
static Value native_window_open_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_INT);
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    const char *failed = ensure_display();
    if (failed != NULL) return window_err(ok, failed);

    int64_t width = args[1].as.i;
    int64_t height = args[2].as.i;
    if (width < 1 || height < 1 || width > 16384 || height > 16384) {
        return window_err(ok, "a window's width and height must each be between 1 and 16384");
    }

    int index = -1;
    for (int i = 0; i < WINDOW_MAX; i++) {
        if (!g_slots[i].used) { index = i; break; }
    }
    if (index < 0) return window_err(ok, "too many windows are open at once");

    int screen = DefaultScreen(g_display);
    Window win = XCreateSimpleWindow(g_display, RootWindow(g_display, screen), 0, 0,
                                     (unsigned)width, (unsigned)height, 0,
                                     BlackPixel(g_display, screen), BlackPixel(g_display, screen));
    if (win == 0) return window_err(ok, "cannot create a window");

    /* Everything the Ziman half can report, asked for up front: a window that selected the wrong
     * events reports an empty queue and looks like a hung program. */
    XSelectInput(g_display, win,
                 KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask | FocusChangeMask | ExposureMask);
    /* Without this the window manager closes the connection when the user clicks the close box,
     * which kills the process instead of delivering a Close the program can refuse. */
    XSetWMProtocols(g_display, win, &g_wm_delete, 1);

    /* The title is Ziman text, which is not NUL-terminated, so it is copied rather than pointed at. */
    size_t title_len = args[0].as.str.len;
    char *title = ziman_xmalloc(title_len + 1);
    memcpy(title, args[0].as.str.ptr, title_len);
    title[title_len] = '\0';
    XStoreName(g_display, win, title);
    free(title);

    XMapWindow(g_display, win);
    XFlush(g_display);

    g_slots[index].used = 1;
    g_slots[index].win = win;
    g_slots[index].gc = XCreateGC(g_display, win, 0, NULL);
    g_slots[index].image = NULL;
    g_slots[index].pixels = NULL;
    g_slots[index].image_width = 0;
    g_slots[index].image_height = 0;
    g_slots[index].width = (int)width;
    g_slots[index].height = (int)height;
    return window_ok(ok, value_int((int64_t)index));
#endif
}

/* window.present_raw(handle, pixels, width, height) -> Result<unit, string>: RGBA onto the window.
 *
 * The bytes are RGBA in that order, one byte each, row-major, with no padding -- the same layout
 * `image.Pixels.Rgba8` already has, so a `raster.render` result needs no conversion in Ziman. */
static Value native_window_present_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_UNIT);
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    int64_t width = args[2].as.i;
    int64_t height = args[3].as.i;
    if (width < 1 || height < 1) return window_err(ok, "the pixel buffer's width and height must be positive");

    int n = 0;
    uint8_t *rgba = array_copy_bytes(args[1].as.arr, &n);
    size_t needed = (size_t)width * (size_t)height * 4u;
    if ((size_t)n != needed) {
        free(rgba);
        char message[160];
        snprintf(message, sizeof message,
                 "the pixel buffer is %d bytes but %lldx%lld RGBA needs %zu", n,
                 (long long)width, (long long)height, needed);
        return window_err(ok, message);
    }

    const char *failed = ensure_image(s, (int)width, (int)height);
    if (failed != NULL) {
        free(rgba);
        return window_err(ok, failed);
    }

    /* RGBA to the visual's own packing. Alpha is dropped rather than blended: a window has no
     * backdrop to blend against, and compositing belongs to whatever produced these pixels. */
    uint32_t *out = (uint32_t *)s->pixels;
    size_t pixels = (size_t)width * (size_t)height;
    for (size_t i = 0; i < pixels; i++) {
        uint32_t r = rgba[i * 4 + 0];
        uint32_t g = rgba[i * 4 + 1];
        uint32_t b = rgba[i * 4 + 2];
        out[i] = (r << g_red_shift) | (g << g_green_shift) | (b << g_blue_shift);
    }
    free(rgba);

    put_image(s, 0, 0, (int)width, (int)height);
    return window_ok(ok, value_unit());
#endif
}

/* window.poll_raw(handle) -> Result<[int], string>: every event waiting, five ints each, oldest
 * first. Never blocks: an empty array means nothing happened, which is what a render loop with a
 * timeout needs. */
static Value native_window_poll_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_array(type_scalar(TY_INT));
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");

    int64_t *out = NULL;
    int count = 0;
    int capacity = 0;
    while (XPending(g_display) > 0) {
        XEvent e;
        XNextEvent(g_display, &e);
        int64_t fields[EVENT_FIELDS] = { 0, 0, 0, 0, 0 };
        int have = 0;
        /* A second event out of the same X event: the text a key composed. Held rather than
         * appended where it is computed, because **it has to follow the `KeyDown`, not precede
         * it.** It used to precede it -- the text arrived before the keystroke that produced it,
         * which breaks any program that decides from the `KeyDown` whether to consume the
         * character, and which no unit test over `decode_events` could see, because the order is
         * the native's and the decoder only maps what it is handed. */
        int64_t extra[EVENT_FIELDS] = { 0, 0, 0, 0, 0 };
        int have_extra = 0;

        switch (e.type) {
        case KeyPress:
        case KeyRelease: {
            KeySym sym = XLookupKeysym(&e.xkey, 0);
            fields[0] = e.type == KeyPress ? EV_KEY_DOWN : EV_KEY_UP;
            fields[1] = (int64_t)sym;
            fields[2] = (int64_t)(e.xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask));
            /* The hardware keycode as well as the keysym, in a field the five-wide stride already
             * had spare. An input method is defined to take both -- see src/native_ibus.c -- and a
             * binding that passed a fabricated zero for it would be guessing on the application's
             * behalf. Nothing else reads it. */
            fields[3] = (int64_t)e.xkey.keycode;
            have = 1;
            if (e.type == KeyPress) {
                /* The text the key produced, as well as the key itself. Both, because a shortcut
                 * wants the key and a text field wants the character, and deriving either from the
                 * other is what makes keyboard layouts go wrong.
                 *
                 * **The field carries a code point, not a byte.** XLookupString answers ISO
                 * Latin-1, whose values are Unicode U+0000..U+00FF at the same numbers, so the
                 * byte it gives *is* the code point and no conversion belongs here. The Ziman
                 * half encodes it as UTF-8. It read this as a byte until 2026-11-13, which made
                 * `Event.Char` ASCII only: anything above 127 failed the UTF-8 check on the way
                 * in and the event was dropped, so an accented letter did nothing at all.
                 *
                 * Only the first character is taken. A keysym mapping to several Latin-1
                 * characters at once does not occur in the standard table, and composition --
                 * the thing that really produces more than one -- goes through ibus (F274), not
                 * through here. */
                char buf[16];
                KeySym ignored;
                int len = XLookupString(&e.xkey, buf, sizeof buf, &ignored, NULL);
                if (len > 0 && (unsigned char)buf[0] >= 0x20 && (unsigned char)buf[0] != 0x7f) {
                    extra[0] = EV_CHAR;
                    extra[1] = (int64_t)(unsigned char)buf[0];
                    have_extra = 1;
                }
            }
            break;
        }
        case ButtonPress:
        case ButtonRelease:
            /* Buttons 4 and 5 are the wheel on X11, not buttons. Reported as scroll so the Ziman
             * half never has to know that. */
            if (e.xbutton.button == 4 || e.xbutton.button == 5) {
                if (e.type == ButtonPress) {
                    fields[0] = EV_SCROLL;
                    fields[1] = (int64_t)e.xbutton.x;
                    fields[2] = (int64_t)e.xbutton.y;
                    fields[3] = 0;
                    fields[4] = e.xbutton.button == 4 ? 1 : -1;
                    have = 1;
                }
            } else {
                fields[0] = e.type == ButtonPress ? EV_POINTER_DOWN : EV_POINTER_UP;
                fields[1] = (int64_t)e.xbutton.x;
                fields[2] = (int64_t)e.xbutton.y;
                fields[3] = (int64_t)e.xbutton.button;
                have = 1;
            }
            break;
        case MotionNotify:
            fields[0] = EV_POINTER_MOVE;
            fields[1] = (int64_t)e.xmotion.x;
            fields[2] = (int64_t)e.xmotion.y;
            have = 1;
            break;
        case ConfigureNotify:
            /* Only a real size change: a window manager sends ConfigureNotify for a move too, and
             * a resize event per mouse-drag pixel would make a program rebuild its layout for
             * nothing. */
            if (e.xconfigure.width != s->width || e.xconfigure.height != s->height) {
                s->width = e.xconfigure.width;
                s->height = e.xconfigure.height;
                fields[0] = EV_RESIZE;
                fields[1] = (int64_t)e.xconfigure.width;
                fields[2] = (int64_t)e.xconfigure.height;
                have = 1;
            }
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == g_wm_delete) {
                fields[0] = EV_CLOSE;
                have = 1;
            }
            break;
        case FocusIn:
            fields[0] = EV_FOCUS_IN;
            have = 1;
            break;
        case FocusOut:
            fields[0] = EV_FOCUS_OUT;
            have = 1;
            break;
        case Expose:
            /* Coalesced by the Ziman half if it wants: the last one is the only one that matters,
             * but a program that repaints per expose is correct, only wasteful. */
            fields[0] = EV_EXPOSED;
            have = 1;
            break;
        default:
            break;

        /* **The clipboard is served from here, and never reaches Ziman.** Somebody else has asked
         * for what we put on the clipboard, or has taken it from us. Neither is something an
         * application has any decision to make about, so neither becomes an event: answering is
         * the whole of the work, and a program that forgot to would look, to every other window on
         * the display, like a clipboard holding nothing. */
        case SelectionRequest:
            answer_selection_request(&e.xselectionrequest);
            continue;

        case SelectionClear:
            /* Somebody else owns it now. Dropping our copy matters: a later paste must ask the new
             * owner rather than hand back what we put there before. */
            g_clip_text = NULL;
            continue;
        }

        if (!have) continue;
        out = push_event(out, &count, &capacity, fields);
        if (have_extra) out = push_event(out, &count, &capacity, extra);
    }

#ifdef ZIMAN_PORTAL
    /* **The one event here that X11 never sent.** The desktop's appearance is a D-Bus setting, so
     * it arrives on glib's loop; draining that loop here and appending the result is what lets an
     * application hear about it through the same `poll` it already calls, instead of learning a
     * second mechanism. See `portal_poll_changed`, and D11. */
    portal_open();
    if (portal_poll_changed()) {
        int64_t scheme[EVENT_FIELDS] = { EV_SCHEME, g_scheme, 0, 0, 0 };
        out = push_event(out, &count, &capacity, scheme);
    }
#endif

    /* `out` is still NULL when nothing happened, which is the common case in a render loop --
     * so the empty answer is built without it rather than handing a NULL to value_int_array. */
    int64_t none = 0;
    Value answer = window_ok(ok, value_int_array(count > 0 ? out : &none,
                                                 (size_t)count * EVENT_FIELDS));
    free(out);
    return answer;
#endif
}

/* window.size_raw(handle) -> Result<[int], string>: [width, height] as the server last reported. */
static Value native_window_size_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_array(type_scalar(TY_INT));
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    int64_t dims[2] = { (int64_t)s->width, (int64_t)s->height };
    return window_ok(ok, value_int_array(dims, 2));
#endif
}

/* window.origin_raw(handle) -> Result<[int], string>: [x, y] of the window's top-left corner on
 * the screen, in pixels.
 *
 * **Asked every time rather than remembered**, because nothing tells us when it changes: a
 * `ConfigureNotify` carries a position that is the *parent's* coordinate system under a
 * reparenting window manager, which is the usual case, so the value in the event is not the one
 * wanted. `XTranslateCoordinates` asks the server for the real answer and costs a round trip,
 * which is why the caller decides when it matters rather than it being pushed as an event.
 *
 * Needed because an input method is told where the caret is in **root** coordinates -- ibus's
 * `SetCursorLocation` is defined that way -- and a window-relative caret is all an application
 * has. Without this the candidate list appears at the corner of the display. */
static Value native_window_origin_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_array(type_scalar(TY_INT));
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    int x = 0;
    int y = 0;
    Window child = None;
    if (!XTranslateCoordinates(g_display, s->win, DefaultRootWindow(g_display), 0, 0, &x, &y,
                               &child)) {
        /* The window and the root are on different screens, which is the only documented way this
         * fails and cannot happen for a window this process mapped. Reported rather than guessed
         * at, because a silent (0, 0) would put the candidate list in the corner and look like a
         * positioning bug somewhere else entirely. */
        return window_err(ok, "the window is not on the default screen");
    }
    int64_t at[2] = { (int64_t)x, (int64_t)y };
    return window_ok(ok, value_int_array(at, 2));
#endif
}

/* window.id_raw(handle) -> Result<int, string>: the X11 window id.
 *
 * **This exists for the gate, and says so.** F269 is checked by screenshotting the real window with
 * `import -window <id>` and comparing pixels against the pattern that was presented, which needs
 * the id the window manager knows it by. It is the one native here that is not something an
 * application would call. */
static Value native_window_id_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_INT);
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    return window_ok(ok, value_int((int64_t)s->win));
#endif
}

/* window.set_icon_raw(handle, pixels, width, height): the picture a task bar, an alt-tab switcher
 * and a window list draw for this window.
 *
 * **`_NET_WM_ICON` is one property holding a *list* of images**, each written as its width, its
 * height, and then its pixels row by row from the top left. This replaces the property rather than
 * appending to it, so a caller giving several sizes gives them in one call; a window manager picks
 * whichever it wants and scales the rest.
 *
 * **The pixels are 32-bit ARGB in an array of `long`**, which is the trap in this API: the property
 * format is 32, and `XChangeProperty` nevertheless reads a `long` per element -- 64 bits wide here
 * -- and narrows each one itself. Passing the caller's packed 32-bit buffer straight through writes
 * half an icon made of garbage on any 64-bit machine, which is why this widens into its own array
 * rather than casting.
 *
 * **Straight alpha, not premultiplied**, which is what `image.Image` carries and what every tool
 * writing this property does. A premultiplied icon reads as a darker one wherever it is
 * translucent, which looks like a theme problem rather than a format mistake. */
/* The pointer's shape over this window.
 *
 * **A window-wide cursor, not a per-region one.** X attaches a cursor to a window, and a widget set
 * that wanted one per control would need a subwindow per control -- which is the architecture this
 * renderer deliberately does not have, since it paints one image per frame. So the application
 * decides what the pointer is over and says so; `lib/ui` answers what a point is over and
 * `desktop` maps that answer onto these numbers. */
static Value native_window_set_cursor_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Type ok = type_scalar(TY_UNIT);
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    int kind = (int)args[1].as.i;
    if (kind < 0 || kind >= CURSOR_KINDS) kind = 0;
    if (s->cursor_kind == kind) return window_ok(ok, value_unit());
    XDefineCursor(g_display, s->win, cursor_for(kind));
    XFlush(g_display);
    s->cursor_kind = kind;
    return window_ok(ok, value_unit());
#endif
}

static Value native_window_set_icon_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    Type ok = type_scalar(TY_UNIT);
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    int64_t width = args[2].as.i;
    int64_t height = args[3].as.i;
    if (width < 1 || height < 1) return window_err(ok, "an icon's width and height must be positive");

    int n = 0;
    uint8_t *rgba = array_copy_bytes(args[1].as.arr, &n);
    size_t pixels = (size_t)width * (size_t)height;
    size_t needed = pixels * 4u;
    if ((size_t)n != needed) {
        free(rgba);
        char message[160];
        snprintf(message, sizeof message,
                 "the icon is %d bytes but %lldx%lld RGBA needs %zu", n,
                 (long long)width, (long long)height, needed);
        return window_err(ok, message);
    }

    size_t count = pixels + 2u;
    unsigned long *data = ziman_xcalloc(count, sizeof(unsigned long));
    data[0] = (unsigned long)width;
    data[1] = (unsigned long)height;
    for (size_t i = 0; i < pixels; i++) {
        unsigned long r = rgba[i * 4u + 0u];
        unsigned long g = rgba[i * 4u + 1u];
        unsigned long b = rgba[i * 4u + 2u];
        unsigned long a = rgba[i * 4u + 3u];
        data[i + 2u] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    Atom property = XInternAtom(g_display, "_NET_WM_ICON", False);
    XChangeProperty(g_display, s->win, property, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)data, (int)count);
    XFlush(g_display);
    free(data);
    free(rgba);
    return window_ok(ok, value_unit());
#endif
}

/* window.close_raw(handle): unmaps and forgets. Not a Result -- closing something already closed is
 * what an unwinding `with_window` does on a path that already failed, and failing there would
 * replace the real error with this one. */
static Value native_window_close_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
#ifdef ZIMAN_X11
    Slot *s = slot_of(args[0].as.i);
    if (s != NULL) {
        free_image(s);
        if (s->back != None) {
            XFreePixmap(g_display, s->back);
            s->back = None;
            s->back_width = 0;
            s->back_height = 0;
        }
        XFreeGC(g_display, s->gc);
        XDestroyWindow(g_display, s->win);
        XFlush(g_display);
        s->used = 0;
    }
#else
    (void)args;
#endif
    return value_unit();
}

/* window.display_scale_raw() -> float: how many pixels the display says a point is, or **0.0 when it
 * will not say**, which the Ziman half turns into `None`.
 *
 * A sentinel rather than an Option because a native returning one is more machinery than this
 * deserves, and 0.0 is not a ratio any display could mean. The public `window.display_scale` hands
 * back an `Option<float>`, so the sentinel never leaves this file.
 *
 * **Read from `Xft.dpi` in the X resource database, over 96.** That is the number every X toolkit
 * has agreed to look at for thirty years -- GTK, Qt and Xft itself -- and 96 is the nominal density
 * it is quoted against, so `Xft.dpi: 144` is a ratio of 1.5. It is not read from `GDK_SCALE` or
 * `QT_SCALE_FACTOR`: those belong to other toolkits and reading another toolkit's setting is how a
 * program ends up scaled differently from everything else on the desktop for reasons nobody can
 * find.
 *
 * **What this cannot tell you**, written down because the gap is not visible from the answer: the
 * resource database is per *display*, not per monitor, so a laptop beside a HiDPI screen reports one
 * number for both, and a window dragged between them does not change it. Per-monitor density needs
 * RandR and the window's position, which is a larger piece of work and a different function.
 *
 * Opens the display if nothing has yet, because the resource database is a property of the
 * connection -- so this costs a connection on a machine that has one, and answers 0.0 on one that
 * does not. */
static Value native_window_display_scale_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
#ifndef ZIMAN_X11
    return value_float(0.0);
#else
    if (ensure_display() != NULL) return value_float(0.0);
    char *db = XResourceManagerString(g_display);
    if (db == NULL) return value_float(0.0);
    const char *key = "Xft.dpi:";
    const char *at = strstr(db, key);
    if (at == NULL) return value_float(0.0);
    at += strlen(key);
    while (*at == ' ' || *at == '\t') at++;
    char *end = NULL;
    double dpi = strtod(at, &end);
    /* A ratio outside this range is a misconfigured database rather than a display anyone has: the
     * Ziman half would refuse it anyway, and answering 0.0 lets the caller fall through to its own
     * default instead of to an error. */
    if (end == at || dpi < 24.0 || dpi > 1536.0) return value_float(0.0);
    return value_float(dpi / 96.0);
#endif
}


/* window.color_scheme_raw() -> int: what the desktop says it wants, as the portal's own numbering.
 *
 *     0 = no preference     1 = DARK     2 = LIGHT
 *
 * **1 is dark and 2 is light**, which is not alphabetical and is the easiest thing in this file to
 * invert. It is the freedesktop portal's numbering and it is repeated in the Ziman half's comment
 * so that neither side can drift from it quietly.
 *
 * A sentinel rather than an Option, for `display_scale_raw`'s reason: a native returning one is
 * more machinery than this deserves, and 0 is a value the spec already defines as "no preference".
 * **Anything this does not recognise is 0**, which the spec also asks for -- a portal that one day
 * answers 3 should read as "no preference" and not as a crash.
 *
 * Without ZIMAN_PORTAL there is no GIO linked in, so this answers 0 and the library behaves on a
 * machine with no portal exactly as it does in a build without one -- which is the same promise
 * `display_scale_raw` makes about X11. */
static Value native_window_color_scheme_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
#ifndef ZIMAN_PORTAL
    return value_int(0);
#else
    portal_open();
    if (g_portal_absent) return value_int(0);
    return value_int(g_scheme);
#endif
}

/* window.present_premul_raw(handle, pixels, width, height): a frame of PREMULTIPLIED RGBA floats
 * straight onto the window, four doubles a pixel.
 *
 * **Why this exists beside the byte one.** The renderer holds its canvas as premultiplied floats,
 * so presenting a frame used to mean `raster.to_bytes_premul` allocating `width * height * 4` fresh
 * bytes, copying them into a Ziman array, and this function copying them out again -- three passes
 * over 4 MB at 1280x800, for a buffer whose only reader was the loop below. The profile put that
 * conversion at the largest single item of a repaint, and the collector named it unprompted
 * (`Repeated allocation of very large block (appr. size 4004 KiB)`). Here the floats go to the
 * visual's packing in one pass with nothing allocated at all.
 *
 * **The arithmetic follows `to_bytes_premul`'s four-channel branch**, un-premultiplying before the
 * `+ 0.5`. That is deliberate and not obviously right: simply ignoring alpha on a premultiplied
 * buffer would be cheaper -- no divide at all -- but it composites against black, so a window that
 * drew a half-transparent shape would show it dimmed. Alpha is *ignored* here, as `present` has
 * always promised, and for the normal case of a scene with an `Options.background` that is the
 * identity because every pixel is opaque.
 *
 * The opaque case is also why `scale` is branched rather than computed: skipping the divide where
 * `a >= 1.0` measured 1.06x-1.09x of a present over three interleaved passes, which is small but
 * consistent in all three -- where two of F270's Ziman-level attempts read 1.00x and were reverted.
 * For an opaque pixel it is *exactly* `to_bytes_premul`'s answer. For a translucent one `255.0 / a`
 * may differ from `(1.0 / a) * 255.0` by an ULP, so a channel sitting precisely on a rounding
 * boundary could land a byte either side of it -- which is not worth a divide per pixel to avoid on
 * a path that discards alpha anyway, but is worth not claiming to be bit-identical. */
#ifdef ZIMAN_X11
/* One rectangle of premultiplied float pixels packed into `s->pixels` in the visual's own order,
 * clipping the rectangle to the buffer first and reporting through the four pointers what was
 * actually packed. Answers 0 when the rectangle has nothing left after clipping.
 *
 * **A region reaching outside the frame is clipped, not refused**, because a window manager
 * reports damage in its own coordinates and may report some that has nothing left to cover. */
static int pack_region(Slot *s, const double *px, int64_t width, int64_t height,
                       int64_t *rxp, int64_t *ryp, int64_t *rwp, int64_t *rhp) {
    int64_t rx = *rxp, ry = *ryp, rw = *rwp, rh = *rhp;
    if (rw < 1 || rh < 1) return 0;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx >= width || ry >= height) return 0;
    if (rx + rw > width) rw = width - rx;
    if (ry + rh > height) rh = height - ry;
    if (rw < 1 || rh < 1) return 0;

    uint32_t *out = (uint32_t *)s->pixels;
    for (int64_t row = ry; row < ry + rh; row++) {
    for (int64_t cx = rx; cx < rx + rw; cx++) {
        const size_t i = (size_t)(row * width + cx);
        const double *p = px + i * 4u;
        const double a = p[3];
        /* An opaque pixel needs no un-premultiplying, and a window's frame is normally opaque
         * throughout -- a scene with an `Options.background` has alpha 1 everywhere. Measured
         * before being kept. */
        const double scale = a >= 1.0 ? 255.0 : (a > 0.0 ? 255.0 / a : 0.0);
        double r = p[0] * scale + 0.5;
        double g = p[1] * scale + 0.5;
        double b = p[2] * scale + 0.5;
        if (r > 255.0) r = 255.0;
        if (g > 255.0) g = 255.0;
        if (b > 255.0) b = 255.0;
        if (r < 0.0) r = 0.0;
        if (g < 0.0) g = 0.0;
        if (b < 0.0) b = 0.0;
        out[i] = ((uint32_t)(int64_t)r << g_red_shift) | ((uint32_t)(int64_t)g << g_green_shift)
                 | ((uint32_t)(int64_t)b << g_blue_shift);
    }
    }
    *rxp = rx; *ryp = ry; *rwp = rw; *rhp = rh;
    return 1;
}
#endif

static Value native_window_present_premul_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_UNIT);
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    int64_t width = args[2].as.i;
    int64_t height = args[3].as.i;
    if (width < 1 || height < 1) return window_err(ok, "the pixel buffer's width and height must be positive");

    /* The rectangle to convert and push. A full frame passes the whole buffer; a repaint that knows
     * what changed passes that, and **the pack loop below shrinks with it** -- pushing a
     * sub-rectangle while still converting a million pixels would leave most of the saving
     * unclaimed and would have measured as nothing. */
    int64_t rx = args[4].as.i;
    int64_t ry = args[5].as.i;
    int64_t rw = args[6].as.i;
    int64_t rh = args[7].as.i;

    size_t n = 0;
    const double *px = ziman_mutable_array_float_data(args[1], &n);
    size_t needed = (size_t)width * (size_t)height * 4u;
    /* **Too short is a mistake; longer is a canvas with room to spare.** This was an equality, and
     * it refused every frame once `raster.surface_fitting` started keeping a buffer across a
     * resize: a canvas is a stride and a window into a buffer, so a window of 1102x740 drawn in a
     * buffer grown for 1152x768 is correct and the 277,024 floats past the end are simply not
     * read -- the pack loop below indexes `row * width + cx`, which cannot reach them. The window
     * exited with "the pixel buffer is 3538944 floats but 1102x740 premultiplied RGBA needs
     * 3261920" on the first drag of its edge, which is how this was found. */
    if (n < needed) {
        char message[176];
        snprintf(message, sizeof message,
                 "the pixel buffer is %zu floats but %lldx%lld premultiplied RGBA needs %zu", n,
                 (long long)width, (long long)height, needed);
        return window_err(ok, message);
    }

    const char *failed = ensure_image(s, (int)width, (int)height);
    if (failed != NULL) return window_err(ok, failed);

    if (!pack_region(s, px, width, height, &rx, &ry, &rw, &rh)) {
        return window_ok(ok, value_unit());
    }
    put_image(s, (int)rx, (int)ry, (int)rw, (int)rh);
    return window_ok(ok, value_unit());
#endif
}

/* The same two doors in a build with no window backend, and **outside the `#ifdef ZIMAN_X11`
 * region above** -- which is where they were, so `make ziman` stopped compiling while
 * `make gui` was fine. A conditional block that already holds everything hides a nested
 * `#else` completely. */
#ifndef ZIMAN_X11
static Value native_window_clipboard_put_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return window_err(type_scalar(TY_UNIT),
                      "this build of ziman has no window backend (build with `make gui`)");
}

static Value native_window_clipboard_get_raw(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return window_err(type_scalar(TY_STRING),
                      "this build of ziman has no window backend (build with `make gui`)");
}
#endif /* !ZIMAN_X11 */

/* window.present_premul_regions_raw(handle, pixels, width, height, rects): a whole frame's damage,
 * composed into the back buffer and then shown in one batch.
 *
 * **This is what the back buffer is for.** `present_premul_raw` above puts one rectangle up and
 * waits for the server; a frame that changed four distant places -- `raster.damage_regions` offers
 * up to four -- therefore cost four round trips and appeared four rectangles at a time. Here every
 * rectangle is packed and put into the pixmap first, then copied to the window one after another
 * with nothing waiting in between, and the single `XSync` at the end is the one round trip the
 * shared segment needs before the next frame packs over it.
 *
 * `rects` is four ints a rectangle: x, y, width, height, in device pixels. A rectangle with
 * nothing left after clipping is skipped rather than refused, exactly as the single-region door
 * skips it. */
static Value native_window_present_premul_regions_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type ok = type_scalar(TY_UNIT);
#ifndef ZIMAN_X11
    (void)args;
    return window_err(ok, "this build of ziman has no window backend (build with `make gui`)");
#else
    Slot *s = slot_of(args[0].as.i);
    if (s == NULL) return window_err(ok, "no such window (already closed?)");
    int64_t width = args[2].as.i;
    int64_t height = args[3].as.i;
    if (width < 1 || height < 1) return window_err(ok, "the pixel buffer's width and height must be positive");

    size_t n = 0;
    const double *px = ziman_mutable_array_float_data(args[1], &n);
    size_t needed = (size_t)width * (size_t)height * 4u;
    if (n < needed) {
        char message[176];
        snprintf(message, sizeof message,
                 "the pixel buffer is %zu floats but %lldx%lld premultiplied RGBA needs %zu", n,
                 (long long)width, (long long)height, needed);
        return window_err(ok, message);
    }

    const ArrayObj *rects = args[4].as.arr;
    int64_t count = rects == NULL ? 0 : (int64_t)rects->count;
    if (count % 4 != 0) return window_err(ok, "the regions must be four numbers each: x, y, width, height");
    if (count == 0) return window_ok(ok, value_unit());

    const char *failed = ensure_image(s, (int)width, (int)height);
    if (failed != NULL) return window_err(ok, failed);

    /* Clipped bounds are kept so the copy below moves exactly what was packed; a rectangle that
     * clipped away entirely is marked with a zero width and skipped twice. */
    int64_t kept[WINDOW_MAX_REGIONS * 4];
    int64_t keeping = count / 4;
    if (keeping > WINDOW_MAX_REGIONS) keeping = WINDOW_MAX_REGIONS;
    for (int64_t i = 0; i < keeping; i++) {
        int64_t rx = rects->ints[i * 4 + 0];
        int64_t ry = rects->ints[i * 4 + 1];
        int64_t rw = rects->ints[i * 4 + 2];
        int64_t rh = rects->ints[i * 4 + 3];
        if (pack_region(s, px, width, height, &rx, &ry, &rw, &rh)) {
            put_into(s, s->back, (int)rx, (int)ry, (int)rw, (int)rh);
            kept[i * 4 + 0] = rx; kept[i * 4 + 1] = ry;
            kept[i * 4 + 2] = rw; kept[i * 4 + 3] = rh;
        } else {
            kept[i * 4 + 2] = 0;
        }
    }
    for (int64_t i = 0; i < keeping; i++) {
        if (kept[i * 4 + 2] < 1) continue;
        XCopyArea(g_display, s->back, s->win, s->gc, (int)kept[i * 4 + 0], (int)kept[i * 4 + 1],
                  (unsigned)kept[i * 4 + 2], (unsigned)kept[i * 4 + 3],
                  (int)kept[i * 4 + 0], (int)kept[i * 4 + 1]);
    }
    finish_frame(s);
    return window_ok(ok, value_unit());
#endif
}

void native_window_register(void) {
    native_register("window::open_raw", native_window_open_raw);
    native_register("window::present_raw", native_window_present_raw);
    native_register("window::present_premul_raw", native_window_present_premul_raw);
    native_register("window::present_premul_regions_raw", native_window_present_premul_regions_raw);
    native_register("window::poll_raw", native_window_poll_raw);
    native_register("window::size_raw", native_window_size_raw);
    native_register("window::id_raw", native_window_id_raw);
    native_register("window::origin_raw", native_window_origin_raw);
    native_register("window::set_icon_raw", native_window_set_icon_raw);
    native_register("window::set_cursor_raw", native_window_set_cursor_raw);
    native_register("window::close_raw", native_window_close_raw);
    native_register("window::display_scale_raw", native_window_display_scale_raw);
    native_register("window::color_scheme_raw", native_window_color_scheme_raw);
    native_register("window::clipboard_put_raw", native_window_clipboard_put_raw);
    native_register("window::clipboard_get_raw", native_window_clipboard_get_raw);
}
