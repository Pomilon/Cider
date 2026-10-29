# Cider documentation

This directory describes the v4 compiler and language as they exist in this
tree. The documents are intentionally implementation-facing: where the code and
a statement here disagree, the code is the current behaviour and the document
should be corrected.

| Document | Purpose |
| --- | --- |
| [LANGUAGE.md](LANGUAGE.md) | Source syntax, types, expressions, declarations, allocation, `Result`, C variadics, diagnostics |
| [COMPILER.md](COMPILER.md) | Front end, IR, optimizer, code generation, VM, interfaces, build cache, and linking |
| [BUILDING.md](BUILDING.md) | Toolchain prerequisites, CMake build, CTest, corpus, gates, fuzzer, UEFI smoke test, and CI |
| [TARGETS.md](TARGETS.md) | Native Linux, UEFI, VM, and external-OS target contracts |
| [STDLIB.md](STDLIB.md) | The `modules/std` modules and their current status |

## Reading order

1. [BUILDING.md](BUILDING.md) to build the compiler and run its checks.
2. [LANGUAGE.md](LANGUAGE.md) for the source surface.
3. [TARGETS.md](TARGETS.md) for entry points and output formats.
4. [COMPILER.md](COMPILER.md) for the implementation pipeline.
5. [STDLIB.md](STDLIB.md) for the bundled library modules.

The repository has no other documentation directory. Test fixtures and source
comments are part of the code and are not duplicated here.
