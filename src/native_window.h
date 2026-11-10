#ifndef ZIMAN_NATIVE_WINDOW_H
#define ZIMAN_NATIVE_WINDOW_H

/* Registers the `window` core module's natives (open, present, poll_raw, size_raw, id_raw, close).
 * Called once from main() before interpret() runs.
 *
 * **The backend is behind ZIMAN_X11 and the default build has none**, so a command-line `ziman`
 * gains no dependency on a display -- see docs/reviews/tasks/f269-f277.md F276. Without it every
 * native answers an error saying so, which is why the module is still registered and `--api window`
 * still works. See lib/core/window.zm and docs/design/desktop-applications.md. */
void native_window_register(void);

#endif
