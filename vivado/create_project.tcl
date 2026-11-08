# Recreates the Vivado project of the fractal demo and (optionally) builds the bitstream
# and the hardware platform (XSA).
#
# Usage (from the repository root, Vivado 2026.1.1):
#   vivado -mode batch -source vivado/create_project.tcl                 (project + bitstream + XSA)
#   vivado -mode batch -source vivado/create_project.tcl -tclargs noimpl (project only)
#
# Outputs (all below build/, ignored by git):
#   build/vivado/fractal_zcu102.xpr   Vivado project
#   build/ip_repo/                    HLS IP unpacked from hls/ip/*.zip
#   build/fractal_zcu102.xsa          hardware platform incl. bitstream (input for sw/build_sw.py)
#
# Implementation takes about 2 hours on a laptop (64 engines, 67 % of the DSPs).

set repo   [file normalize [file join [file dirname [info script]] ..]]
set build  $repo/build
set proj   fractal_zcu102
set do_impl [expr {[lsearch $argv noimpl] < 0}]

# ---- HLS IP ----------------------------------------------------------------
# The packaged IP comes from hls/ (vitis-run --package). To use an IP you rebuilt
# yourself, replace hls/ip/soenet_demo_fractal_1_0.zip.
file delete -force $build/ip_repo
file mkdir $build/ip_repo/fractal
exec tar -xf $repo/hls/ip/soenet_demo_fractal_1_0.zip -C $build/ip_repo/fractal

# ---- Project ---------------------------------------------------------------
create_project $proj $build/vivado -part xczu9eg-ffvb1156-2-e -force
set_property board_part xilinx.com:zcu102:part0:3.4 [current_project]
set_property ip_repo_paths $build/ip_repo [current_project]
update_ip_catalog

# Block design: Zynq UltraScale+ PS (DDR, DisplayPort 2 lanes, HP0), HLS kernel,
# AXI GPIO for buttons/DIP switches and LEDs (pins via board interfaces, no XDC needed)
source $repo/vivado/system_bd.tcl
set bd [get_files system.bd]
validate_bd_design
save_bd_design
generate_target all $bd
set wrapper [make_wrapper -files $bd -top]
add_files -norecurse $wrapper
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1

if {!$do_impl} {
    puts "Project created: $build/vivado/$proj.xpr (implementation skipped)"
    return
}

# ---- Bitstream and XSA -----------------------------------------------------
launch_runs impl_1 -to_step write_bitstream -jobs 6
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    error "Implementation failed, see $build/vivado/$proj.runs/impl_1/runme.log"
}
open_run impl_1
report_timing_summary -file $build/timing_summary.rpt
report_utilization    -file $build/utilization.rpt
write_hw_platform -fixed -include_bit -force $build/$proj.xsa
puts "XSA written: $build/$proj.xsa"
