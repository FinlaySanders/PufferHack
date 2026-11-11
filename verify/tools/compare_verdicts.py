#!/usr/bin/env python3
# compare_verdicts.py MINE OTHER [OTHER...]: two certifications' verdict lines (verdict.txt: "corpus/name <line>"), game by game
# over the games both name; --rename A=B maps a corpus prefix of OTHER's names. Prints the counts and every game whose line
# differs; exit 0 when every common game has the same line.
import sys
args, ren = [], {}
for a in sys.argv[1:]:
    if a.startswith('--rename='): k, v = a[9:].split('=', 1); ren[k] = v
    else: args.append(a)
def load(p, rn={}):
    d = {}
    for l in open(p, errors='replace'):
        n, _, r = l.rstrip('\n').partition(' ')
        if not r: continue
        c, _, g = n.partition('/')
        d[(rn.get(c, c) + '/' + g) if g else n] = r
    return d
mine = load(args[0]); other = {}
for p in args[1:]: other.update(load(p, ren))
common = sorted(set(mine) & set(other))
diff = [n for n in common if mine[n] != other[n]]
print(f'games: mine {len(mine)}, other {len(other)}, both {len(common)}; only mine {len(set(mine) - set(other))}, '
      f'only other {len(set(other) - set(mine))}; equal verdict lines {len(common) - len(diff)}/{len(common)}')
for n in diff[:50]: print(f'DIFF {n}\n  mine:  {mine[n]}\n  other: {other[n]}')
sys.exit(1 if diff or not common else 0)
