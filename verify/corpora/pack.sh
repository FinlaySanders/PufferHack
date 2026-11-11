#!/bin/bash
# pack.sh OUT.tar.gz NAME ROOT MANIFEST [CORPUS...]: a corpus release asset. MANIFEST lists logs as corpora.tsv does (corpus, log
# relative to ROOT, sha256, keys); the rows of the CORPUS names (all rows without any) are checked against their sha256 and packed
# with NAME.tsv, their manifest, into a reproducible tar.gz (sorted, mtime 0, root-owned, gzip -n). Prints the asset's sha256 and
# the manifest's: the two pins of a corpora/assets.tsv row.
set -e
OUT=$1; NAME=$2; ROOT=$3; MAN=$4; shift 4
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -I - "$ROOT" "$MAN" "$T/$NAME.tsv" "$T/list" "$NAME" "$@" <<'PY'
import hashlib, os, sys
root, man, out, lst, name, sel = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], set(sys.argv[6:])
rows = [l.rstrip('\n').split('\t') for l in open(man) if not l.startswith('#') and l.strip()]
rows = [r for r in rows if not sel or r[0] in sel]
bad = [r[1] for r in rows if hashlib.sha256(open(os.path.join(root, r[1]), 'rb').read()).hexdigest() != r[2]]
if bad or not rows:
    sys.exit(f'pack: {len(bad)} logs differ from their sha256 ({bad[:3]}), {len(rows)} rows')
with open(out, 'w') as o:
    o.write('# corpus\tlog\tsha256\tkeys\n')
    for r in rows: o.write('\t'.join(r[:4]) + '\n')
with open(lst, 'w') as o:
    for r in sorted(rows, key=lambda r: r[1]): o.write(r[1] + '\n')
print(f'{name}: {len(rows)} logs, {sum(int(r[3]) for r in rows)} keys, corpora {" ".join(sorted({r[0] for r in rows}))}')
PY
TF="--sort=name --format=gnu --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=u=rw,go=r"
{ tar -C "$T" $TF -cf - "$NAME.tsv"; } > "$T/a.tar"
tar -C "$ROOT" $TF -h --no-recursion -rf "$T/a.tar" -T "$T/list"
gzip -n -9 < "$T/a.tar" > "$OUT"
echo "$(basename "$OUT") $(sha256sum < "$OUT" | cut -d' ' -f1) $(stat -c %s "$OUT") $NAME.tsv $(sha256sum < "$T/$NAME.tsv" | cut -d' ' -f1)"
