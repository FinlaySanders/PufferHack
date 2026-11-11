# Provenance

- **NetHack 3.6.6** (NetHack DevTeam, 2020): the game. `src/`, `include/`, `dat/`, `util/`, `win/tty/`, `doc/`.
- **NLE** (NetHack Learning Environment, Facebook AI Research, now NetHack-LE): the RL window port `win/rl/`, `src/nle.c`,
  `src/nlernd.c` and the observation layout. PufferHack's game matches NLE 0.9.0, the last NLE release on NetHack 3.6.6.
- **fast-nle** (github.com/FinlaySanders/fast-nle): many games per process, the per-game context, checkpoints, the allocation
  pool and the engine speed work. PufferHack starts as a single commit; its
  development history is the `pufferhack-v0` branch of github.com/FinlaySanders/fast-nle (the engine speed work is on
  that repository's `lean-engine` branch).
- **NetHack 3.7**: one fix, `attknum = 0` in `hitum_cleave` (3.6.6 reads it uninitialised).
- **Vendored** (in `src/third_party`): fcontext from deboost.context (MIT), the coroutine switch each game runs on, and
  libtmt (BSD-style), the terminal emulator behind `tty_chars`. The parsers in `util/*_yacc.c`, `util/*_lex.c` were
  generated with bison 3.8.2 and flex 2.6.4 from `util/lev_comp.y`, `util/lev_comp.l`, `util/dgn_comp.y`, `util/dgn_comp.l`.
