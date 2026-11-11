# verify: PufferHack against stock NLE 0.9.0

`verify/` checks a PufferHack build against untouched stock NLE 0.9.0 (NetHack 3.6.6, plus NetHack 3.7's one-line `attknum = 0`
fix in `hitum_cleave`). It replays recorded games, given as key logs, on both engines side by side. After every key it
compares:
- both random-number streams;
- the message, the hero's position and the turn;
- whether the game is over;
- 24 sections of game state;
- every observation buffer.

A build passes only when every game is identical, or ends exactly as a pinned list says.

## One command

From a clean clone:

```
make                          # the engine: build/libpufferhack.so and build/dat
make -C verify                # phv, the verifier, and the special-mode tools (verify/build)
make -C verify stock          # the stock reference (downloads NLE 0.9.0's sdist) and the stock side of the digest
verify/corpora/fetch.sh       # the release corpora (CORPORA_URL, or CORPORA_DIR=directory with the tarballs)
verify/certify.sh quick build # about 3 minutes with --jobs 8
verify/certify.sh full build  # quick plus the reach and trained-policy corpora, about 11 minutes with --jobs 8
```

`make verify` at the top level runs the first three. You need:
- gcc, g++ (the engine's RL window port is C++) and GNU make;
- python3, the standard library only;
- for the stock reference: cmake, patch, and curl or network access from python;
- curl for `fetch.sh` downloads (not with `CORPORA_DIR`);
- sha256sum, GNU tar and gzip;
- a writable `/dev/shm`, where `phv` keeps its per-worker hack directories.

The tools are built with `-march=x86-64-v3`, as the engine is. `--jobs N` sets the number of worker processes (default: one per
hardware thread). `--out DIR` sets where results go (default `verify/runs/`).

## The stock reference

`stock/build.sh` (or `make -C verify stock`) rebuilds the reference from inputs pinned in `refs.toml` `[stock]`:

1. **The source.** NLE 0.9.0's sdist from PyPI: `sdist_url`, checked against `sdist_sha256`. 0.9.0 is the last NLE release
   on NetHack 3.6.6, and PyPI has no wheel for it. Set `NLE_SDIST=FILE` to use a local copy, for example one from
   `pip download nle==0.9.0 --no-binary :all: --no-deps`.
2. **One patch.** `stock/attknum.patch`, checked against `patch_sha256`. It is NetHack 3.7's `attknum = 0` in
   `hitum_cleave`; 3.6.6 reads the variable uninitialised.
3. **The build.** NLE's own CMake build, as `pip install nle==0.9.0` runs it: Release (`-O3 -DNDEBUG`), `USE_SEEDING=ON`,
   gcc and g++, target `nethack`. This step needs cmake; nothing else in `verify/` does.
4. **Two strings in the binary.** The certified reference was built with `__FILE__` mapped to `file_prefix` and with the
   build stamp of `build_epoch`. Both are strings inside the library, not paths on this machine. Neither changes play:
   `__FILE__` is only read by debug tracing, which NLE never enables.

`build.sh` checks three results and notes a fourth:
- `date.h` against `date_h_sha256`;
- the game data against `nhdat_sha256`;
- the library against `library_sha256`;
- it notes when the compiler is not `compiler`.

With the pinned compiler, gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), the library is byte-identical to the reference every
earlier certification used: sha256 f0b4a412a8a3aa2f…. It installs to `stock/build/ref/` (`libpufferhack.so`, `nethackdir/`).

With another compiler, `build.sh` reports `STOCK_DIFFERS` and still installs what it built. `certify.sh` then refuses to run
unless you pass `--ref-any`, and its summary says the reference is not the pinned one. Earlier checks found stock's games
stable across builds:
- gcc -O2 and -O3 agree on all 10,512 ci and cov7 games;
- the NLE 1.3.0 wheel (gcc 10.2.1) agrees with gcc 13.3 on all 10,312 ci games.

