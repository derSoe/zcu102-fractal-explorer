// Fractal kernel - ARCHITECTURE v1 (created with /hls-architect)
//
//   load_input()   : hands out row jobs round-robin to N_ENG compute engines
//   compute()      : N_ENG engines (SLOTS interleaved pixels each) + collector with palette
//   store_output() : writes the pixel stream in wide bursts into the frame buffer
//
// Output is bit-exact with the golden model in the testbench.
#include "fractal.h"
#include <hls_stream.h>
#include <ap_int.h>

typedef ap_int<COORD_W>     coord_t;   // coordinate Q4.36
typedef ap_int<2 * COORD_W> wide_t;    // product of two coordinates

constexpr int N_ENG = 64;   // number of parallel engines (power of two)
constexpr int SLOTS = 8;    // interleaved pixels per engine (>= latency of one iteration)

typedef ap_uint<16> row_t;  // row number
typedef ap_uint<16> iter_t; // iteration count of a pixel

// ---------------------------------------------------------------------------
// load_input: row y goes to engine y % N_ENG
// ---------------------------------------------------------------------------
static void load_input(int height, hls::stream<row_t> jobs[N_ENG]) {
    for (int y = 0; y < height; y++) {
        jobs[y % N_ENG].write(y);
    }
}

// ---------------------------------------------------------------------------
// Interior test in hardware widths; returns the same values as
// in_main_bulbs() from fractal.h (reference for golden model and CPU)
// ---------------------------------------------------------------------------
static bool in_main_bulbs_hw(coord_t cx, coord_t cy) {
#pragma HLS INLINE
    bool range = !(cx < -(5LL << (FRAC - 2)) || cx > (1LL << (FRAC - 1)) ||
                   cy < -(3LL << (FRAC - 2)) || cy > (3LL << (FRAC - 2)));
    ap_int<18> a  = cx >> (FRAC - 16);          // within the valid range |a| < 1.25 * 2^16
    ap_int<18> b  = cy >> (FRAC - 16);
    ap_int<36> bb = b * b;
    ap_int<21> y2 = bb >> 16;
    ap_int<19> xq = a - (1 << 14);
    ap_int<38> xx = xq * xq;
    ap_int<23> q  = (xx >> 16) + y2;
    ap_int<24> qx = q + xq;
    ap_int<47> pr = q * qx;
    ap_int<31> lhs = pr >> 16;
    ap_int<19> xb = a + (1 << 16);
    ap_int<38> bx = xb * xb;
    ap_int<23> bulb = (bx >> 16) + y2;
    return range && ((lhs + TEST_MARGIN < (y2 >> 2)) || (bulb + TEST_MARGIN < (1 << 12)));
}

// ---------------------------------------------------------------------------
// Engine: computes whole rows; SLOTS pixels interleaved round-robin, a finished
// slot is immediately refilled with the next pixel of the row (load balancing).
// ---------------------------------------------------------------------------
static bool period_close_hw(coord_t zx, coord_t zy, coord_t rx, coord_t ry) {
#pragma HLS INLINE
    ap_int<COORD_W + 1> dx = (ap_int<COORD_W + 1>)zx - rx, dy = (ap_int<COORD_W + 1>)zy - ry;
    return dx >= -PERIOD_TOL && dx <= PERIOD_TOL && dy >= -PERIOD_TOL && dy <= PERIOD_TOL;
}

