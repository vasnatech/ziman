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

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "error.h"
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

/* raster.clear_range(row, from_x, to_x): one coverage row's window set to zero.
 *
 * Called once per scanline of every fill, and `raster.zm` explains why it is a window rather than
 * the whole row -- a 26-pixel shape on a 600-pixel canvas should not pay for 600. For the sidebar
 * rectangle of `tests/bench/ui_frame.zm` that is still 240 stores on each of 756 rows. */
static Value native_raster_clear_range(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t n = 0;
    double *row = ziman_mutable_array_float_data(args[0], &n);
    if (row == NULL) return value_unit();
    int64_t from_x = args[1].as.i;
    int64_t to_x = args[2].as.i;
    if (from_x < 0) from_x = 0;
    if (to_x >= (int64_t)n) to_x = (int64_t)n - 1;
    for (int64_t i = from_x; i <= to_x; i++) row[i] = 0.0;
    return value_unit();
}

/* raster.add_span(row, width, from, to, weight): a span's exact overlap with each pixel it touches.
 *
 * **The innermost loop of the anti-aliasing pass**, reached once per span per sub-scanline -- so
 * four times a row at `samples=4`, and the whole-pixel run in the middle is per pixel. The sidebar
 * rectangle alone is 240 pixels x 4 samples x 756 rows of it.
 *
 * Transcribed statement for statement from `raster.zm`'s version, including the order of the three
 * writes and the `floor`-then-clamp on each end. The arithmetic is where exactness lives: coverage
 * is accumulated, so a difference of one ulp in a partial pixel survives into the composite and
 * then into a byte. `tests/lang/graphics_raster.zm` asserts anti-aliased edges and distinct alpha
 * levels, which is what would catch it. */
static Value native_raster_add_span(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t n = 0;
    double *row = ziman_mutable_array_float_data(args[0], &n);
    if (row == NULL) return value_unit();
    int64_t width = args[1].as.i;
    if (width > (int64_t)n) width = (int64_t)n;
    if (width <= 0) return value_unit();
    double xa = args[2].as.f;
    double xb = args[3].as.f;
    double weight = args[4].as.f;

    if (xa < 0.0) xa = 0.0;
    if (xb > (double)width) xb = (double)width;
    if (xb <= xa) return value_unit();

    int64_t ia = (int64_t)floor(xa);
    int64_t ib = (int64_t)floor(xb);
    if (ia < 0) ia = 0;
    if (ia > width - 1) ia = width - 1;
    if (ib < 0) ib = 0;
    if (ib > width - 1) ib = width - 1;

    if (ia == ib) {
        row[ia] += (xb - xa) * weight;
        return value_unit();
    }
    row[ia] += ((double)(ia + 1) - xa) * weight;
    for (int64_t i = ia + 1; i < ib; i++) row[i] += weight;
    row[ib] += (xb - (double)ib) * weight;
    return value_unit();
}

/* raster.composite_row_gradient(canvas, coverage, row_base, first_col, last_col, spec, mask,
 * has_mask): one scanline of a linear or radial gradient fill.
 *
 * **The largest ratio left in the renderer when this was written.** A full-canvas solid fill of
 * 1280x800 took 47 ms through `composite_row_flat`; the same fill with a linear gradient took
 * 2625 ms, because a gradient is evaluated per pixel -- an inverse transform, a search through the
 * stops, a lerp, and a `Premul` built and returned -- all in the interpreter. 56x, for a scene that
 * looks no more complicated.
 *
 * **The paint arrives as one flat `[float]` rather than a dozen parameters**, which is what makes
 * this extensible to the tiled and placed-image inks later without changing the signature:
 *
 *     [0]      kind: 0 linear, 1 radial
 *     [1..6]   the inverse matrix a, b, c, d, e, f
 *     [7..9]   linear: from.x, from.y, span     radial: centre.x, centre.y, radius
 *     [10..11] linear: dx, dy                   radial: unused
 *     [12]     the number of stops, n
 *     [13..]   n groups of five: offset, then premultiplied red, green, blue, alpha
 *
 * The row's device y is a parameter rather than part of the spec, because the spec is built once
 * per fill and y changes per row.
 *
 * Everything below is transcribed from `ready_at` and `ramp_at`: the clamp of the ramp position to
 * 0..1, the `<=` against the first stop, the search for the first stop at or past the position, the
 * zero-width guard, and the lerp written as `a + (b - a) * f` rather than the algebraically equal
 * `a * (1 - f) + b * f`, which rounds differently. Coverage accumulates into bytes, so those are
 * not free choices. */
