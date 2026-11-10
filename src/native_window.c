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

#include "native.h"
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

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

typedef struct {
    int used;
    Window win;
    GC gc;
    XImage *image;      /* owns `pixels` below; recreated when the window is resized */
    char *pixels;
    int image_width;
    int image_height;
    int width;
    int height;
} Slot;

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
    if (s->image != NULL) {
        /* XDestroyImage frees the data pointer too, with libc's free, which is why `pixels` comes
         * from `libc_malloc` and not from the collector. */
        XDestroyImage(s->image);
        s->image = NULL;
        s->pixels = NULL;
        s->image_width = 0;
        s->image_height = 0;
    }
}

static const char *ensure_image(Slot *s, int width, int height) {
    if (s->image != NULL && s->image_width == width && s->image_height == height) return NULL;
    free_image(s);
    int screen = DefaultScreen(g_display);
    Visual *visual = DefaultVisual(g_display, screen);
    int depth = DefaultDepth(g_display, screen);
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

    XPutImage(g_display, s->win, s->gc, s->image, 0, 0, 0, 0, (unsigned)width, (unsigned)height);
    XFlush(g_display);
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
            have = 1;
            if (e.type == KeyPress) {
                /* The text the key produced, as well as the key itself. Both, because a shortcut
                 * wants the key and a text field wants the character, and deriving either from the
                 * other is what makes keyboard layouts go wrong. */
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
        }

        if (!have) continue;
        out = push_event(out, &count, &capacity, fields);
        if (have_extra) out = push_event(out, &count, &capacity, extra);
    }

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

/* window.close_raw(handle): unmaps and forgets. Not a Result -- closing something already closed is
 * what an unwinding `with_window` does on a path that already failed, and failing there would
 * replace the real error with this one. */
static Value native_window_close_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
#ifdef ZIMAN_X11
    Slot *s = slot_of(args[0].as.i);
    if (s != NULL) {
        free_image(s);
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

void native_window_register(void) {
    native_register("window::open_raw", native_window_open_raw);
    native_register("window::present_raw", native_window_present_raw);
    native_register("window::poll_raw", native_window_poll_raw);
    native_register("window::size_raw", native_window_size_raw);
    native_register("window::id_raw", native_window_id_raw);
    native_register("window::close_raw", native_window_close_raw);
}
