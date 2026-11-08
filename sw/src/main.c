/*
 * Fractal demo ZCU102 - bare-metal application for Cortex-A53 #0
 *
 * The HLS kernel (64 compute engines) renders Mandelbrot/Julia images at 1080p into a
 * frame buffer in DDR, which the PS DisplayPort displays. A DIP switch runs the same
 * algorithm on one A53 core instead (CPU comparison).
 *
 * Controls
 *   D-pad N/S/E/W             : pan the image (switches autopilot off)
 *   hold centre + N / S       : zoom in / zoom out
 *   hold centre + E / W       : next / previous zoom target
 *   tap centre briefly        : autopilot on/off
 *   DIP 1            : Julia instead of Mandelbrot
 *   DIP 2            : compute on the CPU instead of the FPGA
 *   DIP 3            : colour animation
 *   DIP 4            : hide the overlay (HUD)
 *   DIP 5+6          : iterations: off/off automatic, on/off 256, off/on 512, on/on 1024
 *   DIP 7            : animate the Julia constant (slower the deeper the zoom)
 * Autopilot in Julia mode: zoom flight towards a point on the set (pre-image of the
 * repelling fixed point), which follows the constant.
 *
 * Button and DIP mapping verified on the board (08 Nov). The DIP bits arrive mirrored
 * (switch 1 = bit 7) and are reversed when read; HUD/UART show the reversed value.
 */
#include "xparameters.h"
#include "xil_printf.h"
#include "xil_io.h"
#include "xiltimer.h"
#include "sleep.h"
#include "dp_display.h"
#include "hud.h"
#include "fractal_cpu.h"

/* ---- Addresses (from the block design) ----------------------------------- */
#ifdef XPAR_GPIO_IN_BASEADDR
#define GPIO_IN_BASE   XPAR_GPIO_IN_BASEADDR
#else
#define GPIO_IN_BASE   0xA0000000u
#endif
#ifdef XPAR_GPIO_LED_BASEADDR
#define GPIO_LED_BASE  XPAR_GPIO_LED_BASEADDR
#else
#define GPIO_LED_BASE  0xA0010000u
#endif
#ifdef XPAR_FRACTAL_0_BASEADDR
#define FRACTAL_BASE   XPAR_FRACTAL_0_BASEADDR
#else
#define FRACTAL_BASE   0xA0020000u
#endif

/* Register offsets of the HLS kernel (from xfractal_hw.h of the exported IP) */
#define F_AP_CTRL    0x00
#define F_FB_LO      0x10
#define F_FB_HI      0x14
#define F_WIDTH      0x1c
#define F_HEIGHT     0x24
#define F_X0         0x2c
#define F_Y0         0x38
#define F_STEP       0x44
#define F_MAX_ITER   0x50
#define F_MODE       0x58
#define F_JX         0x60
#define F_JY         0x6c
#define F_PAL_SHIFT  0x78

/* AXI GPIO: channel 1 = buttons (5 bits), channel 2 = DIP (8 bits) */
#define GPIO_DATA1   0x00
#define GPIO_DATA2   0x08

/* determined on the board (08 Nov): SW5 centre, SW8 up, SW6 down, SW7 right, SW4 left */
#define BTN_C  0x01   /* SW5 centre */
#define BTN_W  0x02   /* SW4 left   */
#define BTN_S  0x04   /* SW6 down   */
#define BTN_E  0x08   /* SW7 right  */
#define BTN_N  0x10   /* SW8 up     */

#define DIP_JULIA    0x01
#define DIP_CPU      0x02
#define DIP_ANIM     0x04
#define DIP_NOHUD    0x08
#define DIP_ITER0    0x10
#define DIP_ITER1    0x20
#define DIP_JANIM    0x40

/* Read the DIP switches; the bits arrive mirrored (switch 1 = bit 7) */
static u32 read_dip(void)
{
	u32 hw = Xil_In32(GPIO_IN_BASE + GPIO_DATA2) & 0xFF, d = 0;
	for (int i = 0; i < 8; i++) if (hw & (1u << i)) d |= 0x80u >> i;
	return d;
}

