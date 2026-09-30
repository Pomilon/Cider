#!/bin/bash
# CIDER-BUG-222: std resolution must not depend on where the compiler binary
# lives, so the same source compiled by binaries in differently deep build
# directories produces byte-identical output.
#
# Fixture: one stdlib, one place in the tree (`$DIR/proj/modules`, a symlink
# to this checkout's), and THREE copies of the same compiler at three
# different depths below it. The old rule -- the binary's own `../modules`
# and `../../modules` -- reached the stdlib from the shallowest and the
# middle copy only, each spelling the resolved path differently
# (`build/../modules/std/io.cid` vs `build/a/../../modules/std/io.cid`), and
# missed it from the deepest copy. Because the resolved path reaches the
# output (`cider bundle` banners, the `-g` DWARF file table), one program
# compiled by two such binaries produced different bytes, which made
# byte-identity comparisons across build directories meaningless.
#
# Three working directories, each of which must give identical bytes for all
# three binaries:
#   1. outside the project   -- the stdlib is reachable only through the
#                              installed/bundled fallback (the binary's own
#                              directory and its ancestors);
#   2. deep inside the build -- reached through the working-directory
#                              ancestor walk, so the resolved path is
#                              `../../../modules/std/io.cid`;
#   3. the project root      -- `modules/std/io.cid`.
# Plus: a relative `modules/` next to the program still wins, and the
# program still RUNS (the resolved stdlib is really used, not just parsed).
#
# Usage: std_resolve_smoke.sh <cider-binary>
# Needs no assembler: it compares emitted text.
set -euo pipefail

BIN=${1:?usage: std_resolve_smoke.sh <cider-binary>}
for tool in cp mktemp cmp diff grep; do
    command -v "$tool" >/dev/null 2>&1 || { echo "std_resolve_smoke: SKIP ($tool not found)"; exit 77; }
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)
[ -f "$REPO/modules/std/io.cid" ] || {
    echo "std_resolve_smoke: SKIP (stdlib not found at $REPO/modules)"; exit 77; }

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

fail() { echo "std_resolve_smoke: FAIL ($1)"; exit 1; }

# One stdlib, one place: a checkout-shaped project.
mkdir -p "$DIR/proj/build/a/b" "$DIR/work" "$DIR/src/modules"
ln -s "$REPO/modules" "$DIR/proj/modules"

# The same compiler at three depths -- the only variable in the experiment.
NAMES=(d1 d2 d3)
BIN_OF=("$DIR/proj/build/cider" "$DIR/proj/build/a/cider" "$DIR/proj/build/a/b/cider")
i=0
for b in "${BIN_OF[@]}"; do
    cp "$BIN" "$b"
    i=$((i + 1))
done

# The program under test: a stdlib import (which has to be discovered) plus
# a project-local module under ./modules (which has to keep winning).
cat > src/main.cid <<'CID'
from std.io import print, printInt;
from std.math import abs;
from helper import twice;

func main() -> int {
  print("cid");
  printInt(twice(abs(0 - 21)));
  print("\n");
  return 0;
}
CID
EXPECTED=cid42
cat > src/modules/helper.cid <<'CID'
export func twice(x: int) -> int { return x * 2; }
CID

# Emit every artifact that can carry a resolved path, from one binary, in one
# working directory. The working directory carries a copy of the program so
# the three scenarios differ in nothing but their location.
emit_all() { # <name> <bin> <cwd> <tag>
    local name=$1 bin=$2 cwd=$3 tag=$4
    local out="$DIR/$tag.$name"
    ( cd "$cwd" && "$bin" compile main.cid -o "$out.asm" ) 2>"$out.compile.err" \
        || fail "$name in $cwd: compile failed: $(head -2 "$out.compile.err")"
    ( cd "$cwd" && "$bin" compile -g main.cid -o "$out.dbg.asm" ) 2>"$out.dbg.err" \
        || fail "$name in $cwd: compile -g failed: $(head -2 "$out.dbg.err")"
    ( cd "$cwd" && "$bin" bundle main.cid -o "$out.cid" ) 2>"$out.bundle.err" \
        || fail "$name in $cwd: bundle failed: $(head -2 "$out.bundle.err")"
    ( cd "$cwd" && "$bin" build main.cid ) >"$out.ir" 2>"$out.build.err" \
        || fail "$name in $cwd: build failed: $(head -2 "$out.build.err")"
}

