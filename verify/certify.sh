#!/bin/bash
# certify.sh quick|full LIBDIR [--jobs N] [--out DIR] [--ref-any]
# certify.sh --corpus SPEC --lib LIBDIR [--dat DIR] [--ref LIB] [--stock-dat DIR] [--bind-all] [--jobs N] [--fork-jobs N] [--out DIR]
#            [--controls N] [--rerun-cap N] [--fresh FRACTION] [--urandom-fixed] [--phv NAME] [--ref-any] [--no-hash] [--keep]
#            [--stock-digest SO] [--refs FILE] [--known FILE] [--aborts FILE] (pin files other than this checkout's are noted)
# The digest-first stock differential (README.md): every game of SPEC on the engine library (LIBDIR/libpufferhack.so, which exports
# ph_digest; game data LIBDIR/dat) and on untouched stock NLE 0.9.0 (stock/build.sh's reference), one process per core on each
# side, games back to back; per-key digests folded into per-game chains and compared; a game that differs is rerun key by key
# on both sides (streamed, nothing stored; up to --rerun-cap games, default 500, the rest classed from the chains) and dumped as
# text at its first differing key; a random --fresh fraction (default 0.005) of the identical games is replayed in fresh
# processes and must reproduce the main run.
# Writes OUT/verdict.txt (one line per game), OUT/divergences.txt (first differing key and field of every game that is not
# identical), OUT/summary.json and OUT/summary.txt (verdict, provenance, per-side throughput), OUT/controls/.
# Exit 0 = PASS: the reference, the digest ids and the corpora equal their pins (refs.toml, corpora/assets.tsv), every game
# identical, aborted on both sides as stock/aborts.tsv lists or differing exactly as known.tsv lists, every rerun reproducing the
# main run, and every negative control caught (--controls N, default 3: N faults planted in the comparison and N real state faults
# poked into the live engine game, in 2N identical games; fewer identical games voids the run). --controls 0: UNCONTROLLED, exit 4.
# A run that would pass with --refs, --known, --aborts other than this checkout's, or --ref-any, reports UNPINNED, exit 5.
# SPEC: ci (11 corpora, 10,312 games), ci+excluded (+ keys_excluded, 10,320), NAME[,NAME...] (corpora of corpora/regress.tsv and
# of the fetched manifests in CORPORA_DATA, default corpora/data), a manifest (corpus, log relative to the manifest, sha256, keys),
# a NAME.manifest.tsv table (logs in its directory's NAME/), or a list file (one key log per line).
# quick: the regression corpus and ci+excluded in default and bind-all modes, plus the special-mode regression tests (pinned
# /dev/urandom, options NetHack rejects, nle_set_seed mid-game, the seeding API); full adds keys_reach, keys_reach_r1 and
# keys_trained. Each run goes to OUT/NAME; OUT/summary.txt has one line per run and the tier's result.
set -o pipefail
V=$(cd "$(dirname "$0")" && pwd); BIN=$V/build; CD=${CORPORA_DATA:-$V/corpora/data}

