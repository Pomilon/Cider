cmake_minimum_required(VERSION 3.16)
project(cider VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror)
endif()

enable_testing()

add_executable(cider src/main.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp src/ir.cpp src/vm.cpp src/codegen.cpp)
target_include_directories(cider PRIVATE src)

add_executable(ir_test tests/unit/ir_test.cpp src/ir.cpp)
target_include_directories(ir_test PRIVATE src)
add_test(NAME ir_test COMMAND ir_test)

# CIDER-FEAT-005: ErrorReporter severity/collection (header-only).
add_executable(errors_test tests/unit/errors_test.cpp)
target_include_directories(errors_test PRIVATE src)
add_test(NAME errors_test COMMAND errors_test)

add_executable(ir_gen_test tests/unit/ir_gen_test.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp)
target_include_directories(ir_gen_test PRIVATE src)
add_test(NAME ir_gen_test COMMAND ir_gen_test)

add_executable(vm_test tests/unit/vm_test.cpp src/vm.cpp)
target_include_directories(vm_test PRIVATE src)
add_test(NAME vm_test COMMAND vm_test)

# CIDER-BUG-156: VM allocation accounting for the caller-owned heap copy of
# a non-sret virtual aggregate return (needs ir_gen + vm in one binary).
add_executable(vm_heap_return_test tests/unit/vm_heap_return_test.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp src/vm.cpp)
target_include_directories(vm_heap_return_test PRIVATE src)
add_test(NAME vm_heap_return_test COMMAND vm_heap_return_test)

# CIDER-BUG-184: VM allocation accounting for the caller-owned heap box of
# an interface-typed return of a concrete value (needs ir_gen + vm, and the
# interface-boxing flag, in one binary).
add_executable(vm_iface_box_test tests/unit/vm_iface_box_test.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp src/vm.cpp)
target_include_directories(vm_iface_box_test PRIVATE src)
add_test(NAME vm_iface_box_test COMMAND vm_iface_box_test)

# CIDER-BUG-202: the same accounting for the caller-owned boxes behind the
# interface ELEMENTS of a returned fixed array (one box per element, released
# at the caller's scope exit).
add_executable(vm_iface_array_box_test tests/unit/vm_iface_array_box_test.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp src/vm.cpp)
target_include_directories(vm_iface_array_box_test PRIVATE src)
add_test(NAME vm_iface_array_box_test COMMAND vm_iface_array_box_test)

# CIDER-BUG-212: the same accounting for the caller-owned boxes behind the
# interface values a returned AGGREGATE embeds (one box per embedded leaf —
# a bare interface field, an interface array field, or either nested — released
# at the caller's scope exit).
add_executable(vm_iface_aggregate_box_test tests/unit/vm_iface_aggregate_box_test.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp src/vm.cpp)
target_include_directories(vm_iface_aggregate_box_test PRIVATE src)
add_test(NAME vm_iface_aggregate_box_test COMMAND vm_iface_aggregate_box_test)

add_executable(ast_test tests/unit/ast_test.cpp)
target_include_directories(ast_test PRIVATE src)
add_test(NAME ast_test COMMAND ast_test)

add_executable(lexer_test tests/unit/lexer_test.cpp src/lexer.cpp)
target_include_directories(lexer_test PRIVATE src)
add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_expr_test tests/unit/parser_expr_test.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(parser_expr_test PRIVATE src)
add_test(NAME parser_expr_test COMMAND parser_expr_test)

add_executable(parser_stmt_test tests/unit/parser_stmt_test.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(parser_stmt_test PRIVATE src)
add_test(NAME parser_stmt_test COMMAND parser_stmt_test)




find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME harness COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py)
endif()

