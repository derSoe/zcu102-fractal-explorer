/* Fractal in software on the ARM Cortex-A53 (CPU <-> FPGA comparison).
 * Algorithm and palette are taken 1:1 from hls/src/fractal.h and the golden model
 * of the testbench (bit-exact same image as the HLS kernel). */
#include "fractal_cpu.h"

/* Interior test, 1:1 from in_main_bulbs() in hls/.../src/fractal.h */
#define TEST_MARGIN 64
static int in_main_bulbs(s64 cx, s64 cy)
{
	if (cx < -(5LL << (FRAC - 2)) || cx > (1LL << (FRAC - 1)) ||
	    cy < -(3LL << (FRAC - 2)) || cy > (3LL << (FRAC - 2))) return 0;
	const s64 one = 1 << 16;
	s64 a    = cx >> (FRAC - 16), b = cy >> (FRAC - 16);
	s64 y2   = (b * b) >> 16;
	s64 xq   = a - (one >> 2);
	s64 q    = ((xq * xq) >> 16) + y2;
	s64 lhs  = (q * (q + xq)) >> 16;
	s64 xb   = a + one;
	s64 bulb = ((xb * xb) >> 16) + y2;
	return (lhs + TEST_MARGIN < (y2 >> 2)) || (bulb + TEST_MARGIN < (one >> 4));
}


/* Periodicity detection, 1:1 from period_close()/PERIOD_TOL in hls/.../src/fractal.h */
#define PERIOD_TOL 255
static int period_close(s64 zx, s64 zy, s64 rx, s64 ry)
{
	s64 dx = (s64)zx - rx, dy = (s64)zy - ry;
	return dx >= -PERIOD_TOL && dx <= PERIOD_TOL && dy >= -PERIOD_TOL && dy <= PERIOD_TOL;
}

/* Truncate to the kernel's coordinate width (like ap_int<40> in the HLS kernel) */
static inline s64 wrap_q(__int128 v)
{
	const int sh = 64 - COORD_W;
	return (s64)((u64)(s64)v << sh) >> sh;
}

static u32 palette(int it, int max_iter, int pal_shift)
{
	static const u8 key[8][3] = {
		{  0,   7, 100}, { 32, 107, 203}, {237, 255, 255}, {255, 170,   0},
		{200,  60,   0}, {120,   0,  40}, { 60,   0, 120}, {  0,   2,  40}};
	if (it >= max_iter) return 0;
	int t   = (it * 4 + pal_shift) & 255;
	int seg = t >> 5;
	int f   = t & 31;
	int nx  = (seg + 1) & 7;
	u32 r = (u32)(key[seg][0] * (32 - f) + key[nx][0] * f) >> 5;
	u32 g = (u32)(key[seg][1] * (32 - f) + key[nx][1] * f) >> 5;
	u32 b = (u32)(key[seg][2] * (32 - f) + key[nx][2] * f) >> 5;
	return (r << 16) | (g << 8) | b;
}

void fractal_cpu_rows(u32 *fb, const fractal_params_t *p, int y_begin, int y_end)
{
	for (int py = y_begin; py < y_end; py++) {
		u32 *row = fb + (long)py * p->width;
		for (int px = 0; px < p->width; px++) {
			s64 cx = p->x0 + px * p->step;
			s64 cy = p->y0 + py * p->step;
			s64 zx = 0, zy = 0;
			if (p->mode == MODE_JULIA) { zx = cx; zy = cy; cx = p->jx; cy = p->jy; }
			int it = (p->mode != MODE_JULIA && in_main_bulbs(cx, cy)) ? p->max_iter : 0;
			s64 rx = zx, ry = zy;
			while (it < p->max_iter) {
				if (it > 0 && period_close(zx, zy, rx, ry)) { it = p->max_iter; break; }
				if ((it & (it - 1)) == 0) { rx = zx; ry = zy; }
				__int128 zx2 = ((__int128)zx * zx) >> FRAC;
				__int128 zy2 = ((__int128)zy * zy) >> FRAC;
				if (zx2 + zy2 > ((__int128)4 << FRAC)) break;
				__int128 zxy = ((__int128)zx * zy) >> (FRAC - 1);
				zx = wrap_q(zx2 - zy2 + cx);
				zy = wrap_q(zxy + cy);
				it++;
			}
			row[px] = palette(it, p->max_iter, p->pal_shift);
		}
	}
}

