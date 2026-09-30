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
#     use-site instantiation is a weak definition);
#   * same-named templates exported by different modules get per-owner
#     instantiation symbols: a module instantiating its OWN template calls
#     the module-mangled clone (not a bare shared name), two such modules
#     link to distinct weak definitions, and the linked program runs each
#     module's own body (CIDER-BUG-107);
#   * same-named STRUCT templates exported by two modules keep per-owner
#     instantiations in separate mode: each module object lays out its own
#     fields/size, the entry object resolves each qualified spelling to its
#     owner (`a.SBox<int>` 8 bytes vs `b.SBox<int>` 12, field only one
#     declares), a `from`-import selects the owner for the bare spelling,
#     a struct embedding another module's instantiation keeps its layout,
#     and the linked program uses each module's own layout (CIDER-BUG-136;
#     whole-program conflation may remain, T3/T7);
#   * a library object's class/struct globals run their field initializers
#     (and vptr) before user code (CIDER-BUG-106), on the linked and the
#     whole-program VM paths;
#   * CIDER-BUG-137: module-scope globals with destructors finalize at
#     program exit. Each module object exports a fini that destroys only
#     its own globals; the entry object calls them once each, diamond
#     imports included, in reverse construction order, matching the
#     whole-program build's trace.
#   * CIDER-BUG-190: a throw raised in another object's function still runs
#     the pending scope exits of the object frame it unwinds through, on the
#     whole-program and the separately linked build.
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

# CIDER-BUG-107: two modules export a SAME-NAMED function template with
# different bodies and each instantiates its own. In separate compilation
# each object's clone must mangle by its owning module, so the two
# specializations stay distinct symbols and each module's own call binds to
# its own body.
cat > modules/twina.cid <<'CID'
export func twin<T>(x: T) -> T { return x * 10 + 1; }
export func ownTwinA(x: int) -> int { return twin<int>(x); }
CID

cat > modules/twinb.cid <<'CID'
export func twin<T>(x: T) -> T { return x * 10 + 2; }
export func ownTwinB(x: int) -> int { return twin<int>(x); }
CID

# mathx: exported const/var/struct/class/interface/functions plus a private
# helper, a use-site template instantiation, and (CIDER-BUG-106) library
# globals whose class-typed fields carry initializers and a vptr.
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

// CIDER-BUG-106: this object's own startup pass must initialise these
// before any user code, exactly as the whole-program build does.
export class Counter {
  var n: int = 41;
  var unset: int;
  virtual func value() -> int { return this.n; }
}

export struct Wrap {
  var c: Counter;
}

export var counter: Counter;
export var wrap: Wrap;
export var counters: [Counter; 2];

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
import twina;
import twinb;
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
  printInt(mathx.useDoubler(d)); print(","); // cross-object dispatch: 42
  // CIDER-BUG-106: class-typed library globals run their field initializers
  // (and vptr) in mathx's object before main's user code.
  printInt(mathx.counter.n); print(",");          // 41
  printInt(mathx.counter.unset); print(",");      // 0
  printInt(mathx.counter.value()); print(",");    // 41 via the global's vptr
  printInt(mathx.wrap.c.n); print(",");           // 41 struct-embedded
  printInt(mathx.counters[1].n); print(",");      // 41 array element
  // CIDER-BUG-107: same-named templates in two modules keep their own
  // instantiation bodies (per-owner symbols) on both use paths.
  printInt(twina.ownTwinA(4)); print(",");        // 41: twina's own twin<int>
  printInt(twinb.ownTwinB(4)); print(",");        // 42: twinb's own twin<int>
  printInt(twina.twin<int>(3)); print(",");       // 31: qualified from main
  printInt(twinb.twin<int>(3)); print("\n");      // 32: qualified from main
  return 0;
}
CID
EXPECTED=$'43,10,7,1,11,41,10,42,42,41,0,41,41,41,41,42,31,32'

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
# VM path: `run` rejects the cache flags (every body must be present), so the
# VM reference is the same whole-program sources.
VM_OUT=$("$BIN" run main.cid 2>&1)
[ "$VM_OUT" = "$EXPECTED" ] || fail "whole-program VM output [$VM_OUT]"

# --- per-file objects -----------------------------------------------------
emit_meta() { # <src> <name>
    "$BIN" compile --cache cache --emit-meta "$1" -o "build/$2.asm"
}
emit_meta "$IO_SRC" io
emit_meta modules/boxy.cid boxy
emit_meta modules/mathx.cid mathx
emit_meta modules/twina.cid twina
emit_meta modules/twinb.cid twinb
"$BIN" compile --cache cache main.cid -o build/main.asm

