/* The two per-pixel loops of `lib/graphics/raster.zm`, in C. docs/reviews/tasks/f269-f277.md F270.
 *
 * **These two and not the drawing, because this is where the time was.** `tests/bench/ui_frame.zm`
 * measures an *empty* 1280x800 scene -- nothing drawn at all -- and it cost about two thirds of a
 * full frame. The reason is visible in `raster.zm`: a canvas is four `float`s per pixel, so
 * clearing one is 4 million interpreted stores and converting it to bytes is a million iterations
 * of float arithmetic, before a single shape is considered. Moving the shapes to C first would have
 * left that floor exactly where it was.
 *
 * **Exactness is the requirement, not a nice property**, and the oracle is a fixture that already
 * existed: `tests/lang/graphics_raster.zm` asserts real pixels over 418 lines -- anti-aliased fills
 * under both winding rules, strokes, gradients, alpha, and all three output formats -- against a
 * committed `.out`. Moving these two loops into C left it byte-identical, which is the whole claim
 * this file has to make. `tests/lang/graphics_stroke.zm` likewise.
 *
 * Ziman's `float` is a C `double` (src/value.h), so the arithmetic here is the same arithmetic in
 * the same order -- including `+ 0.5` before truncation and the upper clamp with no lower one,
 * which `raster.zm` explains and this file must not improve on. A tidier version that clamped the
 * bottom too would differ from the reference on nothing that can occur, which is the worst kind of
 * difference to debug.
 *
 * Neither function allocates a canvas or knows what is drawn on one. They take the `MutableArray`
 * `raster.zm` already has and work on it in place, which is why this is a few hundred lines rather
 * than a renderer. */
#define _POSIX_C_SOURCE 200809L
#include "native_raster.h"

#include <stdint.h>
#include <stdlib.h>

#include "native.h"
#include "prelude.h"
#include "value.h"
#include "xalloc.h"

/* The canvas is premultiplied RGBA, four doubles per pixel, exactly as `raster.Target.data` is. */
#define CHANNELS 4

/* raster.clear_premul(data, r, g, b, a): every pixel set to one premultiplied colour.
 *
 * The loop `clear_to` runs, which is a million iterations of four indexed stores for a 1280x800
 * canvas. Nothing here is clamped or converted: a `Premul` is already in the canvas's own space,
 * and clamping a value the caller computed would be this file deciding something. */
static Value native_raster_clear_premul(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t n = 0;
    double *data = ziman_mutable_array_float_data(args[0], &n);
    /* A canvas that has adopted no packed representation yet has nothing to clear: `raster.zm`
     * fills it at construction, so this is the empty-canvas case rather than a disagreement. */
    if (data == NULL) return value_unit();
    double r = args[1].as.f;
    double g = args[2].as.f;
    double b = args[3].as.f;
    double a = args[4].as.f;
    /* Truncated to whole pixels: a canvas whose length is not a multiple of four is not something
     * to half-fill. `raster.zm` always allocates width*height*4. */
    size_t pixels = n / CHANNELS;
    for (size_t i = 0; i < pixels; i++) {
        data[i * CHANNELS + 0] = r;
        data[i * CHANNELS + 1] = g;
        data[i * CHANNELS + 2] = b;
        data[i * CHANNELS + 3] = a;
    }
    return value_unit();
}

/* raster.to_bytes_premul(data, channels) -> [byte]: the canvas as the bytes an `image.Image` holds.
 *
 * `finish`'s loop, with its three cases kept separate as they are there:
 *
 *   4 channels -- un-premultiplied, because that is what `image.Image` holds and what PNG stores;
 *   3 channels -- the premultiplied value *is* the colour over whatever the canvas started as;
 *   1 channel  -- the same, through the Rec. 601 luma weights.
 *
 * Every case adds 0.5 and truncates, and clamps only the top. `raster.zm` says why there is no
 * lower clamp -- every value is a coverage-weighted colour in 0..1 times 255 and compositing never
 * produces a negative one -- and reproducing that reasoning matters more than tidying it: a lower
 * clamp here would make the two implementations disagree on nothing that can happen, which is the
 * worst kind of difference to debug. */
static Value native_raster_to_bytes_premul(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t n = 0;
    const double *data = ziman_mutable_array_float_data(args[0], &n);
    int64_t channels = args[1].as.i;
    if (channels != 1 && channels != 3 && channels != 4) {
        Type bt = type_array(type_scalar(TY_BYTE));
        return value_result(type_ref(type_result(bt, type_scalar(TY_STRING))),
                            result_obj_new(0, value_string(ziman_xstrdup(
                                "raster: an image has 1, 3 or 4 channels"))));
    }
    size_t pixels = data == NULL ? 0 : n / CHANNELS;
    size_t out_len = pixels * (size_t)channels;
    uint8_t *out = out_len == 0 ? NULL : ziman_xmalloc(out_len);

    for (size_t i = 0; i < pixels; i++) {
        const double *px = data + i * CHANNELS;
        if (channels == 4) {
            double a = px[3];
            double inv_a = a > 0.0 ? 1.0 / a : 0.0;
            double r = px[0] * inv_a * 255.0 + 0.5;
            double g = px[1] * inv_a * 255.0 + 0.5;
            double b = px[2] * inv_a * 255.0 + 0.5;
            double av = a * 255.0 + 0.5;
            if (r > 255.0) r = 255.0;
            if (g > 255.0) g = 255.0;
            if (b > 255.0) b = 255.0;
            if (av > 255.0) av = 255.0;
            out[i * 4 + 0] = (uint8_t)(int64_t)r;
            out[i * 4 + 1] = (uint8_t)(int64_t)g;
            out[i * 4 + 2] = (uint8_t)(int64_t)b;
            out[i * 4 + 3] = (uint8_t)(int64_t)av;
        } else if (channels == 3) {
            double r = px[0] * 255.0 + 0.5;
            double g = px[1] * 255.0 + 0.5;
            double b = px[2] * 255.0 + 0.5;
            if (r > 255.0) r = 255.0;
            if (g > 255.0) g = 255.0;
            if (b > 255.0) b = 255.0;
            out[i * 3 + 0] = (uint8_t)(int64_t)r;
            out[i * 3 + 1] = (uint8_t)(int64_t)g;
            out[i * 3 + 2] = (uint8_t)(int64_t)b;
        } else {
            double y = (0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]) * 255.0 + 0.5;
            if (y > 255.0) y = 255.0;
            out[i] = (uint8_t)(int64_t)y;
        }
    }

    Type bt = type_array(type_scalar(TY_BYTE));
    uint8_t none = 0;
    Value bytes = value_byte_array(out_len == 0 ? &none : out, (int)out_len);
    free(out);
    return value_result(type_ref(type_result(bt, type_scalar(TY_STRING))),
                        result_obj_new(1, bytes));
}

void native_raster_register(void) {
    native_register("raster::clear_premul", native_raster_clear_premul);
    native_register("raster::to_bytes_premul", native_raster_to_bytes_premul);
}
