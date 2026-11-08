/*
 * DisplayPort output (PS DP + DPDMA) for the fractal demo.
 *
 * Based on the AMD examples xdpdma_video_example.c and xdppsu_interrupt.c
 * (Copyright (C) 2017-2022 Xilinx, Inc., (C) 2022-2023 Advanced Micro Devices,
 * Inc., SPDX-License-Identifier: MIT). Changes for the demo:
 *   - D-cache stays ON (fair CPU comparison). Instead, the DPDMA driver instance and
 *     the frame buffers live in a non-cacheable MMU region, because the driver
 *     does not flush its descriptors from the cache.
 *   - The fractal kernel writes 0x00RRGGBB (alpha 0). The DP graphics channel
 *     (format RGBA8888) expects R and B swapped and evaluates alpha.
 *     -> global alpha (graphics fully visible) and R/B swap in the
 *        colour space matrix (IN2CSC) of the blender.
 *   - Resolution fallback to 1280x720 if the link cannot carry 1080p60.
 */
#include "dp_display.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_mmu.h"
#include "xil_io.h"
#include "xiltimer.h"
#include "sleep.h"
#include "xdpdma.h"
#include "xdppsu.h"
#include "gtr_mux.h"
#include "xavbuf.h"
#include "xavbuf_clk.h"
#ifdef SDT
#include "xinterrupt_wrap.h"
#else
#include "xscugic.h"
#endif

/* ------------------------------------------------------------------------ */
#ifdef SDT
#define DPPSU_BASEADDR   XPAR_XDPPSU_0_BASEADDR
#define DPDMA_BASEADDR   XPAR_XDPDMA_0_BASEADDR
#else
#define DPPSU_DEVICE_ID  XPAR_PSU_DP_DEVICE_ID
#define DPDMA_DEVICE_ID  XPAR_XDPDMA_0_DEVICE_ID
#define INTC_DEVICE_ID   XPAR_SCUGIC_0_DEVICE_ID
#define DPPSU_INTR_ID    151
#define DPDMA_INTR_ID    154
#endif

#define REGION_SIZE   0x200000u                    /* MMU block size (2 MB) */
#define FB_BYTES      (DP_MAX_W * DP_MAX_H * 4)
#define NC_BYTES      (((DP_NUM_BUFFERS * FB_BYTES + REGION_SIZE) / REGION_SIZE + 1) * REGION_SIZE)

/* Non-cacheable region: [0..2 MB) driver instance, then the frame buffers */
static u8 nc_mem[NC_BYTES] __attribute__((aligned(REGION_SIZE)));

typedef struct {
	XDpPsu *DpPsuPtr;
	XAVBuf *AVBufPtr;
	XDpDma *DpDmaPtr;
	XVidC_VideoMode VideoMode;
	XVidC_ColorDepth Bpc;
	XDpPsu_ColorEncoding ColorEncode;
	u8 UseMaxLaneCount, UseMaxLinkRate, LaneCount, LinkRate, EnSynchClkMode;
	u32 PixClkHz;
} Run_Config;

static XDpPsu DpPsu;
static XAVBuf AVBuf;
static XDpDma *DpDmaPtr;                 /* lives in nc_mem */
static Run_Config RunCfg;
#ifndef SDT
static XScuGic Intr;
#endif
static XDpDma_FrameBuffer FrameBuffer;   /* the driver keeps a pointer to it */
static volatile int g_w = DP_MAX_W, g_h = DP_MAX_H, g_link_up = 0;

static int  InitDpDmaSubsystem(Run_Config *Cfg);
static void DpPsu_Run(Run_Config *Cfg);
static void DpPsu_SetupVideoStream(Run_Config *Cfg);
static void DpPsu_IsrHpdEvent(void *ref);
static void DpPsu_IsrHpdPulse(void *ref);

/* ------------------------------------------------------------------------ */
int  dp_width(void)   { return g_w; }
int  dp_height(void)  { return g_h; }
int  dp_link_up(void) { return g_link_up; }
u32 *dp_buffer(int i) { return (u32 *)(nc_mem + REGION_SIZE + (u32)i * FB_BYTES); }