# Imported definitions are externs in main's object and definitions in the
# module's object.
grep -q "extern mod_mathx__add" build/main.asm || fail "main.asm misses extern mod_mathx__add"
grep -q "^mod_mathx__add:" build/main.asm && fail "main.asm defines mod_mathx__add"
grep -q "^mod_mathx__add:" build/mathx.asm || fail "mathx.asm misses mod_mathx__add"
grep -q "global mod_mathx__hits" build/mathx.asm || fail "mathx.asm does not export mod_mathx__hits"
# An imported const is folded from the snapshot into main's static data.
grep -q "folded: dd 42" build/main.asm || fail "cross-module const did not fold into main.o"
# CIDER-BUG-106: mathx's object exports its own global-init pass, and main's
# startup chains it (and only its own `_cider_global_init` is local).
grep -q "global mod_mathx__.*uinit" build/mathx.asm \
    || fail "mathx.asm does not export its global-init pass"
grep -q "call mod_mathx__.*uinit" build/main.asm \
    || fail "main.asm does not chain mathx's global-init pass"
grep -q "^_cider_global_init:" build/mathx.asm \
    && fail "mathx.asm emits the whole-program global-init name"

for name in io boxy mathx twina twinb main; do assemble "$name"; done
link_run sep build/main.o build/mathx.o build/boxy.o build/twina.o build/twinb.o build/io.o
[ "$RUN_RC" -eq 0 ] || fail "separate build exit $RUN_RC"
[ "$RUN_OUT" = "$EXPECTED" ] || fail "separate build output [$RUN_OUT]"

# Use-site template instantiation: both using objects carry the same weak
# clone, so the link above only succeeds if duplicates are tolerated.
nm build/mathx.o | grep -q " W mod_boxy__boxed" || fail "mathx.o: weak boxed<int> clone missing"
nm build/main.o | grep -q " W mod_boxy__boxed" || fail "main.o: weak boxed<int> clone missing"

# CIDER-BUG-107: same-named templates in different modules get per-owner
# instantiation symbols. Each module's object defines its OWN weak clone and
# calls the module-mangled name — the bare shared `twin_l_int_g_` reference
# this pins was the bug — and main's object carries both distinct clones.
grep -q "global mod_twina__twin.*:weak" build/twina.asm \
    || fail "twina.asm does not define its own weak twin<int> clone"
grep -q "global mod_twinb__twin.*:weak" build/twinb.asm \
    || fail "twinb.asm does not define its own weak twin<int> clone"
grep -q "call mod_twina__twin" build/twina.asm \
    || fail "twina.asm does not call its module-mangled twin<int> clone"
grep -q "call mod_twinb__twin" build/twinb.asm \
    || fail "twinb.asm does not call its module-mangled twin<int> clone"
grep -q "call twin_l_int_g_" build/twina.asm \
    && fail "twina.asm calls the bare shared twin<int> name"
grep -q "call twin_l_int_g_" build/twinb.asm \
    && fail "twinb.asm calls the bare shared twin<int> name"
nm build/twina.o | grep -q " W mod_twina__twin" \
    || fail "twina.o: per-owner weak twin<int> clone missing"
nm build/twinb.o | grep -q " W mod_twinb__twin" \
    || fail "twinb.o: per-owner weak twin<int> clone missing"
nm build/main.o | grep -q " W mod_twina__twin" \
    || fail "main.o: weak mod_twina twin<int> clone missing"
nm build/main.o | grep -q " W mod_twinb__twin" \
    || fail "main.o: weak mod_twinb twin<int> clone missing"

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
gcc build/main.corrupt.o build/mathx.o build/boxy.o build/twina.o build/twinb.o build/io.o -o build/sep_corrupt -no-pie
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

# --- CIDER-BUG-137: global destructors across objects ---------------------
# A diamond import: da's globals are imported by main directly AND through db
# and dc. Each module object exports a fini that destroys only its OWN
# globals; the entry object calls them exactly once, in reverse construction
# order: its own globals, then the last-imported module, ..., then the shared
# dependency. The whole-program and separate builds must run the same trace.
mkdir -p fini/modules fini/cache fini/build
cat > fini/modules/da.cid <<'CID'
from std.io import print, printInt;
export class R { var label: int; func ~R() { printInt(this.label); print(","); } }
export var ga: R;
export func setA(n: int) -> void { ga.label = n; }
CID
cat > fini/modules/db.cid <<'CID'
import da;
from da import R;
export var gb: R;
export func setB(n: int) -> void { gb.label = n; }
CID
cat > fini/modules/dc.cid <<'CID'
import da;
from da import R;
export var gc: R;
export func setC(n: int) -> void { gc.label = n; }
CID
cat > fini/main.cid <<'CID'
import da;
import db;
import dc;
from da import R;
from std.io import print, printInt;
var gm: R;
func main() -> int {
  da.setA(1);
  db.setB(2);
  dc.setC(3);
  gm.label = 4;
  print("main,");
  return 0;
}
CID
FINI_EXPECTED=$'main,4,3,2,1,'

