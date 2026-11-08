// Testbench: compares the HLS kernel (fixed point) with a bit-exact golden model and a
// double reference, and writes the images as PPM for visual inspection.
#include "fractal.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

// Truncate to the kernel's coordinate width (two's complement, like ap_int<COORD_W>)
static int64_t wrap_q(__int128 v) {
    const int sh = 64 - COORD_W;
    return (int64_t)((uint64_t)(int64_t)v << sh) >> sh;
}
// One iteration step z -> z^2 + c in fixed point; true if |z|^2 > 4 (escaped)
static bool fx_step(int64_t &zx, int64_t &zy, int64_t cx, int64_t cy) {
    __int128 zx2 = ((__int128)zx * zx) >> FRAC;
    __int128 zy2 = ((__int128)zy * zy) >> FRAC;
    if (zx2 + zy2 > ((__int128)4 << FRAC)) return true;
    __int128 zxy = ((__int128)zx * zy) >> (FRAC - 1);
    zx = wrap_q(zx2 - zy2 + cx);
    zy = wrap_q(zxy + cy);
    return false;
}

// Golden model, bit-exact in fixed point Q4.36 (40 bits). Every optimised version of the
// kernel must deliver exactly the same pixels. Since periodicity detection was added
// (period=true) it is part of the algorithm; period=false gives plain escape time (info only).
static void fractal_golden(uint32_t *fb, int w, int h, int64_t x0, int64_t y0, int64_t step,
                           int max_iter, int mode, int64_t jx, int64_t jy, int pal_shift,
                           bool period = true) {
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            int64_t cx = x0 + px * step, cy = y0 + py * step, zx = 0, zy = 0;
            if (mode == MODE_JULIA) { zx = cx; zy = cy; cx = jx; cy = jy; }
            int it = (mode != MODE_JULIA && in_main_bulbs(cx, cy)) ? max_iter : 0;
            int64_t rx = zx, ry = zy;
            while (it < max_iter) {
                if (period && it > 0 && period_close(zx, zy, rx, ry)) { it = max_iter; break; }
                if (is_pow2_or_0(it)) { rx = zx; ry = zy; }
                if (fx_step(zx, zy, cx, cy)) break;
                it++;
            }
            fb[py * w + px] = palette(it, max_iter, pal_shift);
        }
}

// Reference in double (for information on the fixed-point precision only;
// also the basis of the CPU comparison on the A53)
static void fractal_ref(uint32_t *fb, int w, int h, double x0, double y0, double step,
                        int max_iter, int mode, double jx, double jy, int pal_shift) {
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            double cx = x0 + px * step, cy = y0 + py * step, zx = 0, zy = 0;
            if (mode == MODE_JULIA) { zx = cx; zy = cy; cx = jx; cy = jy; }
            int it = 0;
            while (it < max_iter) {
                double zx2 = zx * zx, zy2 = zy * zy;
                if (zx2 + zy2 > 4.0) break;
                zy = 2 * zx * zy + cy;
                zx = zx2 - zy2 + cx;
                it++;
            }
            fb[py * w + px] = palette(it, max_iter, pal_shift);
        }
}

