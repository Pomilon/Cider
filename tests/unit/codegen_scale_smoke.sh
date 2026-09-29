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
#
# CIDER-BUG-172: the liveness fixpoint still materialised per-value
# std::set<std::string> live sets and per-value std::set<int> block
# histories, so allocate() stayed O(blocks x live values) with a
# string-compare/node-allocation constant. On an 80f44b0 binary this chain
# costs ~2.3 s at N=400, ~13 s at N=800 and ~41 s at N=1200 (~N^2.5).
# With id-numbered bitset live sets the same points are ~0.5 s, ~3.0 s and
# ~6.9 s. The large-N stage (N=1200) must stay well under its 25 s bound:
# the set-based allocator takes ~41 s here and times out, the fixed
# allocator has ~3.5x headroom.
set -euo pipefail
BIN=${1:?usage: codegen_scale_smoke.sh <cider-binary>}
command -v timeout >/dev/null 2>&1 || exit 77

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# gen <n> <out>: the destructor chain with n temporaries in main.
gen() {
    local n=$1 out=$2
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
        for i in $(seq 0 $((n - 1))); do
            echo "  total = total + mk($i).label;"
        done
        echo "  return total;"
        echo "}"
    } > "$out"
}

# CIDER-BUG-166 scale point.
gen 300 "$TMP/scale300.cid"
timeout 45 "$BIN" compile "$TMP/scale300.cid" -o "$TMP/scale300.asm" \
    || { echo "codegen_scale_smoke: N=300 compile failed or timed out"; exit 1; }
[ -s "$TMP/scale300.asm" ] || { echo "codegen_scale_smoke: empty N=300 asm output"; exit 1; }

# CIDER-BUG-172 large-N bound.
gen 1200 "$TMP/scale1200.cid"
timeout 25 "$BIN" compile "$TMP/scale1200.cid" -o "$TMP/scale1200.asm" \
    || { echo "codegen_scale_smoke: N=1200 compile failed or timed out"; exit 1; }
[ -s "$TMP/scale1200.asm" ] || { echo "codegen_scale_smoke: empty N=1200 asm output"; exit 1; }

echo "codegen_scale_smoke: OK"
