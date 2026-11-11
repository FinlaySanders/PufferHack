# PufferHack

A fast NetHack 3.6.6 engine for reinforcement learning, built for [PufferLib](https://github.com/PufferAI/PufferLib).

PufferHack began as the NetHack Learning Environment ([NLE](https://github.com/NetHack-LE/nle)) and keeps its game: for the same
seeds and keystrokes it plays the same game as stock NLE 0.9.0 (NetHack 3.6.6) built with gcc, with one upstream fix taken from
NetHack 3.7 (`attknum = 0` in `hitum_cleave`, which 3.6.6 leaves uninitialised). What changes is the speed: many games run in one
process, the engine skips work no observer can see, and the observation export is rebuilt for batched training.

## Speed

Keys per second on 8 cores, mature games from a trained agent, 37 games per core:

| | keys/s | PufferHack is |
|---|---|---|
| PufferHack | 915,459 | |
| stock NLE 0.9.0, one process per game (the fastest way we found to run it) | 135,885 | 6.7x faster |
| stock NLE 0.9.0, as NLE runs it (a library copy per game) | 74,520 | 12.3x faster |

Medians of quiet-machine trials (PufferHack: 6 trials, 891,818-926,094; stock: 3 each), 8 P-cores, the same 888 games on every
side. Early-game play shows larger gaps (up to 24x against NLE's own setup).

## Exactness

Every engine change is checked by replaying recorded games on PufferHack and on stock NLE side by side and comparing, after
every keystroke, both random number streams, the messages, the full game state and every observation. The certified corpora are
10,320 test games in three modes (plain, checkpoint save/load every 97 keys, every observation buffer bound) and 100,015 games
played by a trained agent. Known differences from stock are listed in `DEVIATIONS.md`.

`verify/` checks any build this way against a stock reference rebuilt from NLE 0.9.0's sdist, byte-identical to the certified
one: `make verify`, then `verify/corpora/fetch.sh` and `verify/certify.sh quick build` (`verify/README.md`).

## Build

```
make            # build/libnethack.so and the game data in build/dat
make test       # plays short games through the public API, also with options NetHack rejects and one cut mid-attack
make install PREFIX=/usr/local
```

Needs gcc and GNU make only: the level and dungeon compilers' parsers are checked in, and bzip2 and fcontext are vendored.

## Use

The C API is `include/pufferhack.h` (plain C, no other header needed). Each game needs a writable copy of `build/dat` (it
writes `record`, `logfile` and save files there):

```c
ph_config cfg;
ph_config_default(&cfg);
cfg.data_dir = "/tmp/hack";                      /* a copy of build/dat */
cfg.options = "name:Agent,role:Val,!autopickup"; /* or cfg.options_file */
cfg.seeded = 1, cfg.seed_core = 1, cfg.seed_disp = 2;

ph_obs obs = {0};
long blstats[PH_BLSTATS_SIZE];
obs.blstats = blstats;                           /* bind the observations you want */

ph_game *g = ph_start(&obs, NULL, &cfg);
while (!obs.done) {
    obs.action = choose_key(&obs);
    g = ph_step(g, &obs);
}
ph_end(g);
```

Compile with `-Iinclude -Lbuild -lnethack`; `tests/smoke.c` is a complete example. Many games can run in one process and
on many threads, one game per thread at a time. The probes in the header (`ph_weight`, `ph_shop_price`, `ph_in_getpos`, ...)
read the current game without changing it. All behaviour comes from `ph_config`: the engine reads no environment variables
of its own, and the few NetHack's own code reads (`HACKOPTIONS` when no options are given, `NETHACKDIR`, `USER`,
`DEBUGFILES`) are read as stock NLE reads them.

PufferLib's NetHack environment (`./build.sh nethack`) builds and links PufferHack directly.

## Licence

NetHack General Public License (`LICENSE`). Third-party code and credits: `NOTICE`. Where the code came from: `docs/PROVENANCE.md`.
