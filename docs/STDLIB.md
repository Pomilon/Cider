# Bundled `std` modules

The `modules/std` directory is an explicitly imported Cider library. It is not
part of the compiler, the language runtime, or the freestanding contract. The
modules are convenience and test code: their implementations currently target
Linux x86-64 syscalls, with the exception of the compiler-recognized
`std.varargs` declarations.

Import a module by name:

```cider
import std.io;
from std.io import print, exit;
```

Dotted imports resolve through `CIDER_LIB`, which normally points at the
repository's `modules/` directory.

## Status summary

| Module | Purpose | Portability / status |
| --- | --- | --- |
| `std.alloc` | Bump allocator and allocation operators | Linux `mmap`; test-only |
| `std.io` | Printing, line input, process exit | Linux syscalls; test-only |
| `std.fs` | File syscall wrappers | Linux syscalls; test-only |
| `std.time` | Monotonic time and sleep | Linux syscalls; test-only |
| `std.string` | C string helpers, owning `String`, formatting | Explicit allocation; usable library code |
| `std.vector` | Growable vector | Explicit allocation; usable library code |
| `std.map` | Open-addressed hash map | Explicit allocation; usable library code |
| `std.result` | Helpers over core `Result<T, E>` | Uses `std.io` and heap helpers |
| `std.math` | Integer/float helpers and libm externs | Pure subset plus extern symbols |
| `std.varargs` | `VaList` and C variadic intrinsics | Compiler-recognized |

None of these modules is implicitly imported. A program that uses `new`,
`try_new`, or `delete` must provide `operator new` and `operator delete`, for
example by importing `std.alloc`.

## `std.alloc`

A bump allocator over anonymous `mmap` arenas. It is deliberately simple:
`free` and `operator delete` are no-ops because the bump cursor cannot reclaim
an earlier block. The module caps a single request and total mapped storage at
1 GiB. A failed mapping returns `nilPtr`; `oom()` writes `vm error: out of
memory` and exits with status 2.

Exports:

- `init_heap()`
- `malloc(size: i64) -> *void`
- `free(ptr: *void)`
- `operator new(size: i64, align: i64) -> *void`
- `operator delete(ptr: *void) -> void`
- `oom()`

This is a test allocator, not a general-purpose memory manager. Programs that
need reclamation or a different memory source provide their own operators.

## `std.io`

Output and input through Linux `write`, `read`, and `exit` syscalls. It imports
`std.alloc` for heap-backed line input.

Exports include:

- `print(s: string)`
- `puts(s: *char)`, `puts_string(s: string)`
- `printChar(c: char)`, `putchar(c: int) -> int`
- `printInt(v: int)`
- `printF64(f: f64)`, `printFloat(f: float)`
- `read_stdin(buf: *u8, count: int) -> int`
- `read_line() -> *char`, `read_all() -> *char`
- `exit(code: int)`

`read_line` and `read_all` return heap buffers capped at 4096 bytes; their
storage is not freed by the module. `exit` performs the Linux exit syscall with
the supplied status.

## `std.fs`

Thin Linux syscall wrappers. File operations return the kernel's result,
including negative errno values, as `int`.

Exports:

- constants `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`,
  `O_APPEND`;
- `open`, `read`, `write`, `close`, `lseek`, `unlink`.

An empty path returns `-2` (ENOENT) without issuing a syscall.

## `std.time`

Linux monotonic time and sleeping:

- `sleep_ms(ms: int) -> void` uses `nanosleep`;
- `timestamp_ms() -> u64` and `timestamp_us() -> u64` use
  `clock_gettime(CLOCK_MONOTONIC)`.

The timespec is an ordinary two-word Cider struct.

## `std.string`

C-style helpers plus an explicit owning `String`:

- C helpers: `strlen`, `strcmp`, `strcpy`, `strcat`, `strchr`, `memset`,
  `memcpy`, `to_upper`, `to_lower`, `to_upper_string`, `to_lower_string`,
  `trim`, `parse_int`, `parse_float`;
- owning type `String { data: *u8, len: int, cap: int }`;
- constructors and lifetime: `string_new`, `string_from`, `string_copy`,
  `string_move`, `string_deinit`, `string_free`;
- access and mutation: `string_data`, `string_view`, `string_len`,
  `string_cap`, `string_reserve`, `string_clear`, `string_append`,
  `string_append_bytes`, `string_append_byte`;
- formatting into caller storage: `format_int`, `format_uint`, `format_hex`;
- append formatting: `string_append_int`, `string_append_uint`,
  `string_append_hex`.

The built-in `string` is a non-owning pointer/length view. `String` owns heap
storage allocated through `std.alloc`; ownership transfer is explicit through
`string_move`, not assignment. A view returned by `string_view` is invalidated
by any append that grows the buffer.

## `std.vector`

`Vec<T>` with explicit growth and lifetime:

- `new_vec<T>() -> Vec<T>`
- `push<T>`, `pop<T>`, `get<T>`
- `len_vec<T>`, `cap_vec<T>`
- `reserve<T>`, `insert<T>`, `remove<T>`, `clear<T>`, `free_vec<T>`

Storage comes from `std.alloc`. `pop` and `remove` on an invalid index return a
zeroed `T` rather than reading out of bounds. `free_vec` releases the backing
block through `std.alloc`.

## `std.map`

Open-addressed hash map with linear probing and tombstones:

- `Entry<K, V>` and `HashMap<K, V>`
- `new_map<K, V>() -> HashMap<K, V>`
- `insert_map`, `get_map`, `contains_map`, `remove_map`, `len_map`,
  `free_map`

The current hash path is defined for integer and pointer keys. Storage comes
from `std.alloc`; allocation failure calls `oom()`.

## `std.result`

Convenience functions over the core `Result<T, E>` type:

- `ok<T, E>(val: T) -> Result<T, E>`
- `err<T, E>(val: E) -> Result<T, E>`
- `is_ok<T, E>`, `is_err<T, E>`
- `unwrap<T, E>`, `expect<T, E>`

`unwrap` and `expect` print a message through `std.io` and exit with status 1
when the result is an error. `zeroed<T>()` obtains zeroed storage through
`std.alloc`; it is an implementation helper, not an exported API.

## `std.math`

Pure helpers:

- `abs(x: int) -> int`
- `fabs(x: float) -> float`
- generic `min<T>`, `max<T>`, `clamp<T>`

Extern wrappers:

- `sqrt(x: float) -> float`
- `sin(x: float) -> float`
- `cos(x: float) -> float`
- exported `sqrt_f`, `sin_f`, `cos_f`

The extern symbols must be provided by the link environment. The native
freestanding link path does not add libm automatically.

## `std.varargs`

The public C variadic API:

- alias `VaList = va_list`
- `va_start`, `va_copy`, `va_end`
- `va_arg_i32`, `va_arg_i64`, `va_arg_u64`, `va_arg_f64`, `va_arg_ptr`

`VaList` is compiler-owned and may only travel through `*VaList`. The functions
are declared `extern` and recognized by the compiler when their defining module
is `std.varargs`; a user function with one of these names stays an ordinary
function. See [LANGUAGE.md](LANGUAGE.md#c-variadics) for promotion rules and
usage.

## Library boundaries

The modules deliberately do not provide:

- a portable target abstraction over Linux, UEFI, and external-OS services;
- a reclaiming general-purpose allocator;
- a formatting runtime with hidden allocation;
- implicit cleanup, destructors, or finalizers;
- a compiler prelude.

UEFI and external-OS programs should supply their own firmware or syscall
wrappers. The Linux-oriented modules remain useful for native tests and
examples.