static void engine(int id, hls::stream<row_t> &jobs, hls::stream<iter_t> &out,
                   int width, int height, coord_t x0, coord_t y0, coord_t step,
                   int max_iter, int mode, coord_t jx, coord_t jy) {
    int nrows = (height > id) ? (height - id + N_ENG - 1) / N_ENG : 0;

    for (int r = 0; r < nrows; r++) {
        int y = jobs.read();
        coord_t cy_row = y0 + (coord_t)y * step;

        iter_t  buf[MAX_W];
        // Interior test up front for the whole row (separate loop): this keeps the
        // "fill slot" path in the slot loop short (a single memory access) and
        // within the asserted dependence distance SLOTS.
        bool inner_row[MAX_W];
        for (int x = 0; x < width; x++) {
#pragma HLS PIPELINE II=1
            inner_row[x] = (mode != MODE_JULIA) && in_main_bulbs_hw(x0 + (coord_t)x * step, cy_row);
        }

        coord_t zx[SLOTS], zy[SLOTS], cx[SLOTS], cy[SLOTS];
        coord_t rx[SLOTS], ry[SLOTS];   // reference point of the periodicity detection
        iter_t  it[SLOTS];
        ap_uint<11> px[SLOTS];
        bool    act[SLOTS];

        for (int s = 0; s < SLOTS; s++) act[s] = false;

        int next = 0, done = 0, s = 0;
        // The exit test checks the count this slot saw on its last visit
        // (SLOTS passes ago). This way the loop control does not depend on
        // the mul -> compare chain of the current pass.
        // The few extra passes at the end of a row are empty (all slots inactive).
        int dhist[SLOTS];
        for (int k = 0; k < SLOTS; k++) dhist[k] = 0;
        while (dhist[s] < width) {
#pragma HLS PIPELINE II=1
#pragma HLS DEPENDENCE variable=dhist type=inter distance=SLOTS dependent=true
// The same slot only comes round again after SLOTS passes -> true distance = SLOTS
#pragma HLS DEPENDENCE variable=zx  type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=zy  type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=cx  type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=cy  type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=it  type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=px  type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=act type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=rx  type=inter distance=SLOTS dependent=true
#pragma HLS DEPENDENCE variable=ry  type=inter distance=SLOTS dependent=true
            if (!act[s]) {
                if (next < width) {                      // fill slot with a new pixel
                    coord_t c = x0 + (coord_t)next * step;
                    if (mode == MODE_JULIA) { zx[s] = c; zy[s] = cy_row; cx[s] = jx; cy[s] = jy; }
                    else                    { zx[s] = 0; zy[s] = 0;      cx[s] = c;  cy[s] = cy_row; }
                    // interior: mark as finished right away (next visit outputs
                    // max_iter without iterating)
                    it[s] = inner_row[next] ? (iter_t)max_iter : (iter_t)0;
                    px[s] = next; act[s] = true;
                    next++;
                }
            } else {                                     // one iteration for this slot
                bool fin = false;
                // periodicity check on the current z (no multiplier in the path)
                bool periodic = (it[s] != 0) && period_close_hw(zx[s], zy[s], rx[s], ry[s]);
                if (it[s] >= max_iter) {
                    fin = true;
                } else if (periodic) {
                    it[s] = max_iter; fin = true;
                } else {
                    if ((it[s] & (iter_t)(it[s] - 1)) == 0) { rx[s] = zx[s]; ry[s] = zy[s]; }
                    wide_t zx2 = ((wide_t)zx[s] * zx[s]) >> FRAC;
                    wide_t zy2 = ((wide_t)zy[s] * zy[s]) >> FRAC;
                    if (zx2 + zy2 > ((wide_t)4 << FRAC)) {
                        fin = true;
                    } else {
                        wide_t zxy = ((wide_t)zx[s] * zy[s]) >> (FRAC - 1);
                        zx[s] = (coord_t)(zx2 - zy2 + cx[s]);   // overflow truncated as in the golden model
                        zy[s] = (coord_t)(zxy + cy[s]);
                        it[s] = it[s] + 1;
                    }
                }
                if (fin) { buf[px[s]] = it[s]; act[s] = false; done++; }
            }
            dhist[s] = done;
            s = (s == SLOTS - 1) ? 0 : s + 1;
        }

        for (int x = 0; x < width; x++) out.write(buf[x]);   // hand over the finished row
    }
}

// ---------------------------------------------------------------------------
// Collector: fetches the rows in image order and applies the colour palette
// ---------------------------------------------------------------------------
static void collect(hls::stream<iter_t> in[N_ENG], hls::stream<uint32_t> &pix,
                    int width, int height, int max_iter, int pal_shift) {
    for (int y = 0; y < height; y++) {
        int e = y % N_ENG;
        for (int x = 0; x < width; x++) {
            iter_t it = in[e].read();
            pix.write(palette(it, max_iter, pal_shift));
        }
    }
}

