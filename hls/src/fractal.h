// Fractal generator for ZCU102 - shared definitions
// Coordinates are fixed point Q4.36 (40 bits, int64_t in software): range +/-8,
// resolution ~1.5e-11 (previously Q4.28 in 32 bits)
#pragma once
#include <cstdint>

constexpr int MAX_W    = 1920;
constexpr int MAX_H    = 1080;
constexpr int FRAC     = 36;          // fractional bits
constexpr int COORD_W  = 40;          // width of a coordinate in the kernel
constexpr int MAX_ITER = 1024;        // upper limit for max_iter

enum FractalMode { MODE_MANDELBROT = 0, MODE_JULIA = 1 };

// converts double to Q4.36
inline int64_t to_q(double v) { return (int64_t)(v * (double)(1LL << FRAC)); }

// Hardware kernel: renders a complete image into the frame buffer (0x00RRGGBB)
//   fb        : frame buffer, width*height pixels, rows back to back
//   x0, y0    : coordinate of the top-left pixel (Q4.36)
//   step      : distance between two pixels (Q4.36)
//   max_iter  : maximum iterations (1..MAX_ITER)
//   mode      : MODE_MANDELBROT or MODE_JULIA
//   jx, jy    : Julia constant c (Q4.36), MODE_JULIA only
//   pal_shift : palette offset (colour animation)
void fractal(uint32_t *fb, int width, int height,
             int64_t x0, int64_t y0, int64_t step,
             int max_iter, int mode, int64_t jx, int64_t jy, int pal_shift);

// Colour palette: iteration count -> 0x00RRGGBB (black for points in the set)
inline uint32_t palette(int it, int max_iter, int pal_shift) {
    if (it >= max_iter) return 0;
    static const uint8_t key[8][3] = {
        {  0,   7, 100}, { 32, 107, 203}, {237, 255, 255}, {255, 170,   0},
        {200,  60,   0}, {120,   0,  40}, { 60,   0, 120}, {  0,   2,  40}};
    int t   = (it * 4 + pal_shift) & 255;
    int seg = t >> 5;
    int f   = t & 31;
    int nx  = (seg + 1) & 7;
    uint32_t r = (key[seg][0] * (32 - f) + key[nx][0] * f) >> 5;
    uint32_t g = (key[seg][1] * (32 - f) + key[nx][1] * f) >> 5;
    uint32_t b = (key[seg][2] * (32 - f) + key[nx][2] * f) >> 5;
    return (r << 16) | (g << 8) | b;
}

// Interior test (Mandelbrot only): if c lies safely inside the main cardioid or the
// period-2 bulb, the result is max_iter without iterating (point belongs to the set).
//   cardioid: q*(q + x - 1/4) < y^2/4  with q = (x - 1/4)^2 + y^2
//   bulb:     (x + 1)^2 + y^2 < 1/16
// Computed in Q.16 (18-bit operands -> one DSP each in the kernel) with safety margin
// TEST_MARGIN, so rounding never classifies a point outside the set as "inside".
// Reference for kernel, golden model and CPU version (all bit-identical).
constexpr int TEST_MARGIN = 64;       // in Q.16, ~1e-3
inline bool in_main_bulbs(int64_t cx, int64_t cy) {
    // extent of both regions: x in [-1.25, 0.5], |y| <= 0.75
    if (cx < -(5LL << (FRAC - 2)) || cx > (1LL << (FRAC - 1)) ||
        cy < -(3LL << (FRAC - 2)) || cy > (3LL << (FRAC - 2))) return false;
    const int64_t one = 1 << 16;
    int64_t a    = cx >> (FRAC - 16), b = cy >> (FRAC - 16);
    int64_t y2   = (b * b) >> 16;
    int64_t xq   = a - (one >> 2);
    int64_t q    = ((xq * xq) >> 16) + y2;
    int64_t lhs  = (q * (q + xq)) >> 16;
    int64_t xb   = a + one;
    int64_t bulb = ((xb * xb) >> 16) + y2;
    return (lhs + TEST_MARGIN < (y2 >> 2)) || (bulb + TEST_MARGIN < (one >> 4));
}

// Periodicity detection (Brent): at every count 2^k (and at 0) z is stored as reference.
// If z later comes back to within PERIOD_TOL of the reference (per component, in LSBs of
// the coordinate), the orbit is cycling -> point belongs to the set, result max_iter.
// The check runs at the start of each iteration on the current z (before the multiply),
// so no multiplier sits in that path in the kernel.
// Tolerance 255: in the PC experiment practically no difference to plain escape time.
constexpr int PERIOD_TOL = 255;
inline bool period_close(int64_t zx, int64_t zy, int64_t rx, int64_t ry) {
    int64_t dx = (int64_t)zx - rx, dy = (int64_t)zy - ry;
    return dx >= -PERIOD_TOL && dx <= PERIOD_TOL && dy >= -PERIOD_TOL && dy <= PERIOD_TOL;
}
inline bool is_pow2_or_0(int it) { return (it & (it - 1)) == 0; }
