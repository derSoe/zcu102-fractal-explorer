# Time log: fractal demo on ZCU102 with AMD agentic AI

English translation of the original German log ([timelog_de.md](timelog_de.md)), which was
written during development. Times are local time on the development machine
(i5-13420H, 8 cores, 24 GB RAM). Breaks and discussions about the concept are included in
the time spans.

## Day 1 (6 Nov 2026)

| Time | Step | Result |
|---|---|---|
| 13:19 | Vivado session started | Vivado 2026.1.1, ZCU102 board files available |
| 13:19–13:3x | Concept discussed, architecture decided | Fractal via HLS, frame buffer in DDR, DisplayPort from the PS |
| 13:3x | Project created, path length problem (Windows 260 characters) | Project moved to `C:\Projekte\fractal` |
| 13:37 | Vivado restarted in the project folder | Block design: Zynq + DisplayPort, HP0, 250 MHz, GPIO for buttons/DIP/LEDs, validated |
| 13:38 | Naive fractal kernel and testbench written | Mandelbrot + Julia, fixed point Q4.28, colour palette |
| 13:40 | C simulation | Passed bit-exact against the golden model |
| 13:41 | C synthesis of the naive version (41 s) | II=5, 1 engine, 15 DSP, approx. 0.5 fps at 1080p |
| 15:24 | Start of HLS optimisation with `/hls-architect` | |
| 15:27 | Architecture v1 written: 16 engines with 8 slots each, collector, burst output | C simulation bit-exact at the first attempt |
| 15:31 | C synthesis v1 (171 s) | Dataflow ok, bursts ok; engines still II=7 (false dependence), 282 DSP, 18 % LUT |
| 15:33 | Handover to `/hls-optimize`, git history for every attempt | |
| 15:38 | Attempt 1: PIPELINE + DEPENDENCE (slot distance 8) | II 7 → 3 |
| 15:42 | Attempt 2: delayed loop exit | II 3 → 1, but clock 6.6 ns |
| 15:46 | Attempt 3: exit via slot array with dependence distance | **II = 1 at 250 MHz, timing met** |
| 15:53 | Attempt 4: 32 engines | Latency −50 %, 554 DSP (21 %), 39 % LUT; model ≈ 63 fps at 1080p |
| 15:55–16:00 | Two tool problems solved: Vitis path bug on Windows (report), testbench buffer for co-simulation | IP export works |
| 16:01 | Co-simulation of the optimised version started | |
| 16:13 | Baseline implementation (16 engines) finished: 35 min | Timing met at 250 MHz (3.44 ns) |
| 16:20 | Co-simulation (19 min) | **PASS, RTL bit-exact**; cycle counts confirm the performance model |
| 16:25 | Fractal IP added to the block design (control via HPM0, frame buffer via HP0, interrupt) | Block design validated |
| 16:27 | Synthesis + implementation + bitstream of the complete system started | running |
| 16:29 | Board checked via JTAG | Digilent cable, XCZU9EG + ARM DAP detected; UART driver (CP2108) missing |
| 16:30–16:39 | Bare-metal software written (in parallel with implementation) | DisplayPort/DPDMA with triple buffering, kernel control, CPU comparison, HUD, buttons/DIP; build and load scripts |
| 16:40–16:56 | Driver for the serial console (CP2108) set up | COM9–COM12 available |
| 17:10 | **Bitstream finished** (complete system, 42 min on the laptop) | Timing met at 250 MHz (WNS +0.16 ns); 41k LUT (15 %), 72k FF (13 %), 557 DSP (22 %), 128 BRAM36 |
| 17:13 | XSA exported, platform built (FSBL + standalone BSP) | |
| 17:18 | Application built (-O2, no warnings) | fractal_app.elf ready; board test pending |

## Day 2: board test (7 Nov 2026)

