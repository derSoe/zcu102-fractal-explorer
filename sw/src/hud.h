/* Simple text overlay (5x7 font, scaled) drawn directly into the frame buffer */
#ifndef HUD_H_
#define HUD_H_

#include "xil_types.h"

/* Dark box as background for text */
void hud_box(u32 *fb, int stride, int x, int y, int w, int h);

/* Text in capitals (lower case is converted), colour 0x00RRGGBB.
 * Returns the width in pixels. */
int  hud_text(u32 *fb, int stride, int x, int y, int scale, u32 color, const char *s);

/* Number formatting helpers without printf floating point */
char *fmt_uint(char *p, unsigned long v);
char *fmt_fix1(char *p, unsigned long v_x10);    /* 123 -> "12.3" */
char *fmt_str(char *p, const char *s);

#endif
