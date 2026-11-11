#!/bin/bash
# corpora/fetch.sh [NAME...]: the release-asset corpora of assets.tsv (ci, reach, trained; all of them without a NAME) into
# CORPORA_DATA (default corpora/data, where certify.sh looks). Each tarball comes from CORPORA_DIR when set (a local directory
# holding the tarballs), else from $CORPORA_URL/<asset>; its sha256, its manifest's sha256 and every log's sha256 must equal the
# pins (assets.tsv, then the manifest), else nothing is kept.
set -e
C=$(cd "$(dirname "$0")" && pwd); D=${CORPORA_DATA:-$C/data}
# the download base of the published release assets (placeholder: set it here or in the environment once they are published)
CORPORA_URL=${CORPORA_URL:-https://github.com/OWNER/PufferHack/releases/download/TAG}
for n in "$@"; do
  grep -v '^#' "$C/assets.tsv" | cut -f4 | grep -qx "$n.tsv" || { echo "FETCH_FAIL no corpus asset $n in assets.tsv"; exit 1; }; done
mkdir -p "$D"
sha() { sha256sum < "$1" | cut -d' ' -f1; }
grep -v '^#' "$C/assets.tsv" | while IFS=$'\t' read -r A ASHA BYTES MAN MSHA CORPORA; do
  N=${MAN%.tsv}
  [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$N" && continue
  if [ -s "$D/$MAN" ] && [ "$(cat "$D/.$N.ok" 2> /dev/null)" = "$ASHA" ] && [ "$(sha "$D/$MAN")" = "$MSHA" ]; then
    echo "FETCH_OK $N (already in $D)"; continue; fi
  T=$(mktemp -d "$D/.fetch.XXXXXX")
  if [ -n "$CORPORA_DIR" ]; then cp "$CORPORA_DIR/$A" "$T/$A" || { echo "FETCH_FAIL $N: $CORPORA_DIR/$A"; rm -rf "$T"; exit 1; }
  else
    case $CORPORA_URL in *OWNER*) echo "FETCH_FAIL $N: set CORPORA_URL to the release's download base, or CORPORA_DIR to a directory with $A"; rm -rf "$T"; exit 1;; esac
    curl -fsSL -o "$T/$A" "$CORPORA_URL/$A" || { echo "FETCH_FAIL $N: download $CORPORA_URL/$A"; rm -rf "$T"; exit 1; }
  fi
  [ "$(sha "$T/$A")" = "$ASHA" ] || { echo "FETCH_FAIL $N: $A has sha256 $(sha "$T/$A"), assets.tsv pins $ASHA"; rm -rf "$T"; exit 1; }
  mkdir "$T/x"; tar -xzf "$T/$A" -C "$T/x"
  [ "$(sha "$T/x/$MAN")" = "$MSHA" ] || { echo "FETCH_FAIL $N: $MAN differs from its pin"; rm -rf "$T"; exit 1; }
  (cd "$T/x" && grep -v '^#' "$MAN" | awk -F'\t' '{print $3 "  " $2}' | sha256sum -c --quiet --strict) \
    || { echo "FETCH_FAIL $N: logs differ from $MAN"; rm -rf "$T"; exit 1; }
  for c in $CORPORA; do rm -rf "${D:?}/$c"; mv "$T/x/$c" "$D/$c"; done
  mv "$T/x/$MAN" "$D/$MAN"; echo "$ASHA" > "$D/.$N.ok"; rm -rf "$T"
  echo "FETCH_OK $N: $(grep -vc '^#' "$D/$MAN") logs ($CORPORA) in $D"
done
