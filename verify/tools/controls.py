#!/usr/bin/env python3
# controls.py plan OUT N MODE SEED | sweep OUT SUB NAME SEED | check OUT
# The negative controls of a certify run (REVIEW M2), planted in identical games of OUT's main run and replayed by phv:
#   fault: the low bit of one digest component flipped at one key (--fault): the comparison's plumbing, for the digest line and
#          the observations (stream, message, position/turn, done; observation buffers);
#   poke:  one field of the live engine game changed after one key, before the digest (--poke, phv_poke.so): the digest must read
#          the field's state section.
# plan: N of each in 2N distinct identical games (games of 10..20,000 keys first, then longer ones); writes OUT/controls.tsv;
#       exit 3 (VOID) when the main run has fewer than 2N identical games. sweep: poke NAME into every identical game of OUT's
#       run (tools/selfcheck.sh, once per name). check: every control reported at exactly its key and component (a poke: the state section it
#       changed, from poked.tsv); exit 0 when all are, 1 otherwise, 3 when none was planted.
import json, random, re, sys
POKES = ('u attr prop skill spell inv invname mon monext floor buried migr trap engr timer light cell lastseen vis room oc disco '
         'ctx lflags mv linfo').split()
FAULTS = [['core', 'disp', 'msg', 'xyt', 'done'], ['obs.gl', 'obs.msg', 'obs.ig', 'obs.bl', 'obs.in', 'obs.tc']]

def plan(out, n, mode, seed):
    r = random.Random(seed)
    path = {l.split('\t')[1]: l.split('\t')[0] for l in open(out + '/games.tsv')}
    ok = []
    for l in open(out + '/verdict.txt'):
        name, rest = l.rstrip('\n').split(' ', 1)
        m = re.match(r'IDENTICAL keys (\d+) .*\| STATE_IDENTICAL keys', rest)
        if m and int(m.group(1)) >= 3:
            ok.append((name, int(m.group(1))))
    short = [g for g in ok if 10 <= g[1] <= 20000]; rest = [g for g in ok if not 10 <= g[1] <= 20000]
    r.shuffle(short); r.shuffle(rest)
    pick = (short + rest)[:2 * n]
    if len(pick) < 2 * n:
        print(f'control VOID {len(ok)} identical games in the main run, {2 * n} needed for {n} faults and {n} pokes')
        sys.exit(3)
    with open(out + '/controls.tsv', 'w') as f:
        for i, (name, keys) in enumerate(pick):
            k = r.randrange(0, keys - 2)
            if i < n:
                comps = [c for c in FAULTS[i % 2] if not (mode == 'bind-all' and c == 'obs.bl')]  # bind-all ignores obs.bl
                f.write(f'fault\t{path[name]}\t{name}\t{k}\t{r.choice(comps)}\n')
            else:
                f.write(f'poke\t{path[name]}\t{name}\t{k}\t{POKES[(i - n + r.randrange(len(POKES))) % len(POKES)]}\n')

def sweep(out, sub, name, seed):
    # one poke NAME in every identical game of OUT's run, at a random key in its second half (richer state); into OUT/SUB
    r = random.Random(seed)
    path = {l.split('\t')[1]: l.split('\t')[0] for l in open(out + '/games.tsv')}
    with open(sub + '/controls.tsv', 'w') as f:
        for l in open(out + '/verdict.txt'):
            g, rest = l.rstrip('\n').split(' ', 1)
            m = re.match(r'IDENTICAL keys (\d+) .*\| STATE_IDENTICAL keys', rest)
            if m and int(m.group(1)) >= 6:
                n = int(m.group(1))
                f.write(f'poke\t{path[g]}\t{g}\t{r.randrange(n // 2, n - 2)}\t{name}\n')

def check(out):
    rows = [l.rstrip('\n').split('\t') for l in open(out + '/controls.tsv')]
    got = {l.split(' ', 1)[0]: l.rstrip('\n').split(' ', 1)[1] for l in open(out + '/controls/verdict.txt')}
    poked = {}
    try:
        for l in open(out + '/controls/poked.tsv'):
            f = l.rstrip('\n').split('\t'); poked[f[0]] = (f[3], f[4] == '1')
    except OSError:
        pass
    caught, desc, fb = 0, [], 0
    for kind, path, name, k, what in rows:
        k = int(k); r = got.get(name, '')
        if kind == 'poke':
            sec, f = poked.get(name, ('NOT_RUN', False)); fb += f
            state = r.split(' | ', 1)[1] if ' | ' in r else ''
            m = re.match(r'STATE_DIVERGE first at key (-?\d+):', state)
            ok = m and int(m.group(1)) == k and re.search(r'(^|\s)' + re.escape(sec) + '@' + str(k) + r'\b', state)
            label = f'poke {what}->{sec}@{k}' + (' (fallback)' if f else '')
        else:
            keys = [int(x) for x in re.findall(r'first at key (-?\d+)', r)]
            ok = keys and min(keys) == k and re.search(r'\b' + re.escape(what) + '@' + str(k) + r'\b', r)
            label = f'fault {what}@{k}'
        desc.append(label)
        if ok: caught += 1
        else: print(f'control MISSED {name}: {label}; got {r}')
    n = len(rows); ok = n > 0 and caught == n
    reached = sorted({f'{w}->{poked[g][0]}' for kind, p, g, k, w in rows if kind == 'poke' and g in poked and not poked[g][1]
                      and poked[g][0] not in ('NOT_REACHED', 'FAILED')})
    if reached:
        print('poke    applied without fallback: ' + ' '.join(reached))
    print(f"control {'PASS' if ok else 'VOID' if not n else 'FAIL'} {caught}/{n} planted controls reported at their key and "
          f"component ({', '.join(desc)})")
    try:
        s = json.load(open(out + '/summary.json'))
        s.update(controls_planted=n, controls_caught=caught, controls=desc, controls_poke_fallbacks=fb)
        json.dump(s, open(out + '/summary.json', 'w'), indent=1)
    except OSError:
        pass
    sys.exit(0 if ok else 3 if not n else 1)

if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'plan': plan(sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5])
    elif c == 'sweep': sweep(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    elif c == 'check': check(sys.argv[2])
    else: sys.exit(2)