Games where stock reads freed or uninitialised memory can still depend on the build. `known.tsv` D4 and the "suddenly
disappears!" games of the trainer corpus are examples. With a different compiler, a difference in such a game may come from
stock itself.

`stock/aborts.tsv` lists the games on which the reference itself panics or crashes. A game on that list must end the same way
on the engine: equal on every line stock wrote before it aborted.

## The corpora

| corpus | games | keys | where | what |
|---|---|---|---|---|
| `regress` | 30 | 10,315 | in the repository | logs of fixed engine bugs and their controls: AUTOCOMPLETE lines, the fire trap's mhpmax, goldX, hidden gold in containers, the status_updates variant of the levitation trap game |
| `regress_bindall` | 1 | 1,802 | in the repository | `m_bare_35_fz`: bind-all mode only (see "What PASS does not cover") |
| `regress_urandom` | 6 | 230 | in the repository | the seeding logs (reseed=1 games, their reseed=0 controls, the set_seed log); reseed=1 needs /dev/urandom pinned on both sides |
| `regress_setseed`, `regress_pre`, `regress_ckpt`, `regress_probe` | 1, 3, 3, 1 | | in the repository | the special modes below |
| `ci` | 10,312 | 22,255,989 | release asset `pufferhack-corpus-ci.tar.gz` (10 MB) | the stock-diff harness's 11 test corpora: scripted coverage, scenarios, fuzzing, wizard-mode fuzzing, champion-policy games |
| `keys_excluded` | 8 | 30,844 | same asset | 8 games kept as a corpus of their own; `ci+excluded`, all 10,320, is what the stock-diff harness's ci.sh ran |
| `keys_reach`, `keys_reach_r1` | 3,222 + 2,778 | 10,041,217 | `pufferhack-corpus-reach.tar.gz` (7 MB) | the reach audit's generated games: quests, Sokoban, Minetown, the Planes, artifacts, shops |
| `keys_trained` | 1,995 | 17,659,805 | `pufferhack-corpus-trained.tar.gz` (7 MB) | games of a trained agent (champion policy, 1B steps), recorded through PufferLib's NetHack env |

**How each log is checked.**
- `corpora/regress.tsv` and each release manifest (`ci.tsv`, `reach.tsv`, `trained.tsv`) give every log's sha256.
- `corpora/assets.tsv` pins each tarball's sha256 and its manifest's sha256.
- `fetch.sh` checks all three levels before it keeps anything.
- `certify.sh` checks the manifest and every log again on every run.

**Fetching.** `CORPORA_URL` is a placeholder in `fetch.sh` until the tarballs are published as release assets. Until then,
set `CORPORA_DIR` to a directory that holds them. `pack.sh` builds such a tarball from a manifest. Its output is
reproducible: sorted entries, mtime 0, root-owned files, `gzip -n`.

**The trained corpus.** Its games end the way the 100K-game trainer corpus's do: score distribution KS p = 0.36, death
classes within a percentage point. The 100K corpus itself (3.7 GB of logs) is not packaged. It is too large for a tier.

## Tiers

`certify.sh quick LIBDIR` and `certify.sh full LIBDIR` run one certification per row below, into `OUT/NAME`. `OUT/summary.txt`
has one line per run and the tier's result. The tier passes when every run gives its expected result.

| run | tier | what | expected |
|---|---|---|---|
| `regress` | quick | `regress`, default mode, one engine process (`--fork-jobs 1`: a flag leaking from one game into the next shows) | PASS |
| `regress_bindall` | quick | `regress` + `regress_bindall`, bind-all mode, one engine process | PASS |
| `ci`, `ci_bindall` | quick | `ci+excluded`, both modes | PASS |
| `urandom`, `urandom_bindall` | quick | `regress_urandom` with `build/urandom_fixed.so` preloaded: /dev/urandom reads as a fixed seed on both sides | PASS |
| `badoptions`, `badoptions_bindall` | quick | `regress_pre` with `phv_pre`: options NetHack rejects, each with a wait before any hero exists | UNCONTROLLED (see below) |
| `setseed` | quick | `tools/setseed.sh`: `nle_set_seed` before key 3 on both sides, 4 seed tuples × 2 modes (`phv_seed`) | PASS |
| `seedapi` | quick | `tools/seedapi.sh`: the seeding API with several games in one process (engine only, 14 checks) | PASS |
| `reach`, `reach_bindall` | full | `keys_reach` + `keys_reach_r1`, both modes | PASS |
| `trained` | full | `keys_trained`, default mode | PASS |

