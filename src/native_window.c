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
    if (s->shm_active) {
        XShmPutImage(g_display, s->win, s->gc, s->image, x, y, x, y, (unsigned)width,
                     (unsigned)height, False);
        XSync(g_display, False);
        return;
    }
    XPutImage(g_display, s->win, s->gc, s->image, x, y, x, y, (unsigned)width, (unsigned)height);
    XFlush(g_display);
}

static const char *ensure_image(Slot *s, int width, int height) {
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
    if (n != needed) {
        char message[176];
        snprintf(message, sizeof message,
                 "the pixel buffer is %zu floats but %lldx%lld premultiplied RGBA needs %zu", n,
                 (long long)width, (long long)height, needed);
        return window_err(ok, message);
    }

    if (rw < 1 || rh < 1) return window_err(ok, "the region to present must be larger than nothing");
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx >= width || ry >= height) return window_ok(ok, value_unit());
    if (rx + rw > width) rw = width - rx;
    if (ry + rh > height) rh = height - ry;
    if (rw < 1 || rh < 1) return window_ok(ok, value_unit());

    const char *failed = ensure_image(s, (int)width, (int)height);
    if (failed != NULL) return window_err(ok, failed);

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

    put_image(s, (int)rx, (int)ry, (int)rw, (int)rh);
    return window_ok(ok, value_unit());
#endif
}

void native_window_register(void) {
    native_register("window::open_raw", native_window_open_raw);
    native_register("window::present_raw", native_window_present_raw);
    native_register("window::present_premul_raw", native_window_present_premul_raw);
    native_register("window::poll_raw", native_window_poll_raw);
    native_register("window::size_raw", native_window_size_raw);
    native_register("window::id_raw", native_window_id_raw);
    native_register("window::close_raw", native_window_close_raw);
    native_register("window::display_scale_raw", native_window_display_scale_raw);
}
