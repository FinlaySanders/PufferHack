# PufferHack

PufferHack is NetHack 3.6.6 for reinforcement learning, built for [PufferLib](https://github.com/PufferAI/PufferLib). It
plays the same game as stock [NLE](https://github.com/NetHack-LE/nle) 0.9.0 for the same seeds and keys, runs many games
per process, and steps about 12x faster than NLE as it ships (7x faster than stock run one process per game).

`make` builds `build/libpufferhack.so` and the game data with nothing but gcc and make, and `make test` runs the tests. The C
API is `include/pufferhack.h`, with a complete example in `tests/smoke.c`. PufferLib's NetHack environment builds and links
it for you.

Exactness is checked, not assumed: `verify/` replays recorded games on PufferHack and on a byte-identical rebuild of stock
NLE and compares them after every key. Every known difference from stock is listed in `DEVIATIONS.md`.

Licensed under the NetHack General Public License. Credits are in `NOTICE` and `docs/PROVENANCE.md`.