option(CIDER_UEFI_TESTS "Enable QEMU/OVMF UEFI smoke tests" OFF)
if(CIDER_UEFI_TESTS AND Python3_FOUND)
    add_test(NAME uefi_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/uefi/run_uefi_test.py)
    set_tests_properties(uefi_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()

# CID-005/T8: MS-ABI callee-saved XMM preservation smoke. Unlike the QEMU
# harness above it needs no OVMF, just nasm/objcopy/ld (the script exits 77
# when they are missing).
add_test(NAME ms_abi_xmm_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/ms_abi_xmm_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(ms_abi_xmm_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-ISS-001: opt-in --checks bounds checks must trap identically on the
# VM and the native backend; default (no flag) stays silent. The script
# exits 77 when nasm/gcc are missing.
add_test(NAME bounds_check_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/bounds_check_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(bounds_check_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-211: a method call on a nil interface reports
# `vm error: nil interface dispatch` and exits 2 identically on the VM
# (optimized and not) and the native backend, in the DEFAULT configuration
# (the guard is not --checks-gated). Also pins the codegen-side invariants:
# the helper is emitted without a flag, a user-defined
# `_nil_dispatch_fail` suppresses it, and uefi traps with ud2. Exits 77 when
# nasm/gcc are missing.
add_test(NAME nil_iface_dispatch_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/nil_iface_dispatch_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(nil_iface_dispatch_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-219: a dereference through a nil pointer reports
# `vm error: null pointer dereference` and exits 2 identically on the VM
# (optimized and not) and the native backend, in the DEFAULT configuration
# (the guard is not --checks-gated). Covers every shape the guard is attached
# to — read, store, compound store, member read/write, element read/write,
# direct and virtual method call, aggregate copy and field, pointer-to-pointer,
# a nil receiver behind a pointer to an interface, and the value arriving
# through a parameter, an array element, a class field and a pointer-to-class
# field — each with a live counterpart first. Also pins the codegen-side
# invariants: the helper is emitted without a flag with the VM's message
# payload byte for byte, the index guard precedes the element scale, a
# user-defined `_nil_deref_fail` suppresses the helper, pomeos gets its own
# syscall numbers, uefi traps with ud2, --checks still reports the bounds
# error, and the nil-interface diagnostic is unchanged. Exits 77 when
# nasm/gcc are missing.
add_test(NAME nil_pointer_deref_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/nil_pointer_deref_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(nil_pointer_deref_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-225: a call through a nil function pointer reports
# `vm error: call: nil function pointer` and exits 2 identically on the VM
# (optimized and not) and the native backend, in the DEFAULT configuration
# (the guard is not --checks-gated). Covers every shape the guard is attached
# to — local, alias spelling, parameter, class field, pointer-to-class field,
# array element, a nil returned from a function (direct and via a temporary),
# a module-scope global, the `msabi fn` route, and the closure route on its
# live path — each with a live counterpart first. Also pins the codegen-side
# invariants: the helper is emitted without a flag with the VM's message
# payload byte for byte, the guard precedes the indirect call, a statically
# known callee pays nothing, a user-defined `_nil_fnptr_fail` suppresses the
# helper, pomeos gets its own syscall numbers, uefi traps with ud2, --checks
# still reports the bounds error, and the nil-deref and nil-interface
# diagnostics are unchanged (this is its own pseudo-builtin, not
# `_nil_deref_fail`). Exits 77 when nasm/gcc are missing.
add_test(NAME nil_fnptr_call_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/nil_fnptr_call_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(nil_fnptr_call_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-FEAT-001: `compile -g` DWARF line-table/function-DIE smoke. Assembles
# with plain `nasm -f elf64` and inspects the result with readelf/objdump;
# exits 77 when nasm/gcc/readelf are missing.
add_test(NAME debug_info_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/debug_info_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(debug_info_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-132: .bss globals of heterogeneous alignment must assemble
# warning-free with nasm (no "attempt to initialize memory in BSS section"
# from padding) while keeping identical .bss symbol offsets.
# CIDER-BUG-142: top-level user asm blocks must land in .text (not .bss),
# assemble warning-free and actually execute. Exits 77 when nasm/nm/gcc are
# missing.
add_test(NAME bss_align_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/bss_align_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(bss_align_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-121: VM allocation failures (frame-local chunks / heap growth)
# under RLIMIT_AS must surface as `vm error: out of memory` (exit 2), never
# as an uncaught std::bad_alloc reported by the CLI (exit 1). Skips (77) when
# the inherited hard RLIMIT_AS cannot allow the 1536 MiB cap.
add_test(NAME vm_oom_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/vm_oom_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(vm_oom_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-169: the asm exit syscall must flush putchar's buffer exactly like
# the exit builtin. A mixed extern-putchar + asm-exit program must keep its
# buffered output on both VM paths; the exit-builtin control also covers the
# native path (raw exit syscalls do not flush glibc's pipe buffer, libc exit
# does). Exits 77 when nasm/gcc are missing.
add_test(NAME vm_asm_exit_flush_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/vm_asm_exit_flush_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(vm_asm_exit_flush_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-CHORE-001: CLI usage/--target validation runs in the suite too
# (previously manual only).
add_test(NAME cli_test
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/cli_test.sh $<TARGET_FILE:cider>)

# CIDER-FEAT-006: separate-compilation smoke: per-file objects via
# `--emit-meta` + `--cache`, cross-object linking, cache reuse without the
# module source, corrupted-cache fallback and snapshot rebuild byte-identity.
# Exits 77 when nasm/gcc are missing.
add_test(NAME separate_compilation_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/separate_compilation_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(separate_compilation_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-222: std resolution must not depend on where the compiler binary
# lives. Copies one binary into three differently deep build directories
# around a single stdlib and requires `compile`, `compile -g`, `bundle` and
# `build` to be byte-identical from all three (before the fix the resolved
# path — which lands in bundle banners and the DWARF file table — carried the
# binary's own `..` segments, and the deepest copy resolved nothing at all).
# Needs no assembler: it compares emitted text.
add_test(NAME std_resolve_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/std_resolve_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(std_resolve_smoke PROPERTIES SKIP_RETURN_CODE 77)

# CIDER-BUG-162: the inliner's site term must not grow with the clones it
# produces. A 20-site destructor chain whose helpers each call the next twice
# used to double the IR every optimizer round and take minutes to compile;
# the bounded site budget keeps it sub-second.
add_test(NAME inliner_sites_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/inliner_sites_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(inliner_sites_smoke PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME codegen_scale_smoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unit/codegen_scale_smoke.sh $<TARGET_FILE:cider>)
set_tests_properties(codegen_scale_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

add_executable(parser_recovery_test tests/unit/parser_recovery_test.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(parser_recovery_test PRIVATE src)
add_test(NAME parser_recovery_test COMMAND parser_recovery_test)

add_executable(ast_visitor_test tests/unit/ast_visitor_test.cpp)
target_include_directories(ast_visitor_test PRIVATE src)
add_test(NAME ast_visitor_test COMMAND ast_visitor_test)

add_executable(bundle_test tests/unit/bundle_test.cpp src/parser.cpp src/lexer.cpp src/module_resolver.cpp)
target_include_directories(bundle_test PRIVATE src)
add_test(NAME bundle_test COMMAND bundle_test)

add_executable(symbols_test tests/unit/symbols_test.cpp src/symbols.cpp)
target_include_directories(symbols_test PRIVATE src)
add_test(NAME symbols_test COMMAND symbols_test)

add_executable(module_scope_test tests/unit/module_scope_test.cpp)
target_include_directories(module_scope_test PRIVATE src)
add_test(NAME module_scope_test COMMAND module_scope_test)

# CIDER-FEAT-006: `.cider-meta` snapshot helpers (SHA-256, header parsing,
# template translation, declaration spans, module-name derivation).
add_executable(module_meta_test tests/unit/module_meta_test.cpp)
target_include_directories(module_meta_test PRIVATE src)
add_test(NAME module_meta_test COMMAND module_meta_test)

add_executable(symbol_collector_test tests/unit/symbol_collector_test.cpp src/symbols.cpp src/parser.cpp src/lexer.cpp src/module_resolver.cpp)
target_include_directories(symbol_collector_test PRIVATE src)
add_test(NAME symbol_collector_test COMMAND symbol_collector_test)

add_executable(type_checker_test tests/unit/type_checker_test.cpp src/symbols.cpp src/parser.cpp src/lexer.cpp src/module_resolver.cpp)
target_include_directories(type_checker_test PRIVATE src)
add_test(NAME type_checker_test COMMAND type_checker_test)

add_executable(module_resolver_test tests/unit/module_resolver_test.cpp src/module_resolver.cpp src/symbols.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(module_resolver_test PRIVATE src)
add_test(NAME module_resolver_test COMMAND module_resolver_test)

add_executable(vm_asm_test tests/unit/vm_asm_test.cpp src/vm.cpp)
target_include_directories(vm_asm_test PRIVATE src)
add_test(NAME vm_asm_test COMMAND vm_asm_test)

add_executable(codegen_test tests/unit/codegen_test.cpp src/codegen.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp)
target_include_directories(codegen_test PRIVATE src)
add_test(NAME codegen_test COMMAND codegen_test)

add_executable(pass_test tests/unit/pass_test.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp)
target_include_directories(pass_test PRIVATE src)
add_test(NAME pass_test COMMAND pass_test)

add_executable(reg_alloc_test tests/unit/reg_alloc_test.cpp src/ir.cpp)
target_include_directories(reg_alloc_test PRIVATE src)
add_test(NAME reg_alloc_test COMMAND reg_alloc_test)
