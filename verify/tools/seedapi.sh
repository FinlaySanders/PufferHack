#!/bin/bash
# tools/seedapi.sh LIBDIR OUT [--ref-any] [--jobs N]: the seeding API with several games in one process (build/seedapi, from
# src/seedapi.c): nle_get_seed and nle_set_seed act on the game passed in, from any thread, and lgen keeps NLE 0.9.0's streams
# unless a game started with it. Engine only; every check must pass.
V=$(cd "$(dirname "$0")/.." && pwd); L=$(cd "$1" && pwd) || exit 2; O=$2; mkdir -p "$O"; O=$(cd "$O" && pwd); T0=$(date +%s)
D=$(mktemp -d); TMPDIR=$D "$V/build/seedapi" "$L" > "$O/seedapi.out" 2>&1; RC=$?; rm -rf "$D"
NP=$(grep -c '^PASS' "$O/seedapi.out"); NF=$(grep -c '^FAIL' "$O/seedapi.out")
RES=$([ $RC = 0 ] && [ "$NF" = 0 ] && [ "$NP" -gt 0 ] && echo PASS || echo FAIL)
{ echo "engine  $L/libnethack.so $(sha256sum < "$L/libnethack.so" | cut -c1-16)"
  grep '^FAIL' "$O/seedapi.out" | sed 's/^/  /'
  echo "check   $RES $NP/$((NP + NF)) seeding API checks passed, exit $RC ($O/seedapi.out)"
  echo "result  $RES ($O) in $(($(date +%s) - T0)) s"; } | tee "$O/summary.txt"
[ $RES = PASS ]
