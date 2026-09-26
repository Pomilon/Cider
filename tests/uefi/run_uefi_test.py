#!/usr/bin/env python3
"""CID-005 end-to-end UEFI smoke: compile -> link -> boot under QEMU+OVMF.

The ELF harness (tests/run_tests.py) is untouched; this script is separate
and exits 77 (ctest SKIP) when QEMU or OVMF is unavailable.

Each case compiles one .cid with `cider compile --target uefi`, links it with
scripts/link_uefi.sh, drops the .efi on a directory-backed FAT image as
\\EFI\\BOOT\\BOOTX64.EFI, boots QEMU with the PomeOS OVMF binaries and asserts
the expected marker on the serial log.

TRAP documented for future maintainers: the OVMF_VARS store may carry
`Boot####` entries from previous use (the shipped PomeOS VARS boots the EFI
Internal Shell and never tries the default \\EFI\\BOOT\\BOOTX64.EFI path).
The harness therefore boots from a zeroed copy of the VARS file: OVMF treats
an all-zero store as pristine, discovers the FAT disk and starts the app.
The reused firmware binaries are never modified.

Exit codes: 0 all cases pass, 1 a case failed, 77 skipped (no cider binary /
OVMF / qemu-system-x86_64).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "cider"))
LINK = os.path.join(REPO, "scripts", "link_uefi.sh")

OVMF_CANDIDATES = [
    (os.environ.get("OVMF_CODE"), os.environ.get("OVMF_VARS")),
    ("<external-os>/OVMF/OVMF_CODE-pure-efi.fd",
     "<external-os>/OVMF/OVMF_VARS-pure-efi.fd"),
    ("<external-port>/ovmf/OVMF_CODE-pure-efi.fd",
     "<external-port>/ovmf/OVMF_VARS-pure-efi.fd"),
    ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
]


def find_ovmf():
    for code, var in OVMF_CANDIDATES:
        if code and var and os.path.exists(code) and os.path.exists(var):
            return code, var
    return None


def qemu_boot_command(code, vars_copy, esp, log):
    """The exact boot command used for every case (also printed on failure)."""
    return [
        "qemu-system-x86_64", "-m", "256", "-display", "none", "-no-reboot",
        "-serial", "file:" + log,
        "-drive", "if=pflash,format=raw,unit=0,file=" + code + ",readonly=on",
        "-drive", "if=pflash,format=raw,unit=1,file=" + vars_copy,
        "-drive", "file=fat:rw:" + esp + ",format=raw",
    ]


def read_log(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def print_cmd(cmd):
    print("boot command: " + " ".join(cmd))


def build_and_boot(src, expect, code, var, timeout=90):
    with tempfile.TemporaryDirectory() as tmp:
        asm = os.path.join(tmp, "app.asm")
        efi = os.path.join(tmp, "app.efi")
        for step in ([CIDER, "compile", "--target", "uefi", src, "-o", asm],
                     ["bash", LINK, asm, "-o", efi]):
            r = subprocess.run(step, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL {src}: {' '.join(step)}\n{r.stdout}{r.stderr}")
                return False
        esp = os.path.join(tmp, "esp")
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(efi, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))

        # Pristine zeroed variable store (see module docstring).
        vars_copy = os.path.join(tmp, "OVMF_VARS.fd")
        with open(vars_copy, "wb") as f:
            f.truncate(os.path.getsize(var))

        log = os.path.join(tmp, "serial.log")
        cmd = qemu_boot_command(code, vars_copy, esp, log)
        if os.environ.get("CIDER_UEFI_VERBOSE"):
            print_cmd(cmd)
        qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + timeout
        text = ""
        try:
            while time.monotonic() < deadline:
                text = read_log(log)
                if expect in text or qemu.poll() is not None:
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
        text = read_log(log)
        if expect not in text:
            print(f"FAIL {src}: missing {expect!r}")
            print_cmd(cmd)
            tail = text[-2000:]
            print("--- serial (tail) ---\n" + tail)
            return False
        print(f"PASS {src}: {expect!r}")
        return True


def check_abi_identity_by_design():
    """Carry-over T5 Q4: tests/programs/fn_ptr_return uses unmarked `fn`
    types, which stay SysV, while under --target uefi unmarked definitions
    are MS. The ABI identity rule therefore rejects it — a by-design
    diagnostic, not a crash. The msabi-spelled counterpart that does pass
    is tests/uefi/abi_torture.cid (pick/addOne/addThree)."""
    src = os.path.join(REPO, "tests", "programs", "fn_ptr_return", "main.cid")
    r = subprocess.run([CIDER, "check", "--target", "uefi", src],
                       capture_output=True, text=True)
    expected = ("Return type mismatch. Expected fn(int, int) -> int, "
                "got msabi fn(int, int) -> int")
    if r.returncode != 1 or expected not in r.stderr:
        print(f"FAIL {src}: expected the by-design ABI identity diagnostic\n"
              f"{r.stdout}{r.stderr}")
        return False
    print(f"PASS {src}: by-design ABI identity diagnostic")
    return True


def main():
    if not os.path.exists(CIDER):
        print("cider binary missing; build first", file=sys.stderr)
        return 77
    ovmf = find_ovmf()
    if ovmf is None:
        print("OVMF not found (set OVMF_CODE/OVMF_VARS)", file=sys.stderr)
        return 77
    if shutil.which("qemu-system-x86_64") is None:
        print("qemu-system-x86_64 not found", file=sys.stderr)
        return 77
    code, var = ovmf
    print(f"cider: {CIDER}")
    print(f"OVMF:  {code}")
    print(f"       {var}")
    cases = [
        ("examples/uefi/hello.cid", "Hello from Cider UEFI!"),
        ("examples/uefi/info.cid", "FirmwareRevision: "),
        ("examples/uefi/args.cid", "LoadOptions: "),
        ("tests/uefi/abi_torture.cid", "ABI OK"),
    ]
    ok = check_abi_identity_by_design()
    for src, expect in cases:
        ok = build_and_boot(os.path.join(REPO, src), expect, code, var) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
