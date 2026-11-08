/*******************************************************************************
* Copyright (C) 2017 - 2020 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
*
* Modified copy of xdppsu_serdes.c (dppsu driver, Vitis 2026.1).
* Change: the original driver always writes voltage swing/pre-emphasis to
* PS-GTR lanes 0..LaneCount-1. On the ZCU102 (preset "Single Lower") DP is on
* GTR lane 1, while lane 0 is PCIe. Without the fix the DP lane stays at minimum
* level and link training only succeeds at RBR (1.62 Gbit/s), cf. AR 71416.
* This version determines the DP lanes from ICM_CFG0/1 (protocol code 4 = DP).
* At link time it replaces the functions of the same name from libxil.a.
*******************************************************************************/
#include <string.h>
#include "xil_io.h"
#include "xdppsu_serdes.h"

#define ICM_CFG0        0xFD410010U
#define ICM_CFG1        0xFD410014U
#define ICM_PROTO_DP    4U

static const u32 vs[4][4] = {
		{ 0x2a, 0x27, 0x24, 0x20 },
		{ 0x27, 0x23, 0x20, 0xff },
		{ 0x24, 0x20, 0xff, 0xff },
		{ 0xff, 0xff, 0xff, 0xff },
};
static const u32 pe[4][4] = {
		{ 0x02, 0x02, 0x02, 0x02 },
		{ 0x01, 0x01, 0x01, 0xff },
		{ 0x00, 0x00, 0xff, 0xff },
		{ 0xff, 0xff, 0xff, 0xff },
};

/* GTR lane of DP lane n. According to the ICM table (UG1085/UG1182) DP lane 0 is on
   GTR lane 1 or 3, DP lane 1 on GTR lane 0 or 2. Fallback: n */
static u32 dp_gtr_lane(u32 n)
{
	u32 cfg = Xil_In32(ICM_CFG0) | (Xil_In32(ICM_CFG1) << 8);
	u32 parity = (n == 0) ? 1U : 0U;
	for (u32 gtr = 0; gtr < 4; gtr++)
		if ((gtr & 1U) == parity && ((cfg >> (gtr * 4)) & 0x7U) == ICM_PROTO_DP)
			return gtr;
	return n;
}

static void XDpPsu_SetSerdesVswingPreemp(XDpPsu *InstancePtr)
{
	u8 VsLevelRx = InstancePtr->LinkConfig.VsLevel;
	u8 PeLevelRx = InstancePtr->LinkConfig.PeLevel;

	for (u8 Index = 0; Index < InstancePtr->LinkConfig.LaneCount; Index++) {
		u32 off = dp_gtr_lane(Index) * SERDES_LANE_OFFSET;
		XDpPsu_WriteReg(SERDES_BASEADDR, SERDES_L0_TX_MARGININGF + off,
				vs[PeLevelRx][VsLevelRx]);
		XDpPsu_WriteReg(SERDES_BASEADDR, SERDES_L0_TX_DEEMPHASIS + off,
				pe[PeLevelRx][VsLevelRx]);
	}
}

void XDpPsu_SetVswingPreemp(XDpPsu *InstancePtr, u8 *AuxData)
{
	u8 Data = 0;
	u8 VsLevelRx = InstancePtr->LinkConfig.VsLevel;
	u8 PeLevelRx = InstancePtr->LinkConfig.PeLevel;

	if (VsLevelRx >= XDPPSU_MAXIMUM_VS_LEVEL)
		Data |= XDPPSU_DPCD_TRAINING_LANEX_SET_MAX_VS_MASK;
	if (PeLevelRx >= XDPPSU_MAXIMUM_PE_LEVEL)
		Data |= XDPPSU_DPCD_TRAINING_LANEX_SET_MAX_PE_MASK;
	Data |= (PeLevelRx << XDPPSU_DPCD_TRAINING_LANEX_SET_PE_SHIFT) | VsLevelRx;
	memset(AuxData, Data, 4);

	XDpPsu_SetSerdesVswingPreemp(InstancePtr);
}

void XDpPsu_CfgTxVsLevel(XDpPsu *InstancePtr, u8 Level, u8 TxLevel)
{
	Xil_AssertVoid(InstancePtr != NULL);
	Xil_AssertVoid(Level < 4);
	Xil_AssertVoid(TxLevel < 16);
	InstancePtr->BoardChar.TxVsLevels[Level] = TxLevel;
}

void XDpPsu_CfgTxPeLevel(XDpPsu *InstancePtr, u8 Level, u8 TxLevel)
{
	Xil_AssertVoid(InstancePtr != NULL);
	Xil_AssertVoid(Level < 4);
	Xil_AssertVoid(TxLevel < 32);
	InstancePtr->BoardChar.TxPeLevels[Level] = TxLevel;
}