static void set_framebuffer(u32 *fb)
{
	FrameBuffer.Address  = (u64)(UINTPTR)fb;
	FrameBuffer.LineSize = (u32)g_w * 4;
	FrameBuffer.Stride   = (u32)g_w * 4;       /* 1920*4 and 1280*4 are 256-byte aligned */
	FrameBuffer.Size     = (u32)g_w * (u32)g_h * 4;
}

void dp_show(u32 *fb)
{
	set_framebuffer(fb);
	XDpDma_DisplayGfxFrameBuffer(DpDmaPtr, &FrameBuffer);
}

/* Diagnostics: lane status (DPCD 0x202-0x204) and symbol error counters (0x210-0x213,
   reading clears them) of the monitor, plus the MSA values of the source */
void dp_diag(void)
{
	u8 st[3] = {0}, err[4] = {0};
	if (!g_link_up) return;
	XDpPsu_AuxRead(&DpPsu, 0x202, 3, st);
	XDpPsu_AuxRead(&DpPsu, 0x210, 4, err);
	u32 e0 = ((err[1] & 0x7F) << 8) | err[0], e1 = ((err[3] & 0x7F) << 8) | err[2];
	XDpPsu_MainStreamAttributes *m = &DpPsu.MsaConfig;
	xil_printf("DPDIAG lane01=%02x align=%02x err0=%lu%s err1=%lu%s | MSA %lux%lu htot=%lu hstart=%lu pclk=%lu tu=%lu\r\n",
	           st[0], st[2], e0, (err[1] & 0x80) ? "v" : "", e1, (err[3] & 0x80) ? "v" : "",
	           (u32)m->Vtm.Timing.HActive, (u32)m->Vtm.Timing.VActive,
	           (u32)m->Vtm.Timing.HTotal, (u32)m->HStart, (u32)m->PixelClockHz, (u32)m->TransferUnitSize);
}

int dp_wait_flip(int timeout_ms)
{
	XTime t0, t;
	XTime_GetTime(&t0);
	/* the first flip ends with TRIGGER_DONE, all later ones with RETRIGGER_DONE */
	while (DpDmaPtr->Gfx.TriggerStatus != XDPDMA_TRIGGER_DONE &&
	       DpDmaPtr->Gfx.TriggerStatus != XDPDMA_RETRIGGER_DONE) {
		XTime_GetTime(&t);
		if ((t - t0) > (XTime)COUNTS_PER_SECOND / 1000 * (XTime)timeout_ms)
			return 0;
	}
	return 1;
}

/* The kernel delivers 0x00RRGGBB, alpha 0: global alpha + R/B swap in IN2CSC */
static void apply_gfx_color_fix(void)
{
	UINTPTR b = DpPsu.Config.BaseAddr;
	/* Global alpha 255 -> graphics layer fully opaque, independent of pixel alpha.
	   (The driver comment "0 passes stream2" is wrong here; verified on the board.) */
	XAVBuf_SetBlenderAlpha(&AVBuf, 255, 1);
	/* Identity matrix with rows R <-> B swapped (coefficient 1.0 = 0x1000) */
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF0, 0x0000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF1, 0x0000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF2, 0x1000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF3, 0x0000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF4, 0x1000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF5, 0x0000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF6, 0x1000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF7, 0x0000);
	Xil_Out32(b + XAVBUF_V_BLEND_IN2CSC_COEFF8, 0x0000);
}

