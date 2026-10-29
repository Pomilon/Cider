#!/usr/bin/env python3
"""UEFI end-to-end smoke: compile, link PE32+, and boot under QEMU + OVMF.

This test is opt-in through CMake's CIDER_UEFI_TESTS option. It exits 77
(ctest SKIP) when the toolchain or firmware is unavailable, so a normal build
matrix does not require a UEFI environment.

The compiler emits GNU Intel-syntax assembly. The link script assembles it
with clang's COFF target and links the object with GNU ld's i386pep emulation.
The resulting image is checked for the PE32+ / EFI application signature
before QEMU is started.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))
LINK = os.path.join(REPO, "scripts", "link_uefi.sh")
HELLO = os.path.join(REPO, "examples", "uefi", "hello.cid")
MARKER = "Hello from Cider UEFI!"

OVMF_CANDIDATES = [
    (os.environ.get("OVMF_CODE"), os.environ.get("OVMF_VARS")),
    ("/usr/share/OVMF/x64/OVMF_CODE.4m.fd",
     "/usr/share/OVMF/x64/OVMF_VARS.4m.fd"),
    ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
]


def fail(message):
    print("FAIL: " + message, file=sys.stderr)
    return 1


def find_ovmf():
    for code, var in OVMF_CANDIDATES:
        if code and var and os.path.exists(code) and os.path.exists(var):
            return code, var
    return None


def ld_has_i386pep():
    if shutil.which("ld") is None:
        return False
    r = subprocess.run(["ld", "-V"], capture_output=True, text=True)
    return r.returncode == 0 and "i386pep" in (r.stdout + r.stderr)


def validate_pe32plus(path):
    data = open(path, "rb").read()
    if len(data) < 0x100 or data[:2] != b"MZ":
        return "missing DOS/PE header"
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if pe + 0x60 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return "missing PE signature"
    machine = int.from_bytes(data[pe + 4:pe + 6], "little")
    if machine != 0x8664:
        return f"machine is 0x{machine:04x}, not x86-64"
    optional = pe + 24
    magic = int.from_bytes(data[optional:optional + 2], "little")
    if magic != 0x20B:
        return f"optional-header magic is 0x{magic:04x}, not PE32+"
    subsystem = int.from_bytes(data[optional + 68:optional + 70], "little")
    if subsystem != 10:
        return f"subsystem is {subsystem}, not EFI application (10)"
    return None


def read_text(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def run_boot(code, var, efi, log, timeout=60):
    with tempfile.TemporaryDirectory() as tmp:
        esp = os.path.join(tmp, "esp")
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(efi, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))

        # A zeroed variable store is pristine; reusing firmware NVRAM may
        # contain a stale boot entry that never reaches BOOTX64.EFI.
        vars_copy = os.path.join(tmp, "OVMF_VARS.fd")
        with open(vars_copy, "wb") as f:
            f.truncate(os.path.getsize(var))

        cmd = [
            "qemu-system-x86_64", "-m", "256", "-display", "none",
            "-no-reboot", "-serial", "file:" + log,
            "-drive", "if=pflash,format=raw,unit=0,file=" + code + ",readonly=on",
            "-drive", "if=pflash,format=raw,unit=1,file=" + vars_copy,
            "-drive", "file=fat:rw:" + esp + ",format=raw",
        ]
        if os.environ.get("CIDER_UEFI_VERBOSE"):
            print("boot: " + " ".join(cmd))
        qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + timeout
        text = ""
        try:
            while time.monotonic() < deadline:
                text = read_text(log)
                if MARKER in text or qemu.poll() is not None:
                    break
                time.sleep(0.25)
        finally:
            if qemu.poll() is None:
                qemu.terminate()
                try:
                    qemu.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    qemu.kill()
                    qemu.wait()
        text = read_text(log)
        if MARKER not in text:
            print("FAIL: serial did not contain " + repr(MARKER))
            print("--- serial tail ---")
            print(text[-2000:])
            return 1
        print("boot: " + MARKER)
        return 0


def main():
    if not os.path.exists(CIDER):
        return fail(f"compiler not found: {CIDER} (build first)")
    if not os.path.exists(HELLO):
        return fail(f"example not found: {HELLO}")
    if not os.path.exists(LINK):
        return fail(f"link script not found: {LINK}")
    for tool in ("clang", "ld"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            return 77
    if not ld_has_i386pep():
        print("SKIP: GNU ld has no i386pep emulation", file=sys.stderr)
        return 77
    ovmf = find_ovmf()
    if ovmf is None:
        print("SKIP: OVMF firmware not found", file=sys.stderr)
        return 77
    if shutil.which("qemu-system-x86_64") is None:
        print("SKIP: qemu-system-x86_64 not found", file=sys.stderr)
        return 77

    with tempfile.TemporaryDirectory() as tmp:
        asm = os.path.join(tmp, "hello.asm")
        efi = os.path.join(tmp, "hello.efi")
        build = subprocess.run(
            [CIDER, "compile", "--target", "uefi", HELLO, "-o", asm],
            capture_output=True, text=True)
        if build.returncode != 0:
            return fail("compile failed:\n" + build.stdout + build.stderr)
        link = subprocess.run(
            ["bash", LINK, asm, "-o", efi],
            capture_output=True, text=True)
        if link.returncode != 0:
            return fail("link failed:\n" + link.stdout + link.stderr)
        error = validate_pe32plus(efi)
        if error is not None:
            return fail("invalid PE32+ image: " + error)
        print("PE32+: EFI application, x86-64")

        log = os.path.join(tmp, "serial.log")
        code, var = ovmf
        return run_boot(code, var, efi, log)


if __name__ == "__main__":
    sys.exit(main())
