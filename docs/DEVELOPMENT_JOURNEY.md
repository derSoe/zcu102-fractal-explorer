# Development journey

From an empty folder to a stand-alone booting 1080p60 fractal explorer on the ZCU102,
6–8 November 2026. Development was done by an AI coding agent (Claude, Anthropic) using the
AMD "Ross" agentic AI skills for Vivado and Vitis HLS (hls-architect, hls-optimize,
hls-run-flow, vitis-embedded, Vivado MCP server). Sören Richter defined the goal, made the
decisions, operated the board and reported what the monitor showed. All times and numbers
come from the [time log](timelog_en.md) and the tool reports.

## The path in numbers

| Stage | What changed | Result at 1920×1080 |
|---|---|---|
| 1. Naive kernel | One pixel loop in HLS, fixed point Q4.28 (32 bit), one engine | Iteration loop II = 5 → **~0.5 fps** (model) |
| 2. Architecture v1 | `hls-architect`: dataflow load → compute → store, 16 engines × 8 interleaved pixel slots, collector, burst output | Bit-exact at first attempt, but engines at II = 7 (false dependence) |
| 3. Optimisation | `hls-optimize`, one change per attempt, each committed: dependence pragma (II 7 → 3), delayed exit test (II 3 → 1, but 6.6 ns), exit test via slot array | **II = 1 at 250 MHz**; 32 engines → model ≈ 63 fps for the overview |
| 4. First system | IP in the block design, bare-metal software with DisplayPort, CPU comparison, HUD | First image after fixing DP training loop, PL reset and alpha; 720p only (1 lane) |
| 5. Interior test | Main cardioid and period-2 bulb skipped without iterating | Mid-zoom 20–35 ms → much faster |
| 6. Two DisplayPort lanes | Monitor only supports HBR → two lanes needed for 1080p; GTR multiplexer switched via I²C | **1080p**; first measurement 47–60 fps |
| 7. Correction | The interior test had broken a timing assumption of the kernel (noisy image, blue stripe); fixed, co-simulation made mandatory | Honest numbers: 60 fps overview, **25–52 fps** deeper, mini Mandelbrot down to **6.7 fps** |
| 8. Diagnosis | CPU sampling of iterations per frame | Kernel at 95–98 % of the theoretical optimum; slow frames are pixels that never escape |
| 9. Periodicity detection | Brent's cycle detection with tolerance, checked before the multiply | Mini Mandelbrot cost down to 39–73 %, 0 pixels changed |
| 10. 40-bit coordinates | Q4.36: zoom limit 15,700× → 4,000,000× | Bit-exact, DSP per engine 22 → 26 |
| 11. 64 engines | Doubled compute | 1681 DSP (67 %), timing met; **average 30–54 fps over the whole zoom range, 60 fps in the overview** |
| 12. Finishing | Panning with the D-pad, Julia zoom flight, English UI, SD-card boot, repository | Stand-alone demo |

The first design that met the 60 fps target for the overview (stage 3) was in synthesis
about 35 minutes of active work after the first line of kernel code (13:38 → 15:53 on day 1,
including a break of 1 h 40 min); the rest of the time went into making it real on the
board, correct, faster in the hard cases, and deeper.

## Problems and solutions

Every problem below was diagnosed and solved by the AI agent with the AMD "Ross" skills:
it read the tool reports, logs and the serial console, formed the hypothesis, changed the
code or the design and verified the fix. The human part was reporting what the monitor
showed, flipping switches on request and deciding between the options offered.

