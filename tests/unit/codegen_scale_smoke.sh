#!/bin/bash
# CIDER-BUG-166: codegen work used to be superlinear in IR size. The
# destructor chain below gives `main` N call sites whose temporaries all
# stay live until their (reverse-order) destructor blocks; the register
# allocator's liveness fixpoint swept blocks forward (one block of
# propagation per sweep: O(B^2) set merges) and the CALL emitter rescanned
# every allocated value per call site (O(values) per call, quadratic on
# call-heavy functions). A pristine a61a6b3 binary takes ~111 s for N=240
# and would keep growing ~N^3; with the fix N=300 compiles in about a
# second. This shape must compile well under the timeout.
set -euo pipefail
BIN=${1:?usage: codegen_scale_smoke.sh <cider-binary>}
command -v timeout >/dev/null 2>&1 || exit 77

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SRC=$TMP/scale.cid
ASM=$TMP/scale.asm

{
    echo "var drops: int = 0;"
    echo "func p0() -> int { return 1; }"
    for i in 1 2 3 4 5 6; do
        echo "func p$i() -> int { return p$((i - 1))() + p$((i - 1))(); }"
    done
    echo "class D {"
    echo "  var label: int;"
    echo "  func ~D() {"
    echo "    drops = drops + p6();"
    echo "    drops = drops + p6();"
    echo "  }"
    echo "}"
    echo "func mk(n: int) -> D {"
    echo "  var d: D;"
    echo "  d.label = n;"
    echo "  return d;"
    echo "}"
    echo "func main() -> int {"
    echo "  var total: int = 0;"
    for i in $(seq 0 299); do
        echo "  total = total + mk($i).label;"
    done
    echo "  return total;"
    echo "}"
} > "$SRC"

timeout 45 "$BIN" compile "$SRC" -o "$ASM" \
    || { echo "codegen_scale_smoke: compile failed or timed out"; exit 1; }
[ -s "$ASM" ] || { echo "codegen_scale_smoke: empty asm output"; exit 1; }
echo "codegen_scale_smoke: OK"
