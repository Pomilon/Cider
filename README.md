# Cider

Cider is a statically typed, ahead-of-time systems programming language with a
C++20 compiler. The compiler emits freestanding x86-64 programs: generated
code does not link a C runtime, a compiler runtime archive, or a garbage
collector. Programs define their own entry point and import any library code
they use.

The current tree is the v4 generation of the compiler and language. It
supports native Linux, UEFI, the Cider virtual machine, and the external-OS
`pomeos` target. The bundled `modules/std` tree is convenience and test
library code, not part of the language runtime.

## Current status

The repository is green on its four main verification surfaces:

| Surface | Result |
| --- | --- |
| CTest | 45 of 45 tests pass |
| Differential corpus | 318 programs, 0 failures |
| Architecture gates | 12 pass, 0 fail |
| Differential fuzzer | 250 of 250 programs pass (seed 20261027) |

The corpus compares native execution, unoptimized VM execution, and optimized
VM execution. A test harness may wrap a fixture that spells `main` with a
source-level `_start`; the compiler itself never synthesizes an entry point.

## Quick start

Requirements for the native compiler:

- CMake 3.20 or newer and a C++20 compiler
- GNU `as` and `ld` on Linux x86-64
- Python 3 for the corpus, gates, and fuzzer
- Ninja is recommended

Build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

Point the compiler at the bundled library:

```sh
export CIDER_LIB="$PWD/modules"
```

A minimal program:

```cider
from std.io import print, exit;

func _start() -> void {
    exit(main());
}

func main() -> int {
    print("Hello, Cider!\n");
    return 0;
}
```

Check and run it:

```sh
./build/cider check hello.cid
./build/cider run hello.cid
./build/cider run --target vm hello.cid
```

The native and VM legs should print the same output. `compile` writes GNU
Intel-syntax assembly; `compile -c` assembles it into an ELF64 relocatable
object. See [docs/BUILDING.md](docs/BUILDING.md) for the full test and toolchain
matrix.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/lexer`, `src/parse`, `src/ast` | Source text to tokens and an AST |
| `src/resolve` | Modules, imports, scopes, layouts, generics, and signatures |
| `src/check` | Typed body validation and the one AST traversal |
| `src/lower` | Checked AST to SSA IR |
| `src/ir` | Instruction set, values, aggregates, and trap reasons |
| `src/opt` | Structural validation and the optimization pipeline |
| `src/codegen` | x86-64 assembly emission and register allocation |
| `src/vm` | IR interpreter used by the optimized and unoptimized VM legs |
| `src/target` | The target descriptor table |
| `src/interface` | `.cidm` module interfaces and hashing |
| `src/driver` | Command-line driver, object emission, linking, and build cache |
| `modules/std` | Explicitly imported Cider library modules |
| `tests/unit` | Per-layer C++ tests |
| `tests/corpus` | Differential language corpus |
| `tests/v4` | End-to-end target, ABI, optimizer, and build tests |
| `scripts/gates.py` | Architecture and reproducibility gates |
| `examples` | Small native and UEFI programs |

## Documentation

- [docs/README.md](docs/README.md) — documentation index
- [docs/LANGUAGE.md](docs/LANGUAGE.md) — language surface
- [docs/COMPILER.md](docs/COMPILER.md) — compiler pipeline and artifacts
- [docs/BUILDING.md](docs/BUILDING.md) — building, testing, and CI
- [docs/TARGETS.md](docs/TARGETS.md) — native, UEFI, VM, and external-OS targets
- [docs/STDLIB.md](docs/STDLIB.md) — bundled `std` modules

## License

Licensed under the Apache License, Version 2.0 with the LLVM Exception.
See [LICENSE.txt](LICENSE.txt) and [NOTICE](NOTICE) for details.