if [ "$1" = quick ] || [ "$1" = full ]; then
  TIER=$1; L=$2; shift 2
  [ -n "$L" ] && [ -f "$L/libpufferhack.so" ] || { echo "usage: certify.sh quick|full LIBDIR [--jobs N] [--out DIR] [--ref-any]" >&2; exit 2; }
  L=$(cd "$L" && pwd); OUT=; PASS_ON=()
  while [ $# -gt 0 ]; do case $1 in
    --out) OUT=$2; shift 2;; --jobs) PASS_ON+=(--jobs "$2"); shift 2;; --ref-any) PASS_ON+=(--ref-any); shift;;
    *) echo "certify.sh $TIER: unknown argument $1" >&2; exit 2;; esac; done
  for f in phv phv_pre phv_seed urandom_fixed.so seedapi phd_stock.so; do
    [ -e "$BIN/$f" ] || { echo "certify.sh: $BIN/$f missing (make -C verify; make -C verify stock)" >&2; exit 2; }; done
  for m in ci $([ $TIER = full ] && echo reach trained); do
    [ -s "$CD/$m.tsv" ] || { echo "certify.sh: corpus $m not in $CD (verify/corpora/fetch.sh $m)" >&2; exit 2; }; done
  OUT=${OUT:-$V/runs/${TIER}_$(date -u +%m%d_%H%M%S)}; mkdir -p "$OUT"; : > "$OUT/summary.txt"
  T0=$(date +%s); NF=0
  # NAME|expected result|arguments (UNCONTROLLED: three logs, every key before the hero exists is out of the controls' reach)
  RUNS=("regress|PASS|--corpus regress --fork-jobs 1"
        "regress_bindall|PASS|--corpus regress,regress_bindall --bind-all --fork-jobs 1"
        "ci|PASS|--corpus ci+excluded"
        "ci_bindall|PASS|--corpus ci+excluded --bind-all"
        "urandom|PASS|--corpus regress_urandom --urandom-fixed --controls 1"
        "urandom_bindall|PASS|--corpus regress_urandom --urandom-fixed --controls 1 --bind-all"
        "badoptions|UNCONTROLLED|--corpus regress_pre --phv phv_pre --fork-jobs 1 --fresh 0 --controls 0"
        "badoptions_bindall|UNCONTROLLED|--corpus regress_pre --phv phv_pre --fork-jobs 1 --fresh 0 --controls 0 --bind-all"
        "setseed|PASS|tools/setseed.sh"
        "seedapi|PASS|tools/seedapi.sh")
  [ $TIER = full ] && RUNS+=("reach|PASS|--corpus keys_reach,keys_reach_r1" "reach_bindall|PASS|--corpus keys_reach,keys_reach_r1 --bind-all"
                             "trained|PASS|--corpus keys_trained")
  for R in "${RUNS[@]}"; do
    N=${R%%|*}; R=${R#*|}; WANT=${R%%|*}; A=${R#*|}; rm -rf "$OUT/$N"
    case $A in
      tools/*) "$V/$A" "$L" "$OUT/$N" "${PASS_ON[@]}" > "$OUT/$N.log" 2>&1;;
      *) "$V/certify.sh" --lib "$L" --out "$OUT/$N" "${PASS_ON[@]}" $A > "$OUT/$N.log" 2>&1;;
    esac
    RES=$(grep -E '^result' "$OUT/$N/summary.txt" 2> /dev/null | tail -1 | awk '{print $2}')
    ST=$(grep -E '^(corpus|digest|stock|check)  .*(PASS|FAIL)' "$OUT/$N/summary.txt" 2> /dev/null | tail -1 | cut -c9- | cut -c1-160)
    [ "$RES" = "$WANT" ] && OK=ok || { OK=FAIL; NF=$((NF + 1)); }
    printf 'run     %-18s %-4s %s: %s\n' "$N" "$OK" "${RES:-NONE}" "${ST:-see $OUT/$N.log}" | tee -a "$OUT/summary.txt"
  done
  RES=$([ $NF = 0 ] && echo PASS || echo FAIL)
  echo "result  $TIER $RES: $((${#RUNS[@]} - NF))/${#RUNS[@]} runs as expected, ${L}/libpufferhack.so $(sha256sum < "$L/libpufferhack.so" | cut -c1-16) ($OUT) in $(($(date +%s) - T0)) s" | tee -a "$OUT/summary.txt"
  [ $RES = PASS ]; exit $?
fi

SPEC=; LIB=; DAT=; REF=$V/stock/build/ref/libnethack.so; SDAT=$V/stock/build/ref/nethackdir; SDIG=$BIN/phd_stock.so; MODE=default
JOBS=$(nproc); FJ=; OUT=; CONTROLS=3; HASH=1; KEEP=; CAP=500; FRESH=0.005; REFS=$V/refs.toml; ABORTS=$V/stock/aborts.tsv
KNOWN=$V/known.tsv; PHV=phv; REFANY=0; URAND=0
while [ $# -gt 0 ]; do case $1 in
  --corpus) SPEC=$2; shift 2;; --ref) REF=$2; shift 2;; --lib) LIB=$2; shift 2;; --dat) DAT=$2; shift 2;;
  --stock-dat) SDAT=$2; shift 2;; --stock-digest) SDIG=$2; shift 2;; --bind-all) MODE=bind-all; shift;;
  --jobs) JOBS=$2; shift 2;; --fork-jobs) FJ=$2; shift 2;; --out) OUT=$2; shift 2;; --controls) CONTROLS=$2; shift 2;;
  --rerun-cap) CAP=$2; shift 2;; --fresh) FRESH=$2; shift 2;; --refs) REFS=$2; shift 2;; --aborts) ABORTS=$2; shift 2;;
  --known) KNOWN=$2; shift 2;; --phv) PHV=$2; shift 2;; --ref-any) REFANY=1; shift;; --urandom-fixed) URAND=1; shift;;
  --no-hash) HASH=0; shift;; --keep) KEEP=--keep; shift;;
  *) echo "certify.sh: unknown argument $1" >&2; exit 2;; esac; done
[ -n "$SPEC" ] && [ -n "$LIB" ] || { echo "usage: certify.sh --corpus SPEC --lib LIBDIR ... | certify.sh quick|full LIBDIR (see the header)" >&2; exit 2; }
[ -n "${PHV_FAULT+x}${PHV_POKE+x}" ] && { echo "certify.sh: PHV_FAULT / PHV_POKE are set in the environment; controls are planted by this script only" >&2; exit 2; }
LIB=$(cd "$LIB" && pwd) || exit 2; DAT=$(realpath -m "${DAT:-$LIB/dat}"); REF=$(realpath -m "$REF"); SDAT=$(realpath -m "$SDAT"); SDIG=$(realpath -m "$SDIG"); P=$BIN/$PHV
for f in "$P" "$SDIG" "$REF" "$LIB/libpufferhack.so"; do
  [ -e "$f" ] || { echo "certify.sh: $f missing (make -C verify; make -C verify stock)" >&2; exit 2; }; done
if [ $URAND = 1 ]; then  # /dev/urandom read by both sides as a fixed seed: reseed=1 games reseed alike after nle_start
  [ -e "$BIN/urandom_fixed.so" ] || { echo "certify.sh: $BIN/urandom_fixed.so missing (make -C verify)" >&2; exit 2; }
  export LD_PRELOAD=$BIN/urandom_fixed.so${LD_PRELOAD:+:$LD_PRELOAD}; fi
NAME=$(basename "${SPEC%%.*}" | tr ',/+' '__p'); OUT=${OUT:-$V/runs/${NAME}_$(date -u +%m%d_%H%M%S)}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
say() { echo "$*" | tee -a "$OUT/summary.txt"; }
fail() { say "$1"; say "result  FAIL ($OUT) in $(python3 -c "import time; print(round(time.time() - $T0, 1))") s"; exit 1; }
T0=$(date +%s.%N)

# 1. the game list (path, name, logged keys), every log checked against its manifest's sha256 and every fetched manifest against
# its pin in corpora/assets.tsv
python3 -I - "$SPEC" "$V/corpora" "$CD" "$OUT/games.tsv" "$HASH" "$JOBS" > "$OUT/corpus.txt" <<'PY'
import hashlib, os, sys
from concurrent.futures import ProcessPoolExecutor
spec, cv, cd, out, dohash, jobs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] == '1', int(sys.argv[6])
CI = 'keys_both keys_champ keys_fz2wiz keys_fz2plain keys_scen keys_cov keys_cov2 keys_cov3 keys_cov4 keys_cov5 keys_cov6'.split()
def h(p):
    try: return hashlib.sha256(open(p, 'rb').read()).hexdigest()
    except OSError: return 'MISSING'
def table(m):  # corpus, log (relative to the manifest), sha256, keys
    d = os.path.dirname(os.path.abspath(m))
    return [(r[0], os.path.join(d, r[1]), r[2], int(r[3])) for r in (l.rstrip('\n').split('\t') for l in open(m))
            if r[0] and not r[0].startswith('#')]
rows, src, bad = [], '', []
if spec in ('ci', 'ci+excluded') or (not spec.endswith('.tsv') and not os.path.isfile(spec)):
    sel = CI + (['keys_excluded'] if spec == 'ci+excluded' else []) if spec in ('ci', 'ci+excluded') else spec.split(',')
    known = [r + ('corpora/regress.tsv',) for r in table(os.path.join(cv, 'regress.tsv'))]
    pins = {r[3]: r[4] for r in (l.rstrip('\n').split('\t') for l in open(os.path.join(cv, 'assets.tsv'))) if r[0] and not r[0].startswith('#')}
    for m, want in pins.items():
        p = os.path.join(cd, m)
        if not os.path.exists(p): continue
        if h(p) != want: bad.append(f'{p}: sha256 {h(p)[:16]}, corpora/assets.tsv pins {want[:16]}'); continue
        known += [r + (p,) for r in table(p)]
    used = []
    for c in sel:
        cr = [r for r in known if r[0] == c]
        if not cr: bad.append(f'no corpus {c} in corpora/regress.tsv or {cd} (corpora/fetch.sh fetches the release corpora)'); continue
        rows += [r[1:4] for r in cr]; used += sorted({r[4] for r in cr} - set(used))
    src = ' '.join(used)
elif spec.endswith('.manifest.tsv'):
    src = spec; d = os.path.join(os.path.dirname(spec), os.path.basename(spec).replace('.manifest.tsv', ''))
    hdr = None
    for l in open(spec):
        f = l.rstrip('\n').split('\t')
        if hdr is None: hdr = f; continue
        r = dict(zip(hdr, f)); rows.append((os.path.join(d, r['name']), r['sha256'], int(r['keys'])))
elif spec.endswith('.tsv'):
    src = spec; rows = [r[1:] for r in table(spec)]
else:
    src = spec
    rows = [(l.strip(), '', 0) for l in open(spec) if l.strip()]
if dohash:
    with ProcessPoolExecutor(max(1, jobs)) as ex:
        for (p, want, k), got in zip(rows, ex.map(h, [r[0] for r in rows], chunksize=256)):
            if got == 'MISSING' or (want and got != want): bad.append(f'{p}: {got[:16]} want {want[:16]}')
names = set()
with open(out, 'w') as o:
    for p, want, k in rows:
        n = os.path.basename(os.path.dirname(p)) + '/' + os.path.basename(p)[:-5] if p.endswith('.keys') else p
        if n in names: bad.append(f'{n}: listed twice')
        names.add(n); o.write(f'{p}\t{n}\t{k}\n')
nsha = sum(1 for r in rows if r[1])
print(len(rows), sum(r[2] for r in rows), len(bad), nsha, src)
for b in bad[:50]: print('BAD', b)
PY
read NG LOGGED NBAD NSHA SRC < <(head -1 "$OUT/corpus.txt")
[ "${NBAD:-1}" = 0 ] && [ "${NG:-0}" -gt 0 ] || fail "corpus  FAIL ${NBAD:-?} logs, manifests or corpora missing or changed: $(grep -m1 '^BAD' "$OUT/corpus.txt" | cut -c5-200) ($OUT/corpus.txt)"
say "corpus  PASS $NG games, $LOGGED logged keys ($SPEC from $SRC$([ $HASH = 1 ] && { [ "$NSHA" = "$NG" ] && echo ', every log checked against its sha256' || echo ", $NSHA logs with a manifest sha256 checked, the rest only found"; } || echo ', logs NOT checked against their sha256 (--no-hash)'))"

# 2. provenance, the environment, the reference and the digest definition on both sides against the pins
sha() { sha256sum < "$1" | cut -c1-16; }
pin() { awk -v s="[$1]" -v k="$2" '/^\[/ {sec = ($0 == s)} sec && $1 == k {sub(/^[^=]*= */, ""); gsub(/"/, ""); print; exit}' "$REFS"; }
IDS=$("$P" id --lib "$LIB" --ref "$REF" --stock-digest "$SDIG")
FID=$(echo "$IDS" | awk '$1 == "fork" {print $2}'); SID=$(echo "$IDS" | awk '$1 == "stock" {print $2}'); AID=$(echo "$IDS" | awk '$1 == "adapter" {print $2}')
PID=$(pin digest id); PAID=$(pin digest adapter_id); PREF=$(pin stock library_sha256); RSHA=$(sha256sum < "$REF" | cut -d' ' -f1)
ENVL=$(env | grep -E '^(NH_|NLE_|PHV_|LD_PRELOAD|LD_LIBRARY_PATH|MALLOC_|GLIBC_TUNABLES)' | sort | tr '\n' ' ')
say "engine  $LIB/libpufferhack.so $(sha $LIB/libpufferhack.so)$([ -s $LIB/SOURCE ] && echo " source $(cut -c1-12 $LIB/SOURCE)"), data $DAT"
say "stock   $REF ${RSHA:0:16}$([ "$RSHA" = "$PREF" ] && echo ' (the pinned reference)' || echo " (NOT the pinned reference ${PREF:0:16})"), data $SDAT"
say "harness $PHV $(sha $P), phd_stock.so $(sha $SDIG), certify.sh $(sha $V/certify.sh), refs $REFS $(sha $REFS 2> /dev/null), known $(sha $KNOWN 2> /dev/null), mode $MODE"
OVR=$(for x in "--refs $REFS $V/refs.toml" "--known $KNOWN $V/known.tsv" "--aborts $ABORTS $V/stock/aborts.tsv"; do
  set -- $x; [ "$(realpath -m "$2")" = "$(realpath -m "$3")" ] || printf ' %s %s %s' "$1" "$2" "$(sha "$2" 2> /dev/null)"; done)
[ -n "$OVR" ] && say "pins    NOTE not this checkout's:$OVR"
say "env     ${ENVL:-none of NH_* NLE_* PHV_* LD_PRELOAD LD_LIBRARY_PATH MALLOC_* GLIBC_TUNABLES set}"
[ "$RSHA" = "$PREF" ] || [ $REFANY = 1 ] || fail "stock   FAIL the reference is not the pinned build (refs.toml library_sha256; stock/build.sh, or --ref-any to certify against it anyway)"
[ -n "$FID" ] && [ "$FID" = "$SID" ] || fail "digest  FAIL definition ids differ or missing: engine '$FID', stock '$SID'"
[ -n "$PID" ] && [ "$FID" = "$PID" ] && [ "$AID" = "$PAID" ] || fail "digest  FAIL ids not the pinned ones: digest $FID (pinned ${PID:-none}), stock adapter $AID (pinned ${PAID:-none}) in $REFS"
say "digest  PASS one definition on both sides, the pinned one (ph_digest id $FID, stock adapter $AID)"

# 3. both sides, compare, reruns
EXTRA=; [ $MODE = bind-all ] && EXTRA="--bind-all --ignore obs.bl"; [ -n "$FJ" ] && EXTRA="$EXTRA --fork-jobs $FJ"
"$P" certify "$OUT/games.tsv" "$OUT" --lib "$LIB" --dat "$DAT" --ref "$REF" --stock-dat "$SDAT" --stock-digest "$SDIG" \
  --jobs "$JOBS" --rerun-cap "$CAP" --fresh "$FRESH" $EXTRA $KEEP 2> "$OUT/phv.log" > "$OUT/phv.out"; RC=$?
[ $RC = 0 ] || fail "stock   FAIL phv exited $RC ($OUT/phv.log)"

# 4. verdict: every game identical, aborted on both sides as stock/aborts.tsv says, or differing exactly as known.tsv says
python3 -I "$V/tools/verdict.py" "$OUT" "$ABORTS" "$NG" "$KNOWN" "$MODE" | tee -a "$OUT/summary.txt"
PASS=$?

# 5. negative controls (tools/controls.py): CONTROLS faults in the comparison and CONTROLS real state faults poked into the live
# engine game (phv_poke.so built for this library), in distinct identical games at random keys; each must be reported at exactly
# its key and component, else the run is void
CRC=4
if [ "$CONTROLS" -gt 0 ]; then
  CRC=3; PL=$BIN/poke/$(sha $LIB/libpufferhack.so)/phv_poke.so
  [ -s "$PL" ] || make -s -C "$V" poke LIBDIR="$LIB" POKE="$PL" > "$OUT/poke_build.log" 2>&1 || PL=
  if [ -z "$PL" ]; then say "control VOID the poke library for $LIB could not be built ($OUT/poke_build.log)"
  elif python3 -I "$V/tools/controls.py" plan "$OUT" "$CONTROLS" "$MODE" "$(cat /proc/sys/kernel/random/uuid)" | tee -a "$OUT/summary.txt"; then
    F=$(awk -F'\t' '$1 == "fault" {printf "%s%s:%s:%s", (n++ ? "," : ""), $2, $4, $5}' "$OUT/controls.tsv")
    K=$(awk -F'\t' '$1 == "poke" {printf "%s%s:%s:%s", (n++ ? "," : ""), $2, $4, $5}' "$OUT/controls.tsv")
    awk -F'\t' '{print $2"\t"$3"\t0"}' "$OUT/controls.tsv" > "$OUT/controls_games.tsv"
    "$P" certify "$OUT/controls_games.tsv" "$OUT/controls" --lib "$LIB" --dat "$DAT" --ref "$REF" --stock-dat "$SDAT" \
      --stock-digest "$SDIG" --jobs $((CONTROLS * 4 < JOBS ? CONTROLS * 4 : JOBS)) --fresh 0 --fault "$F" --poke "$K" --poke-lib "$PL" \
      $([ $MODE = bind-all ] && echo "--bind-all --ignore obs.bl") > /dev/null 2> "$OUT/controls.log"
    python3 -I "$V/tools/controls.py" check "$OUT" | tee -a "$OUT/summary.txt"; CRC=$?
  fi
fi
RES=$([ $PASS = 0 ] && { [ $CRC = 0 ] && echo PASS || { [ $CRC = 4 ] && echo UNCONTROLLED || echo FAIL; }; } || echo FAIL)
[ $RES = PASS ] && { [ -n "$OVR" ] || [ $REFANY = 1 ]; } && RES=UNPINNED  # overridden pins or reference: not a certificate
python3 -I - "$OUT" "$RES" "$SPEC" "$LIB" "$REF" "$FID" "$AID" "$(sha $LIB/libpufferhack.so)" "${RSHA:0:16}" "$T0" "$ENVL" "$([ "$RSHA" = "$PREF" ] && echo 1 || echo 0)" <<'PY'
import json, sys, time
out, res, spec, lib, ref, fid, aid, lsha, rsha, t0, envl, pinned = sys.argv[1:13]
s = json.load(open(out + '/summary.json'))
s.update(result=res, corpus=spec, engine=lib, engine_sha256_16=lsha, reference=ref, reference_sha256_16=rsha,
         reference_pinned=pinned == '1', digest_id=fid, adapter_id=aid, env=dict(x.split('=', 1) for x in envl.split()),
         total_wall_s=round(time.time() - float(t0), 3))
json.dump(s, open(out + '/summary.json', 'w'), indent=1)
PY
say "result  $RES ($OUT) in $(python3 -c "import time; print(round(time.time() - $T0, 1))") s"
[ $RES = PASS ] && exit 0; [ $RES = UNCONTROLLED ] && exit 4; [ $RES = UNPINNED ] && exit 5; exit 1