**Single runs.** `certify.sh --corpus SPEC --lib LIBDIR [--bind-all] ...` runs one certification. SPEC is one of:
- `ci` or `ci+excluded`;
- corpus names separated by commas;
- a manifest;
- a list of key logs.

**The two modes.**
- **Default mode** replays each game with the observation buffers its log recorded as bound, as the trainer binds them.
- **Bind-all mode** binds every buffer on both sides, and ignores `obs.bl` (see below).

**Controls.** Every `certify.sh` run plants negative controls in identical games, then requires each one to be reported at
exactly its key and component:
- `--controls N` faults in the comparison;
- N real state changes poked into the live engine game (`phv_poke.so`, built for the library under test).

`--controls 0` gives UNCONTROLLED, never PASS.

The badoptions runs are UNCONTROLLED. Their three games spend their first keys before the hero exists, where `phv_pre`
digests no state, so a control planted there would go unreported and void the run by chance. They use the same comparator as
the other runs. The `setseed` and `seedapi` runs are not `certify.sh` runs and plant no controls: `setseed` compares with
`phv cmpkey`, the comparator of the reruns, and `seedapi` checks the engine alone.

## Reading a certificate

Each run directory holds:
- `summary.txt`, the certificate;
- `verdict.txt`, one line per game;
- `divergences.txt`, the first differing field of each game that is not identical;
- `summary.json` and `timing.tsv`, machine-readable;
- `controls/`, the planted controls' own run.

The lines of `summary.txt`:
- `corpus`: games and logged keys, and where the logs came from. Every log was checked against its sha256, unless the line
  says `--no-hash`.
- `engine`, `stock`: the libraries and their sha256. `stock` says whether the reference is the pinned build.
- `harness`, `env`: the tools' sha256, the mode, and environment variables that could change the engine's code path.
- `pins`: present only when `--refs`, `--known` or `--aborts` named a file other than this checkout's. The pins and
  expected outcomes then come from that file, not the repository, and a run that would pass reports `UNPINNED` (exit 5)
  instead: it is not a certificate. `--ref-any` does the same.
- `digest`: both sides compute the digest with one definition, the pinned one.
  - The definition is `src/ph_digest.c`. The engine compiles it in; `phd_stock.so` compiles it against stock's headers.
  - Its id covers the source, and `refs.toml` `[digest]` pins it.
  - An edit that changes what the digest reads changes the id, and certify.sh refuses the run.
- `stock`: the verdict: identical games, aborted-both games (`stock/aborts.tsv`), known differences (`known.tsv`), and any
  other game that differs, listed.
- `fresh`: a sample of identical games was replayed in fresh processes. It must reproduce the back-to-back run, which shows
  that no state leaks from one game into the next.
- `rerun`: present only when needed.
  - `rerun FAIL`: an engine-side rerun did not reproduce the main run.
  - `rerun note`: stock is not reproducible there (it reads uninitialised memory).
- `speed`, `time`: throughput per side. Informational only, never certified.
- `poke`, `control`: the negative controls and whether each was caught.
- `result`: PASS, FAIL or UNCONTROLLED (exit 0, 1 or 4). A refused run (corpus, reference or digest not as pinned) ends at
  its FAIL line and this one.

### Verdict lines

A `verdict.txt` line has the form `NAME RESULT | STATE_RESULT`.

**Identical:**

    keys_both/g000000_696088f7aeb287f2 IDENTICAL keys 1042 (fork 1043 lines, stock 1043, log 1042 keys) | STATE_IDENTICAL keys 1043