/* ------------------------------------------------------------------------ */
int dp_init(void)
{
	XDpPsu_Config *DpPsuCfgPtr;
	XDpDma_Config *DpDmaCfgPtr;
	u32 IntrMask = XDPPSU_INTR_HPD_IRQ_MASK | XDPPSU_INTR_HPD_EVENT_MASK;

	/* Set up the non-cacheable region (flush everything first) */
	Xil_DCacheFlush();
	for (UINTPTR a = (UINTPTR)nc_mem; a < (UINTPTR)nc_mem + NC_BYTES; a += REGION_SIZE)
		Xil_SetTlbAttributes(a, NORM_NONCACHE);
	DpDmaPtr = (XDpDma *)nc_mem;

	RunCfg.DpPsuPtr        = &DpPsu;
	RunCfg.AVBufPtr        = &AVBuf;
	RunCfg.DpDmaPtr        = DpDmaPtr;
	RunCfg.VideoMode       = XVIDC_VM_1920x1080_60_P;
	RunCfg.Bpc             = XVIDC_BPC_8;
	RunCfg.ColorEncode     = XDPPSU_CENC_RGB;
	/* Lane count and rate: minimum of hardware (XPAR, 1 or 2 lanes) and monitor DPCD.
	   The driver falls back by itself during training if needed. */
	RunCfg.UseMaxLaneCount = 1;
	RunCfg.UseMaxLinkRate  = 1;
	RunCfg.LaneCount       = 2;
	RunCfg.LinkRate        = 0x14;

	/* Two-lane DP ("Dual Lower"): GTR lane 0 to the DP connector via the board multiplexer */
	if ((Xil_In32(0xFD410010U) & 0x7U) == 4U) gtr_mux_dp_dual();
	RunCfg.EnSynchClkMode  = 0;

	for (int i = 0; i < DP_NUM_BUFFERS; i++) {          /* black start images */
		u32 *p = dp_buffer(i);
		for (int k = 0; k < DP_MAX_W * DP_MAX_H; k++) p[k] = 0;
	}
	set_framebuffer(dp_buffer(0));

#ifdef SDT
	DpPsuCfgPtr = XDpPsu_LookupConfig(DPPSU_BASEADDR);
#else
	DpPsuCfgPtr = XDpPsu_LookupConfig(DPPSU_DEVICE_ID);
#endif
	if (!DpPsuCfgPtr) { xil_printf("DP: no configuration\r\n"); return XST_FAILURE; }
	XDpPsu_CfgInitialize(&DpPsu, DpPsuCfgPtr, DpPsuCfgPtr->BaseAddr);
#ifdef SDT
	XAVBuf_CfgInitialize(&AVBuf, DpPsu.Config.BaseAddr);
	DpDmaCfgPtr = XDpDma_LookupConfig(DPDMA_BASEADDR);
#else
	XAVBuf_CfgInitialize(&AVBuf, DpPsu.Config.BaseAddr, DPPSU_DEVICE_ID);
	DpDmaCfgPtr = XDpDma_LookupConfig(DPDMA_DEVICE_ID);
#endif
	if (!DpDmaCfgPtr) { xil_printf("DPDMA: no configuration\r\n"); return XST_FAILURE; }
	XDpDma_CfgInitialize(DpDmaPtr, DpDmaCfgPtr);

	if (InitDpDmaSubsystem(&RunCfg) != XST_SUCCESS) return XST_FAILURE;

	/* Interrupts: hot-plug (starts link training) and DPDMA vsync (buffer flip) */
	XDpPsu_WriteReg(DpPsu.Config.BaseAddr, XDPPSU_INTR_DIS, 0xFFFFFFFF);
	XDpPsu_WriteReg(DpPsu.Config.BaseAddr, XDPPSU_INTR_MASK, 0xFFFFFFFF);
	XDpPsu_SetHpdEventHandler(&DpPsu, DpPsu_IsrHpdEvent, &RunCfg);
	XDpPsu_SetHpdPulseHandler(&DpPsu, DpPsu_IsrHpdPulse, &RunCfg);
#ifdef SDT
	XSetupInterruptSystem(&DpPsu, &XDpPsu_HpdInterruptHandler, DpPsu.Config.IntrId,
			      DpPsu.Config.IntrParent, XINTERRUPT_DEFAULT_PRIORITY);
	XDpPsu_WriteReg(DpPsu.Config.BaseAddr, XDPPSU_INTR_EN, IntrMask);
	XSetupInterruptSystem(DpDmaPtr, &XDpDma_InterruptHandler, DpDmaPtr->Config.IntrId,
			      DpDmaPtr->Config.IntrParent, XINTERRUPT_DEFAULT_PRIORITY);
	XDpDma_InterruptEnable(DpDmaPtr, XDPDMA_IEN_VSYNC_INT_MASK);
#else
	XScuGic_Config *IntrCfgPtr = XScuGic_LookupConfig(INTC_DEVICE_ID);
	XScuGic_CfgInitialize(&Intr, IntrCfgPtr, IntrCfgPtr->CpuBaseAddress);
	XScuGic_Connect(&Intr, DPPSU_INTR_ID, (Xil_InterruptHandler)XDpPsu_HpdInterruptHandler, &DpPsu);
	XScuGic_SetPriorityTriggerType(&Intr, DPPSU_INTR_ID, 0x0, 0x03);
	XScuGic_Connect(&Intr, DPDMA_INTR_ID, (Xil_ExceptionHandler)XDpDma_InterruptHandler, DpDmaPtr);
	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
				     (Xil_ExceptionHandler)XScuGic_DeviceInterruptHandler, INTC_DEVICE_ID);
	Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);
	Xil_ExceptionEnable();
	XScuGic_Enable(&Intr, DPPSU_INTR_ID);
	XDpPsu_WriteReg(DpPsu.Config.BaseAddr, XDPPSU_INTR_EN, IntrMask);
	XScuGic_Enable(&Intr, DPDMA_INTR_ID);
	XDpDma_InterruptEnable(DpDmaPtr, XDPDMA_IEN_VSYNC_INT_MASK);