| Time | Step | Result |
|---|---|---|
| 10:07 | First start on the ZCU102 via JTAG | FSBL and application run without PMU firmware |
| 10:10–10:25 | DisplayPort brought up | Retraining loop fixed; PL reset was missing in the JTAG flow (bus hung); global alpha 255 instead of 0 → **first image** |
| 10:25–10:35 | Error images on the monitor analysed | DIP switches were all ON (CPU mode); driver bug found: signal level on the wrong GTR lane → only RBR/640x480; fixed → HBR, 1280x720 |
| 10:35–10:45 | Stutter and frame rate | Buffer flip synchronisation, zoom rounding, 4 frame buffers, info box without reads; measurement output on the console |
| 10:45 | Measurement at 720p | 60 fps at deep zoom, 28–50 fps at medium zoom (kernel 20–35 ms due to interior areas) |
| 10:47–10:55 | Kernel: interior test (cardioid + bulb) | csim bit-exact, 79 % of the pixels in the test image skipped; csynth II=1, 2.92 ns, 714 DSP |
| 10:58 | Cause of missing HBR2: monitor only supports HBR, but 4 lanes | PS switched to DP with 2 lanes (PCIe off), GTR multiplexer via I²C; new bitstream run started |
| 11:41 | Bitstream finished (interior test + DP 2 lanes) | Timing met (WNS +0.16 ns); 45k LUT (16 %), 724 DSP (29 %) |
| 11:45–12:39 | Platform rebuilt; FSBL used an outdated PS initialisation (Vitis quirk) | Sync added to the build script |
| 12:40 | **1920x1080 over 2 lanes (HBR)** | Medium zoom now 8.5 ms instead of 20–35 ms (at 2.25× the pixel count); 47–60 fps over the whole flight |
| 12:45 | Board: image noisy/blurred, blue stripe on the left | Diagnosis: the interior test violated the asserted slot latency (co-simulation had been skipped) |
| 12:50–13:09 | Fix (test in a pre-loop), csim, csynth, **co-simulation PASS** (17 min) | Slot loop back to depth 10, RTL bit-exact |
| 13:10 | Bitstream run started | |
| 14:21 | Bitstream finished (corrected kernel) | Timing met (WNS +0.123 ns, WHS +0.010 ns) |
| 14:25–14:35 | XSA, platform, app rebuilt; board loaded | Link 2 lanes HBR, 1080p, 0 symbol errors |
| 14:36 | Measurement with the correct kernel (autopilot flight) | Overview up to zoom 50: 60 fps (8.6–15 ms); deeper 25–52 fps (19–42 ms). **The 47–60 fps from 12:40 were too good because of the bug** (wrong pixels terminated early) |
| until 14:40 | Diagnosis: CPU sample of iterations per frame (UART line DIAG) | Kernel runs at 95–98 % of the theoretical optimum, load balancing is not a bottleneck. Lower bound 8.6 ms = 1 pixel/clock at the output. Slow frames: mini Mandelbrot (24–39 % of the pixels at max_iter, up to 63 ms) and deep boundary (42 ms, hardly any max_iter) |
| 14:40 | PC experiment periodicity detection (Brent) | Exact comparison almost never triggers in fixed point. With tolerance 255 LSB: mini Mandelbrot cost down to 39–73 %, practically no differing pixels; tolerance 4095: 30–56 %, ~0.01 % of pixels differ |
| 14:40–15:00 | Periodicity detection (tolerance 255) in golden model, kernel and CPU path; csim bit-exact, new test image mini Mandelbrot; csynth: slot loop II=1, depth 10 (unchanged), DSP 714 (unchanged) | Co-simulation started |
| 15:00–15:21 | Experiments 48/40/36 bit, csynth with 2 engines (in parallel with the co-simulation) | DSP per engine: 32 bit 22, 36 bit 23, **40 bit 26**, 48 bit 40. Slot depth unchanged at 10 up to 40 bit, 13 at 48 bit. 40 bit (Q4.36) → zoom limit ~4 million× at 1080p |
| 15:31 | **Co-simulation periodicity detection PASS** (31 min, 5 test images bit-exact) | Commit 34a2fd4 |
| 15:32–15:47 | Kernel at 40 bit (Q4.36): golden model with 128-bit intermediates, new test image zoom ~1e6; csim bit-exact; csynth: DSP 842, slot depth 10 | Commit 955bc05 |
| 15:40–16:04 | PC experiments: precision at 4 million× ok (0–5 % chaotic boundary pixels as before); zoom targets checked: Elephant Valley flat from 1e4, Spiral from 1e6 → coordinates refined automatically. Software switched to 64-bit registers, new register offsets | |
| 15:47–16:03 | 64 engines: csim bit-exact; csynth (10 min): DSP 1674 (66 %), slot depth 10, estimated 2.92 ns | Commit 308e729 |
| 16:03–20:53 | **Co-simulation 64 engines × 40 bit PASS** (4 h 49 min, 6 test images bit-exact incl. mini Mandelbrot and deep zoom; periodicity detection changes 0 pixels) | Bitstream started only after the co-simulation so as not to load the machine twice |
| Correction | The times of the entries from 14:40 onwards were initially about 1 h too late in the log; corrected at 20:55 from file timestamps (logs, commits) | |
| 20:55–20:58 | IP packaged, updated in the block design (addresses unchanged), bitstream run started | old IP backed up |
| 22:48 | Bitstream finished (64 engines × 40 bit, periodicity detection), run 110 min | Timing met (WNS +0.071 ns, WHS +0.010 ns); 139.6k LUT (51 %), 174k FF (32 %), 1681 DSP (67 %), 352.5 BRAM (39 %) |
| 22:50–22:57 | XSA exported, platform and app built | Register offsets in the generated driver = main.c; board test pending (board off) |

