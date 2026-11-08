#ifndef GTR_MUX_H_
#define GTR_MUX_H_

/* Routes PS-GTR lane 0 to the DisplayPort connector via I2C port expander U97
 * (ZCU102, two-lane DP). Returns XST_SUCCESS or XST_FAILURE. */
int gtr_mux_dp_dual(void);

#endif