#endif
	return XST_SUCCESS;
}

/* ------------------------------------------------------------------------ */
static int InitDpDmaSubsystem(Run_Config *Cfg)
{
	if (XDpPsu_InitializeTx(Cfg->DpPsuPtr) != XST_SUCCESS) {
		xil_printf("DP: InitializeTx failed\r\n");
		return XST_FAILURE;
	}
	if (XDpDma_SetGraphicsFormat(Cfg->DpDmaPtr, RGBA8888) != XST_SUCCESS) return XST_FAILURE;
	if (XAVBuf_SetInputNonLiveGraphicsFormat(Cfg->AVBufPtr, RGBA8888) != XST_SUCCESS) return XST_FAILURE;
	XDpDma_SetQOS(Cfg->DpDmaPtr, 11);
	XAVBuf_EnableGraphicsBuffers(Cfg->AVBufPtr, 1);
	XAVBuf_SetOutputVideoFormat(Cfg->AVBufPtr, RGB_8BPC);
	XAVBuf_InputVideoSelect(Cfg->AVBufPtr, XAVBUF_VIDSTREAM1_NONE, XAVBUF_VIDSTREAM2_NONLIVE_GFX);
	XAVBuf_ConfigureGraphicsPipeline(Cfg->AVBufPtr);
	XAVBuf_ConfigureOutputVideo(Cfg->AVBufPtr);
	XDpPsu_CfgMsaEnSynchClkMode(Cfg->DpPsuPtr, Cfg->EnSynchClkMode);
	XAVBuf_SetAudioVideoClkSrc(Cfg->AVBufPtr, XAVBUF_PS_CLK, XAVBUF_PS_CLK);
	XAVBuf_SoftReset(Cfg->AVBufPtr);
	apply_gfx_color_fix();
	return XST_SUCCESS;
}

static u32 DpPsu_Wakeup(Run_Config *Cfg)
{
	u8 AuxData = 0x1;
	u32 Status = XDpPsu_AuxWrite(Cfg->DpPsuPtr, XDPPSU_DPCD_SET_POWER_DP_PWR_VOLTAGE, 1, &AuxData);
	if (Status != XST_SUCCESS) xil_printf("DP: 1st wake-up failed\r\n");
	Status = XDpPsu_AuxWrite(Cfg->DpPsuPtr, XDPPSU_DPCD_SET_POWER_DP_PWR_VOLTAGE, 1, &AuxData);
	if (Status != XST_SUCCESS) xil_printf("DP: 2nd wake-up failed\r\n");
	return Status;
}

/* Payload rate of the link in Mbit/s (8b/10b), e.g. 1 lane HBR2 = 4320 */
static u32 link_payload_mbps(XDpPsu *Dp)
{
	return (u32)Dp->LinkConfig.LaneCount * (u32)Dp->LinkConfig.LinkRate * 270u * 8u / 10u;
}