"$BIN" compile fini/main.cid -o fini/build/whole.asm
nasm -O0 -f elf64 fini/build/whole.asm -o fini/build/whole.o
gcc fini/build/whole.o -o fini/build/whole -no-pie
set +e
F_OUT=$(fini/build/whole 2>&1)
F_RC=$?
set -e
[ "$F_RC" -eq 0 ] || fail "fini whole-program exit $F_RC"
[ "$F_OUT" = "$FINI_EXPECTED" ] || fail "fini whole-program output [$F_OUT]"
# The VM entry return runs the same pass (run rejects --cache, so the VM
# reference is the whole-program source).
F_VM_OUT=$("$BIN" run fini/main.cid 2>&1)
[ "$F_VM_OUT" = "$FINI_EXPECTED" ] || fail "fini whole-program VM output [$F_VM_OUT]"

# A `_start`-only program: the VM's _start entry return is the program exit,
# so it must run the fini too (the native kernel entry cannot be checked
# here: this _start returns instead of issuing an exit syscall).
cat > fini/start_only.cid <<'CID'
from std.io import print, printInt;
var drops: int = 0;
class D { var label: int; func ~D() { drops = drops + 1; printInt(this.label); print(","); } }
var g: D;
func _start() -> void {
  g.label = 9;
  print("start,");
  return;
}
CID
F_START_OUT=$("$BIN" run fini/start_only.cid 2>&1)
[ "$F_START_OUT" = "start,9," ] || fail "fini VM _start output [$F_START_OUT]"

"$BIN" compile --cache fini/cache --emit-meta "$IO_SRC" -o fini/build/io.asm
for m in da db dc; do
    "$BIN" compile --cache fini/cache --emit-meta "fini/modules/$m.cid" -o "fini/build/$m.asm"
done
"$BIN" compile --cache fini/cache fini/main.cid -o fini/build/main.asm
for m in io da db dc main; do nasm -O0 -f elf64 "fini/build/$m.asm" -o "fini/build/$m.o"; done
gcc fini/build/main.o fini/build/da.o fini/build/db.o fini/build/dc.o fini/build/io.o \
    -o fini/build/sep -no-pie
set +e
F_SEP_OUT=$(fini/build/sep 2>&1)
F_SEP_RC=$?
set -e
[ "$F_SEP_RC" -eq 0 ] || fail "fini separate exit $F_SEP_RC"
[ "$F_SEP_OUT" = "$FINI_EXPECTED" ] || fail "fini separate output [$F_SEP_OUT]"
# Every module object exports its own fini symbol (empty or not) and does
# NOT chain its dependencies; the entry object calls each exactly once, in
# reverse construction order.
for m in da db dc; do
    grep -q "global mod_${m}__.*ufini" "fini/build/$m.asm" \
        || fail "$m.asm does not export its global-fini pass"
done
FINI_ORDER=$(grep -o "mod_d[abc]___ucider_uglobal_ufini" fini/build/main.asm | awk '!seen[$0]++' | tr '\n' ' ')
[ "$FINI_ORDER" = "mod_dc___ucider_uglobal_ufini mod_db___ucider_uglobal_ufini mod_da___ucider_uglobal_ufini " ] \
    || fail "main.asm fini call order [$FINI_ORDER]"
# The entry's fini function is emitted once, so each dependency's call site
# there must appear exactly once (the runtime trace above proves one run).
[ "$(grep -c "call mod_da___ucider_uglobal_ufini" fini/build/main.asm)" = "1" ] \
    || fail "main.asm calls the shared dependency's fini more than once"
grep -q "call mod_db__.*ufini" fini/build/da.asm \
    && fail "da.asm chains its importer's fini"

# --- CIDER-BUG-136: same-named struct templates across modules ------------
# Two modules export a same-named struct template with different field
# layouts, and each instantiates its own. In separate compilation the
# instantiations must keep per-owner identities (`sboxa::SBox<int>` vs
# `sboxb::SBox<int>`), so the entry object resolves each qualified spelling
# to its own owner: `sizeof` differs (8 vs 12), field `y` exists only in
# sboxb's template, and each module object allocates its own layout. Under
# the old shared root-scope `SBox<int>` the entry object resolved both to
# whichever template was collected first (rejecting `y` when sboxa won,
# mis-sizing when sboxb won). Whole-program builds keep the documented
# T3/T7 conflation, so only the separate objects are exercised here.
mkdir -p sbox/modules sbox/cache sbox/build
cat > sbox/modules/sboxa.cid <<'CID'
export struct SBox<T> {
  var v: T;
  var x: int;
}