// ---------------------------------------------------------------------------
// compute: N_ENG engines in parallel + collector
// ---------------------------------------------------------------------------
#define ENGINE(i) engine(i, jobs[i], rows[i], width, height, x0, y0, step, max_iter, mode, jx, jy)

static void compute(hls::stream<row_t> jobs[N_ENG], hls::stream<uint32_t> &pix,
                    int width, int height, coord_t x0, coord_t y0, coord_t step,
                    int max_iter, int mode, coord_t jx, coord_t jy, int pal_shift) {
#pragma HLS DATAFLOW
    hls::stream<iter_t> rows[N_ENG];
#pragma HLS STREAM variable=rows depth=2048

    ENGINE(0); ENGINE(1); ENGINE(2); ENGINE(3);
    ENGINE(4); ENGINE(5); ENGINE(6); ENGINE(7);
    ENGINE(8); ENGINE(9); ENGINE(10); ENGINE(11);
    ENGINE(12); ENGINE(13); ENGINE(14); ENGINE(15);
    ENGINE(16); ENGINE(17); ENGINE(18); ENGINE(19);
    ENGINE(20); ENGINE(21); ENGINE(22); ENGINE(23);
    ENGINE(24); ENGINE(25); ENGINE(26); ENGINE(27);
    ENGINE(28); ENGINE(29); ENGINE(30); ENGINE(31);
    ENGINE(32); ENGINE(33); ENGINE(34); ENGINE(35);
    ENGINE(36); ENGINE(37); ENGINE(38); ENGINE(39);
    ENGINE(40); ENGINE(41); ENGINE(42); ENGINE(43);
    ENGINE(44); ENGINE(45); ENGINE(46); ENGINE(47);
    ENGINE(48); ENGINE(49); ENGINE(50); ENGINE(51);
    ENGINE(52); ENGINE(53); ENGINE(54); ENGINE(55);
    ENGINE(56); ENGINE(57); ENGINE(58); ENGINE(59);
    ENGINE(60); ENGINE(61); ENGINE(62); ENGINE(63);
    collect(rows, pix, width, height, max_iter, pal_shift);
}

// ---------------------------------------------------------------------------
// store_output: pixel stream -> frame buffer (sequential, burst + widening)
// ---------------------------------------------------------------------------
static void store_output(hls::stream<uint32_t> &pix, uint32_t *fb, int width, int height) {
    int n = width * height;
    for (int i = 0; i < n; i++) fb[i] = pix.read();
}

// ---------------------------------------------------------------------------
// Top-Level
// ---------------------------------------------------------------------------
void fractal(uint32_t *fb, int width, int height,
             int64_t x0, int64_t y0, int64_t step,
             int max_iter, int mode, int64_t jx, int64_t jy, int pal_shift) {
#pragma HLS INTERFACE m_axi     port=fb offset=slave bundle=gmem depth=2073600 max_widen_bitwidth=128 max_write_burst_length=64
#pragma HLS INTERFACE s_axilite port=width
#pragma HLS INTERFACE s_axilite port=height
#pragma HLS INTERFACE s_axilite port=x0
#pragma HLS INTERFACE s_axilite port=y0
#pragma HLS INTERFACE s_axilite port=step
#pragma HLS INTERFACE s_axilite port=max_iter
#pragma HLS INTERFACE s_axilite port=mode
#pragma HLS INTERFACE s_axilite port=jx
#pragma HLS INTERFACE s_axilite port=jy
#pragma HLS INTERFACE s_axilite port=pal_shift
#pragma HLS INTERFACE s_axilite port=return
#pragma HLS DATAFLOW

    hls::stream<row_t>    jobs[N_ENG];
#pragma HLS STREAM variable=jobs depth=4
    hls::stream<uint32_t> pix("pix");
#pragma HLS STREAM variable=pix depth=64

    load_input(height, jobs);
    compute(jobs, pix, width, height, (coord_t)x0, (coord_t)y0, (coord_t)step, max_iter, mode,
            (coord_t)jx, (coord_t)jy, pal_shift);
    store_output(pix, fb, width, height);
}