/* Square root without libm (Newton, accurate enough for 0 <= v < 100) */
static double sqrt_d(double v)
{
	if (v <= 0.0) return 0.0;
	double x = v > 1.0 ? v : 1.0;
	for (int i = 0; i < 40; i++) x = 0.5 * (x + v / x);
	return x;
}

/* Target point of the Julia zoom flight: pre-image z = sqrt(-beta - c) of the repelling
   fixed point beta = 1/2 + sqrt(1/4 - c). Always lies on the Julia set. */
static void csqrt_d(double a, double b, double *x, double *y)
{
	double r = sqrt_d(a * a + b * b);
	*x = sqrt_d((r + a) / 2.0); *y = sqrt_d((r - a) / 2.0);
	if (b < 0.0) *y = -*y;
}

static void julia_target(double jcx, double jcy, double *tx, double *ty)
{
	double bx, by;
	csqrt_d(0.25 - jcx, -jcy, &bx, &by); bx += 0.5;
	csqrt_d(-bx - jcx, -by - jcy, tx, ty);
}

/* ---- Zoom targets --------------------------------------------------------- */
typedef struct { const char *name; double cx, cy; } target_t;
static const target_t targets[] = {
	{"SEAHORSE VALLEY", -0.743643887037151,  0.131825904205330},
	{"ELEPHANT VALLEY",  0.285246693732915,  0.012126870104112},
	{"SPIRAL",          -0.761587984248763, -0.084857729802025},
	{"MINI MANDELBROT", -1.768778833000000, -0.001738996000000},
	{"DOUBLE SPIRAL",   -0.101096363845740,  0.956286510809140},
};
#define N_TARGETS ((int)(sizeof(targets) / sizeof(targets[0])))

/* Image centre without panning: Mandelbrot overview or zoom target; Julia glides
   with the zoom from the origin to the target point on the set */
static void auto_center(int julia, int target, double zoom, double jcx, double jcy,
                        double *cx, double *cy)
{
	if (julia) {
		double tx, ty;
		julia_target(jcx, jcy, &tx, &ty);
		*cx = tx * (1.0 - 1.0 / zoom); *cy = ty * (1.0 - 1.0 / zoom);
	} else if (zoom < 1.5) { *cx = -0.6; *cy = 0.0; }   /* overview, centred */
	else { *cx = targets[target].cx; *cy = targets[target].cy; }
}

#define BASE_WIDTH_M   3.6        /* image width in the complex plane at zoom 1 */
#define BASE_WIDTH_J   3.2
#define MIN_STEP_RAW   32         /* smallest pixel step in Q4.36: rounding steps <= 3 % */

static double fix_scale(void) { return (double)(1LL << FRAC); }
static s64 to_q(double v)     { double q = v * fix_scale(); return (s64)(q >= 0 ? q + 0.5 : q - 0.5); }

/* 64-bit kernel registers (two 32-bit words, low word first) */
static void out64(UINTPTR a, s64 v) { Xil_Out32(a, (u32)v); Xil_Out32(a + 4, (u32)((u64)v >> 32)); }

static int ilog2(double z) { int n = 0; while (z >= 2.0) { z *= 0.5; n++; } return n; }

/* ---- FPGA kernel ---------------------------------------------------------- */
static int fpga_render(u32 *fb, const fractal_params_t *p)
{
	UINTPTR b = FRACTAL_BASE;
	u64 a = (u64)(UINTPTR)fb;
	Xil_Out32(b + F_FB_LO, (u32)a);
	Xil_Out32(b + F_FB_HI, (u32)(a >> 32));
	Xil_Out32(b + F_WIDTH, (u32)p->width);
	Xil_Out32(b + F_HEIGHT, (u32)p->height);
	out64(b + F_X0, p->x0);
	out64(b + F_Y0, p->y0);
	out64(b + F_STEP, p->step);
	Xil_Out32(b + F_MAX_ITER, (u32)p->max_iter);
	Xil_Out32(b + F_MODE, (u32)p->mode);
	out64(b + F_JX, p->jx);
	out64(b + F_JY, p->jy);
	Xil_Out32(b + F_PAL_SHIFT, (u32)p->pal_shift);
	Xil_Out32(b + F_AP_CTRL, 0x1);                          /* ap_start */

	XTime t0, t;
	XTime_GetTime(&t0);
	while (!(Xil_In32(b + F_AP_CTRL) & 0x2)) {              /* ap_done */
		XTime_GetTime(&t);
		if (t - t0 > (XTime)COUNTS_PER_SECOND * 2) return -1;
	}
	return 0;
}

