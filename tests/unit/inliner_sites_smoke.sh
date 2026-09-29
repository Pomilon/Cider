#!/bin/bash
# CIDER-BUG-162: the inliner's `CALL_OVERHEAD * nSites >= bodyCost` feedback
# used to re-count sites introduced by inlining itself. A body whose calls
# are cloned at many sites doubled the next callee's site count every
# optimizer round, so a 20-site destructor chain ballooned the IR
# exponentially and compile time grew ~5x per +2 sites (N=12 took ~45s).
# The site budget is now captured before inlining and only consumed; this
# shape must compile quickly (sub-second) and fail only by timing out.
set -euo pipefail
BIN=${1:?usage: inliner_sites_smoke.sh <cider-binary>}
command -v timeout >/dev/null 2>&1 || exit 77

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SRC=$TMP/sites.cid
ASM=$TMP/sites.asm

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
    for i in $(seq 0 19); do
        echo "  total = total + mk($i).label;"
    done
    echo "  return total;"
    echo "}"
} > "$SRC"

timeout 30 "$BIN" compile "$SRC" -o "$ASM" \
    || { echo "inliner_sites_smoke: compile failed or timed out"; exit 1; }
[ -s "$ASM" ] || { echo "inliner_sites_smoke: empty asm output"; exit 1; }
echo "inliner_sites_smoke: OK"