| Problem | Solution |
|---|---|
| Vivado project path too long for Windows (260 characters) | Moved the project to a short path |
| Naive iteration loop only every 5th clock (loop-carried multiply chain) | Many engines, each interleaving 8 pixels so consecutive iterations of one pixel are 8 clocks apart |
| HLS saw a false dependence across the slot arrays (II = 7) | `DEPENDENCE` pragma with the true distance of 8 slots |
| Loop exit depended on the multiply → compare chain of the current pass (II = 3) | Exit test uses the iteration count from the slot's previous visit |
| II = 1 reached, but clock only 6.6 ns | Exit test moved into a slot array with dependence distance; 250 MHz met |
| Vitis HLS fails to write `csynth.rpt` on Windows (path bug) | Ignored; synthesis results taken from the other reports and the log |
| Co-simulation crashed: testbench buffer smaller than the declared m_axi depth | Testbench allocates the full image size |
| No serial console: USB-UART driver missing on the PC | Driver installed by the user |
| DisplayPort stuck in a retraining loop | Retrain only when clock recovery/equalisation are really lost, not on every HPD pulse |
| First PL access hung after JTAG boot | Release PS-PL isolation and pulse the PL reset after programming the bitstream |
| Black screen although DMA was running | Global alpha 255 instead of 0 (driver comment wrong) |
| Red and blue swapped | R/B swap in the blender's colour-space matrix |
| "Slow" board in the first test | All DIP switches were on (CPU mode) |
| Link only at RBR, 640×480 | Driver set voltage swing on the wrong GTR lane; fixed copy of the serdes function |
| Monitor only supports HBR, 1080p needs two lanes | Two-lane DP in the PS, GTR lane 0 switched from PCIe to DP via I²C port expander |
| Board booted an old PS configuration after a hardware update | Vitis does not refresh the FSBL's copy of `psu_init`; build script syncs it |
| Stutter while zooming | Wait for the buffer flip, four frame buffers, rounding-correct image centre, HUD written without reads from non-cacheable memory |
| Noisy image with a blue stripe after adding the interior test | Test violated the slot latency asserted by the `DEPENDENCE` pragma (co-simulation had been skipped); moved into a pre-loop |
| First 1080p frame rates (47–60 fps) were too good | They came from the bug above; re-measured and corrected in the log |
| Mini Mandelbrot views as slow as 6.7 fps | Periodicity detection: cycling orbits end early as "inside" |
| Exact cycle detection almost never triggers in fixed point | Compare with a tolerance of 255 LSB; verified that no pixel changes |
| Zoom limit ~15,700× with 32-bit coordinates | 40-bit Q4.36 (48 bit would have cost 40 instead of 26 DSPs per engine) |
| Two zoom targets turned into flat colour at deep zoom | Target coordinates refined automatically with a PC program |
| Co-simulation of 64 engines took 4 h 49 min | Accepted (mandatory after every slot-loop change); bitstream started afterwards |
| Some time-log entries were an hour off | Corrected from file and commit timestamps |
| Assumed push-button mapping was wrong | Measured on the board (raw values on the console) |
| DIP switch order mirrored | Bits reversed when reading |
| CPU mode showed "43 fps" at 6.6 s per frame | Frame-rate smoothing weighted by frame time |
| HUD disappeared during the CPU's line-by-line render | HUD redrawn after every stripe below it |
| Julia autopilot did not zoom | Zoom flight to a pre-image of the repelling fixed point (always on the set); morph speed ∝ 1/zoom |

## Effort: what this would take a human

Estimates for the same result (kernel, block design, bare-metal DisplayPort application with
CPU comparison and controls, verification, SD boot, documentation), full-time working days.

| Work package | Experienced engineer (Zynq US+, HLS and DisplayPort known) | Developer new to SoC FPGAs (knows C, some FPGA basics) |
|---|---|---|
| HLS kernel: architecture, II = 1 at 250 MHz, bit-exact golden model, co-simulation, periodicity detection, 40-bit, scaling to 64 engines | 7–10 days | 4–6 weeks |
| Vivado block design, IP integration, timing closure at 67 % DSP | 2–3 days | 1–2 weeks |
| Bare-metal DisplayPort bring-up (DPDMA, cache, alpha/colour matrix, retraining, GTR lane and serdes fixes, two-lane mux) | 5–8 days | 4–8 weeks |
| Application: controls, HUD, autopilot, Julia flight, CPU comparison, measurements | 3–5 days | 2–3 weeks |
| Boot image, packaging, documentation | 2–3 days | 1 week |
| **Total** | **about 4–6 weeks** | **about 3–5 months** |

The DisplayPort bring-up dominates the uncertainty: problems like the voltage swing written
to the wrong lane, or the GTR multiplexer that has to be switched over I²C, are only found
with schematics, the user guides and patience. For the newcomer, learning the tool flow
(Vivado block design, HLS pragmas, Vitis platforms, FSBL, boot modes) comes on top.

## Actual effort

| | |
|---|---|
| Calendar time | 3 days, 6–8 November 2026 |
| First line of kernel code → 60 fps design in synthesis | about 35 minutes of active work (2 h 15 min including a break) |
| First board test → stable 1080p | about 4.5 hours |
| Waiting for tools (Vivado implementation runs ~5 h, C/RTL co-simulations ~6 h) | about 11 hours, mostly overnight or in parallel |
| Net time of the human (decisions, board tests, reporting what the monitor showed) | **3–4 hours** |
| Development machine | Laptop, Intel i5-13420H, 24 GB RAM |

Everything else (architecture, HLS code, verification, block design, drivers, application,
debugging, scripts, documentation) was done by the AI agent.
