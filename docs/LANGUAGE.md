# Cider language surface

Cider is a statically typed, freestanding-first systems language. This document
describes the v4 source surface accepted by the compiler in this tree. It is
not a tutorial for a hosted runtime: the default profile has no implicit
library, no compiler-provided heap, no startup trampoline, and no runtime
archive.

## Freestanding profile

A Cider program is responsible for its own entry point and its own services:

- Native code defines `func _start() -> void`.
- UEFI code defines `msabi func efi_main(image_handle: *void, system_table: *void) -> int`.
- The external-OS target defines `func _start() -> void` or
  `func _start(argc: int, argv: **char) -> void`.
- The VM interprets a program with the target's required `_start` entry.

There is no prelude. Names such as `print`, `malloc`, `panic`, and `exit` are
not compiler-provided; they come from an explicit import. A native program
links with `ld -nostdlib` and may use Linux syscalls through its own inline
assembly or an imported module.

The compiler does not synthesize `_start`, `efi_main`, a `main` trampoline, an
exit syscall, a destructor runner, or a hidden exception handler.

## Modules and imports

A module is a `.cid` file. Imports are explicit:

```cider
import geometry;
from std.io import print, exit;
```

- `import name;` brings the module name into scope for qualified use such as
  `geometry.area(...)`.
- `from name import a, b;` binds selected exported declarations directly.
- Plain module names resolve beside the entry file and through the nearby
  `modules/` search roots.
- Dotted names such as `std.io` prefer the library root named by `CIDER_LIB`
  and map dots to directory separators.

Declarations are private to their module unless marked `export`. Imports are
resolved before bodies so module scope is order-independent; functions may
call one another regardless of declaration order.

## Types

### Scalar types

| Spelling | Meaning |
| --- | --- |
| `void` | No value |
| `bool` | One byte, `true` or `false` |
| `char` | One-byte character type |
| `byte` | Alias of `u8` |
| `int` | Alias of `i32` |
| `i8`, `i16`, `i32`, `i64` | Signed integers |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers |
| `f32` | 32-bit float; `float` is an alias |
| `f64` | 64-bit float |

Integer literals may be decimal, hexadecimal, or binary, and may contain `_`
separators. A floating literal may carry an `f` suffix to request `f32`.
Character literals use single quotes; string literals use double quotes. The
`u"..."` and `u'...'` forms produce UTF-16 data.

### Composite types

| Spelling | Meaning |
| --- | --- |
| `*T` | Pointer to `T` |
| `*volatile T` | Pointer whose pointee accesses are volatile |
| `[T; N]` | Fixed-size array |
| `[]T` | Slice: pointer plus length |
| `string` | Non-owning immutable view: pointer plus length |
| `*func(A) -> R` | Function pointer |
| `func(A) -> R` | Closure value: code word plus captures word |
| `va_list` | Opaque C variadic cursor |

A closure value is distinct from a function pointer. The language accepts
zero-capture function literals; a literal that would capture an enclosing
local is rejected with `captures are not supported`. A function pointer is a
single code address.

### Aggregates and named types

- `struct` declarations group fields and methods.
- `@packed struct` removes padding between fields.
- `class` declarations add inheritance, `init`/`deinit`, and virtual methods.
- `interface` declarations describe method signatures; class pointers and
  suitable structs can be boxed into an interface value.
- `union` declarations hold one live member at a time and carry a tag.
- `enum` declarations name integer values.
- `type Name = Target;` declares an alias.
- Generic declarations take explicit type parameters. Call sites pass them
  explicitly, for example `id::<int>(7)` or `Vec::<int>`.

Fields are public by default; `private` restricts access to the declaring
module. Methods may be `virtual`, may use `override` as an assertion, and use
`this` or `self` for the receiver. A class may have one or more bases; only the
first base may carry a virtual table. `super` names the base receiver and
`super_init(args)` runs a base constructor.

## Variables, constants, and initialization

Local variables require an initializer or an explicit `uninit`:

```cider
var x: int = 0;
var raw: [u8; 64] = uninit;
```

`uninit` is a deliberate raw value, not implicit zero-fill. Globals must have a
constant initializer; an uninitialized global is rejected.

`const` declares a compile-time constant. `var` at module scope declares a
global. A global may be `volatile` when reads and writes must not be elided or
reordered.

## Expressions and operators

The usual C-family operators are available: arithmetic, remainder, bitwise
operators, shifts, comparisons, logical `&&`/`||`/`!`, `and`/`or`/`not`
aliases, the ternary `?:`, assignment and compound assignment, pre/post
increment and decrement, member access with `.` and `->`, indexing, slicing,
calls, casts with `as`, `sizeof`, `alignof`, and `@bitcast`.

`@bitcast(T, value)` reinterprets bits without numeric conversion and requires
equal widths. Pointer arithmetic is explicit through the pointer type. Strings
compare by content with `==` and `!=`; other aggregate values do not gain
implicit comparison operators.

Casts are explicit. There are no implicit user-defined conversions, no
implicit generic argument inference, and no implicit aggregate zero-fill.

### Operator overloading

Declarations use `operator` with the usual spelling, for example
`func operator+(other: T) -> T` or a free `func operator_add(a: A, b: B) -> C`.
The parser stores canonical names such as `operator_add`, `operator_sub`,
`operator_index`, `operator_call`, and `operator_arrow`.

- At least one operand must be a class, struct, or enum type.
- `=`, `[]`, `()`, and `->` are member operators.
- `&&`, `||`, and `!` are built-in only and cannot be overloaded.
- Assignment and compound assignment return `void` and cannot be chained.
- `operator new` and `operator delete` are global functions with exact
  signatures and are not ordinary class members.