/* ---- HUD ------------------------------------------------------------------ */
typedef struct {
	int cpu, julia, autopilot, max_iter;
	double zoom;
	unsigned long fps_x10, render_ms_x10;
	int target;
	u32 btn_raw, dip_raw;
} hud_state_t;

#define HUD_ROWS 160                     /* HUD extends to row ~138 (1080p) */

static void draw_hud(u32 *fb, int w, const hud_state_t *s)
{
	char l1[64], l2[64], l3[64], l4[64], *p;
	const int sc = (w >= 1920) ? 3 : 2, lh = 9 * sc, x = 24, y = 24;

	fmt_str(l1, s->cpu ? "CPU   1 CORE ARM A53" : "FPGA  64 ENGINES (HLS)");
	p = fmt_str(l2, "FRAME RATE "); p = fmt_fix1(p, s->fps_x10);
	p = fmt_str(p, " FPS   RENDER "); p = fmt_fix1(p, s->render_ms_x10); fmt_str(p, " MS");
	p = fmt_str(l3, "ZOOM "); p = fmt_uint(p, (unsigned long)s->zoom);
	p = fmt_str(p, "X   ITERATIONS "); fmt_uint(p, (unsigned long)s->max_iter);
	p = fmt_str(l4, s->julia ? "JULIA" : targets[s->target].name);
	p = fmt_str(p, s->autopilot ? "   AUTOPILOT" : "   MANUAL");
	p = fmt_str(p, "   B:"); p = fmt_uint(p, s->btn_raw);
	p = fmt_str(p, " D:"); fmt_uint(p, s->dip_raw);

	int box_w = 42 * 6 * sc + 24;                  /* room for up to 42 characters */
	if (x - 12 + box_w > w) box_w = w - (x - 12);
	hud_box(fb, w, x - 12, y - 12, box_w, 4 * lh + 18);
	hud_text(fb, w, x, y,          sc, s->cpu ? 0x00FF6040 : 0x0040FF80, l1);
	hud_text(fb, w, x, y + lh,     sc, 0x00FFFFFF, l2);
	hud_text(fb, w, x, y + 2 * lh, sc, 0x00FFFFFF, l3);
	hud_text(fb, w, x, y + 3 * lh, sc, 0x00A0C0FF, l4);
}