static Value native_raster_composite_row_gradient(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t canvas_len = 0;
    double *data = ziman_mutable_array_float_data(args[0], &canvas_len);
    size_t cov_len = 0;
    const double *coverage = ziman_mutable_array_float_data(args[1], &cov_len);
    const ArrayObj *spec_arr = args[6].as.arr;
    const double *spec = spec_arr == NULL ? NULL : spec_arr->floats;
    if (data == NULL || coverage == NULL || spec == NULL) return value_unit();
    size_t spec_len = spec_arr->count < 0 ? 0 : (size_t)spec_arr->count;
    if (spec_len < 13) return value_unit();

    int64_t row_base = args[2].as.i;
    int64_t first_col = args[3].as.i;
    int64_t last_col = args[4].as.i;
    double row_y = args[5].as.f;
    const ArrayObj *mask_arr = args[7].as.arr;
    const double *mask = mask_arr == NULL ? NULL : mask_arr->floats;
    int has_mask = args[8].as.b != 0;
    if (has_mask && mask == NULL) return value_unit();
    size_t mask_len = (has_mask && mask_arr != NULL && mask_arr->count > 0)
                          ? (size_t)mask_arr->count : 0;

    int radial = spec[0] != 0.0;
    double ia = spec[1], ib = spec[2], ic = spec[3], id = spec[4], ie = spec[5], iff = spec[6];
    double g0 = spec[7], g1 = spec[8], g2 = spec[9], g3 = spec[10], g4 = spec[11];
    int64_t stops = (int64_t)spec[12];
    if (stops < 1 || spec_len < (size_t)(13 + stops * 5)) return value_unit();
    const double *st = spec + 13;

    for (int64_t x = first_col; x <= last_col; x++) {
        if (x < 0 || (size_t)x >= cov_len) continue;
        double cov = coverage[x];
        if (cov <= 0.002) continue;
        double through = 1.0;
        int64_t pi = row_base + x;
        if (pi < 0) continue;
        if (has_mask) {
            if ((size_t)pi >= mask_len) continue;
            through = mask[pi];
        }
        if (through <= 0.002) continue;

        /* The pixel's centre, through the inverse transform: `ready_at`'s own first step, and
         * `geom.transform_point` written out. */
        double px = (double)x + 0.5;
        double ux = ia * px + ic * row_y + ie;
        double uy = ib * px + id * row_y + iff;

        double t;
        if (radial) {
            double ddx = ux - g0;
            double ddy = uy - g1;
            t = sqrt(ddx * ddx + ddy * ddy) / g2;
        } else {
            t = ((ux - g0) * g3 + (uy - g1) * g4) / g2;
        }
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;

        double r, g, b, a;
        if (t <= st[0]) {
            r = st[1]; g = st[2]; b = st[3]; a = st[4];
        } else {
            int64_t i = 1;
            r = st[(stops - 1) * 5 + 1];
            g = st[(stops - 1) * 5 + 2];
            b = st[(stops - 1) * 5 + 3];
            a = st[(stops - 1) * 5 + 4];
            while (i < stops) {
                const double *lo = st + (i - 1) * 5;
                const double *hi = st + i * 5;
                if (t <= hi[0]) {
                    double width = hi[0] - lo[0];
                    double f = width > 0.0 ? (t - lo[0]) / width : 0.0;
                    r = lo[1] + (hi[1] - lo[1]) * f;
                    g = lo[2] + (hi[2] - lo[2]) * f;
                    b = lo[3] + (hi[3] - lo[3]) * f;
                    a = lo[4] + (hi[4] - lo[4]) * f;
                    break;
                }
                i++;
            }
        }

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

/* raster.composite_row_image(canvas, coverage, row_base, first_col, last_col, row_y, spec, pixels,
 * mask, has_mask): one scanline of a tiled pattern or a placed image.
 *
 * The last two inks that were still evaluated per pixel in the interpreter. Measured over a
 * 1280x800 canvas against the 46 ms a solid fill of the same shape takes: a pattern 2889 ms, a
 * placed image 2677 ms.
 *
 * **The spec is the gradient one extended, and the pixels are a separate argument** -- an image's
 * bytes cannot live in a `[float]`, which is the only reason this is a second function rather than
 * two more `kind` values on `composite_row_gradient`:
 *
 *     [0]      kind: 2 tiled, 3 placed
 *     [1..6]   the group's inverse matrix a, b, c, d, e, f
 *     [7..9]   the image's width, height and channel count
 *     tiled:   [10..15] the pattern's own inverse matrix
 *     placed:  [10..13] the destination rect x, y, width, height
 *
 * `sample_image` is transcribed exactly, including that the sampling is nearest-neighbour --
 * `raster.zm` calls that "honest and obviously improvable, where a half-finished bilinear would be
 * neither", and improving it here would make the two implementations disagree. The 1- and
 * 3-channel cases answer opaque; the 4-channel case premultiplies, because `image.Image` stores
 * un-premultiplied RGBA and the canvas is premultiplied. */
static Value native_raster_composite_row_image(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t canvas_len = 0;
    double *data = ziman_mutable_array_float_data(args[0], &canvas_len);
    size_t cov_len = 0;
    const double *coverage = ziman_mutable_array_float_data(args[1], &cov_len);
    const ArrayObj *spec_arr = args[6].as.arr;
    const double *spec = spec_arr == NULL ? NULL : spec_arr->floats;
    const ArrayObj *px_arr = args[7].as.arr;
    const uint8_t *pixels = px_arr == NULL ? NULL : px_arr->bytes;
    if (data == NULL || coverage == NULL || spec == NULL || pixels == NULL) return value_unit();
    if ((spec_arr->count < 0 ? 0 : (size_t)spec_arr->count) < 14) return value_unit();
    size_t px_len = px_arr->count < 0 ? 0 : (size_t)px_arr->count;

    int64_t row_base = args[2].as.i;
    int64_t first_col = args[3].as.i;
    int64_t last_col = args[4].as.i;
    double row_y = args[5].as.f;
    const ArrayObj *mask_arr = args[8].as.arr;
    const double *mask = mask_arr == NULL ? NULL : mask_arr->floats;
    int has_mask = args[9].as.b != 0;
    if (has_mask && mask == NULL) return value_unit();
    size_t mask_len = (has_mask && mask_arr != NULL && mask_arr->count > 0)
                          ? (size_t)mask_arr->count : 0;

    int placed = spec[0] == 3.0;
    double ia = spec[1], ib = spec[2], ic = spec[3], id = spec[4], ie = spec[5], iff = spec[6];
    int64_t iw = (int64_t)spec[7], ih = (int64_t)spec[8], chan = (int64_t)spec[9];
    if (iw < 1 || ih < 1 || (chan != 1 && chan != 3 && chan != 4)) return value_unit();

    for (int64_t x = first_col; x <= last_col; x++) {
        if (x < 0 || (size_t)x >= cov_len) continue;
        double cov = coverage[x];
        if (cov <= 0.002) continue;
        int64_t pi = row_base + x;
        if (pi < 0) continue;
        double through = 1.0;
        if (has_mask) {
            if ((size_t)pi >= mask_len) continue;
            through = mask[pi];
        }
        if (through <= 0.002) continue;

        double px = (double)x + 0.5;
        double ux = ia * px + ic * row_y + ie;
        double uy = ib * px + id * row_y + iff;

        double u, v;
        if (placed) {
            u = (ux - spec[10]) / spec[12];
            v = (uy - spec[11]) / spec[13];
        } else {
            /* A pattern has its own transform as well as the group's, applied after it. */
            double lx = spec[10] * ux + spec[12] * uy + spec[14];
            double ly = spec[11] * ux + spec[13] * uy + spec[15];
            u = lx / (double)iw;
            v = ly / (double)ih;
        }

        int64_t sx = (int64_t)floor(u * (double)iw);
        int64_t sy = (int64_t)floor(v * (double)ih);
        if (sx < 0) sx = 0;
        if (sx > iw - 1) sx = iw - 1;
        if (sy < 0) sy = 0;
        if (sy > ih - 1) sy = ih - 1;
        size_t at = (size_t)((sy * iw + sx) * chan);
        if (at + (size_t)chan > px_len) continue;

        double r, g, b, a;
        if (chan == 1) {
            double grey = (double)pixels[at] / 255.0;
            r = grey; g = grey; b = grey; a = 1.0;
        } else if (chan == 3) {
            r = (double)pixels[at] / 255.0;
            g = (double)pixels[at + 1] / 255.0;
            b = (double)pixels[at + 2] / 255.0;
            a = 1.0;
        } else {
            a = (double)pixels[at + 3] / 255.0;
            r = (double)pixels[at] / 255.0 * a;
            g = (double)pixels[at + 1] / 255.0 * a;
            b = (double)pixels[at + 2] / 255.0 * a;
        }

        double weight = cov * through;
        size_t to = (size_t)pi * CHANNELS;
        if (to + 3 >= canvas_len) continue;
        double keep = 1.0 - a * weight;
        data[to + 0] = r * weight + data[to + 0] * keep;
        data[to + 1] = g * weight + data[to + 1] * keep;
        data[to + 2] = b * weight + data[to + 2] * keep;
        data[to + 3] = a * weight + data[to + 3] * keep;
    }
    return value_unit();
}

/* raster.row_coverage_raw(row, width, edges, live, winding, samples, y, from_x, to_x, shift): one
 * scanline's anti-aliased coverage, the whole of it.
 *
 * **The item F270 measured its way to.** Two Ziman-level attempts on this loop -- one removing a
 * divide and three comparisons per crossing, one removing 1,600 builder allocations per fill --
 * each measured at 1.00x over three interleaved passes. In a tree-walking interpreter the work here
 * is not the arithmetic or the allocation, it is the per-operation overhead of doing anything at
 * all a few million times, and the only way to remove that is to stop interpreting. Which is also
 * why this takes the *whole* row rather than a piece of it: `crossings_at`, the insertion sort,
 * `spans_from` and the `add_span` per span were four interpreted calls deep per sub-scanline, and
 * splitting them differently would have left the dispatch in place.
 *
 * `edges` is four floats an edge (x0, y0, x1, y1) and `live` the indices of the edges this row can
 * touch, rebuilt per row by `advance_active` and read in place here -- a copy per row would cost
 * more than the walk. `winding` is 0 for non-zero and 1 for even-odd, because an enum does not
 * cross.
 *
 * The crossings are insertion-sorted into a fixed buffer. A scanline crosses a handful of edges
 * even on a complex path, so the cap is generous -- but it is **checked and raised as an error**,
 * not clamped. The Ziman implementation this replaces built the crossing list in a growable array
 * and so had no limit at all; dropping the crossings past the cap would silently draw a wrong
 * shape, which is the one outcome worse than refusing, because no fixture and no reader would see
 * it. A path that genuinely needs more wants this number raised, and the error says so by name. */
#define ROW_MAX_CROSSINGS 1024

static Value native_raster_row_coverage_raw(Value *args, int argc, int line, int col) {
    (void)argc;
    size_t row_len = 0;
    double *row = ziman_mutable_array_float_data(args[0], &row_len);
    if (row == NULL) return value_unit();
    int64_t width = args[1].as.i;
    if (width > (int64_t)row_len) width = (int64_t)row_len;
    if (width <= 0) return value_unit();

    const ArrayObj *edge_arr = args[2].as.arr;
    const double *edges = edge_arr == NULL ? NULL : edge_arr->floats;
    int64_t edge_floats = edge_arr == NULL ? 0 : (int64_t)edge_arr->count;

    size_t live_len = 0;
    const int64_t *live = ziman_mutable_array_int_data(args[3], &live_len);

    int64_t winding = args[4].as.i;
    int64_t samples = args[5].as.i;
    int64_t y = args[6].as.i;
    int64_t from_x = args[7].as.i;
    int64_t to_x = args[8].as.i;
    int64_t shift = args[9].as.i;

    /* The window the shape can touch, cleared exactly as `clear_range` does: every crossing comes
     * from an edge and every edge lies within the list's own x extent, so no span can write
     * outside it. */
    if (from_x < 0) from_x = 0;
    if (to_x > width - 1) to_x = width - 1;
    for (int64_t i = from_x; i <= to_x; i++) row[i] = 0.0;

    if (edges == NULL || live == NULL || samples <= 0) return value_unit();

    const double weight = 1.0 / (double)samples;
    double xs[ROW_MAX_CROSSINGS];
    int dirs[ROW_MAX_CROSSINGS];

    for (int64_t s = 0; s < samples; s++) {
        const double sy = (double)y + ((double)s + 0.5) * weight;
        int n = 0;
        for (size_t at = 0; at < live_len; at++) {
            const int64_t e = live[at] * 4;
            if (e < 0 || e + 3 >= edge_floats) continue;
            const double x0 = edges[e], y0 = edges[e + 1], x1 = edges[e + 2], y1 = edges[e + 3];
            const double low = y0 < y1 ? y0 : y1;
            const double high = y0 < y1 ? y1 : y0;
            /* **Half-open on purpose: `low <= sy < high`.** It is what stops a vertex shared by
             * two edges being counted twice, and getting it wrong produces a one-pixel seam along
             * every shared boundary -- which looks like a rounding problem and is not one. This
             * sentence was the doc comment of the Ziman `crossings_at` that this replaced; it is
             * repeated here because the reasoning is the part a port loses, and because
             * `tests/lang/graphics_text.zm` is the *only* fixture that catches `<` becoming `<=`
             * (glyph outlines are the only contours here that meet on a scanline). */
            if (!(sy >= low && sy < high)) continue;
            const double x = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);
            const int dir = y1 > y0 ? 1 : -1;
            if (n >= ROW_MAX_CROSSINGS) {
                ziman_error("runtime", line, col,
                            "raster.row_coverage_raw: more than %d edge crossings on one "
                            "sub-scanline (y=%lld); raise ROW_MAX_CROSSINGS in src/native_raster.c",
                            ROW_MAX_CROSSINGS, (long long)y);
            }
            /* Insertion sort, keeping the two arrays parallel -- as the Ziman original did, and for
             * the same reason: a handful of crossings beats a comparator over a struct. */
            int k = n;
            while (k > 0 && xs[k - 1] > x) {
                xs[k] = xs[k - 1];
                dirs[k] = dirs[k - 1];
                k--;
            }
            xs[k] = x;
            dirs[k] = dir;
            n++;
        }

        /* Both winding rules out of the one crossing list, which is why they are together: they
         * differ only in the predicate that opens and closes a span. */
        int inside = 0;
        double start = 0.0;
        for (int i = 0; i < n; i++) {
            const int was_inside = winding == 0 ? (inside != 0) : (inside % 2 != 0);
            inside = winding == 0 ? inside + dirs[i] : inside + 1;
            const int now_inside = winding == 0 ? (inside != 0) : (inside % 2 != 0);
            if (!was_inside && now_inside) {
                start = xs[i];
            } else if (was_inside && !now_inside) {
                /* `add_span`'s body, inlined: it was a native call per span, and a span is the unit
                 * this loop produces most of. */
                double xa = start - (double)shift;
                double xb = xs[i] - (double)shift;
                if (xa < 0.0) xa = 0.0;
                if (xb > (double)width) xb = (double)width;
                if (xb <= xa) continue;
                int64_t ia = (int64_t)floor(xa);
                int64_t ib = (int64_t)floor(xb);
                if (ia < 0) ia = 0;
                if (ia > width - 1) ia = width - 1;
                if (ib < 0) ib = 0;
                if (ib > width - 1) ib = width - 1;
                if (ia == ib) {
                    row[ia] += (xb - xa) * weight;
                    continue;
                }
                row[ia] += ((double)(ia + 1) - xa) * weight;
                for (int64_t k = ia + 1; k < ib; k++) row[k] += weight;
                row[ib] += (xb - (double)ib) * weight;
            }
        }
    }
    return value_unit();
}

void native_raster_register(void) {
    native_register("raster::clear_premul", native_raster_clear_premul);
    native_register("raster::to_bytes_premul", native_raster_to_bytes_premul);
    native_register("raster::composite_row_flat", native_raster_composite_row_flat);
    native_register("raster::composite_span_flat", native_raster_composite_span_flat);
    native_register("raster::clear_range", native_raster_clear_range);
    native_register("raster::add_span", native_raster_add_span);
    native_register("raster::row_coverage_raw", native_raster_row_coverage_raw);
    native_register("raster::composite_row_gradient", native_raster_composite_row_gradient);
    native_register("raster::composite_row_image", native_raster_composite_row_image);
}
