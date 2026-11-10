/* Synthesises input for tests/run_window_test.sh: the half of F269 that no Ziman code can test.
 *
 * `xdotool` is the usual way to do this and is not installed on the machine this was written on.
 * It is a wrapper over X's own input synthesis, which is installed, so this is that wrapper cut
 * down to the six things the window test needs. Compiled on demand by the test script rather than
 * by the Makefile, because libXtst must not become a dependency of `ziman` -- the same reason the
 * X11 backend itself is behind `make gui`.
 *
 * **There are two ways to deliver a key and they do not test the same thing.**
 *
 * XTEST asks the *server* to behave as though the hardware fired, so the event travels the whole
 * real path: keymap, modifier state, focus, delivery. It cannot be aimed at a window -- it goes
 * wherever the focus is -- and on a Wayland session that focus belongs to the compositor, which no
 * X client can claim. On the GNOME/Wayland machine this was written on, XTEST keys and clicks went
 * to the terminal and the window under test saw nothing, which is a property of the session and not
 * of the code being tested.
 *
 * XSendEvent posts a ready-made event onto one named window's queue. It is immune to focus, so it
 * works the same on X11 and under XWayland, and it is what `xdotool key --window` does. What it
 * proves is the client's half: that an X event of that shape becomes the right `window.Event`,
 * keycode through the server's keymap to a keysym and to text, modifier mask to flags, button and
 * coordinates through unchanged. What it does **not** prove is the server's half -- focus, grabs,
 * and delivery to the right window -- and it sets `send_event = True` where real input sets it
 * false. The native does not inspect that field, which is why this works at all.
 *
 * So this uses XSendEvent and the test says so. A gate that depended on the session type would be a
 * gate that skipped on the machine it was written on, and XTEST is not even linked here: dropping it
 * leaves this needing nothing `make gui` does not already need.
 *
 * A resize and a close are the real thing either way: any client may resize any window, and
 * WM_DELETE_WINDOW is exactly the message a close box sends. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *dpy;

static void settle(void) { XSync(dpy, False); }

static unsigned int mask_of(const char *modifier) {
    if (modifier == NULL) return 0;
    if (strcmp(modifier, "shift") == 0) return ShiftMask;
    if (strcmp(modifier, "control") == 0) return ControlMask;
    if (strcmp(modifier, "alt") == 0) return Mod1Mask;
    if (strcmp(modifier, "meta") == 0) return Mod4Mask;
    fprintf(stderr, "x11_inject: unknown modifier '%s'\n", modifier);
    exit(1);
}

/* A KeyPress/KeyRelease pair on one window's queue.
 *
 * The keycode is looked up from the keysym, so the decoder's job is to turn it back: the round trip
 * through the server's keymap is the part worth testing, and a decoder that answered a fixed keysym
 * would fail the assertion on the other side. */
static int do_key(Window w, const char *name, const char *modifier) {
    KeySym sym = XStringToKeysym(name);
    if (sym == NoSymbol) {
        fprintf(stderr, "x11_inject: no keysym named '%s'\n", name);
        return 1;
    }
    int code = XKeysymToKeycode(dpy, sym);
    if (code == 0) {
        fprintf(stderr, "x11_inject: keysym '%s' is not on this keymap\n", name);
        return 1;
    }
    XKeyEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.display = dpy;
    ev.window = w;
    ev.root = DefaultRootWindow(dpy);
    ev.subwindow = None;
    ev.time = CurrentTime;
    ev.x = 1; ev.y = 1; ev.x_root = 1; ev.y_root = 1;
    ev.same_screen = True;
    ev.state = mask_of(modifier);
    ev.keycode = (unsigned)code;
    ev.type = KeyPress;
    XSendEvent(dpy, w, True, KeyPressMask, (XEvent *)&ev);
    ev.type = KeyRelease;
    XSendEvent(dpy, w, True, KeyReleaseMask, (XEvent *)&ev);
    settle();
    return 0;
}

/* A press/release pair at a point inside the window. The coordinates are carried by the event, so
 * the pointer is not moved: a test that warped the real pointer would move the user's mouse, and
 * under XWayland the warp is refused anyway. */