export func makeSBoxA(n: int) -> *SBox<int> {
  var b: *SBox<int> = new SBox<int>;
  b->v = n;
  b->x = 10;
  return b;
}

export func sumSBoxA(b: *SBox<int>) -> int { return b->v + b->x; }
CID
cat > sbox/modules/sboxb.cid <<'CID'
export struct SBox<T> {
  var v: T;
  var x: int;
  var y: int;
}

export func makeSBoxB(n: int) -> *SBox<int> {
  var b: *SBox<int> = new SBox<int>;
  b->v = n;
  b->x = 20;
  b->y = 30;
  return b;
}

export func sumSBoxB(b: *SBox<int>) -> int { return b->v + b->x + b->y; }
CID
cat > sbox/modules/holder.cid <<'CID'
import sboxb;

export struct Holder {
  var b: sboxb.SBox<int>;
}

export func readHolder(h: *Holder) -> int { return h->b.v + h->b.x + h->b.y; }
CID
cat > sbox/main.cid <<'CID'
import sboxa;
import sboxb;
import holder;
from sboxa import SBox;
from std.io import printInt, print;

func main() -> int {
  printInt(sizeof(sboxa.SBox<int>)); print(",");  // 8
  printInt(sizeof(sboxb.SBox<int>)); print(",");  // 12
  var a: *sboxa.SBox<int> = sboxa.makeSBoxA(1);
  printInt(sboxa.sumSBoxA(a)); print(",");        // 11
  var b: *sboxb.SBox<int> = sboxb.makeSBoxB(2);
  printInt(sboxb.sumSBoxB(b)); print(",");        // 52
  b->y = 100;
  printInt(sboxb.sumSBoxB(b)); print(",");        // 122
  // The from-import selects sboxa's template for the bare spelling.
  var z: *SBox<int> = sboxa.makeSBoxA(3);
  printInt(z->v + z->x); print(",");              // 13
  printInt(sizeof(SBox<int>)); print(",");        // 8
  // A struct embedding sboxb's instantiation keeps its 12-byte layout.
  var h: holder.Holder;
  h.b.v = 1;
  h.b.x = 2;
  h.b.y = 3;
  printInt(holder.readHolder(&h)); print(",");    // 6
  printInt(sizeof(holder.Holder)); print("\n");   // 12
  return 0;
}
CID
SBOX_EXPECTED=$'8,12,11,52,122,13,8,6,12'

"$BIN" compile --cache sbox/cache --emit-meta "$IO_SRC" -o sbox/build/io.asm
"$BIN" compile --cache sbox/cache --emit-meta sbox/modules/sboxa.cid -o sbox/build/sboxa.asm
"$BIN" compile --cache sbox/cache --emit-meta sbox/modules/sboxb.cid -o sbox/build/sboxb.asm
"$BIN" compile --cache sbox/cache --emit-meta sbox/modules/holder.cid -o sbox/build/holder.asm
"$BIN" compile --cache sbox/cache sbox/main.cid -o sbox/build/main.asm
for m in io sboxa sboxb holder main; do
    nasm -O0 -f elf64 "sbox/build/$m.asm" -o "sbox/build/$m.o"
done
gcc sbox/build/main.o sbox/build/sboxa.o sbox/build/sboxb.o sbox/build/holder.o \
    sbox/build/io.o -o sbox/build/sep -no-pie
set +e
SBOX_OUT=$(sbox/build/sep 2>&1)
SBOX_RC=$?
set -e
[ "$SBOX_RC" -eq 0 ] || fail "struct-template separate exit $SBOX_RC"
[ "$SBOX_OUT" = "$SBOX_EXPECTED" ] || fail "struct-template separate output [$SBOX_OUT]"
# Each module's own object uses its own layout; the entry object picks up
# sboxb's 12-byte layout (the allocation constants) and calls each module's
# mangled factory.
grep -q "push 8" sbox/build/sboxa.asm || fail "sboxa.asm does not use its 8-byte layout"
grep -q "push 12" sbox/build/sboxb.asm || fail "sboxb.asm does not use its 12-byte layout"
grep -q "push 12" sbox/build/main.asm || fail "main.asm does not use sboxb's 12-byte layout"
grep -q "call mod_sboxa__makeSBoxA" sbox/build/main.asm \
    || fail "main.asm does not call sboxa's mangled factory"
