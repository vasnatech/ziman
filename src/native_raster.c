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

/* raster.composite_row_flat(canvas, coverage, row_base, first_col, last_col, r, g, b, a, mask,
 * has_mask): one scanline of a solid fill, composited.
 *
 * **The third loop, and the one left after the floor was dealt with.** `fill_shape`'s inner loop
 * runs once per pixel of a shape's bounding box and does, in the interpreter: an array index for
 * coverage, a compare, maybe a second index for the clip mask, and then `composite` -- four reads
 * and four writes of `double` plus the blend. For the sidebar rectangle of
 * `tests/bench/ui_frame.zm` that is 181,000 iterations of roughly fifteen interpreted operations.
 *
 * Only the **flat** case comes here, which is `Paint.Solid`. A gradient, a tiled paint or an image
 * evaluates its colour per pixel through `ready_at` and an inverse transform, and stays in Ziman:
 * moving it would mean moving the paint model too, and `raster.zm`'s own comment already notes that
 * solid is the common case. So this is a fast path beside the general one, not a replacement for it.
 *
 * The two `> 0.002` guards are reproduced in the same order and with the same constant.
 * `raster.zm` explains the number -- below a 255th of a level there is nothing to add to an 8-bit
 * channel -- and a fast path that skipped a pixel the reference composites, or composited one it
 * skips, would differ on real images. */
static Value native_raster_composite_row_flat(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t canvas_len = 0;
    double *data = ziman_mutable_array_float_data(args[0], &canvas_len);
    size_t cov_len = 0;
    const double *coverage = ziman_mutable_array_float_data(args[1], &cov_len);
    if (data == NULL || coverage == NULL) return value_unit();

    int64_t row_base = args[2].as.i;
    int64_t first_col = args[3].as.i;
    int64_t last_col = args[4].as.i;
    double r = args[5].as.f;
    double g = args[6].as.f;
    double b = args[7].as.f;
    double a = args[8].as.f;
    /* An immutable `[float]`: its packed buffer directly, so a clip mask costs no copy. A boxed or
     * empty array answers NULL, which `has_mask` then must not have claimed. */
    const ArrayObj *mask_arr = args[9].as.arr;
    const double *mask = mask_arr == NULL ? NULL : mask_arr->floats;
    int has_mask = args[10].as.b != 0;
    if (has_mask && mask == NULL) return value_unit();

    for (int64_t x = first_col; x <= last_col; x++) {
        if (x < 0 || (size_t)x >= cov_len) continue;
        double cov = coverage[x];
        if (cov <= 0.002) continue;
        double through = 1.0;
        if (has_mask) {
            int64_t at = row_base + x;
            if (at < 0) continue;
            through = mask[at];
        }
        if (through <= 0.002) continue;
        double weight = cov * through;
        size_t at = (size_t)(row_base + x) * CHANNELS;
        if (at + 3 >= canvas_len) continue;
        double keep = 1.0 - a * weight;
        data[at + 0] = r * weight + data[at + 0] * keep;
        data[at + 1] = g * weight + data[at + 1] * keep;
        data[at + 2] = b * weight + data[at + 2] * keep;
        data[at + 3] = a * weight + data[at + 3] * keep;
    }
    return value_unit();
}

/* raster.composite_span_flat(canvas, cover, cover_at, canvas_at, count, r, g, b, a, mask,
 * has_mask): a run of pixels of a solid fill, with the coverage and the canvas indexed
 * independently.
 *
 * **This is `blit`'s loop, which is text.** A glyph's coverage lives in a tile just big enough to
 * hold it, so its index advances by the tile's width while the canvas index advances by the
 * canvas's -- which is the only way this differs from `composite_row_flat` above, and the reason it
 * is a second function rather than a parameter on the first: a fill's coverage is a `MutableArray`
 * being reused row by row and a tile's is an immutable `[float]`, so the two cannot share a
 * signature without one of them copying.
 *
 * The caller clips the run to the canvas, because it knows the tile's placement; this only checks
 * that the indices it was handed are inside the buffers it was handed. Guards and blend are
 * `composite`'s, as above. */
static Value native_raster_composite_span_flat(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t canvas_len = 0;
    double *data = ziman_mutable_array_float_data(args[0], &canvas_len);
    const ArrayObj *cover_arr = args[1].as.arr;
    const double *cover = cover_arr == NULL ? NULL : cover_arr->floats;
    if (data == NULL || cover == NULL) return value_unit();
    size_t cover_len = cover_arr->count < 0 ? 0 : (size_t)cover_arr->count;

    int64_t cover_at = args[2].as.i;
    int64_t canvas_at = args[3].as.i;
    int64_t count = args[4].as.i;
    double r = args[5].as.f;
    double g = args[6].as.f;
    double b = args[7].as.f;
    double a = args[8].as.f;
    const ArrayObj *mask_arr = args[9].as.arr;
    const double *mask = mask_arr == NULL ? NULL : mask_arr->floats;
    int has_mask = args[10].as.b != 0;
    if (has_mask && mask == NULL) return value_unit();
    size_t mask_len = (has_mask && mask_arr != NULL && mask_arr->count > 0)
                          ? (size_t)mask_arr->count : 0;

    for (int64_t i = 0; i < count; i++) {
        int64_t ci = cover_at + i;
        int64_t pi = canvas_at + i;
        if (ci < 0 || (size_t)ci >= cover_len) continue;
        if (pi < 0) continue;
        double cov = cover[ci];
        if (cov <= 0.002) continue;
        double through = 1.0;
        if (has_mask) {
            if ((size_t)pi >= mask_len) continue;
            through = mask[pi];
        }
        if (through <= 0.002) continue;
        double weight = cov * through;
        size_t at = (size_t)pi * CHANNELS;
        if (at + 3 >= canvas_len) continue;
        double keep = 1.0 - a * weight;
        data[at + 0] = r * weight + data[at + 0] * keep;
        data[at + 1] = g * weight + data[at + 1] * keep;
        data[at + 2] = b * weight + data[at + 2] * keep;
        data[at + 3] = a * weight + data[at + 3] * keep;
    }
    return value_unit();
}

void native_raster_register(void) {
    native_register("raster::clear_premul", native_raster_clear_premul);
    native_register("raster::to_bytes_premul", native_raster_to_bytes_premul);
    native_register("raster::composite_row_flat", native_raster_composite_row_flat);
    native_register("raster::composite_span_flat", native_raster_composite_span_flat);
}
