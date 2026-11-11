# tests

`make test` builds the library and runs these against it:

- `smoke.c`: the public API end to end, compiled with `pufferhack.h` alone (`-Wall -Wextra`).
- `badoptions.c`: options NetHack rejects play as stock does (config error, then the game) instead of crashing.
- `teardown.c`: ending a game mid-command frees it as stock would.
- `rndargs.c`: `rn2`, `rnl`, `rnd` and `d` answer a bad argument as stock's post-release build does, without a draw.
- `caches.c`: per-process caches hold what each game reads (options files rewritten in place, two data directories,
  a refused wizard mode).
- `latebind.c`: observation buffers bound after the first step show what stock would, or are refused.
- `ckpt_save.c`: saving a checkpoint leaves the saving game unchanged.
- `ckpt_history.c`: a loaded game shows the saving game's `^P` message history.
- `ckptopts.c`: a checkpoint loads only into a game started with the same options.

The tests of engine internals use the engine's own interface through `engine.h`; programs use `include/pufferhack.h`.
Exactness against stock NLE is checked by `verify/` (see its README).
