/* Fractal in software on the ARM Cortex-A53 (CPU <-> FPGA comparison).
 * Same algorithm as the HLS kernel (fixed point Q4.36 in 40 bits, same palette),
 * so both paths deliver a pixel-identical image. */
#ifndef FRACTAL_CPU_H_
#define FRACTAL_CPU_H_

#include "xil_types.h"

#define FRAC      36
#define COORD_W   40          /* width of a coordinate in the kernel */
#define MODE_MANDELBROT 0
#define MODE_JULIA      1

typedef struct {
	int width, height;
	s64 x0, y0, step;       /* Q4.36, values fit into 40 bits */
	int max_iter, mode;
	s64 jx, jy;
	int pal_shift;
} fractal_params_t;

/* Renders rows [y_begin, y_end) into fb (line stride = width) */
void fractal_cpu_rows(u32 *fb, const fractal_params_t *p, int y_begin, int y_end);

#endif