**A divergence:**

    keys_reach/l_minetn_00_fz2 DIVERGE first at key 403 of 3033: msg@403 (...) | STATE_DIVERGE first at key 403: obs.msg@403

This line reads:
- the first key where the two engines differ is 403;
- the differing components are listed with the key where each first differs: `core` and `disp` are the random streams,
  `msg`, `xyt`, `done`;
- after the `|`, the state sections (`u`, `inv`, `mon`, `cell`, ...) and observation buffers (`obs.gl`, `obs.bl`, ...) that
  differ;
- `divergences.txt` then names the first differing record and field in each section.

**A stock abort:** `ABORTED_BOTH keys K (stock rc R, fork rc F) | STATE_ABORTED_BOTH`. Stock aborted after completing K keys, and the
engine equalled it on every line stock wrote.

## What PASS means

For every key of every game in the corpus, the two engines agree on all 46 digest components:
- both random streams, whole: position and both ISAAC64 tables;
- the message;
- position and turn;
- done;
- the 24 state sections, pointer-free and packed at full field width;
- every observation buffer bound in that mode.

The exceptions are pinned: games the reference aborts on, and games `known.tsv` lists. A listed game passes only when its
verdict line is exactly the listed one, so a known difference that changes shape fails the run. Every planted control was
caught, and every rerun reproduced the main run.

## What PASS does not cover

- **Other inputs.** Games outside the corpora: PASS is evidence about these games, not a proof about all games.
- **Checkpoints and probes.** The stock-diff harness's third mode (`PROBES=1 CKPT_EVERY=97`) has no fast equivalent here. It
  transplants a checkpoint every 97 keys and calls every probe after every key, and it still needs that harness (see the
  special modes below). The trainer calls probes on every step.
- **status_updates in default mode.** Stock fills blstats only through its status pipeline, so default mode deletes
  `!status_updates` from stock's options: it compares a `!status_updates` log on the engine against stock with status on. A
  game whose play depends on that option differs there by construction.
  - `m_bare_35_fz` is one: stuck in a bear trap while levitating, then wriggling free. Stock NLE 0.9.0's own two settings
    play it differently.
  - Such games are certified in bind-all mode, where both sides keep the log's options and `obs.bl` is not compared.
  - `known.tsv` lists `m_bare_35_fz`'s default-mode line as D5.
- **Undefined behaviour in stock.** Where stock reads freed or uninitialised memory, its game can depend on its own heap
  layout, its build or its bindings. The known cases are pinned (`known.tsv`):
  - **D4**: `keys_reach_r1/x_invoke_3_fz`, default mode only. A zap at the high priestess with a wand that Moloch's lightning
    destroys during the zap: `learnwand` reads the freed wand.
  - **D6**: `l_minetn_00_fz2` in both reach corpora and both modes. `impossible("shopkeeper career change")` prints `mnum` read
    from a freed monster, so only the message differs; the state is identical.
- **Other differences from stock.** Deliberate differences of the engine's observations are emulated on the stock side and
  compared: the glyph handling in `src/phd_stock.c`. The engraving-underfoot channel (`internal[6]`) has no stock counterpart
  and is not compared.
- **State the digest does not hash.** The per-key digest covers 24 state sections, both random streams, the messages and every
  bound buffer, but not the #overview records' feature counts (the audit's gap 6). A difference there shows only when the
  overview is displayed: the `overview` regression game checks it through `tty_chars` in bind-all mode.
- **The stock side's environment.** The stock side runs with its clock pinned (`time` and `localtime`, from each game's seed)
  and a 256 KiB coroutine stack in place of stock's 64 KiB, which deep call chains overflow. The stock-diff harness ran it
  the same way.
- **Speed.** It is reported, not certified.

## Special-mode regression tests

Some of the regression logs need more than a plain replay. Each needs the following.