/* ---- Main program --------------------------------------------------------- */
int main(void)
{
	xil_printf("\r\n=== Fractal demo ZCU102: HLS kernel + DisplayPort ===\r\n");
	if (dp_init() != XST_SUCCESS) {
		xil_printf("DisplayPort initialisation failed\r\n");
		return -1;
	}

	hud_state_t hs = {0};
	int target = 0, autopilot = 1, zoom_dir = 1, cur = 0, pal = 0, led = 0;
	double zoom = 1.0;
	double jcx = -0.8, jcy = 0.156;                   /* Julia constant */
	int julia_last = 0;
	int c_used = 0;                                   /* centre held and used as a modifier */
	int free_view = 0;                                /* 1: centre freely panned */
	double vcx = 0.0, vcy = 0.0;                      /* freely panned centre */
	u32 btn_last = 0, btn_prev = 0, dip_prev = 0xFFFFFFFF, btn_prev_raw = 0xFFFFFFFF;
	double fps = 0.0;
	XTime t_last, t_now, t_r0, t_r1, t_stat;
	XTime_GetTime(&t_last);
	t_stat = t_last;

	for (;;) {
		/* --- Inputs -------------------------------------------------------- */
		u32 btn = Xil_In32(GPIO_IN_BASE + GPIO_DATA1) & 0x1F;
		u32 dip = read_dip();
		u32 press = btn & ~btn_prev;                   /* rising edges */
		btn_prev = btn;
		if (btn != btn_prev_raw || dip != dip_prev)
			xil_printf("Buttons=0x%02x DIP=0x%02x\r\n", btn, dip);
		btn_prev_raw = btn; dip_prev = dip;

		XTime_GetTime(&t_now);
		double dt = (double)(t_now - t_last) / (double)COUNTS_PER_SECOND;
		t_last = t_now;
		if (dt > 0.5) dt = 0.5;

		int julia = (dip & DIP_JULIA) != 0;
		int cpu   = (dip & DIP_CPU) != 0;
		if (julia != julia_last) { free_view = 0; zoom = 1.0; zoom_dir = 1; julia_last = julia; }
		int w = dp_width(), h = dp_height();
		double base = julia ? BASE_WIDTH_J : BASE_WIDTH_M;
		double zoom_max = (base / w * fix_scale()) / MIN_STEP_RAW;

		u32 release = ~btn & btn_last;                 /* falling edges */
		btn_last = btn;
		const u32 DIRS = BTN_N | BTN_S | BTN_E | BTN_W;
		if (press & BTN_C) c_used = 0;
		if (btn & BTN_C) {                             /* centre held: zoom and target */
			if (btn & DIRS) { c_used = 1; autopilot = 0; }
			if (btn & BTN_N) zoom *= 1.0 + 1.2 * dt;
			if (btn & BTN_S) zoom /= 1.0 + 1.2 * dt;
			if (press & BTN_E) { target = (target + 1) % N_TARGETS; zoom = 1.0; zoom_dir = 1; free_view = 0; }
			if (press & BTN_W) { target = (target + N_TARGETS - 1) % N_TARGETS; zoom = 1.0; zoom_dir = 1; free_view = 0; }
		} else if (btn & DIRS) {                      /* D-pad alone: pan */
			if (!free_view) {                          /* take over the current centre */
				auto_center(julia, target, zoom, jcx, jcy, &vcx, &vcy);
				free_view = 1;
			}
			autopilot = 0;
			double d = 0.4 * base / zoom * dt;       /* 40 % of the image width per second */
			if (btn & BTN_N) vcy -= d;                 /* row 0 = top = smallest imaginary part */
			if (btn & BTN_S) vcy += d;
			if (btn & BTN_E) vcx += d;
			if (btn & BTN_W) vcx -= d;
			if (vcx < -2.5) vcx = -2.5;
			if (vcx > 2.5) vcx = 2.5;   /* representable in Q4.36 */
			if (vcy < -2.5) vcy = -2.5;
			if (vcy > 2.5) vcy = 2.5;
		}
		if ((release & BTN_C) && !c_used) { autopilot = !autopilot; zoom_dir = 1; }   /* tapped briefly */

		if (autopilot) {                               /* zoom flight: in, out, next target */
			if (zoom_dir > 0) { zoom *= 1.0 + 0.5 * dt; if (zoom >= zoom_max) zoom_dir = -1; }
			else {
				zoom /= 1.0 + 1.5 * dt;
				if (zoom <= 1.0) { zoom = 1.0; zoom_dir = 1; target = (target + 1) % N_TARGETS; free_view = 0; }
			}
		}
		if (zoom < 1.0) zoom = 1.0;
		if (zoom > zoom_max) zoom = zoom_max;

		if (dip & DIP_ANIM) pal = (pal + 2) & 255;
		if (julia && (dip & DIP_JANIM)) {              /* rotate c on a circle (without libm) */
			double d = 0.25 * dt / zoom;               /* almost still when zoomed in deep */
			double c = 1.0 - d * d / 2.0, s = d - d * d * d / 6.0;
			double nx = jcx * c - jcy * s, ny = jcx * s + jcy * c;
			jcx = nx; jcy = ny;
			double r2 = jcx * jcx + jcy * jcy, k = 0.7885 * 0.7885 / r2;   /* keep the radius */
			jcx *= (1.0 + k) / 2.0; jcy *= (1.0 + k) / 2.0;
		}

		int max_iter;
		switch (dip & (DIP_ITER0 | DIP_ITER1)) {
		case DIP_ITER0:             max_iter = 256;  break;
		case DIP_ITER1:             max_iter = 512;  break;
		case DIP_ITER0 | DIP_ITER1: max_iter = 1024; break;
		default: max_iter = 192 + 48 * ilog2(zoom); if (max_iter > 1024) max_iter = 1024;
		}

		/* --- View -> kernel parameters ------------------------------------- */
		double cx, cy;
		auto_center(julia, target, zoom, jcx, jcy, &cx, &cy);
		if (free_view) { cx = vcx; cy = vcy; }             /* freely panned */
		double step = base / w / zoom;
		fractal_params_t p;
		p.width = w; p.height = h;
		/* Compute the corner with the rounded pixel step the kernel actually uses;
		   otherwise the image centre drifts at deep zoom */
		p.step = to_q(step); if (p.step < 1) p.step = 1;
		p.x0 = to_q(cx) - p.step * (w / 2);
		p.y0 = to_q(cy) - p.step * (h / 2);
		p.max_iter = max_iter;
		p.mode = julia ? MODE_JULIA : MODE_MANDELBROT;
		p.jx = to_q(jcx); p.jy = to_q(jcy);
		p.pal_shift = pal;

		/* --- Render -------------------------------------------------------- */
		int next = (cur + 1) % DP_NUM_BUFFERS;
		u32 *fb = dp_buffer(next);
		XTime_GetTime(&t_r0);
		if (cpu) {
			/* CPU: render straight into the displayed buffer so the image builds up visibly */
			int hud_on = !(dip & DIP_NOHUD);
			hs.cpu = 1; hs.julia = julia; hs.autopilot = autopilot; hs.max_iter = max_iter;
			hs.zoom = zoom; hs.target = target; hs.btn_raw = btn; hs.dip_raw = dip;
			if (hud_on) draw_hud(fb, w, &hs);          /* HUD visible immediately */
			dp_show(fb); dp_wait_flip(50);
			for (int y = 0; y < h; y += 8) {
				fractal_cpu_rows(fb, &p, y, (y + 8 < h) ? y + 8 : h);
				if (hud_on && y < HUD_ROWS) draw_hud(fb, w, &hs);   /* stripe overwrote the HUD */
				if (!(read_dip() & DIP_CPU)) break;   /* back to the FPGA */
			}
		} else {
			if (fpga_render(fb, &p) != 0) xil_printf("Kernel: timeout\r\n");
		}
		XTime_GetTime(&t_r1);
		double render_s = (double)(t_r1 - t_r0) / (double)COUNTS_PER_SECOND;

		/* --- Display ------------------------------------------------------- */
		hs.cpu = cpu; hs.julia = julia; hs.autopilot = autopilot; hs.max_iter = max_iter;
		hs.zoom = zoom; hs.target = target; hs.btn_raw = btn; hs.dip_raw = dip;
		hs.render_ms_x10 = (unsigned long)(render_s * 10000.0);
		if (!(dip & DIP_NOHUD)) draw_hud(fb, w, &hs);

		if (!cpu) { dp_wait_flip(50); dp_show(fb); }
		cur = next;

		XTime_GetTime(&t_now);
		double frame_s = (double)(t_now - t_r0) / (double)COUNTS_PER_SECOND;
		double inst = frame_s > 0 ? 1.0 / frame_s : 0;
		/* Smooth over about 0.2 s; slow frames (CPU) are taken over directly */
		double a = frame_s / 0.2; if (a > 1.0) a = 1.0;
		fps = (fps == 0.0) ? inst : fps + a * (inst - fps);
		hs.fps_x10 = (unsigned long)(fps * 10.0);

		/* Measurements to the console every 2 s (frame rate, kernel render time, view) */
		if ((double)(t_now - t_stat) / (double)COUNTS_PER_SECOND >= 2.0) {
			t_stat = t_now;
			xil_printf("STAT %s %dx%d fps=%lu.%lu render=%lu.%lums zoom=%lu iter=%d\r\n",
			           cpu ? "CPU" : "FPGA", w, h, hs.fps_x10 / 10, hs.fps_x10 % 10,
			           hs.render_ms_x10 / 10, hs.render_ms_x10 % 10,
			           (unsigned long)zoom, max_iter);
		}

		led ^= 1;
		Xil_Out32(GPIO_LED_BASE, (u32)(led | (cpu ? 0x04 : 0x02) | (autopilot ? 0x80 : 0) |
		                               (dp_link_up() ? 0x40 : 0)));
	}
	return 0;
}
