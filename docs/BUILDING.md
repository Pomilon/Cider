# Building and testing Cider

## Prerequisites

For the native compiler and tests:

- CMake 3.20 or newer;
- a C++20 compiler (GCC or Clang);
- Ninja, or another CMake generator;
- GNU `as` and `ld` on Linux x86-64;
- Python 3 for the corpus, gates, fuzzer, and Python-driven tests.

The optional UEFI smoke test additionally needs:

- `clang` with the `x86_64-pc-win32-coff` target;
- GNU `ld` built with the `i386pep` emulation;
- QEMU and OVMF when running the firmware boot test.

## Configure and build

A Release build with Ninja:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

The compiler is `build/cider`. The build enables `-Wall -Wextra -Werror` so a
missing switch case or unused-value warning fails the build.

## Library path

Dotted imports such as `std.io` resolve through `CIDER_LIB`:

```sh
export CIDER_LIB="$PWD/modules"
```

Without it, a dotted import fails at resolve time. The test runners set their
own default to the repository's `modules/` directory, but exporting the path
explicitly keeps manual compiler invocations consistent.

## Compiler quick check

```sh
./build/cider check examples/hello.cid
./build/cider run examples/hello.cid
./build/cider run --target vm examples/hello.cid
```

`check` is silent on success. `run` uses native code generation by default;
`run --target vm` interprets the optimized IR. Add `--no-opt` to the VM leg to
interpret the unoptimized module.

## CTest

The CTest suite contains 45 tests: unit tests for each compiler layer, the
architecture gates, and end-to-end tests for entries, targets, allocation,
interfaces, objects, optimizer slices, and runtime diagnostics.

```sh
export CIDER_BIN="$PWD/build/cider"
export CIDER_LIB="$PWD/modules"
ctest --test-dir build --output-on-failure
```

Expected result: `100% tests passed, 0 tests failed out of 45`.

The UEFI boot smoke test is opt-in because it depends on firmware images:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCIDER_UEFI_TESTS=ON
cmake --build build -j2
ctest --test-dir build -R uefi --output-on-failure
```

If the required tools are absent, the test reports `SKIP` rather than passing
silently.

## Corpus

The corpus runner compares native, unoptimized VM, and optimized VM execution
for every program under `tests/corpus/`:

```sh
export CIDER_BIN="$PWD/build/cider"
export CIDER_LIB="$PWD/modules"
python3 tests/run_corpus.py
```

Expected result:

```text
corpus: 318 programs, 0 failures, 0 pending(D), 0 skipped
```

Class A entries must match stdout, stderr, and exit status exactly on all three
legs. Class B and C entries must fail loudly and identically. A fixture that
defines `main` without `_start` is wrapped at test time by
`tests/source_entry.py`; the compiler is never given a hidden entry shim.

## Architecture gates

```sh
python3 scripts/gates.py
```

Expected result: `12 pass, 0 fail, 0 pending`. The gates check type-switch
discipline, layout ownership, visitor and layer boundaries, structural
validation, deterministic emission, and the top-level diagnostic boundary.

Useful options:

```sh
python3 scripts/gates.py --strict
python3 scripts/gates.py --only T1,T4
python3 scripts/gates.py --list
```

`--strict` turns pending gates into failures. A pending gate is not treated as
a pass.

## Differential fuzzer

```sh
export CIDER_BIN="$PWD/build/cider"
export CIDER_LIB="$PWD/modules"
python3 tests/fuzz.py --seed 20261027 --count 250
```

Expected result: `fuzz: 250/250 pass, 0 failures (seed=20261027)`.

The fuzzer generates defined-behavior integer programs, evaluates the same
expression tree with C integer semantics, and compares the compiled program's
exit status. It deliberately stays inside the covered integer surface; a
refusal is a fuzzer failure, not a silent skip.

## Continuous integration

`.github/workflows/ci.yml` runs on pushes and pull requests to `main`, and can
be started manually. It installs a C++/CMake/Ninja/Python toolchain, configures
a Release Ninja build, and then runs:

1. `cmake --build build -j2`;
2. `ctest --test-dir build --output-on-failure`;
3. `python3 tests/run_corpus.py`;
4. `python3 scripts/gates.py`;
5. `python3 tests/fuzz.py --seed 20261027 --count 250`.

The workflow exports `CIDER_BIN` and `CIDER_LIB` for the Python steps. It reads
the repository only and has no deployment step.

## Troubleshooting

- `cannot open ...`: check the source path and whether the file exists.
- A dotted import fails: set `CIDER_LIB` to the repository's `modules/`.
- `as` or `ld` is missing: install GNU binutils; native `run` and `compile -c`
  need both.
- A UEFI link test reports `SKIP`: install clang and a GNU `ld` with the
  `i386pep` emulation, or leave `CIDER_UEFI_TESTS=OFF`.
- A VM test and native test disagree: keep the generated program and run all
  three legs separately with `--target vm` and `--no-opt` to identify the
  divergent pipeline.
