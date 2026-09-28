#!/bin/bash
# CIDER-FEAT-006: separate-compilation smoke. Builds a multi-module program
# one object at a time (`--emit-meta` module snapshots + `--cache` imports),
# links the objects, and checks:
#   * the separately linked program behaves like the whole-program build;
#   * main's object externs imported definitions instead of emitting them,
#     and the defining objects export them;
#   * a using object still compiles with the module source deleted, and the
#     rebuilt assembly is byte-identical (cache reuse);
#   * a corrupted `.cider-meta` falls back to source without breaking the
#     build, and re-emitting the snapshot restores the byte-identical
#     assembly (cache rebuild);
#   * `.hash` holds the SHA-256 of the module source;
#   * two objects that instantiate the same template link cleanly (the
#     use-site instantiation is a weak definition).
#
# Usage: separate_compilation_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: separate_compilation_smoke.sh <cider-binary>}
for tool in nasm gcc sha256sum nm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "separate_compilation_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)
IO_SRC=$REPO/modules/std/io.cid
if [ ! -f "$IO_SRC" ]; then
    echo "separate_compilation_smoke: SKIP (stdlib module $IO_SRC not found)"
    exit 77
fi

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
cd "$DIR"
mkdir -p modules build cache

fail() {
    echo "separate_compilation_smoke: FAIL ($1)"
    exit 1
}

# --- test program ---------------------------------------------------------
# boxy: a function template used from two objects (duplicate weak clones).
cat > modules/boxy.cid <<'CID'
export func boxed<T>(x: T) -> T { return x + x; }
CID

# mathx: exported const/var/struct/class/interface/functions plus a private
# helper and a use-site template instantiation.
cat > modules/mathx.cid <<'CID'
from boxy import boxed;

export const BASE: int = 40;
export var hits: int = 0;

export struct Pair {
  var a: int;
  var b: int;
}

export interface Doubler {
  func twice() -> int;
}

export class Twice {
  var n: int;
  func twice() -> int { return this.n * 2; }
}

func hidden(x: int) -> int { return x * 2; }

export func add(a: int, b: int) -> int {
  hits = hits + 1;
  return a + b + BASE;
}

export func twiced(x: int) -> int { return hidden(x); }

export func viaTemplate(x: int) -> int { return boxed<int>(x) + 1; }

export func pairSum(p: Pair) -> int { return p.a + p.b; }

export func useDoubler(d: Doubler) -> int { return d.twice(); }
CID

cat > main.cid <<'CID'
import mathx;
from boxy import boxed;
from std.io import printInt, print;

var folded: int = mathx.BASE + 2;   // cross-module const folds via the snapshot

func main() -> int {
  var p: mathx.Pair;
  p.a = 3;
  p.b = 4;
  printInt(mathx.add(1, 2)); print(",");       // 1 + 2 + 40 = 43
  printInt(mathx.twiced(5)); print(",");      // private helper in mathx.o
  printInt(mathx.pairSum(p)); print(",");     // 7
  printInt(mathx.hits); print(",");           // 1
  mathx.hits = mathx.hits + 10;
  printInt(mathx.hits); print(",");           // 11
  printInt(mathx.viaTemplate(20)); print(","); // boxed(20) + 1 = 41
  printInt(boxed<int>(5)); print(",");        // 10
  printInt(folded); print(",");               // 42
  var tw: mathx.Twice;
  tw.n = 21;
  var d: mathx.Doubler = tw;
  printInt(mathx.useDoubler(d)); print("\n"); // cross-object dispatch: 42
  return 0;
}
CID
EXPECTED=$'43,10,7,1,11,41,10,42,42'

assemble() { # <name>
    nasm -O0 -f elf64 "build/$1.asm" -o "build/$1.o"
}
link_run() { # <out> <objs...>
    local out=$1; shift
    gcc "$@" -o "build/$out" -no-pie
    set +e
    RUN_OUT=$(./build/"$out" 2>&1)
    RUN_RC=$?
    set -e
}

# --- whole-program reference ---------------------------------------------
"$BIN" compile main.cid -o build/whole.asm
assemble whole
link_run whole build/whole.o
[ "$RUN_RC" -eq 0 ] || fail "whole-program exit $RUN_RC"
[ "$RUN_OUT" = "$EXPECTED" ] || fail "whole-program output [$RUN_OUT]"