static void write_ppm(const char *name, const uint32_t *fb, int w, int h) {
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

struct View { const char *name; double cx, cy, width; int max_iter, mode; double jx, jy; };

int main() {
    const int W = 320, H = 180;
    const View views[] = {
        {"mandel_full",    -0.6,     0.0,     3.6,    256, MODE_MANDELBROT, 0, 0},
        {"mandel_seahorse",-0.7453,  0.1127,  0.01,   512, MODE_MANDELBROT, 0, 0},
        {"julia",           0.0,     0.0,     3.2,    256, MODE_JULIA, -0.8, 0.156},
        // medium zoom with a lot of interior area (cardioid + bulb in the image)
        {"mandel_interior",-0.7436,  0.1318,  0.3,    384, MODE_MANDELBROT, 0, 0},
        // mini Mandelbrot (zoom ~140): many interior points outside the main body ->
        // this is where periodicity detection kicks in
        {"mandel_mini",    -1.768778833, -0.001738996, 0.025, 528, MODE_MANDELBROT, 0, 0},
        // zoom ~1e6 (image width 3.6e-6): not representable in Q4.28 (pixel step < 1 LSB)
        {"mandel_deep",    -0.743643887037151, 0.131825904205330, 3.6e-6, 1024, MODE_MANDELBROT, 0, 0},
    };
    std::vector<uint32_t> plain(MAX_W * MAX_H);
    // Full image size: the m_axi interface declares depth=MAX_W*MAX_H, the
    // cosim wrapper copies that many words.
    std::vector<uint32_t> hw(MAX_W * MAX_H), gold(MAX_W * MAX_H), ref(MAX_W * MAX_H);
    int fails = 0;

    for (const View &v : views) {
        double step = v.width / W;
        double x0 = v.cx - step * W / 2, y0 = v.cy - step * H / 2;
        fractal(hw.data(), W, H, to_q(x0), to_q(y0), to_q(step), v.max_iter, v.mode,
                to_q(v.jx), to_q(v.jy), 0);
        // compute the reference with the same quantised start values
        const double q = 1.0 / (double)(1LL << FRAC);
        fractal_ref(ref.data(), W, H, to_q(x0) * q, to_q(y0) * q, to_q(step) * q, v.max_iter, v.mode,
                    to_q(v.jx) * q, to_q(v.jy) * q, 0);

        fractal_golden(gold.data(), W, H, to_q(x0), to_q(y0), to_q(step), v.max_iter, v.mode,
                       to_q(v.jx), to_q(v.jy), 0);
        fractal_golden(plain.data(), W, H, to_q(x0), to_q(y0), to_q(step), v.max_iter, v.mode,
                       to_q(v.jx), to_q(v.jy), 0, false);
        int pdiff = 0;
        for (int i = 0; i < W * H; i++) pdiff += (gold[i] != plain[i]);
        printf("%-16s periodicity detection: %d pixels differ from plain escape time\n", v.name, pdiff);

        // Plausibility of the interior test: points classified as "inside" must
        // also reach max_iter without the test (otherwise the test would not be conservative)
        int inner = 0, wrong_inner = 0;
        if (v.mode == MODE_MANDELBROT) {
            for (int py = 0; py < H; py++) for (int px = 0; px < W; px++) {
                int64_t cx = to_q(x0) + px * to_q(step), cy = to_q(y0) + py * to_q(step);
                if (!in_main_bulbs(cx, cy)) continue;
                inner++;
                int64_t zx = 0, zy = 0; int it = 0;
                while (it < v.max_iter) {
                    if (fx_step(zx, zy, cx, cy)) break;
                    it++;
                }
                if (it < v.max_iter) wrong_inner++;
            }
            printf("%-16s interior test: %d of %d pixels skipped, %d misclassified\n",
                   v.name, inner, W * H, wrong_inner);
            if (wrong_inner) fails++;
        }

        int bad = 0, diff = 0;
        for (int i = 0; i < W * H; i++) { bad += (hw[i] != gold[i]); diff += (hw[i] != ref[i]); }
        bool ok = (bad == 0);
        // fixed point vs. double: deviations at chaotic boundary points are normal
        printf("%-16s bit-exact vs. golden: %s (%d pixels wrong) | info vs. double: %.2f %% boundary pixels differ\n",
               v.name, ok ? "OK" : "ERROR", bad, 100.0 * diff / (W * H));
        if (!ok) fails++;

        char fn[64];
        snprintf(fn, sizeof fn, "%s_hw.ppm", v.name);  write_ppm(fn, hw.data(), W, H);
        snprintf(fn, sizeof fn, "%s_ref.ppm", v.name); write_ppm(fn, ref.data(), W, H);
    }
    printf(fails ? "TEST FAILED\n" : "TEST PASSED\n");
    return fails;
}