static u32 DpPsu_Hpd_Train(Run_Config *Cfg)
{
	XDpPsu *Dp = Cfg->DpPsuPtr;
	XDpPsu_LinkConfig *L = &Dp->LinkConfig;

	if (XDpPsu_GetRxCapabilities(Dp) != XST_SUCCESS) { xil_printf("DP: cannot read RX capabilities\r\n"); return XST_FAILURE; }
	{	/* Diagnostics: raw DPCD capabilities (0x0000 and extended fields from 0x2200) */
		static int dumped = 0;
		u8 d[16];
		if (!dumped) {
			dumped = 1;
			u32 addr[2] = {0x0000, 0x2200};
			for (int k = 0; k < 2; k++) {
				if (XDpPsu_AuxRead(Dp, addr[k], 16, d) != XST_SUCCESS) { xil_printf("DPCD %04x: read error\r\n", addr[k]); continue; }
				xil_printf("DPCD %04x:", addr[k]);
				for (int i = 0; i < 16; i++) xil_printf(" %02x", d[i]);
				xil_printf("\r\n");
			}
		}
	}
	if (XDpPsu_SetEnhancedFrameMode(Dp, L->SupportEnhancedFramingMode ? 1 : 0) != XST_SUCCESS) return XST_FAILURE;
	if (XDpPsu_SetLaneCount(Dp, Cfg->UseMaxLaneCount ? L->MaxLaneCount : Cfg->LaneCount) != XST_SUCCESS) return XST_FAILURE;
	if (XDpPsu_SetLinkRate(Dp, Cfg->UseMaxLinkRate ? L->MaxLinkRate : Cfg->LinkRate) != XST_SUCCESS) return XST_FAILURE;
	if (XDpPsu_SetDownspread(Dp, L->SupportDownspreadControl) != XST_SUCCESS) return XST_FAILURE;

	xil_printf("DP: monitor supports max. %d lane(s), rate 0x%02x; requested %d lane(s), rate 0x%02x\r\n",
	           L->MaxLaneCount, L->MaxLinkRate, L->LaneCount, L->LinkRate);
	u32 Status = XDpPsu_EstablishLink(Dp);
	xil_printf(Status == XST_SUCCESS ? "DP: link training ok, %d lane(s), rate 0x%02x\r\n"
	                                 : "DP: link training failed (%d, 0x%02x)\r\n",
	           L->LaneCount, L->LinkRate);

	/* RGB 8 bpc: 1080p60 = 148.5 MHz * 24 = 3564 Mbit/s, 720p60 = 1782, 480p60 = 604 */
	if (Status == XST_SUCCESS) {
		u32 mbps = link_payload_mbps(Dp);
		if (mbps >= 3600)      { Cfg->VideoMode = XVIDC_VM_1920x1080_60_P; g_w = 1920; g_h = 1080; }
		else if (mbps >= 1800) { Cfg->VideoMode = XVIDC_VM_1280x720_60_P;  g_w = 1280; g_h = 720; }
		else                   { Cfg->VideoMode = XVIDC_VM_640x480_60_P;   g_w = 640;  g_h = 480; }
		xil_printf("DP: %lu Mbit/s -> %dx%d\r\n", mbps, g_w, g_h);
		set_framebuffer((u32 *)(UINTPTR)FrameBuffer.Address);
	}
	return Status;
}

