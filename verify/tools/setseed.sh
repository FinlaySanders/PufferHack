#!/bin/bash
# tools/setseed.sh LIBDIR OUT [--ref-any] [--jobs N]: nle_set_seed in mid-game on both sides. phv_seed (phv + src/phv_seed.patch)
# calls it before key 3 of the regression corpus's set_seed log with four seed tuples (the last one with reseed and lgen, under
# the pinned /dev/urandom), in default and bind-all modes, and compares key by key; every verdict must be IDENTICAL |
# STATE_IDENTICAL. Writes OUT/verdict.txt, OUT/summary.txt and the per-key files.
set -o pipefail
V=$(cd "$(dirname "$0")/.." && pwd); B=$V/build; L=$(cd "$1" && pwd) || exit 2; O=$2; shift 2; REFANY=0
while [ $# -gt 0 ]; do case $1 in --ref-any) REFANY=1; shift;; --jobs) shift 2;; *) echo "setseed.sh: unknown argument $1" >&2; exit 2;; esac; done
mkdir -p "$O"; O=$(cd "$O" && pwd); : > "$O/verdict.txt"; : > "$O/summary.txt"
say() { echo "$*" | tee -a "$O/summary.txt"; }
pin() { awk -v s="[$1]" -v k="$2" '/^\[/ {sec = ($0 == s)} sec && $1 == k {sub(/^[^=]*= */, ""); gsub(/"/, ""); print; exit}' "$V/refs.toml"; }
LOG=$V/corpora/regress/seeds/f2_setseed.keys; REF=$V/stock/build/ref/libnethack.so; T0=$(date +%s)
WANT=$(awk -F'\t' '$1 == "regress_setseed" {print $3}' "$V/corpora/regress.tsv")
[ "$(sha256sum < "$LOG" | cut -d' ' -f1)" = "$WANT" ] || { say "check   FAIL $LOG differs from corpora/regress.tsv"; say "result  FAIL"; exit 1; }
RSHA=$(sha256sum < "$REF" | cut -d' ' -f1)
[ "$RSHA" = "$(pin stock library_sha256)" ] || [ $REFANY = 1 ] || { say "stock   FAIL $REF is not the pinned reference"; say "result  FAIL"; exit 1; }
IDS=$("$B/phv_seed" id --lib "$L" --ref "$REF" --stock-digest "$B/phd_stock.so" | awk '{print $2}' | sort -u | tr '\n' ' ')
[ "$IDS" = "$(printf '%s\n' "$(pin digest id)" "$(pin digest adapter_id)" | sort -u | tr '\n' ' ')" ] || { say "digest  FAIL ids $IDS, pinned $(pin digest id) $(pin digest adapter_id)"; say "result  FAIL"; exit 1; }
say "engine  $L/libnethack.so $(sha256sum < "$L/libnethack.so" | cut -c1-16), stock $REF ${RSHA:0:16}, phv_seed $(sha256sum < "$B/phv_seed" | cut -c1-16)"
A="--lib $L --dat $L/dat --ref $REF --stock-dat $V/stock/build/ref/nethackdir --stock-digest $B/phd_stock.so"
N=0; BAD=0
for S in 3,1234,5678,0,0 3,9999,4321,0,0 3,1234,5678,0,77 u:3,abcdef,123,1,5; do
  for M in default bind-all; do
    P=; [ "${S%%:*}" = u ] && P=$B/urandom_fixed.so; s=${S#u:}; X=; [ $M = bind-all ] && X=--bind-all; t=$(echo "$s.$M" | tr ',' '_')
    env ${P:+LD_PRELOAD=$P} SETSEED=$s "$B/phv_seed" game fork "$LOG" --perkey "$O/$t.fork.pk" $A $X > "$O/$t.fork.out" 2>&1; FR=$?
    env ${P:+LD_PRELOAD=$P} SETSEED=$s "$B/phv_seed" game stock "$LOG" --perkey "$O/$t.stock.pk" $A $X > "$O/$t.stock.out" 2>&1; SR=$?
    R=$("$B/phv_seed" cmpkey "$O/$t.fork.pk" "$O/$t.stock.pk" "$LOG" --fork-rc $FR --stock-rc $SR ${X:+--ignore obs.bl} | head -1)
    echo "seeds/f2_setseed@$s${P:+,urandom},$M $R" >> "$O/verdict.txt"; N=$((N + 1))
    case $R in "IDENTICAL keys "*"| STATE_IDENTICAL keys"*) ;; *) BAD=$((BAD + 1)); echo "  differ seeds/f2_setseed@$s,$M $R" | tee -a "$O/summary.txt";; esac
  done
done
RES=$([ $BAD = 0 ] && echo PASS || echo FAIL)
say "check   $RES $((N - BAD))/$N set_seed replays identical in RNG, messages, done, state and observations (nle_set_seed before key 3, both modes)"
say "result  $RES ($O) in $(($(date +%s) - T0)) s"
[ $RES = PASS ]
