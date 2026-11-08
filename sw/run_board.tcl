# Loads bitstream, FSBL and the fractal application onto the ZCU102 via JTAG
# (for development; for stand-alone operation use BOOT.BIN on the SD card).
#
# Usage (from the repository root, after sw/build_sw.py):
#   xsdb sw/run_board.tcl
# Board boot mode: JTAG (SW6 all ON). Serial console: 115200 8N1.

set repo  [file normalize [file join [file dirname [info script]] ..]]
set ws    $repo/build/ws
if {[info exists ::env(FRACTAL_WS)] && $::env(FRACTAL_WS) ne ""} { set ws [file normalize $::env(FRACTAL_WS)] }
set plat  zcu102_fractal_plat
set exp   $ws/$plat/export/$plat

proc find_one {pattern} {
    set hits [glob -nocomplain -types f {*}$pattern]
    if {[llength $hits] == 0} { error "not found: $pattern (run sw/build_sw.py first)" }
    return [lindex $hits 0]
}

# Bitstream: environment variable FRACTAL_BIT overrides the one in the platform
if {[info exists ::env(FRACTAL_BIT)] && $::env(FRACTAL_BIT) ne ""} {
    set bit $::env(FRACTAL_BIT)
} else {
    set bit [find_one [list $exp/hw/sdt/*.bit $exp/hw/*.bit]]
}
set fsbl  [find_one [list $exp/sw/boot/fsbl.elf $ws/$plat/zynqmp_fsbl/build/*.elf]]
set app   [find_one [list $ws/fractal_app/build/fractal_app.elf $ws/fractal_app/build/*.elf]]
puts "Bitstream : $bit"
puts "FSBL      : $fsbl"
puts "App       : $app"

connect
targets -set -nocase -filter {name =~ "*PSU*"}
rst -system
after 1500

# The FSBL initialises the PS (clocks, DDR, MIO, DP reference clocks).
# No PMU firmware: the demo uses no power-management APIs.
targets -set -nocase -filter {name =~ "*A53*#0"}
rst -processor -clear-registers
dow $fsbl
con
after 4000
stop

# Program the PL
fpga -file $bit
after 500

# Remove the PS-PL isolation and pulse the PL reset (pl_resetn0 via EMIO). When booting
# from SD the FSBL does this after loading the bitstream; in the JTAG flow the bitstream
# only comes after the FSBL, otherwise the AXI interconnect stays in reset and the first
# PL access hangs.
targets -set -nocase -filter {name =~ "*PSU*"}
source [find_one [list $exp/hw/sdt/psu_init.tcl $ws/$plat/hw/sdt/psu_init.tcl]]
psu_ps_pl_isolation_removal
psu_ps_pl_reset_config
after 100
puts "PL reset released"

# Start the application
targets -set -nocase -filter {name =~ "*A53*#0"}
rst -processor -clear-registers
dow $app
con
puts "Application running."