grep -q "call mod_sboxb__makeSBoxB" sbox/build/main.asm \
    || fail "main.asm does not call sboxb's mangled factory"
# Corrupt-cache fallback: recompiling the entry against sboxb's SOURCE (its
# snapshot is rejected) must still resolve sboxb's owner identity and link
# with the already-built objects.
printf 'this is not a .cider-meta file\n' > sbox/cache/sboxb.cider-meta
"$BIN" compile --cache sbox/cache sbox/main.cid -o sbox/build/main.src.asm
nasm -O0 -f elf64 sbox/build/main.src.asm -o sbox/build/main.src.o
gcc sbox/build/main.src.o sbox/build/sboxa.o sbox/build/sboxb.o sbox/build/holder.o \
    sbox/build/io.o -o sbox/build/sep_src -no-pie
set +e
SBOX_SRC_OUT=$(sbox/build/sep_src 2>&1)
SBOX_SRC_RC=$?
set -e
[ "$SBOX_SRC_RC" -eq 0 ] || fail "struct-template source-fallback exit $SBOX_SRC_RC"
[ "$SBOX_SRC_OUT" = "$SBOX_EXPECTED" ] || fail "struct-template source-fallback output [$SBOX_SRC_OUT]"

# --- CIDER-BUG-190: a throw crossing an object boundary ------------------
# `midmod` holds a destructor-bearing local and calls a function defined in
# ANOTHER object; the throw unwinds through midmod's frame. The unwinder's
# landing pad for that call site can only be built if the analysis treats a
# callee this object does not define as able to throw, so the destructor
# prints on both the whole-program and the separately linked build. Before
# the fix the separate build's midmod object had no landing and the trace
# lost the `3,`.
mkdir -p unw/modules unw/cache unw/build
cat > unw/modules/thrower.cid <<'CID'
from std.io import print, printInt;
export func boom() -> int { throw 7; }
export class D { var n: int; func ~D() { printInt(this.n); print(","); } }
CID
cat > unw/modules/midmod.cid <<'CID'
import thrower;
export func go(n: int) -> int {
  var d: thrower.D; d.n = n;
  return thrower.boom();
}
CID
cat > unw/main.cid <<'CID'
import midmod;
from std.io import print;
func main() -> int {
  try { midmod.go(3); } catch (e) { print("C;"); }
  print("|");
  return 0;
}
CID
UNW_EXPECTED='3,C;|'
"$BIN" compile unw/main.cid -o unw/build/whole.asm
nasm -O0 -f elf64 unw/build/whole.asm -o unw/build/whole.o
gcc unw/build/whole.o -o unw/build/whole -no-pie
set +e
UNW_WHOLE=$(unw/build/whole 2>&1)
UNW_WHOLE_RC=$?
set -e
[ "$UNW_WHOLE_RC" -eq 0 ] || fail "unwind whole-program exit $UNW_WHOLE_RC"
[ "$UNW_WHOLE" = "$UNW_EXPECTED" ] || fail "unwind whole-program output [$UNW_WHOLE]"
UNW_VM=$("$BIN" run unw/main.cid 2>&1)
[ "$UNW_VM" = "$UNW_EXPECTED" ] || fail "unwind whole-program VM output [$UNW_VM]"

"$BIN" compile --cache unw/cache --emit-meta "$IO_SRC" -o unw/build/io.asm
for m in thrower midmod; do
    "$BIN" compile --cache unw/cache --emit-meta "unw/modules/$m.cid" -o "unw/build/$m.asm"
done
"$BIN" compile --cache unw/cache unw/main.cid -o unw/build/main.asm
for m in io thrower midmod main; do nasm -O0 -f elf64 "unw/build/$m.asm" -o "unw/build/$m.o"; done
gcc unw/build/main.o unw/build/midmod.o unw/build/thrower.o unw/build/io.o \
    -o unw/build/sep -no-pie
set +e
UNW_SEP=$(unw/build/sep 2>&1)
UNW_SEP_RC=$?
set -e
[ "$UNW_SEP_RC" -eq 0 ] || fail "unwind separate exit $UNW_SEP_RC"
[ "$UNW_SEP" = "$UNW_EXPECTED" ] || fail "unwind separate output [$UNW_SEP]"
# The landing pad itself: midmod's object must set a jmp_buf around the
# cross-object call, which is what runs `d`'s finalizer.
grep -q "call _setjmp" unw/build/midmod.asm \
    || fail "midmod.asm has no landing pad for its cross-object call"

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
