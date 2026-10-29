# Cider compiler

The compiler is a C++20 program with one executable, `cider`. It is organized
as static libraries so each layer can be tested without the layers above it.
This document describes the current pipeline and the artifacts it produces.

## Pipeline

```text
source .cid
  -> lexer        tokens with positions and decoded literals
  -> parser       AST
  -> resolver     modules, scopes, declarations, layout, signatures
  -> checker      typed body walk (and lowering callbacks)
  -> lowering     SSA IR in stack-slot form
  -> optimizer    structural checks and IR passes
  -> codegen      GNU Intel-syntax x86-64 assembly
     or VM        direct IR interpretation
```

The driver runs the same front end for `check`, `compile`, `ir`, `run`, and
`build`. A split-module compile stops after lowering only the named module;
a whole-program build lowers every source module into one IR module, then
prunes definitions unreachable from the target entry.

### Lexer

`src/lexer` turns bytes into `Token` values. Every token carries its spelling,
its source position, and a decoded literal value. Keywords are distinct token
kinds; integers retain their base and digits, floats retain their text, and
string literals retain their parts. Lexing records malformed literals and
continues so one file can report several errors.

### Parser

`src/parse` builds the AST declared in `src/ast/ast.h`. The parser decides
syntax only: it does not resolve names, infer types, or consult the resolver.
It recovers after a bad declaration so downstream phases can report true
cascades rather than stopping at the first malformed construct.

### Resolver

`src/resolve` performs the header pass for every imported module before any
body is checked. It binds module and local scopes, loads imports, resolves type
spellings and signatures, computes struct/class/union layouts, instantiates
explicit generics, and records declaration identities. Module scope is
order-independent, so globals and functions may be used above their
declarations.

The resolver also owns `src/layout.h`, the single place where sizes and
alignments are written. Layout queries cover scalars, pointers, arrays, slices,
strings, closures, interfaces, unions, enums, classes, and the opaque
`va_list`.

### Checker and lowering

`src/check` contains the compiler's only `ASTVisitor`. The checker walks every
body once, records each expression's type, validates visibility, assignment,
calls, patterns, casts, allocation, variadics, and target-specific rules, and
emits IR through a `LowerSink` while it walks. Lowering (`src/lower`) therefore
queries the checker's results rather than re-walking the AST.

`src/eval` folds source-level constant expressions for the resolver, checker,
and layout queries.

## IR

`src/ir` defines the SSA instruction set. A `Value` carries an id, a machine
width (`IRType`), and the language type it came from. Lowering initially uses
stack slots for named locals and temporaries; `mem2reg` promotes eligible
scalar slots to SSA form and inserts phis at joins.

Core operation groups:

- constants, copies, phis, and integer/float arithmetic;
- comparisons with signed and unsigned predicates;
- loads, stores, address formation, field extraction/insertion, and globals;
- calls, including indirect, SysV/MS ABI, and variadic metadata;
- `VaStart` and `VaArg` for C variadic access;
- casts, traps, and inline assembly.

Instructions carry `mayTrap`, `isVolatile`, `isPure`, and an overflow mode so
optimizations can respect faulting, volatile, and defined-wrap behavior.
Aggregates are represented as multiple words; `layout.h` decides how many.

## Optimizer

`src/opt` runs a fixed pipeline after structural validation. The structural
check verifies definitions, uses, terminators, phi shapes, dominance, and
variadic operations. It runs again after selected passes, so a malformed
rewrite becomes a compiler error rather than invalid assembly.

The pipeline order is:

1. inline small direct callees;
2. promote scalar slots with `mem2reg`;
3. fold integer constants;
4. reduce integer identities and strength cases;
5. simplify control flow from constant branches;
6. remove empty blocks and merge single-predecessor blocks;
7. value-number duplicate pure expressions;
8. hoist loop-invariant expressions into existing preheaders;
9. forward remaining scalar slots and remove dead stores;
10. eliminate dead instructions;
11. deduplicate exact-byte read-only data.

Whole-program executable builds then prune functions, globals, and rodata that
the entry cannot reach. Split objects keep every definition because another
object may reference it. `--no-opt` skips the optimizer entirely, which is what
the corpus's unoptimized VM leg uses.