static void DpPsu_Run(Run_Config *Cfg)
{
	XDpPsu *Dp = Cfg->DpPsuPtr;
	u32 Status;

	g_link_up = 0;
	if (InitDpDmaSubsystem(Cfg) != XST_SUCCESS) { xil_printf("DP: init failed\r\n"); return; }
	XDpPsu_EnableMainLink(Dp, 0);

	if (!XDpPsu_IsConnected(Dp)) {
		XDpDma_SetChannelState(Cfg->DpDmaPtr, GraphicsChan, XDPDMA_DISABLE);
		xil_printf("DP: no monitor connected\r\n");
		return;
	}
	xil_printf("DP: monitor connected\r\n");
	if (DpPsu_Wakeup(Cfg) != XST_SUCCESS) return;

	u8 Count = 0;
	do {
		usleep(100000);
		Count++;
		Status = DpPsu_Hpd_Train(Cfg);
		if (Status == XST_DEVICE_NOT_FOUND) { xil_printf("DP: connection lost\r\n"); return; }
		if (Status != XST_SUCCESS) continue;

		XDpDma_DisplayGfxFrameBuffer(Cfg->DpDmaPtr, &FrameBuffer);
		DpPsu_SetupVideoStream(Cfg);
		XDpPsu_EnableMainLink(Dp, 1);
		Status = XDpPsu_CheckLinkStatus(Dp, Dp->LinkConfig.LaneCount);
		if (Status == XST_DEVICE_NOT_FOUND) return;
	} while ((Status != XST_SUCCESS) && (Count < 2));

	if (Status == XST_SUCCESS) {
		apply_gfx_color_fix();
		g_link_up = 1;
		xil_printf("DP: video output %dx%d active\r\n", g_w, g_h);
	}
}

static void DpPsu_IsrHpdEvent(void *ref)
{
	xil_printf("DP: hot-plug event\r\n");
	DpPsu_Run((Run_Config *)ref);
}

static void DpPsu_IsrHpdPulse(void *ref)
{
	Run_Config *Cfg = (Run_Config *)ref;
	XDpPsu *Dp = Cfg->DpPsuPtr;
	u32 Status = XDpPsu_CheckLinkStatus(Dp, Dp->LinkConfig.LaneCount);
	if (Status == XST_DEVICE_NOT_FOUND) { xil_printf("DP: connection lost (pulse)\r\n"); return; }
	/* Monitors send IRQ_HPD pulses even without losing the link (status messages). Only
	   retrain if clock recovery/equalisation are really lost. */
	if (Status == XST_SUCCESS && g_link_up) { xil_printf("DP: HPD pulse, link ok\r\n"); return; }

	xil_printf("DP: retraining required\r\n");
	XDpPsu_EnableMainLink(Dp, 0);
	u8 Count = 0;
	do {
		Count++;
		Status = DpPsu_Hpd_Train(Cfg);
		if (Status == XST_DEVICE_NOT_FOUND) return;
		if (Status != XST_SUCCESS) continue;
		DpPsu_SetupVideoStream(Cfg);
		XDpPsu_EnableMainLink(Dp, 1);
		Status = XDpPsu_CheckLinkStatus(Dp, Dp->LinkConfig.LaneCount);
		if (Status == XST_DEVICE_NOT_FOUND) return;
	} while ((Status != XST_SUCCESS) && (Count < 2));
	if (Status == XST_SUCCESS) { apply_gfx_color_fix(); g_link_up = 1; }
}

static void DpPsu_SetupVideoStream(Run_Config *Cfg)
{
	XDpPsu *Dp = Cfg->DpPsuPtr;
	XDpPsu_MainStreamAttributes *Msa = &Dp->MsaConfig;

	XDpPsu_SetColorEncode(Dp, Cfg->ColorEncode);
	XDpPsu_CfgMsaSetBpc(Dp, Cfg->Bpc);
	XDpPsu_CfgMsaUseStandardVideoMode(Dp, Cfg->VideoMode);

	Cfg->PixClkHz = Msa->PixelClockHz;
	XAVBuf_SetPixelClock(Cfg->PixClkHz);

	XDpPsu_WriteReg(Dp->Config.BaseAddr, XDPPSU_SOFT_RESET, 0x1);
	usleep(10);
	XDpPsu_WriteReg(Dp->Config.BaseAddr, XDPPSU_SOFT_RESET, 0x0);

	XDpPsu_SetMsaValues(Dp);
	XDpPsu_WriteReg(Dp->Config.BaseAddr, 0xB124, 0x3);    /* AV_BUF_SRST: assert reset */
	usleep(10);
	XDpPsu_WriteReg(Dp->Config.BaseAddr, 0xB124, 0x0);    /* release reset */
	XDpPsu_EnableMainLink(Dp, 1);
}