| logs | needs | wired |
|---|---|---|
| `seeds/f1_*` (5) and `seeds/f2_setseed` | /dev/urandom pinned on both sides: with reseed=1, stock reseeds every new level from `sys_random_seed()` | yes: `urandom`, `urandom_bindall` (`--urandom-fixed`) |
| `seeds/f2_setseed` with `nle_set_seed` | the call before key 3 on both sides (`phv_seed` = `phv.c` + `src/phv_seed.patch`, `SETSEED=K,core,disp,reseed,lgen`) | yes: `setseed` |
| the seeding API | several games in one process, the API from other threads (`src/seedapi.c`) | yes: `seedapi` |
| `badoptions/*` (3) | a digest before the hero exists. The state digest reads `youmonst.data` (via ACURR) and would crash both sides. `phv_pre` (`src/phv_pre.patch`) digests the digest line and the raw observation buffers until `in_normal_game`, and names the options file by its content, so the error text, which names the file, matches | yes: `badoptions`, `badoptions_bindall` (UNCONTROLLED) |
| `ckptrender/*` (3) | checkpoint mode: a checkpoint transplant every 97 keys (stock-diff `sjob.sh` with `CKPT_EVERY=97`). The `ckpt_bindall_*` logs also need `REPLAY_BIND_ALL=1 STATE_IGNORE=obs.bl`; `ckpt_probes_*` also needs `PROBES=1 STOCK_STATUS_UPDATES=1`. Expected: IDENTICAL \| STATE_IDENTICAL | no: needs the stock-diff harness |
| `weightprobe/x_drain_2_fz2` | probe mode: every probe after every key (stock-diff `fork_replay_env` with `PROBES=1`, also `PROBE_MASK=0x8000` for `nle_weight` alone), against stock with status_updates, `obs.bl` compared. Expected: IDENTICAL \| STATE_IDENTICAL. The fix's wider check compared `struct nh_ctx` before and after every `nle_weight` call over 63 reach and ci logs. | no: needs the stock-diff harness |

`corpora/regress/README.md` describes each group, its fix and how it failed before.

## Files

- `certify.sh` runs the tiers and single certifications.
- `src/phv.c` is the verifier. It runs both sides, compares and reruns.
- `src/phd_stock.c` is the stock side of the digest, with the emulation of the engine's deliberate observation differences.
- `src/phv_poke.c` is the state-perturbation control.
- `src/keylog.h` holds key-log parsing and the per-worker hack directory.
- `src/appearance_canon.h` is used by `phd_stock.c`.
- `src/phv_seed.patch`, `src/phv_pre.patch`, `src/urandom_fixed.c` and `src/seedapi.c` serve the special modes.
- `tools/verdict.py` is the verdict step.
- `tools/controls.py` plans and checks the controls.
- `tools/setseed.sh` and `tools/seedapi.sh` are the special runs.
- `tools/compare_verdicts.py` compares two runs' verdict lines game by game.
- `refs.toml` holds the pins: the sdist, the patch, the compiler, the `__FILE__` prefix and build stamp, the reference library
  and its data, and the digest and stock adapter ids.
- `stock/build.sh` builds the reference; `stock/attknum.patch` is its patch; `stock/aborts.tsv` lists the reference's aborts.
- `known.tsv` lists the known differences: mode, game, class, and the exact verdict line.
- `corpora/` holds `regress/` and `regress.tsv` (in the repository), `assets.tsv`, `fetch.sh` and `pack.sh`.

## Changing the digest

The digest id covers `src/ph_digest.c` and `include/ph_digest.h`, and certify.sh refuses an id other than the one pinned in
`refs.toml`.

Re-pinning is a review step, not a build step. A new definition is pinned only after three coverage checks pass on it:
1. Static field parity against the stock-diff harness's `state_dump.c`.
2. Partition parity on 82 games, both sides: no two keys that `state_dump.c` tells apart may hash equal.
3. A poke sweep: every poke in every identical game, with and without the digest workspace.

Those checks need the stock-diff harness. They are not part of this directory.
