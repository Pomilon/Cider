# CMakeLists.txt — Cider v3.
#
# OUTPUT.md §3 requires byte-reproducible output, and gate L5 checks it by
# compiling the same IR twice from two different build directories and comparing
# bytes. So there is no absolute path in this file, no timestamp, and no
# generator-dependent ordering: nothing here may write a path into an artifact.
#
# There is one compiler target (`cider`) and one test target (`layout_test`).
# The test target exists because gate T4's second half is a test, not a grep —
# see tests/unit/layout_test.cpp for why both halves are needed.

cmake_minimum_required(VERSION 3.20)
project(cider LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Gate T1's first half is -Werror=switch, which is what makes a missing
# `enum class Kind` case a build failure instead of a silent fallthrough. It is
# here rather than in the test target so that every future target inherits it.
add_compile_options(-Wall -Wextra -Werror -Werror=switch -Wno-unused-parameter)

# --- The type system ----------------------------------------------------------
#
# TYPES.md §2.1: a type is a tagged union, and the tag is the one list that
# every switch in the compiler must agree with. Gate T1 polices that agreement,
# which is why these switches have no `default:` -- see kind.cpp.

add_library(cider_types STATIC src/types/kind.cpp src/types/type.cpp
                               src/types/signature.cpp src/types/context.cpp
                               src/types/variadic.cpp)
target_include_directories(cider_types PUBLIC src)

# --- The lexer ---------------------------------------------------------------
#
# LAYERS.md §3.1: one character in, one token out, with a position. May read
# the source text and nothing else.

add_library(cider_lexer STATIC src/lexer/token.cpp src/lexer/lexer.cpp)
target_include_directories(cider_lexer PUBLIC src)

# --- The AST ------------------------------------------------------------------
#
# Header-only: every node and the one visitor live in src/ast/ast.h, so there
# is nothing to compile. The INTERFACE target exists so tests link against
# something named, and so a future ast.cpp has a home.

add_library(cider_ast INTERFACE)
target_include_directories(cider_ast INTERFACE src)

# --- Targets -------------------------------------------------------------------
#
# OUTPUT.md §2: a target is a description, not a layer. This is the one
# authority for ABI, entry, output form and target policy.

add_library(cider_target STATIC src/target/target.cpp)
target_include_directories(cider_target PUBLIC src)

# --- Constant evaluation -------------------------------------------------------
#
# LAYERS.md §3.4: one folder for source expressions, shared by the resolver,
# the checker and the optimiser.

add_library(cider_eval STATIC src/eval/eval.cpp)
target_link_libraries(cider_eval PUBLIC cider_types cider_ast)
target_include_directories(cider_eval PUBLIC src)

# --- The resolver ---------------------------------------------------------------
#
# LAYERS.md §3.3: headers of every module first, so module scope is
# order-independent and imports resolve recursively.

add_library(cider_resolve STATIC src/resolve/resolver.cpp)
target_link_libraries(cider_resolve PUBLIC cider_eval cider_ast cider_types cider_target)
target_include_directories(cider_resolve PUBLIC src)

# --- The checker -----------------------------------------------------------------
#
# LAYERS.md §3.4: the body walk. Writes each expression's Type and checks it
# in place. This is the one `public ASTVisitor` in src/ (gates T5/L1).

add_library(cider_check STATIC src/check/checker.cpp)
target_link_libraries(cider_check PUBLIC cider_resolve cider_eval cider_types cider_target)
target_include_directories(cider_check PUBLIC src)

# --- The IR --------------------------------------------------------------------

# IR.md: the instruction set, the aggregate representation and the ABI. Three
# per-instruction flags exist because dead-code elimination, LICM and inlining
# all depend on them.

add_library(cider_ir STATIC src/ir/ir.cpp)
target_link_libraries(cider_ir PUBLIC cider_types)
target_include_directories(cider_ir PUBLIC src)

# --- Lowering ------------------------------------------------------------------

# Driven by the checker's walk: LAYERS.md 3.5.1 says lowering queries the walk's
# result and does not re-walk the AST.

add_library(cider_lower STATIC src/lower/lower.cpp)
target_link_libraries(cider_lower PUBLIC cider_check cider_ir)
target_include_directories(cider_lower PUBLIC src)

# --- The parser ---------------------------------------------------------------
#
# LAYERS.md §3.2: accepts or rejects on syntax alone. Never resolves a name,
# never infers a type, never consults the resolver.

add_library(cider_parse STATIC src/parse/parser.cpp)
target_link_libraries(cider_parse PUBLIC cider_ast cider_lexer)
target_include_directories(cider_parse PUBLIC src)

# --- Tests --------------------------------------------------------------------

enable_testing()

add_executable(layout_test tests/unit/layout_test.cpp)
target_include_directories(layout_test PRIVATE src)

add_executable(type_test tests/unit/type_test.cpp)
target_link_libraries(type_test PRIVATE cider_types)
target_include_directories(type_test PRIVATE src)

add_executable(lexer_test tests/unit/lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE cider_lexer)
target_include_directories(lexer_test PRIVATE src)

# The gates are a test like any other, so `ctest` alone is the whole gate. The
# script reports three states (PASS/FAIL/PENDING) and exits non-zero only on
# FAIL; a pending gate is unexamined, not satisfied, and the script says which
# file will make it checkable.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME gates COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gates.py)
add_test(NAME layout COMMAND layout_test)
add_test(NAME types COMMAND type_test)
add_test(NAME lexer COMMAND lexer_test)

add_executable(ast_test tests/unit/ast_test.cpp)
target_link_libraries(ast_test PRIVATE cider_ast cider_lexer)
target_include_directories(ast_test PRIVATE src)
add_test(NAME ast COMMAND ast_test)

add_executable(parser_test tests/unit/parser_test.cpp)
target_link_libraries(parser_test PRIVATE cider_parse cider_lexer)
target_include_directories(parser_test PRIVATE src)
add_test(NAME parser COMMAND parser_test)

add_executable(resolve_test tests/unit/resolve_test.cpp)
target_link_libraries(resolve_test PRIVATE cider_resolve cider_parse cider_lexer)
target_include_directories(resolve_test PRIVATE src)
add_test(NAME resolver COMMAND resolve_test)

add_executable(check_test tests/unit/check_test.cpp)
target_link_libraries(check_test PRIVATE cider_check cider_resolve cider_parse cider_lexer)
target_include_directories(check_test PRIVATE src)
add_test(NAME checker COMMAND check_test)

add_executable(lower_test tests/unit/lower_test.cpp)
target_link_libraries(lower_test PRIVATE cider_lower cider_check cider_parse cider_lexer)
target_include_directories(lower_test PRIVATE src)
add_test(NAME lowering COMMAND lower_test)

# --- The optimiser ---------------------------------------------------------------
#
# IR.md section 7: a pass may change the shape and not the meaning. The
# structural check runs at the entry, before any pass (gate L4).

add_library(cider_opt STATIC src/opt/structural.cpp src/opt/dominance.cpp
                               src/opt/memory.cpp src/opt/mem2reg.cpp
                               src/opt/inline.cpp src/opt/gvn.cpp
                               src/opt/licm.cpp src/opt/simplify.cpp
                               src/opt/blocks.cpp src/opt/strength.cpp
                               src/opt/philower.cpp src/opt/dce.cpp
                               src/opt/sccp.cpp src/opt/dedup.cpp
                               src/opt/alias.cpp src/opt/sroa.cpp
                               src/opt/opt.cpp)
target_link_libraries(cider_opt PUBLIC cider_ir cider_types)
target_include_directories(cider_opt PUBLIC src)

add_executable(opt_test tests/unit/opt_test.cpp)
target_link_libraries(opt_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(opt_test PRIVATE src)
add_test(NAME optimise COMMAND opt_test)

add_executable(inline_test tests/unit/inline_test.cpp)
target_link_libraries(inline_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(inline_test PRIVATE src)
add_test(NAME inline COMMAND inline_test)

add_executable(gvn_test tests/unit/gvn_test.cpp)
target_link_libraries(gvn_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(gvn_test PRIVATE src)
add_test(NAME gvn COMMAND gvn_test)

add_executable(sccp_test tests/unit/sccp_test.cpp)
target_link_libraries(sccp_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(sccp_test PRIVATE src)
add_test(NAME sccp COMMAND sccp_test)

add_executable(sroa_test tests/unit/sroa_test.cpp)
target_link_libraries(sroa_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(sroa_test PRIVATE src)
add_test(NAME sroa COMMAND sroa_test)

add_executable(licm_test tests/unit/licm_test.cpp)
target_link_libraries(licm_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(licm_test PRIVATE src)
add_test(NAME licm COMMAND licm_test)

add_executable(simplify_test tests/unit/simplify_test.cpp)
target_link_libraries(simplify_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(simplify_test PRIVATE src)
add_test(NAME simplify COMMAND simplify_test)

add_executable(blocks_test tests/unit/blocks_test.cpp)
target_link_libraries(blocks_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(blocks_test PRIVATE src)
add_test(NAME blocks COMMAND blocks_test)

add_executable(strength_test tests/unit/strength_test.cpp)
target_link_libraries(strength_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(strength_test PRIVATE src)
add_test(NAME strength COMMAND strength_test)

add_executable(dce_test tests/unit/dce_test.cpp)
target_link_libraries(dce_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(dce_test PRIVATE src)
add_test(NAME dce COMMAND dce_test)

# --- The interpreter -----------------------------------------------------------
#
# OUTPUT.md §2: the vm legs run the IR with no toolchain. Reads the IR only,
# under the same layer discipline as codegen — gate L2 names codegen because
# the interpreter did not exist when it was written.

add_library(cider_vm STATIC src/vm/vm.cpp)
target_link_libraries(cider_vm PUBLIC cider_ir cider_types)
target_include_directories(cider_vm PUBLIC src)

add_executable(vm_test tests/unit/vm_test.cpp)
target_link_libraries(vm_test PRIVATE cider_vm cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(vm_test PRIVATE src)
add_test(NAME vm COMMAND vm_test)

# --- Codegen ---------------------------------------------------------------------
#
# LAYERS.md 3.7: checked IR to assembly. Reads the IR and the target, never the
# AST (gate L2). The default is a whole-function linear scan: an eligible value
# is homed in a caller-saved or callee-saved register for its whole live range,
# and everything else keeps its frame slot. The legacy block-local scan stays
# compiled behind a compile-time switch for the Stage 1 gate comparison.

add_library(cider_codegen STATIC src/codegen/codegen.cpp
                                src/codegen/frame.cpp
                                src/codegen/regalloc.cpp
                                src/codegen/linear_scan.cpp
                                src/codegen/liveness.cpp)
target_link_libraries(cider_codegen PUBLIC cider_ir cider_types cider_target)
target_include_directories(cider_codegen PUBLIC src)

add_executable(codegen_test tests/unit/codegen_test.cpp)
target_link_libraries(codegen_test PRIVATE cider_codegen cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(codegen_test PRIVATE src)
add_test(NAME codegen COMMAND codegen_test)

# The whole-function linear scan is the default allocator; this test pins its
# refusal scope, pressure behavior, and per-ABI pools without assembling.
add_executable(regalloc_test tests/unit/regalloc_test.cpp)
target_link_libraries(regalloc_test PRIVATE cider_codegen cider_ir cider_types)
target_include_directories(regalloc_test PRIVATE src)
add_test(NAME regalloc COMMAND regalloc_test)

# Whole-function liveness is a pure analysis: it answers live-in/live-out sets
# and RPO live ranges, and changes no codegen behaviour by itself.
add_executable(liveness_test tests/unit/liveness_test.cpp)
target_link_libraries(liveness_test PRIVATE cider_codegen cider_ir cider_types)
target_include_directories(liveness_test PRIVATE src)
add_test(NAME liveness COMMAND liveness_test)

# The whole-function linear scan is an analysis too: it consumes Liveness and
# answers homes, spilling, and the callee-saved set without assembling anything.
add_executable(linear_scan_test tests/unit/linear_scan_test.cpp)
target_link_libraries(linear_scan_test PRIVATE cider_codegen cider_ir cider_types)
target_include_directories(linear_scan_test PRIVATE src)
add_test(NAME linear_scan COMMAND linear_scan_test)

# Task 10's compact frame: only unhomed values, alloca cells, call words,
# callee-save cells and the variadic area reserve bytes, and every offset is
# disjoint and 16-byte framed.
add_executable(frame_test tests/unit/frame_test.cpp)
target_link_libraries(frame_test PRIVATE cider_codegen cider_ir cider_types)
target_include_directories(frame_test PRIVATE src)
add_test(NAME frame COMMAND frame_test)

# --- The driver --------------------------------------------------------------------
#
# OUTPUT.md section 1: check, compile, ir, run and link. compile -c emits
# ELF64 objects; link drives GNU ld. bundle refuses by name. The top-level
# catch maps every failure to a diagnostic (gate L6).

add_library(cider_driver STATIC src/driver/driver.cpp src/interface/interface.cpp)
target_link_libraries(cider_driver PUBLIC cider_codegen cider_vm cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(cider_driver PUBLIC src)

add_executable(cider src/driver/main.cpp)
target_link_libraries(cider PRIVATE cider_driver)
target_include_directories(cider PRIVATE src)

add_executable(driver_test tests/unit/driver_test.cpp)
target_link_libraries(driver_test PRIVATE cider_driver)
target_include_directories(driver_test PRIVATE src)
add_test(NAME driver COMMAND driver_test)

# WS3: diagnosed runtime faults must agree on native and both VM legs.
add_test(NAME runtime_diagnostics
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/runtime_diagnostics_test.py)
set_tests_properties(runtime_diagnostics PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

add_test(NAME v4_freestanding
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/freestanding_symbol_test.py)
set_tests_properties(v4_freestanding PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Source-defined entry: native `_start` and UEFI `efi_main` are validated
# and emitted directly. The UEFI link smoke exits 77 when clang or ld's
# i386pep emulation is unavailable, so ctest reports SKIP, never a silent
# pass.
add_test(NAME v4_source_entry
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/source_entry_test.py)
set_tests_properties(v4_source_entry PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  SKIP_RETURN_CODE 77)

# v4 has no compiler-provided putchar: a bare extern is an ordinary
# unresolved symbol, while std.io supplies an explicit library function.
add_test(NAME v4_explicit_io
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/explicit_io_test.py)
set_tests_properties(v4_explicit_io PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# The owning String is explicit library code over std.alloc: no prelude,
# no compiler heap, no hidden destructor. The test proves the unimported
# name does not bind and runs the imported type on native and both VM legs.
add_test(NAME v4_string_owning
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/string_owning_test.py)
set_tests_properties(v4_string_owning PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# PomeOS: the emitter shares the SysV x86-64 path with native, but the target
# names its entry (`_start`), object (ELF64), link script (PomeOS
# programs/link.ld), and syscall numbers (PomeOS's own). The link smoke uses
# the reference script; `run` refuses because the image is not a host binary.
add_test(NAME v4_pomeos
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/pomeos_target_test.py)
set_tests_properties(v4_pomeos PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Separate object emission and linking: `compile -c` writes a real ELF64
# ET_REL object with GNU as, and `link` runs ld -nostdlib on the objects.
# Split-module objects keep imported definitions external, while compiler
# minted generic bodies stay local so two objects may use one specialization.
add_test(NAME v4_object_link
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/object_link_test.py)
set_tests_properties(v4_object_link PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  SKIP_RETURN_CODE 77)

# A large all-zero aggregate is storage, not an image: `.bss` plus one
# `.zero` of the declared extent, with ELF size/alignment metadata. Non-zero
# aggregates and strings stay in `.data`/`.rodata`, and the storage remains
# readable and writable.
add_test(NAME v4_zero_global_bss
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/zero_global_bss_test.py)
set_tests_properties(v4_zero_global_bss PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Linux enters a native process with rsp 16-aligned, not at the 8-mod-16
# call frame a normal function assumes. `_start` alone gets the entry
# adjustment; ordinary functions and PomeOS's call-like entry stay unchanged.
add_test(NAME v4_native_entry_alignment
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/native_entry_alignment_test.py)
set_tests_properties(v4_native_entry_alignment PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# The codegen selection flag: `legacy` and `fast` both compile today, and an
# unknown value is rejected by name before any front-end work.
add_test(NAME v4_codegen_flag
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/codegen_flag_test.py)
set_tests_properties(v4_codegen_flag PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 10's compact fast frame: thirty locals and nested calls run on the
# fast backend with and without the optimiser, and the fast assembly is not
# byte-identical to the legacy emitter's.
add_test(NAME v4_fast_frame
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_frame_test.py)
set_tests_properties(v4_fast_frame PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 11's direct arithmetic and fused compares: homed results are written in
# place instead of round-tripping through rax, branch-only compares branch on
# flags, and every integer width keeps Cider's signed/unsigned semantics.
add_test(NAME v4_fast_codegen
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_codegen_test.py)
set_tests_properties(v4_fast_codegen PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 12's memory operands and float homes: width-correct narrow loads and
# stores, GEP address folding, XMM float homes and spill/reload under float
# pressure. The assembly-shape legs fail on the pre-Task-12 xmm0-only emitter.
add_test(NAME v4_fast_memory
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_memory_test.py)
set_tests_properties(v4_fast_memory PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 13's fast ABI: the call row, stack words, xmm/rax/rdx returns, C and
# Cider varargs, div/rem and shift fixed registers, indirect calls, a full
# caller-saved clobber, a 128-bit msabi xmm6-xmm15 save, and rsp alignment.
add_test(NAME v4_fast_abi
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_abi_test.py)
set_tests_properties(v4_fast_abi PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  SKIP_RETURN_CODE 77)

# Task 15.1's fast parameter and call-word homes: the fast allocator no longer
# excludes parameters or call argument words, and the prologue writes a
# parameter's register home instead of its frame slot.
add_test(NAME v4_fast_slot
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_slot_test.py)
set_tests_properties(v4_fast_slot PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 4's first optimizer slice: direct scalar slots forward and drop dead
# stores on every leg, while a function that takes a local's address keeps its
# memory shape because one escaped Alloca can reach a neighbouring frame cell.
add_test(NAME v4_slot_forwarding
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/slot_forwarding_test.py)
set_tests_properties(v4_slot_forwarding PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 5's first slice: block-local caller-saved homes, spill-on-pressure,
# barrier refusal, per-ABI pools. Native programs prove behavior; the msabi
# assembly check proves rdi/rsi stay out of the UEFI pool.
add_test(NAME v4_register_allocation
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/register_allocation_test.py)
set_tests_properties(v4_register_allocation PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 10's native C interop proof for C variadics: a Cider caller calls
# gcc-compiled integer, pointer and f64 variadic helpers (directly and through
# a function pointer), a gcc caller calls Cider-defined variadics using
# va_copy and va_end, and the UEFI msabi fixture carries the shadow-space
# homes and duplicated float. gcc, clang or ld -m i386pep missing is SKIP.
add_test(NAME v4_varargs
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/varargs_test.py)
set_tests_properties(v4_varargs PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  SKIP_RETURN_CODE 77)

# Task 4's second optimizer slice: small direct callees are cloned before
# mem2reg, so parameter slots and return joins promote. Native and both VM legs
# pin parameters, side-effect order, two returns, multiword and sret answers,
# a loop call, and the recursion refusal; two compiles pin stable assembly.
add_test(NAME v4_inlining
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/inlining_test.py)
set_tests_properties(v4_inlining PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 4's dominance-scoped value-numbering slice: duplicate pure arithmetic
# disappears across a join and inside a loop, sibling branches keep their own
# expressions, and both VM legs agree with native. Two compiles pin stability.
add_test(NAME v4_gvn
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/gvn_test.py)
set_tests_properties(v4_gvn PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 4's loop-invariant motion slice: an invariant expression used many
# times moves to the preheader, a nested invariant leaves both loops, and a
# loop body's call and volatile load stay put. Native plus both VM legs agree,
# and two compiles are byte-identical.
add_test(NAME v4_licm
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/licm_test.py)
set_tests_properties(v4_licm PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Task 8's second slice: whole-program pruning reaches globals and rodata.
# Optimized executable assembly drops unreferenced data and data reachable
# only from dead definitions, keeps live initializer chains, and still links
# and runs; split objects and --no-opt keep every definition.
add_test(NAME v4_global_pruning
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/global_pruning_test.py)
set_tests_properties(v4_global_pruning PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Exact-byte rodata deduplication: repeated string literals emit one blob,
# every global/DataAddr reference is rewritten to the canonical label, the
# surviving labels assemble and link, native and both VM legs agree, and
# --no-opt keeps the per-occurrence blobs. The UEFI assembly path is assembled
# through clang's COFF target when the toolchain is available.
add_test(NAME v4_rodata_dedup
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/rodata_dedup_test.py)
set_tests_properties(v4_rodata_dedup PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Constant control-flow simplification: source-level constant if/else branches
# lose their dead arms, unreachable block labels disappear from assembly, a
# nonconstant loop branch survives, and native plus both VM legs agree. Switch
# terminators have no source syntax in this slice; the unit test pins them.
add_test(NAME v4_simplify
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/simplify_test.py)
set_tests_properties(v4_simplify PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Empty-block elimination and single-predecessor merging: nested if/else and
# loop shapes lose blocks in optimized IR and assembly, native plus both VM
# legs agree, and two optimized compiles are byte-identical.
add_test(NAME v4_blocks
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/blocks_test.py)
set_tests_properties(v4_blocks PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Integer strength reduction and peephole identities: zero/one arithmetic,
# all-ones masks, division and remainder by one, and multiplication by zero
# disappear from optimized IR; native plus both VM legs agree; optimized
# assembly is smaller; two compiles are byte-identical.
add_test(NAME v4_strength
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/strength_test.py)
set_tests_properties(v4_strength PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Assignment through a pointer-returning `operator[]`: the member is called
# once and the value is stored through its answer, on native and both VM legs.
# Exact value/index mismatches and a value-returning operator are rejected.
add_test(NAME v4_operator_index_assign
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/operator_index_assign_test.py)
set_tests_properties(v4_operator_index_assign PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Struct-pointer delete is nil-safe and calls the global operator once, with
# no synthesized destructor. Class deinit still runs before release, and every
# unsupported pointee stays a named check error.
add_test(NAME v4_struct_delete
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/struct_delete_test.py)
set_tests_properties(v4_struct_delete PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider")

# Separate front-end compilation: `compile -c` emits a `.cidm` declaration
# sidecar, an importer compiles against it with the source removed, and
# `cider build` keys objects/interface on the module source hash and its
# dependency hashes. Missing, stale and incompatible interfaces each have a
# named diagnostic or an explicit source fallback.
add_test(NAME v4_interfaces
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/interface_build_test.py)
set_tests_properties(v4_interfaces PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  SKIP_RETURN_CODE 77)

# Shared helper for the Stage 1.5 assembly-shape tests: compiles a source
# string on the fast backend, extracts one function body from the Intel
# assembly, and runs a source through a temporary file. The smoke test proves
# the helper itself before later shape tests import it.
add_test(NAME v4_backend_shape
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/backend_shape_test.py)
set_tests_properties(v4_backend_shape PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 16.2's loop-carried homes: a value lowered from a loop phi is defined in
# more than one block, so the allocator must keep one home across every
# definition and mark each definition block present. The body must use no frame
# cells and `loop_sum(5)` must exit 10 through the shared run helper.
add_test(NAME v4_fast_loop
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_loop_test.py)
set_tests_properties(v4_fast_loop PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 16.3's immediate and memory-operand selection: an unhomed 64-bit add
# updates its frame destination in place, an immediate store writes the
# destination directly at the declared width, and an immediate shift uses the
# immediate form instead of staging the count in cl.
add_test(NAME v4_fast_selection
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_selection_test.py)
set_tests_properties(v4_fast_selection PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 16.4's out-of-line trap stubs: the hot path holds only a jump (or the
# null-check branch), and the syscall/message/ud2 body is appended after the
# last block so it cannot sit between a guard and the code it protects.
add_test(NAME v4_fast_trap
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_trap_test.py)
set_tests_properties(v4_fast_trap PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 16.5's fallthrough-aware branches: when a conditional terminator's
# successor is the next block in emission order, the emitter inverts the
# condition and falls through instead of emitting a redundant jump. No `jmp` or
# conditional branch in `choose` may target the label that immediately follows
# it.
add_test(NAME v4_fast_branch
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_branch_test.py)
set_tests_properties(v4_fast_branch PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 16.6's width-aware narrow loads: signed i8/i16 memory loads use one
# 64-bit `movsx rax, BYTE/WORD PTR` and unsigned memory loads a single
# zero-extending `movzx eax, BYTE/WORD PTR`, so no narrow memory load is
# extended twice. Legacy keeps its two-step sequence. Narrow register-resident
# parameters (`emitResidentParamLoad`) still use the two-step form; that
# residual is tracked separately.
add_test(NAME v4_fast_extension
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_extension_test.py)
set_tests_properties(v4_fast_extension PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 17.3: fast integer loads write the allocator's home directly.
add_test(NAME v4_fast_load_dest
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_load_dest_test.py)
set_tests_properties(v4_fast_load_dest PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 17.4: fast resident-parameter loads write the result home directly.
add_test(NAME v4_fast_resident_load
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_resident_load_test.py)
set_tests_properties(v4_fast_resident_load PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 17.5: fast integer stores use the source home directly.
add_test(NAME v4_fast_store_dest
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_store_dest_test.py)
set_tests_properties(v4_fast_store_dest PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 17.6: fast constants, casts, and compares write their homes directly.
add_test(NAME v4_fast_value_dest
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_value_dest_test.py)
set_tests_properties(v4_fast_value_dest PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 18.3: fast boolean results (Not and integer compares) write the flag
# byte and its zero extension in the allocator's GPR home, and a homed Not
# source is tested in place instead of being staged through rax.
add_test(NAME v4_fast_bool_home
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_bool_home_test.py)
set_tests_properties(v4_fast_bool_home PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 18.2: a fast left-immediate add/multiply whose constant home is the
# result home folds into `lea` / three-operand `imul`, eliding the constant
# move. 8/16-bit multiplies, subtraction, division, compares, and floats stay
# on their existing paths.
add_test(NAME v4_fast_imm_fold
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_imm_fold_test.py)
set_tests_properties(v4_fast_imm_fold PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 18.7: a fast `mov D, imm` whose home is dead after its first reader
# folds into that consumer: arithmetic, compare, copy, and cast. A constant
# with a second reader keeps its move; stores, address uses, and the
# three-operand imul shape stay on their existing paths. Call argument words
# are handled by the extension below.
add_test(NAME v4_fast_imm_dead_home
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_imm_dead_home_test.py)
set_tests_properties(v4_fast_imm_dead_home PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Correctness regression: immediate folds may only use a Cast's source value
# when the cast preserves it. Narrowing and signed-to-unsigned widening casts
# must stay on the register path; otherwise the fold silently uses the
# untruncated/unconverted source immediate.
add_test(NAME v4_fast_cast_preserve
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_cast_preserve_test.py)
set_tests_properties(v4_fast_cast_preserve PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Residual correctness coverage from the optimization audit: narrow-width
# live mentions keep their callee-save pair, a non-Ret last block never falls
# through into the shared epilogue, and resident loads may reuse the parameter
# home for the result.
add_test(NAME v4_audit_residual
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/audit_residual_test.py)
set_tests_properties(v4_audit_residual PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 18.7 extension: a dead fast `mov D, imm` whose first reader is a call
# argument word stages the immediate into the ABI register instead of copying
# the constant home. Multi-use constants, address-base uses, asm/volatile
# barriers, and words past the register row keep their materialisation.
add_test(NAME v4_fast_call_imm
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_call_imm_test.py)
set_tests_properties(v4_fast_call_imm PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 17.9: a fast function with at least four returns stages each return
# value and jumps to one shared epilogue, so the callee restores, frame
# adjustment, `leave`, and `ret` are emitted once. One to three returns keep
# the local per-return epilogue; legacy is unchanged.
add_test(NAME v4_fast_epilogue
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_epilogue_test.py)
set_tests_properties(v4_fast_epilogue PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# Task 18.8: after the fast immediate folds, a callee-save pair whose only
# remaining body mention is a dead `mov reg, imm` is removed with that write.
# Live homes, inline-asm functions (implicit clobbers), shared and per-return
# epilogues, and legacy output are covered.
add_test(NAME v4_fast_dead_callee_save
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_SOURCE_DIR}/tests/v4/fast_dead_callee_save_test.py)
set_tests_properties(v4_fast_dead_callee_save PROPERTIES
  ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
  TIMEOUT 300)

# --- Targets -------------------------------------------------------------------
#
# OUTPUT.md §2: a target is a description, not a layer. The descriptor is the
# one authority for its ABI, entry symbol, output form and target policy.

add_executable(target_test tests/unit/target_test.cpp)
target_link_libraries(target_test PRIVATE cider_target)
target_include_directories(target_test PRIVATE src)
add_test(NAME target COMMAND target_test)

# A gate that fails must fail the build, so nothing here is allowed to be
# "informational".
set_tests_properties(gates layout types lexer ast parser resolver checker lowering optimise inline gvn sccp sroa licm simplify blocks codegen regalloc liveness linear_scan frame vm driver target
             PROPERTIES LABELS "gate")

# --- Opt-in UEFI smoke ---------------------------------------------------------
#
# The UEFI test needs clang (COFF assembly), GNU ld with i386pep, QEMU and
# OVMF. It is opt-in so the normal battery does not depend on firmware. The
# script exits 77 when a tool is missing, which ctest reports as SKIP.

option(CIDER_UEFI_TESTS "Run the QEMU/OVMF UEFI smoke test" OFF)
if(CIDER_UEFI_TESTS)
  add_test(NAME uefi
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/uefi/run_uefi_test.py)
  set_tests_properties(uefi PROPERTIES
    ENVIRONMENT "CIDER_BIN=${CMAKE_BINARY_DIR}/cider"
    SKIP_RETURN_CODE 77
    LABELS "uefi")
endif()