# Emit from all three binaries and require every artifact to be byte-identical
# to the shallowest one's. Compared as each binary is emitted, so a binary that
# cannot even resolve the stdlib is reported as such instead of hiding behind
# an earlier diff.
compare_binaries() { # <tag> <cwd>
    local tag=$1 cwd=$2 i n ext
    i=0
    for n in "${NAMES[@]}"; do
        emit_all "$n" "${BIN_OF[$i]}" "$cwd" "$tag"
        if [ "$i" -gt 0 ]; then
            for ext in asm dbg.asm cid ir; do
                cmp -s "$DIR/$tag.d1.$ext" "$DIR/$tag.$n.$ext" || {
                    diff "$DIR/$tag.d1.$ext" "$DIR/$tag.$n.$ext" | head -6
                    fail "$tag: d1 vs $n ($ext) differ in $cwd -- the output depends on the binary's location"
                }
            done
        fi
        i=$((i + 1))
    done
    echo "std_resolve_smoke: $tag: three binary locations agree byte for byte"
}

# --- 1. outside the project: only the installed/bundled fallback applies ---
cp src/main.cid "$DIR/work/"
cp -r src/modules "$DIR/work/modules"
compare_binaries outside "$DIR/work"
# The stdlib module really resolved -- and the program really runs -- from
# the deepest binary, in a working directory with no project above it.
OUT=$( cd "$DIR/work" && "${BIN_OF[2]}" run main.cid 2>&1 ) || fail "outside: run failed: $OUT"
[ "$OUT" = "$EXPECTED" ] || fail "outside: run output [$OUT] (want $EXPECTED)"
# The project-local module next to the program resolved from the CWD.
grep -q '^// ===== bundled from modules/helper.cid =====$' "$DIR/outside.d3.cid" \
    || fail "outside: the CWD's modules/helper.cid did not win"
echo "std_resolve_smoke: outside: the deepest binary compiles and runs it"

# --- 2. deep inside the build tree: the working-directory ancestor walk ---
cp src/main.cid "$DIR/proj/build/a/b/"
cp -r src/modules "$DIR/proj/build/a/b/modules"
compare_binaries deep "$DIR/proj/build/a/b"
# The stdlib path is the working-directory-relative one: no build directory
# and no /proc/self/exe anywhere in it.
grep -q '^// ===== bundled from ../../../modules/std/io.cid =====$' "$DIR/deep.d3.cid" \
    || fail "deep: unexpected stdlib path: $(grep 'bundled from' "$DIR/deep.d3.cid" | tr '\n' ' ')"
echo "std_resolve_smoke: deep: paths anchored at the working directory"

# --- 3. the project root --------------------------------------------------
cp src/main.cid src/modules/helper.cid "$DIR/proj/"
compare_binaries root "$DIR/proj"
grep -q '^// ===== bundled from modules/std/io.cid =====$' "$DIR/root.d3.cid" \
    || fail "root: unexpected stdlib path: $(grep 'bundled from' "$DIR/root.d3.cid" | tr '\n' ' ')"
OUT=$( cd "$DIR/proj" && "${BIN_OF[0]}" run main.cid 2>&1 ) || fail "root: run failed: $OUT"
[ "$OUT" = "$EXPECTED" ] || fail "root: run output [$OUT] (want $EXPECTED)"
echo "std_resolve_smoke: root: the shallowest binary runs it"

echo "std_resolve_smoke: OK"