static int do_button(Window w, int x, int y, int button) {
    XButtonEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.display = dpy;
    ev.window = w;
    ev.root = DefaultRootWindow(dpy);
    ev.subwindow = None;
    ev.time = CurrentTime;
    ev.x = x; ev.y = y; ev.x_root = x; ev.y_root = y;
    ev.same_screen = True;
    ev.button = (unsigned)button;
    ev.type = ButtonPress;
    XSendEvent(dpy, w, True, ButtonPressMask, (XEvent *)&ev);
    ev.type = ButtonRelease;
    ev.state = Button1Mask;
    XSendEvent(dpy, w, True, ButtonReleaseMask, (XEvent *)&ev);
    settle();
    return 0;
}

/* The wheel is buttons 4 and 5 in X11, which the native turns into Scroll -- so this sends a
 * button and the assertion on the other side is about a Scroll. */
static int do_scroll(Window w, int x, int y, const char *direction) {
    if (strcmp(direction, "up") != 0 && strcmp(direction, "down") != 0) {
        fprintf(stderr, "x11_inject: scroll takes 'up' or 'down', not '%s'\n", direction);
        return 1;
    }
    return do_button(w, x, y, strcmp(direction, "up") == 0 ? 4 : 5);
}

static int do_motion(Window w, int x, int y) {
    XMotionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.display = dpy;
    ev.window = w;
    ev.root = DefaultRootWindow(dpy);
    ev.subwindow = None;
    ev.time = CurrentTime;
    ev.x = x; ev.y = y; ev.x_root = x; ev.y_root = y;
    ev.same_screen = True;
    ev.type = MotionNotify;
    XSendEvent(dpy, w, True, PointerMotionMask, (XEvent *)&ev);
    settle();
    return 0;
}

/* Any client may resize any window; a window manager is only the usual one doing it. The server
 * generates the ConfigureNotify itself, so this one is not synthetic. */
static int do_resize(Window w, int width, int height) {
    XResizeWindow(dpy, w, (unsigned)width, (unsigned)height);
    settle();
    return 0;
}

/* Exactly what a close box does: the WM_DELETE_WINDOW client message, not a kill. */
static int do_close(Window w) {
    XClientMessageEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
    ev.format = 32;
    ev.data.l[0] = (long)XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    ev.data.l[1] = CurrentTime;
    XSendEvent(dpy, w, False, NoEventMask, (XEvent *)&ev);
    settle();
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: x11_inject <window-id> key <keysym> [shift|control|alt|meta]\n"
            "       x11_inject <window-id> click <x> <y> <button>\n"
            "       x11_inject <window-id> scroll <x> <y> <up|down>\n"
            "       x11_inject <window-id> move <x> <y>\n"
            "       x11_inject <window-id> resize <width> <height>\n"
            "       x11_inject <window-id> close\n");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 1;
    }
    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "x11_inject: cannot open the display\n");
        return 2;
    }
    Window w = (Window)strtoul(argv[1], NULL, 0);
    const char *what = argc > 2 ? argv[2] : "";
    int rc;
    if (strcmp(what, "key") == 0 && argc > 3) {
        rc = do_key(w, argv[3], argc > 4 ? argv[4] : NULL);
    } else if (strcmp(what, "click") == 0 && argc > 5) {
        rc = do_button(w, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
    } else if (strcmp(what, "scroll") == 0 && argc > 5) {
        rc = do_scroll(w, atoi(argv[3]), atoi(argv[4]), argv[5]);
    } else if (strcmp(what, "move") == 0 && argc > 4) {
        rc = do_motion(w, atoi(argv[3]), atoi(argv[4]));
    } else if (strcmp(what, "resize") == 0 && argc > 4) {
        rc = do_resize(w, atoi(argv[3]), atoi(argv[4]));
    } else if (strcmp(what, "close") == 0) {
        rc = do_close(w);
    } else {
        usage();
        rc = 1;
    }
    XCloseDisplay(dpy);
    return rc;
}