- Precedence, arity, and associativity are those of the built-in spelling.

## Statements and control flow

Cider has blocks, `if`/`else`, `while`, C-style `for`, `for (x in sequence)`,
`break`, `continue`, `return`, `defer`, `match`, `asm`, and `delete`.

`defer { ... }` runs its statement on every exit from the enclosing scope, in
last-in-first-out order. It is lowered at compile time and may duplicate emitted
code.

`match` supports enum members, constants, integer ranges, type and field
patterns, wildcard `_`, alternatives with `|`, guards with `if`, and a
`default` arm. Exhaustiveness is checked for enums and booleans.

Inline assembly is written as an `asm { ... }` statement. String lines are
emitted as written; `{name}` and `{name:q}` placeholders substitute a resolved
local or global. A module-scope `asm { ... }` block emits raw text without
placeholder substitution. Targets may reject inline assembly; the native,
external-OS, UEFI, and VM descriptors all currently permit it.

`# experimental(wrap)` at the top of a file makes integer overflow wrap at the
instruction's width instead of remaining undefined.

## Allocation: `new`, `delete`, and `try_new`

Allocation is library-defined. A program that uses `new`, `try_new`, or
`delete` must provide these global functions, usually by importing
`std.alloc`:

```cider
from std.alloc import operator new, operator delete;

func operator new(size: i64, align: i64) -> *void;
func operator delete(ptr: *void) -> void;
```

The signatures are fixed: `operator new` takes a byte count and an alignment
and returns `*void`; `operator delete` takes `*void` and returns `void`.
Missing operators are compile-time errors.

Construction forms:

```cider
var point: Point = Point { x: 3, y: 4 };  // value construction
var heap: *Point = new Point { x: 3, y: 4 };  // heap construction
var circle: *Circle = new Circle(2);  // class construction through init
```

`new` calls `operator new`, traps without constructing when the result is nil,
and then runs field defaults and any `init`. `delete p` is nil-safe, runs a
class `deinit` when present, and calls `operator delete`; a struct or union
pointer has no synthesized destructor. Array allocation with `new [T; N]` is
parsed but rejected by the current lowering.

`try_new` has the same construction forms but returns
`Result<*T, AllocError>` instead of trapping:

```cider
var made: Result<*Circle, AllocError> = try_new Circle(2);
if (made.is_ok) {
    // made.value is the *Circle
} else {
    // made.error is AllocError.OutOfMemory
}
```

`Result<T, E>` and `AllocError` are core types, not imports. `Result` has the
fields `is_ok: bool`, `value: T`, and `error: E`. `AllocError` currently has
the single value `AllocError.OutOfMemory`. On allocation failure `try_new`
constructs nothing and leaves `value` as the zero value of `*T`.

## C variadics

A trailing `...` is accepted on free functions, `extern` declarations, and
function-pointer types. At least one named parameter must precede it. Methods,
operators, constructors, destructors, and closures cannot be variadic.

```cider
from std.varargs import VaList, va_start, va_arg_i32, va_end;

extern func printf(fmt: *char, ...) -> int;

func sum(n: int, ...) -> int {
    var ap: VaList = uninit;
    va_start(&ap);
    var total: int = n;
    var i: int = 0;
    while (i < n) {
        total = total + va_arg_i32(&ap);
        i = i + 1;
    }
    va_end(&ap);
    return total;
}
```

Variadic arguments promote according to the C default argument rules:

| Source type | Promoted type |
| --- | --- |
| `bool`, `char`, `i8`, `i16`, `u8`, `u16` | `i32` |
| `f32` | `f64` |
| `i32`, `u32`, `i64`, `u64`, `f64` | unchanged |
| enums | `i32` |
| data pointers, function pointers | one word, unchanged |

Only integers, pointers, and `f64` are accepted in the variadic tail; strings,
arrays, structs, classes, unions, interfaces, closures, and `va_list` by value
are rejected. A string must be passed as its data pointer explicitly.

`std.varargs` exports `VaList` and the compiler-recognized entries `va_start`,
`va_copy`, `va_end`, `va_arg_i32`, `va_arg_i64`, `va_arg_u64`, `va_arg_f64`,
and `va_arg_ptr`. `va_start` is valid only in a variadic function; the other
operations work on an initialized list and are legal in ordinary helpers.
Reading past the end of a list is undefined, as in C.

## Diagnostics

Compiler diagnostics are written to standard error as:

```text
path:line:column: message
```

A front-end failure exits with status 1. Command-line and I/O failures exit
with status 2. Successful `check` is silent; successful `compile` writes
assembly to standard output unless `-o` names a file.

Diagnosed runtime traps print one of these lines and exit with status 2 on the
native and VM legs:

```text
vm error: null pointer dereference
vm error: index out of bounds
vm error: nil interface dispatch
vm error: call: nil function pointer
```

Other traps, including the explicit failure of default `new` when the
allocator returns nil, may terminate without a text diagnostic. Programs that
need a diagnostic should test the value and write one through their own I/O
module.

## Deliberate omissions

The following are not part of the language or are rejected by the current
implementation:

- exceptions (`try`/`catch`/`throw`) and hidden unwinding;
- `async`/`await` and a scheduler runtime;
- implicit zero-fill of aggregates or raw locals;
- automatic scope-exit destructors and implicit RAII;
- `fini` and compiler-generated finalizer runners;
- string interpolation and compiler formatting helpers;
- capturing closures (zero-capture function literals are supported);
- `fn(...)` closure syntax (the value spelling is `func(...)`);
- implicit generic argument inference;
- class-specific allocation operators and array `new`/`delete`.
