/* Switch the ZCU102 PS-GTR multiplexers for two-lane DisplayPort.
 *
 * On the ZCU102 every PS-GTR lane passes through an external multiplexer (U125..U128,
 * PI2DBS6212). The selection comes from I2C port expander U97 (TCA6416A, I2C0, address
 * 0x20), bits P00..P03 = PS_GTR_LANE_SEL0..3 (UG1182, table 3-20).
 * Default: GTR lane 0 -> PCIe slot, lane 1 -> DisplayPort. For two-lane DP
 * ("Dual Lower") lane 0 must be routed to the DP connector as well.
 * The select value for "DP" is copied from lane 1, which is on DP by default; this way
 * the function does not depend on the polarity of the multiplexer. */
#include "gtr_mux.h"
#include "xiicps.h"
#include "xil_printf.h"

#define TCA6416_ADDR   0x20
#define REG_IN0        0x00
#define REG_OUT0       0x02
#define REG_CFG0       0x06

static XIicPs Iic;

static int reg_read(u8 reg, u8 *val)
{
	if (XIicPs_MasterSendPolled(&Iic, &reg, 1, TCA6416_ADDR) != XST_SUCCESS) return XST_FAILURE;
	while (XIicPs_BusIsBusy(&Iic)) ;
	if (XIicPs_MasterRecvPolled(&Iic, val, 1, TCA6416_ADDR) != XST_SUCCESS) return XST_FAILURE;
	while (XIicPs_BusIsBusy(&Iic)) ;
	return XST_SUCCESS;
}

static int reg_write(u8 reg, u8 val)
{
	u8 buf[2] = {reg, val};
	if (XIicPs_MasterSendPolled(&Iic, buf, 2, TCA6416_ADDR) != XST_SUCCESS) return XST_FAILURE;
	while (XIicPs_BusIsBusy(&Iic)) ;
	return XST_SUCCESS;
}

int gtr_mux_dp_dual(void)
{
	XIicPs_Config *cfg = XIicPs_LookupConfig(XPAR_XIICPS_0_BASEADDR);
	if (!cfg || XIicPs_CfgInitialize(&Iic, cfg, cfg->BaseAddress) != XST_SUCCESS) {
		xil_printf("GTR mux: I2C0 not available\r\n");
		return XST_FAILURE;
	}
	XIicPs_SetSClk(&Iic, 100000);

	u8 in0, out0, cfg0;
	if (reg_read(REG_IN0, &in0) != XST_SUCCESS || reg_read(REG_OUT0, &out0) != XST_SUCCESS ||
	    reg_read(REG_CFG0, &cfg0) != XST_SUCCESS) {
		xil_printf("GTR mux: port expander U97 not responding\r\n");
		return XST_FAILURE;
	}
	u8 dp_sel = (in0 >> 1) & 1;                       /* lane 1 is on DP */
	xil_printf("GTR mux: before IN0=%02x OUT0=%02x CFG0=%02x, DP select=%d\r\n",
	           in0, out0, cfg0, dp_sel);

	out0 = (u8)((out0 & ~0x01) | dp_sel);             /* SEL0 = value of SEL1 */
	if (reg_write(REG_OUT0, out0) != XST_SUCCESS ||
	    reg_write(REG_CFG0, (u8)(cfg0 & ~0x01)) != XST_SUCCESS) {   /* only P00 as output */
		xil_printf("GTR mux: write failed\r\n");
		return XST_FAILURE;
	}
	reg_read(REG_IN0, &in0);
	xil_printf("GTR mux: after IN0=%02x (lane 0 -> DisplayPort)\r\n", in0);
	return XST_SUCCESS;
}