## Day 3 (8 Nov 2026)

| Time | Step | Result |
|---|---|---|
| morning | **Board test 64 engines × 40 bit** | Image clean; zoom up to 4.0 million× reached (previously ~15,700×). Average fps from UART: zoom <1e3: 54, 1e3–1e5: 51, 1e5–1e6: 41, ≥1e6: 30. Slowest frames: ~9 fps (124 ms) at zoom ~3 million× with 1024 iterations. Mini Mandelbrot smooth by eye (previously down to 6.7 fps) |
| | Push-button mapping determined on the board (SW5 centre 0x01, SW4 left 0x02, SW6 down 0x04, SW7 right 0x08, SW8 up 0x10; the previous assumption was wrong). New controls: D-pad pans, hold centre + up/down zooms, centre + left/right target, tap centre for autopilot. HUD shows 64 engines | Tested on the board, controls confirmed by the user |
| | DIP switches checked on the board: bit order arrives mirrored (switch 1 = bit 7) → reversed when read, mapping now as documented. Julia animation (DIP 1 + 7) works | Julia autopilot does not zoom yet (shows the whole set) |
| | Julia autopilot: zoom flight to the pre-image of the repelling fixed point (always on the set; PC check: at c=-0.8+0.156i 30–40 % structure up to 4 million×), animation speed ∝ 1/zoom. CPU mode: frame-rate smoothing fixed (showed 43 fps at 6.6 s/frame) | built and loaded |
| | Colours checked on the board: red/blue correct. CPU mode: HUD disappeared during the line-by-line render (the CPU overwrote it) → HUD redrawn at the start and after every stripe in the HUD area | built, loaded, confirmed by the user |
| | HUD and console texts in English. BOOT.BIN (FSBL + bitstream + app, without PMU firmware) created with Bootgen, copied to the SD card | **Boot from SD card works** (SW6 = ON/OFF/OFF/OFF), confirmed by the user |
| | Source comments in English, repository with documentation prepared | |
| | Verification rebuild from a clean copy of the repository: `create_project.tcl` (166 min incl. IP synthesis), `build_sw.py` (3 min) | Timing identical (WNS +0.071 ns, WHS +0.010 ns), 139,550 LUT, 1681 DSP; the resulting BOOT.BIN **runs on the board from the SD card**, confirmed by the user |
