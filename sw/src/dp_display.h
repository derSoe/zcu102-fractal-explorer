/*
 * DisplayPort output (PS DP + DPDMA) for the fractal demo.
 * Based on the AMD example xdpdma_video_example (MIT, (C) Xilinx/AMD).
 */
#ifndef DP_DISPLAY_H_
#define DP_DISPLAY_H_

#include "xil_types.h"

#define DP_NUM_BUFFERS  4   /* 4: the most recently replaced buffer is still shown for one frame */
#define DP_MAX_W        1920
#define DP_MAX_H        1080

/* Initialises the MMU region, the DP subsystem and the interrupts. The connection to
 * the monitor (link training) starts automatically on the hot-plug event. */
int  dp_init(void);

/* Current output resolution (1920x1080, fallback 1280x720 on a slow link) */
int  dp_width(void);
int  dp_height(void);
int  dp_link_up(void);
void dp_diag(void);              /* DP diagnostics to the console */

/* Frame buffer i (0..DP_NUM_BUFFERS-1), non-cacheable, line stride = width */
u32 *dp_buffer(int i);

/* Queue a frame buffer for display at the next refresh */
void dp_show(u32 *fb);

/* Waits until the DPDMA has taken over the most recently queued buffer
 * (max. timeout_ms). Returns 1 = taken over, 0 = timeout. */
int  dp_wait_flip(int timeout_ms);

#endif