# --- per-file objects -----------------------------------------------------
emit_meta() { # <src> <name>
    "$BIN" compile --cache cache --emit-meta "$1" -o "build/$2.asm"
}
emit_meta "$IO_SRC" io
emit_meta modules/boxy.cid boxy
emit_meta modules/mathx.cid mathx
"$BIN" compile --cache cache main.cid -o build/main.asm

# Imported definitions are externs in main's object and definitions in the
# module's object.
grep -q "extern mod_mathx__add" build/main.asm || fail "main.asm misses extern mod_mathx__add"
grep -q "^mod_mathx__add:" build/main.asm && fail "main.asm defines mod_mathx__add"
grep -q "^mod_mathx__add:" build/mathx.asm || fail "mathx.asm misses mod_mathx__add"
grep -q "global mod_mathx__hits" build/mathx.asm || fail "mathx.asm does not export mod_mathx__hits"
# An imported const is folded from the snapshot into main's static data.
grep -q "folded: dd 42" build/main.asm || fail "cross-module const did not fold into main.o"

for name in io boxy mathx main; do assemble "$name"; done
link_run sep build/main.o build/mathx.o build/boxy.o build/io.o
[ "$RUN_RC" -eq 0 ] || fail "separate build exit $RUN_RC"
[ "$RUN_OUT" = "$EXPECTED" ] || fail "separate build output [$RUN_OUT]"

# Use-site template instantiation: both using objects carry the same weak
# clone, so the link above only succeeds if duplicates are tolerated.
nm build/mathx.o | grep -q " W mod_boxy__boxed" || fail "mathx.o: weak boxed<int> clone missing"
nm build/main.o | grep -q " W mod_boxy__boxed" || fail "main.o: weak boxed<int> clone missing"

# --- cache reuse: compile with the module source gone ---------------------
cp build/main.asm build/main.cached.asm
mv modules/mathx.cid modules/mathx.cid.off
"$BIN" compile --cache cache main.cid -o build/main.reuse.asm
cmp -s build/main.cached.asm build/main.reuse.asm || fail "cache reuse changed main.asm"
mv modules/mathx.cid.off modules/mathx.cid

# --- hash sidecar ---------------------------------------------------------
[ "$(cat cache/mathx.hash)" = "$(sha256sum modules/mathx.cid | cut -d' ' -f1)" ] \
    || fail "mathx.hash is not the source SHA-256"

# --- corrupted cache: fall back to source, then rebuild -------------------
cp build/mathx.asm build/mathx.cached.asm
printf 'this is not a .cider-meta file\n' > cache/mathx.cider-meta
"$BIN" compile --cache cache main.cid -o build/main.corrupt.asm
nasm -O0 -f elf64 build/main.corrupt.asm -o build/main.corrupt.o
[ -s build/main.corrupt.o ] || fail "corrupt-cache fallback produced no object"
gcc build/main.corrupt.o build/mathx.o build/boxy.o build/io.o -o build/sep_corrupt -no-pie
set +e
CORRUPT_OUT=$(./build/sep_corrupt 2>&1)
CORRUPT_RC=$?
set -e
[ "$CORRUPT_RC" -eq 0 ] || fail "corrupt-cache fallback exit $CORRUPT_RC"
[ "$CORRUPT_OUT" = "$EXPECTED" ] || fail "corrupt-cache fallback output [$CORRUPT_OUT]"

emit_meta modules/mathx.cid mathx
cmp -s build/mathx.cached.asm build/mathx.asm || fail "meta rebuild changed module assembly"
"$BIN" compile --cache cache main.cid -o build/main.rebuilt.asm
cmp -s build/main.cached.asm build/main.rebuilt.asm || fail "cache rebuild changed main.asm"

# --- CLI guards -----------------------------------------------------------
set +e
"$BIN" compile --emit-meta modules/mathx.cid -o build/no-cache.asm >/dev/null 2>&1
NO_CACHE_RC=$?
"$BIN" run --cache cache main.cid >/dev/null 2>&1
RUN_CACHE_RC=$?
set -e
[ "$NO_CACHE_RC" -eq 2 ] || fail "--emit-meta without --cache: exit $NO_CACHE_RC (want 2)"
# The VM needs every function body, so `run` rejects the cache flags.
[ "$RUN_CACHE_RC" -eq 2 ] || fail "run --cache: exit $RUN_CACHE_RC (want 2)"

echo "separate_compilation_smoke: OK"
