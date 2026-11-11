#!/usr/bin/env python3
# verdict.py OUT ABORTS NGAMES [KNOWN MODE]: certify.sh's verdict step over a finished phv run in OUT (verdict.txt,
# divergences.txt, summary.json): PASS when every game is identical, aborted on both sides exactly as the reference's ABORTS list
# says, or differs with exactly the verdict line KNOWN lists for it in MODE, and no engine-side rerun failed to reproduce the main
# run. A stock-side rerun that differs from the main run (stock reading uninitialised memory, e.g. mhitm.c's "suddenly
# disappears!" buffer) is reported, not failed: those games differ anyway.
import json, os, sys
out, ab, ng = sys.argv[1], sys.argv[2], int(sys.argv[3])
kf, mode = (sys.argv[4], sys.argv[5]) if len(sys.argv) > 5 else ('', '')
v = [l.rstrip('\n').split(' ', 1) for l in open(os.path.join(out, 'verdict.txt'))]
ident = {n for n, r in v if r.startswith('IDENTICAL keys') and '| STATE_IDENTICAL keys' in r}
abort = sorted(n for n, r in v if r.endswith('| STATE_ABORTED_BOTH'))
skip = ident | set(abort)
diff = [(n, r) for n, r in v if n not in skip]
names = {n for n, r in v}
kn = {}
if kf and os.path.exists(kf):
    for l in open(kf):
        f = l.rstrip('\n').split('\t')
        if len(f) == 4 and f[0] == mode and not l.startswith('#'): kn[f[1]] = (f[2], f[3])
known = [(n, kn[n][0]) for n, r in diff if n in kn and kn[n][1] == r]
diff = [(n, r) for n, r in diff if not (n in kn and kn[n][1] == r)]
gone = sorted(n for n in kn if n in ident)
exp = sorted(l.split('\t')[0] for l in open(ab) if l.strip() and not l.startswith('#') and l.split('\t')[0] in names) if os.path.exists(ab) else []
rd = [l.split()[:3] for l in open(os.path.join(out, 'divergences.txt')) if ' RERUN_DIFFERS ' in l]
fd = [l.split()[0] for l in open(os.path.join(out, 'divergences.txt')) if ' FRESH_DIFFERS:' in l]
rd_fork = sorted(x[0] for x in rd if x[2] == 'fork'); rd_stock = sorted(x[0] for x in rd if x[2] == 'stock')
s = json.load(open(os.path.join(out, 'summary.json')))
ok = len(v) == ng and not diff and abort == exp and not rd_fork and not fd
print(f"stock   {'PASS' if ok else 'FAIL'} {len(ident)}/{ng} games identical in RNG, messages, done, state and observations, "
      f"{s['keys_played']} keys played ({s['keys_logged']} logged)" + (f"; {len(abort)} aborted on both sides at the same key"
      f"{' as expected' if abort == exp else ' AGAINST the expected list ' + str(exp)}" if abort or exp else '')
      + (f"; {len(known)} differ exactly as {os.path.basename(kf)} lists ({', '.join(c + ' ' + n for n, c in known)})" if known else '')
      + (f"; {len(diff)} differ (divergences.txt)" if diff else ''))
if gone: print(f"known   note {len(gone)} games {os.path.basename(kf)} lists as differing in {mode} mode are identical now: {' '.join(gone[:10])}")
for n, r in diff[:20]: print('  differ ' + n + ' ' + r)
if len(diff) > 20: print(f'  ... {len(diff) - 20} more in verdict.txt')
if rd_fork: print(f"rerun   FAIL {len(rd_fork)} engine-side reruns did not reproduce the back-to-back main run: {' '.join(rd_fork[:10])}")
if rd_stock: print(f"rerun   note {len(rd_stock)} stock-side reruns differ from the main run (stock is not reproducible there): {' '.join(rd_stock[:10])}")
if s.get('not_rerun'):
    print(f"rerun   capped: {s['not_rerun']} of {s['differ_by_chains']} differing games over the rerun cap of {s['rerun_cap']} were not "
          f"rerun; classes from the chains: {json.dumps(s['not_rerun_classes'])[:300]}")
if 'fresh_sample' in s:
    print(f"fresh   {'FAIL' if fd else 'PASS'} {s['fresh_sample']} identical games (seed {s['fresh_seed']}) replayed in fresh processes "
          + (f"{len(fd)} differ from the back-to-back main run: {' '.join(fd[:10])}" if fd else 'reproduce the back-to-back main run'))
# an engine exit status today's verdict line does not show: nonzero in a game both sides played identically (REVIEW m3)
frc = {}
try:
    for l in list(open(os.path.join(out, 'timing.tsv')))[1:]:
        f = l.split('\t'); frc[f[0]] = int(f[5])
except (OSError, ValueError, IndexError):
    pass
crash = sorted((n, frc[n]) for n in ident if frc.get(n))
if crash:
    print(f"crash   note {len(crash)} identical games ended the engine side with a nonzero exit status after their last key (today's "
          f"line does not show it): {' '.join(n + ' rc ' + str(r) for n, r in crash[:10])}")
for side in ('fork', 'stock'):
    x = s[side]; print(f"speed   {side:5s} {x['keys']} keys in {x['side_wall_s']:.1f} s wall = {x['keys_per_s_wall']:.0f} keys/s "
                      f"({x['core_s']:.0f} game-s summed over games, {x['keys_per_core_s']:.0f} keys/s per game-s; digest {100 * x['digest_share']:.1f}% of step+digest)")
print(f"time    {s['wall_s']:.1f} s ({s['main_s']:.1f} s both sides on {s['slots']} slots, {s['rerun_s']:.1f} s reruns of {s['rerun']} games)")
s.update(certify_pass=ok, identical_games=len(ident), aborted_both=abort, expected_aborts=exp, differ_games=[n for n, r in diff], known_differ=[n for n, c in known], known_identical_now=gone,
         rerun_differs_engine=rd_fork, rerun_differs_stock=rd_stock, fresh_differs_games=fd,
         identical_engine_exit_nonzero=crash)
json.dump(s, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
sys.exit(0 if ok else 1)
