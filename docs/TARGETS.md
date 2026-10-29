# Targets

A target is a descriptor in `src/target/target.h`, not a compiler layer. It
owns the ABI, entry symbol, output form, object form, link path, syscall
convention, inline-assembly policy, and null-fault policy that later stages
read. `target::info(Kind)` is exhaustive over the target kinds.

## Descriptor table

| Target | `Kind` | ABI | Entry | Output | Object | Link | Syscalls |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Native Linux | `Kind::Native` | `Abi::SysV` | `_start` | assembly | ELF64 | host `as` + `ld -nostdlib` | Linux |
| External OS (`pomeos`) | `Kind::Pomeos` | `Abi::PomeOS` | `_start` | assembly | ELF64 | external link script, then PXE packing | external-OS numbers |
| UEFI | `Kind::Uefi` | `Abi::MsAbi` | `efi_main` | PE32+ | PE32+ | clang COFF + `ld -m i386pep` | firmware |
| VM | `Kind::Vm` | `Abi::SysV` | `_start` | interpreted | none | none | none |

`target::parse` accepts `native`, `linux`, and `x86_64-linux` for
`Kind::Native`; `pomeos`; `uefi`; and `vm`. Any other name is an unknown-target
diagnostic.

## Native Linux

Native is the default target. It emits GNU Intel-syntax assembly and links an
ELF64 executable with:

```sh
ld -nostdlib -e _start
```

The program must define:

```cider
func _start() -> void
```

The entry takes no parameters, returns `void`, and uses the SysV convention.
The compiler does not emit a `_start` shim, call `main`, or exit on the
program's behalf. Linux enters `_start` with a 16-byte-aligned stack; the
emitter applies the entry adjustment only to `_start`, leaving ordinary
functions and call-like entries unchanged.

`cider run` compiles, assembles, links, and executes a native program. The
process exit status is the program's own.

## External OS (`pomeos`)

The external-OS target uses the `pomeos` name and the `Kind::Pomeos` target
kind. Its ABI is `Abi::PomeOS`: the ordinary x86-64 SysV calling convention
with the external OS's own entry and syscall numbers. It is not a second C
calling convention.

A program defines one of:

```cider
func _start() -> void
func _start(argc: int, argv: **char) -> void
```

Both entries use the SysV convention. The argument form receives `argc` in
`rdi` and `argv` in `rsi`. The compiler does not inject a Linux exit syscall
or rewrite source syscall numbers.

The target emits ELF64 objects. The external link step supplies its own linker
script, which normally places the image at its per-program base address
(`0x2000000` in the reference layout), and a packer adds the PXE header the
kernel loads. `cider link --target pomeos` accepts `--script` and
`--base-address`; the latter defaults to `0x2000000` and is passed as
`BASE_ADDR`.

`cider run --target pomeos` refuses because the result is an external-OS image,
not a host executable. Compile to assembly, assemble it, link it with the
external script, and pack it with the external toolchain.

## UEFI

UEFI emits a PE32+ EFI application. The program must define:

```cider
msabi func efi_main(image_handle: *void, system_table: *void) -> int
```

The entry must be `msabi`, must use exactly those two parameter names and
types, and must return `int`. The compiler injects no shim and no firmware
handle globals; the program stores or uses the handles itself.

Inline assembly is allowed, including the register and firmware-protocol
sequences a bootloader needs. The assembly is GNU syntax; `scripts/link_uefi.sh`
assembles it with clang's `x86_64-pc-win32-coff` target and links with:

```sh
ld -m i386pep -shared -Bsymbolic -nostdlib --subsystem 10 --image-base 0 -e efi_main
```

`cider compile --target uefi -o image.asm` writes assembly. `cider run`
refuses because the result is not a host executable.

Microsoft x64 variadic functions are supported in code generation: argument
registers are homed into shadow space, floating variadic arguments are
duplicated into their positional general-purpose registers, and no `al` count
is emitted. The VM refuses to execute a variadic `msabi` function; that path is
covered by native code generation and the UEFI link tests.

## VM

The VM interprets the typed IR. It is selected with:

```sh
cider run --target vm program.cid
cider run --target vm --no-opt program.cid
```

The default VM leg runs the optimized module; `--no-opt` runs the unoptimized
module. The VM uses the same value widths, trap policy, and SysV variadic
cursor as native code. `compile --target vm` is rejected because the VM is an
interpreter, not an emitter.

The corpus runs native, VM-unoptimized, and VM-optimized legs to detect
divergence between lowering, optimization, and execution.

## Entry validation

The checker validates the source-defined entry before lowering. Missing or
wrong entries fail both `check` and `compile` with a diagnostic that names the
required symbol and shape:

- native and external OS: `_start`; no parameters or the documented
  `argc`/`argv` pair; `void` return; SysV convention;
- UEFI: `efi_main`; `msabi`; `image_handle: *void`,
  `system_table: *void`; `int` return.

`main` is an ordinary function. A program that defines only `main` is rejected
unless a source-level wrapper supplies the target entry.

## Target-specific services

The compiler owns layout, ABI, entry validation, and code generation. It does
not own a heap, I/O, formatting, or firmware wrappers. Programs obtain those
from explicit libraries, inline assembly, or target services. The bundled
`std.io`, `std.fs`, `std.time`, and `std.alloc` modules use Linux syscalls and
are therefore convenience/test code for the native target, not portable target
services.