The optimizer never deletes a trapping instruction, merges or moves a volatile
instruction, changes an overflow mode, or moves an instruction across a
dependency that can trap.

## Code generation

`src/codegen` emits GNU assembler Intel syntax. The emitter reads IR and the
target descriptor, never the AST. It handles:

- SysV AMD64 and Microsoft x64 argument rows, including shadow space and
  floating-point registers;
- scalar and aggregate returns, including hidden-pointer (sret) returns;
- C variadic prologues and `al` counts or Microsoft floating duplicates;
- stack frames, globals, read-only data, `.bss`, weak definitions, and
  relocations;
- diagnosed traps that write `vm error: ...` and exit with status 2;
- inline and module-level assembly.

Register allocation is conservative and global: values that fit a block-local
live range may stay in caller-saved registers, while longer-lived values use a
callee-saved pool. Calls, traps, assembly, and other barriers prevent
caller-saved values from remaining live across them. Values that cannot be
homed keep their stack slot.

## VM

`src/vm` interprets the same IR without an assembler or linker. `cider run
--target vm` runs the optimized module; `--no-opt` runs the unoptimized module.
The VM implements the same value widths, trap policy, and SysV variadic cursor
as native code. It refuses a variadic Microsoft-ABI function because that
layout is exercised through native UEFI code generation.

The VM is a differential test oracle, not an emitter. `compile --target vm` is
rejected with an explicit diagnostic.

## Interfaces

`src/interface` defines the `.cidm` module interface. An interface is a
Cider-source-shaped declaration list with bodies omitted:

- functions end at `;`;
- globals carry types but no initializers;
- aggregates keep their fields, methods, and signatures;
- variadic markers are preserved;
- metadata comments carry the format version, compiler tag, module name,
  source hash, dependency hashes, and a `requires-source` reason when a
  declaration cannot be rendered.

The current format version is 1 and the compiler tag is
`cider-v4-interface-1`. An importer may resolve a dotted or plain module from
`<interface-dir>/<module>.cidm` before source. A missing interface falls back
to source; `--require-interface` makes that missing artifact an error. A stale
or incompatible interface is a diagnostic, and `--allow-stale-interface` may
fall back to source instead. Interfaces that cannot describe a module's
surface are marked `requires-source`, and the loader uses the source.

`compile -c` writes the interface as a sidecar beside the object unless
`--no-interface` is given.

## Objects and linking

`compile -c` emits a real ELF64 `ET_REL` object by assembling the emitted
GNU text with `as`. Split-module compiles name one module with `--module` or
`--no-entry`; non-entry modules mangle every definition, and compiler-minted
generic specializations are weak so two objects can share one. The entry
object keeps the target entry symbol.

`cider link` runs GNU `ld -nostdlib` over ELF64 objects. Native links use
`-e _start`; the external-OS target accepts `--script` and
`--base-address` and passes `BASE_ADDR` to the link script. UEFI uses
`scripts/link_uefi.sh`, which assembles COFF with clang and links a PE32+ EFI
application with GNU `ld -m i386pep`.

`compile` to assembly writes to standard output unless `-o` names a file.
`compile --target uefi` writes assembly; link it with the UEFI script.
`compile --target pomeos` writes assembly for the external-OS link path; `run`
refuses because the image is not a host executable.

## Build cache

`cider build` resolves the import graph, compiles modules in dependency order,
and stores objects and interfaces in a content-addressed cache under
`.cider-cache` by default. A cache key covers:

- the module source hash;
- each dependency's cache key and interface hash;
- the target;
- whether optimization is enabled;
- whether extra checks are enabled;
- the interface format version and compiler tag.

An unchanged module is copied from its cache entry rather than re-parsed.
`--cache-dir` changes the cache root, `--objects-dir` copies final objects to
`<dir>/<module>.o`, `--no-link` stops after objects, and `--verbose` prints
`compiled` or `cached` keys.

## Driver

The commands are `check`, `compile`, `ir`, `run`, `link`, `build`, and
`bundle`. `bundle` is reserved and currently reports `not implemented yet`.

`check` is quiet on success. `ir` prints the lowered module. `run` assembles
and links native code or interprets IR for the VM. The driver maps argument
and I/O failures to status 2, front-end failures to status 1, and returns the
executed program's exit status.
