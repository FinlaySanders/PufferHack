# Regression logs

Key logs found while auditing the engine against stock NLE 0.9.0. Most reproduce a bug that has since been fixed: before its
fix the log differed from stock or crashed, after it the log is identical to stock. The rest are their controls, identical
before and after: the `_noline` twins, the reseed=0 seeds logs and `m_bare_35_fz_su`. `../regress.tsv` assigns each log to
a corpus and gives its sha256 and key count. `../../README.md` lists which corpora run in which tier and mode.

| group | logs | before the fix | the fix |
|---|---|---|---|
| `autocomplete` | `ac_test` (the audit's repro), `ac_eff`, `ac_last`, `ac_menu`, each with an `_noline` twin | an options file with an AUTOCOMPLETE line segfaulted the engine in `initoptions`, before key 0 (exit 139) | AUTOCOMPLETE lines flip bits in a per-game bitmap over the read-only command table. The `_noline` twins are the same games without the lines; played back to back in one engine process (`--fork-jobs 1`), they show any flag that leaks into the next game |
| `firetrap` | `fire1_*` (6 vampire revivals), `ls2_*` (5 life-saved kobolds), `g060292_*` (a trainer game) | `mon` differs in mhpmax: the engine clamped a fire trap's mhpmax loss at mhp | mintrap lowers mhpmax by `rn2(num + 1)` again, as stock does, including down to 0 or below |
| `goldx` | `goldX_on` (rc file), `goldX_optstr` (options string), `opthelp` (the `?` options list) | DIVERGE at key 4: the engine ignored goldX; `opthelp` differed in `obs.tc` from key 2 | goldX's option entry points at the per-game flag |
| `hiddengold` | `hg2`, `hg3`, `hg5`, `hg5a`, `hg5n`, `hg6`, `hg6s` | `obs.bl` (default mode) or `obs.tc` (bind-all) differed at the key where gold merged into gold in a carried container: the cached hidden-gold total was stale | the container's merge branch marks the container dirty |
| `statusupdates` | `m_bare_35_fz` (rc `!status_updates`), `m_bare_35_fz_su` (the same log with the option on) | bind-all: DIVERGE at key 1688 (turn 513 against stock's 512). A hero whose levitation a bear trap blocks wriggles free, and the status pump's capacity refresh unblocked levitation before the move did, so stock's "You start to float in the air!" never came | the RL port's status pumps read capacity without touching game state. In default mode `m_bare_35_fz` differs by construction (`../../README.md`), so it runs in bind-all mode only |
| `seeds` | `f1_reseed*` (reseed=1, revisited levels, a samurai), `f1_reseed0*` (the same with reseed=0), `f2_setseed` | DIVERGE at key 10: the engine restarted both random streams from the initial seeds at every new level, where NLE 0.9.0 takes `sys_random_seed()`. `nle_set_seed` before key 3 diverged at key 5 | the initial seeds are in force only until `nle_start` returns, `nle_set_seed` and `nle_get_seed` act on the game passed in, and `lgen` is a start-time choice. The reseed=1 games need /dev/urandom pinned on both sides |
| `badoptions` | `badopt_rc_unknown`, `badopt_inline_unknown`, `badopt_rc_numpad` | segfault in `nle_start` (DIVERGE `empty@-1`) when NetHack rejects an option and waits for a key before the window port exists | `rl_nhgetch` without a port fills the observation as stock's not-started port does |
| `ckptrender` | `ckpt_bindall_cov7_s370_0000`, `ckpt_bindall_cov7_s373_0034_status_on`, `ckpt_probes_reach_l_air_00` | checkpoint mode: after a load the terminal, map and status buffers showed the target game's screen (STATE_DIVERGE at key 100 or 99 in `obs.ch co sp tc tk tu`); on the Plane of Air a load turned bubbles around (`cell`/`vis` at 212, `core` at 277) | checkpoint version 14 carries the display, and a checkpoint load skips `mv_bubble` |
| `weightprobe` | `x_drain_2_fz2` | probe mode: `nle_weight` set `context.botl` through `float_vs_flight`, so blstats refreshed a key early (`obs.bl` at key 2246: HP 0/-6 against stock's 24/24) | `nle_weight` puts the flags it touches back |

The `ckptrender` and `weightprobe` logs need the stock-diff harness's checkpoint and probe modes, so no tier runs them yet
(`../../README.md`).
