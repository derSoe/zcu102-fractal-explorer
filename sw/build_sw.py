"""Builds the platform, the bare-metal application and BOOT.BIN of the fractal demo
(Vitis, headless).

Usage (from the repository root, Vitis 2026.1.1):
    vitis -s sw/build_sw.py

Input: build/fractal_zcu102.xsa (from vivado/create_project.tcl); if it does not exist,
prebuilt/fractal_zcu102.xsa is used, so the software can be built without running Vivado.

Outputs (below build/, ignored by git):
    build/ws/                 Vitis workspace (platform with FSBL, application);
                              override with FRACTAL_WS (see the path length note below)
    build/BOOT.BIN            boot image for the SD card (FSBL + bitstream + application)

Existing components are reused. The platform is rebuilt only if the XSA is newer than
the exported platform.

Note: if Vitis reports too little disk space in the home directory and exits without
doing anything, set XILINX_VITIS_DATA_DIR to a writable folder with ~100 MB free.
"""
import glob
import os
import shutil
import subprocess
import vitis

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUILD = os.path.join(ROOT, "build")
XSA = os.path.join(BUILD, "fractal_zcu102.xsa")
if not os.path.isfile(XSA):
    XSA = os.path.join(ROOT, "prebuilt", "fractal_zcu102.xsa")
# Vitis workspace. On Windows the BSP build creates paths ~210 characters deep below the
# workspace, and Windows tools fail beyond 260 characters. If the repository sits in a deep
# folder, set FRACTAL_WS to a short path (e.g. C:\fzws) or clone to a short path.
WS = os.environ.get("FRACTAL_WS") or os.path.join(BUILD, "ws")
MAX_WS_LEN = 45
SRC = os.path.join(ROOT, "sw", "src")
PLAT = "zcu102_fractal_plat"
APP = "fractal_app"
DOMAIN = "standalone_psu_cortexa53_0"


def sync_fsbl_psu_init():
    """Copies psu_init.c/.h from the FSBL hw_artifacts into the FSBL sources if they differ."""
    src = os.path.join(WS, PLAT, "zynqmp_fsbl", "zynqmp_fsbl_bsp", "hw_artifacts")
    dst = os.path.join(WS, PLAT, "zynqmp_fsbl")
    changed = False
    for name in ("psu_init.c", "psu_init.h"):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if os.path.isfile(s) and (not os.path.isfile(d) or open(s, "rb").read() != open(d, "rb").read()):
            shutil.copyfile(s, d)
            print("FSBL: updated", name)
            changed = True
    return changed


def build(client):
    # ---- Platform -----------------------------------------------------------
    xpfm = os.path.join(WS, PLAT, "export", PLAT, PLAT + ".xpfm")
    if os.path.isdir(os.path.join(WS, PLAT)):
        platform = client.get_component(name=PLAT)
        need = not os.path.isfile(xpfm) or os.path.getmtime(XSA) > os.path.getmtime(xpfm)
        if need:
            platform.update_hw(hw_design=XSA)
        # update_hw refreshes the PS initialisation only in the BSP (hw_artifacts), not in
        # the copy the FSBL is built from -> sync, otherwise the old PS setup boots
        if sync_fsbl_psu_init():
            need = True
        if need:
            print("PLATFORM_BUILD_RC", platform.build())
        else:
            print("PLATFORM up to date, no rebuild")
    else:
        platform = client.create_platform_component(
            name=PLAT, hw_design=XSA, os="standalone",
            cpu="psu_cortexa53_0", domain_name=DOMAIN)
        print("PLATFORM_BUILD_RC", platform.build())
        if sync_fsbl_psu_init():
            print("PLATFORM_BUILD_RC", platform.build())
    print("XPFM", xpfm, os.path.isfile(xpfm))

    # ---- Application --------------------------------------------------------
    if os.path.isdir(os.path.join(WS, APP)):
        app = client.get_component(name=APP)
    else:
        app = client.create_app_component(name=APP, platform=xpfm, domain=DOMAIN,
                                          template="empty_application")
    app.import_files(from_loc=SRC,
                     files=[f for f in os.listdir(SRC) if f.endswith((".c", ".h"))],
                     dest_dir_in_cmp="src")
    # Optimised build: fair CPU comparison (default would be -O0)
    app.set_app_config("USER_COMPILE_OPTIMIZATION_LEVEL", "-O2")

    rc = app.build()
    build_dir = os.path.join(WS, APP, "build")
    elfs = glob.glob(os.path.join(build_dir, "**", "*.elf"), recursive=True)
    elf = max(elfs, key=os.path.getsize) if elfs else None
    print("APP_BUILD_RC", rc, "ELF", elf, "EXISTS", bool(elf) and os.path.isfile(elf))
    if rc == 0 and elf:
        with open(os.path.join(build_dir, ".buildstatus"), "w") as fh:
            fh.write("hw=SUCCESS\n")
    return elf if rc == 0 else None


def make_boot_bin(app_elf):
    """BOOT.BIN = FSBL + bitstream + application (no PMU firmware, the demo does not need it)."""
    exp = os.path.join(WS, PLAT, "export", PLAT)
    fsbl = os.path.join(exp, "sw", "boot", "fsbl.elf")
    bits = glob.glob(os.path.join(exp, "hw", "**", "*.bit"), recursive=True)
    if not (os.path.isfile(fsbl) and bits and app_elf):
        print("BOOT.BIN: inputs missing (fsbl/bit/elf)")
        return
    bif = os.path.join(BUILD, "boot.bif")
    with open(bif, "w") as fh:
        fh.write("the_ROM_image:\n{\n"
                 f"    [bootloader, destination_cpu=a53-0] {fsbl}\n"
                 f"    [destination_device=pl] {bits[0]}\n"
                 f"    [destination_cpu=a53-0, exception_level=el-3] {app_elf}\n"
                 "}\n")
    exe = "bootgen.bat" if os.name == "nt" else "bootgen"
    vitis_dir = os.environ.get("XILINX_VITIS", "")
    if vitis_dir and os.path.isfile(os.path.join(vitis_dir, "bin", exe)):
        exe = os.path.join(vitis_dir, "bin", exe)
    out = os.path.join(BUILD, "BOOT.BIN")
    rc = subprocess.call([exe, "-arch", "zynqmp", "-image", bif, "-o", out, "-w", "on"])
    print("BOOTGEN_RC", rc, "BOOT.BIN", out, os.path.isfile(out))


assert os.path.isfile(XSA), f"XSA missing: {XSA}"
if os.name == "nt" and len(WS) > MAX_WS_LEN:
    raise SystemExit(f"Workspace path too long for Windows ({len(WS)} > {MAX_WS_LEN} characters): {WS}\n"
                     r"Set FRACTAL_WS to a short folder, e.g.  set FRACTAL_WS=C:\fzws")
print("XSA", XSA)
os.makedirs(WS, exist_ok=True)
client = vitis.create_client()
try:
    client.set_workspace(WS)
    elf = build(client)
finally:
    vitis.dispose()      # always stop the server, otherwise the workspace stays locked
make_boot_bin(elf)
