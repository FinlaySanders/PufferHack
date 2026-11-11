# Determinism: why same-seed NetHack games can differ, and how PufferHack pins them

PufferHack guarantees: **a seeded game is a pure function of its seeds and keys, on any compiler and optimization level,
and on any platform but for stock's macOS apple joke (leak 2).** Stock NLE does not have this property. This document
lists every leak found, how each was diagnosed and the fix. Read it before debugging a `verify/` mismatch: every symptom
below first looked like something else.

## The invariant and its gate

`verify/` replays recorded games on PufferHack and on untouched stock NLE 0.9.0 (gcc, the pinned reference) and compares
both random streams, the messages, the game state and every observation after every key. Stock's clock is pinned from the
game's seed (leak 1) and stock's own code does the rest: it is built with gcc (leak 3) and glibc's stable sort (leak 4).

## Leak 1: wall-clock via `ubirthday`

**What:** `u_init.c` sets `ubirthday = time(0)` at reset; NLE's seeding
never fixed it. Gameplay consumers: shopkeeper names (`shknam.c:517`,
bucketed by 257 seconds — and when the derived index overflows the name
list, an **extra `rn2()` call shifts the whole RNG stream**, a true
trajectory fork), used-item price parity (`shk.c`), scroll-label RNG
(`read.c`), `mkroom.c` anthole species, bones pool (`files.c`).

**Symptoms it produced:** deep goldens flickered ~1 frame per 100k steps
(shop greeting messages) or hard-forked; the value changed with every
rebuild ("heisenbug"), because rebuilds take minutes → a new 257 s bucket.
MSan/ASan/MallocScribble all clean (the data is initialized and
in-bounds); ASLR-stable (not address-dependent).

**Diagnosis:** interpose `time(2)` and sweep the 65 possible
`(nseed%13, nseed%5)` bucket combos — five consecutive buckets replayed a
"forked" 58k-step golden with zero mismatches, and every historical
mystery hash reappeared as some bucket.

**Fix:** `nle_birthday_maybe_fixed()` (`hacklib.c`) derives `ubirthday`
from `time_seed`; recording pins the stock clock to the same epoch.

## Leak 2: platform-conditional gameplay (`#if defined(MACOSX)`)

**What:** eating an apple prints "Delicious!  Must be a Macintosh!" on
mac builds and "Core dumped." on Unix (`eat.c`) — and the Unix branch
draws `rnd(100)` when hallucinating, so the branches are not even
RNG-equivalent: same seeds can fork across platforms.

**Symptoms:** mac-recorded goldens diverged on Linux at exactly the
apple/pear-eating frames (aa_7010 @11222, aa_7014 @7613, aa_7015 @5330…),
at every wall-clock bucket — i.e. "not time, genuinely platform".

**Fix (until 2026-11):** `MACOSX` removed from the condition; every
fast-nle build took the Unix branch. **Since 2026-11** the condition is
stock's again (`MAC || MACOSX`): the engine plays the game stock NLE
plays on the same platform, so a macOS build prints the joke without the
draw, as macOS stock does, and Linux builds take the Unix branch. Seeded
games are therefore platform-independent only where stock's are; the
certified platform is Linux.

## Leak 3: unspecified argument evaluation order

**What:** C does not define the evaluation order of function arguments,
and NetHack passes multiple RNG-drawing expressions as sibling arguments:
`mkgold(0L, somex(croom), somey(croom))` and about 165 similar sites. gcc
evaluates them in a different order than clang — two RNG draws swap, and
the dungeon differs (this was first seen as gcc -O3 against -O2/clang; gcc
13 orders them the same at -O2 and -O3). Deterministic per build,
**invisible to every sanitizer** (unspecified, not undefined, behavior —
`-fwrapv`/`-fno-strict-aliasing` etc. change nothing).

**Symptoms:** the pip-installed stock (gcc -O3 Release) generated a
different level-1 gold position than every -O2/clang build; "recorded on
Linux, fails everywhere else".

**Diagnosis:** per-step RNG-draw tracing (`NLE_RNG_TRACE` /
`NLE_TRACE_AT_STEP` in the msan-hunt branch replayer): the first
differing draw names the call site. Confirmed by hand-sequencing one site
and reproducing the other compiler's hash exactly.

**Fix:** all such sites sequence their draws into locals — grep
`fast-nle: sequence rng draws`. Until 2026-11 the canonical order was
clang's (left to right). Since 2026-11 every site evaluates in the order
gcc gives stock's unsequenced expression, because pip builds NLE from
source with the system gcc and that is the game NLE users play: call
arguments right to left, binary-operator operands left to right. Each
site was pinned to gcc 13.3's measured order (its gimple dump), and the
stock source with the same re-pinned sequencing compiles under gcc to
the same per-function call order as untouched stock in all 3,613
functions. gcc -O2 and -O3 order them identically; gcc 10.2.1 (the NLE
1.x wheels) and 13.3 play the same games. A clang build of stock now
differs from the engine at these sites. Oracle (d) keeps new sites out;
if it ever fires, use the RNG tracer to find the site.

## Leak 4: libc qsort tie order

**What:** `sort_rooms()` (`mklev.c`) sorts rooms with a comparator that
ties whenever two rooms share `lx`. qsort's tie order is
implementation-defined; macOS libc and glibc disagree, so the same seeds
generated different dungeons per platform (found at aa_7015's dlvl 2,
rooms with lx 16,16 and 56,56 — a doorway existed on one platform and not
the other). Mac libc's tie order is neither stable nor any simple total
order, so it cannot be matched; it had to be replaced.

**Fix (2026-11):** `sort_rooms()` is a stable insertion sort on stock's
`lx`-only `do_comp`, so tied rooms keep their creation order. That is the
order stock NLE gets from glibc, whose `qsort` is a stable merge sort for
arrays of at most 40 rooms (glibc 2.17, 2.28, 2.37, 2.38 and 2.39 checked
from source and on 4,709 in-game tie sorts; the unstable fallbacks need a
malloc failure or 51+ elements). Stock on glibc therefore needs no patch.
musl and macOS libc are not stable and are not matched. The earlier fix, a
geometry tiebreak (ly, hx, hy) mirrored into stock by
`tools/stock_record.patch`, differed from every glibc stock build on 1,186
of 10,512 harness games; that hunk no longer mirrors the engine.

## Practical rules

- **A `verify/` mismatch is a real regression**: the comparison has zero tolerance, deliberately. Before suspecting the
  corpus, check which class the divergence falls into: message-only flicker that realigns is a time-like leak; init or
  level differences are a generation-order leak; per-build differences are evaluation order.
- **Any new engine behaviour that differs from stock goes into `DEVIATIONS.md`** with its reach and reason, and, if the
  verifier must emulate it on the stock side, into `verify/src/phd_stock.c`.
