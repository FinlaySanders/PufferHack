# Checkpoints

`nle_ckpt_save` / `nle_ckpt_load` (src/nle_ckpt.c) capture a game at the command prompt and
restore it into any env, in this process or another. Continuing a loaded game is bit-identical to
continuing the original (same RNG, same actions -> same observations, rewards and masks).

## What a checkpoint holds

1. **The context struct** (`struct nh_ctx`, every per-game global), byte for byte.
2. **The lazy slots that hold game state** (objects table, display buffer, shop/apply/kick
   state, ...), byte for byte. Options, files, terminal and constant tables are not copied.
3. **The heap**, serialized by NetHack's own code: current level, game state, and the other
   levels' in-memory files.
4. **List order and engraving fixups** (see below), the env clock settings, the window port's
   status cache (`!status_updates` refreshes it only when the status pipeline fires), and the
   env's high-score file, which past-player statues and corpses are drawn from (`mk_tt_object`).

Pointers in (1) and (2) are stored by kind: into the struct or a copied slot (offset), into
the library (offset from its base), or into the heap (dropped; NetHack's restore rebuilds them).
That makes a blob position independent.

## Loading

The target env must be running a started game, waiting at its prompt, with the same options.
`nle_ckpt_load`:

1. frees the target's heap (NetHack's save code in free mode);
2. lays the struct image over the target's struct, relocating pointers; options, files and
   window-port state stay the target's own (`keep_fields`, `keep_whole_*`);
3. rebuilds the heap with NetHack's restore (`getlev`, `restgamestate`);
4. puts every non-pointer byte back as saved: NetHack's restore recomputes some state (vision and
   display flags, `unweapon`, the message line) that the original game had not;
5. relinks lists into their saved order (restore prepends to light sources, engravings, traps),
   puts back engraving times and text offsets (`wipe_engr_at` advances `engr_txt`), relinks the
   `context` object/monster pointers by id and makemon's special-level cache by level, clears
   dangling shop residents, and drops caches that hold heap pointers. Sizes of arrays the restore
   allocates itself (`restored_names`) keep the restored value.

`nle_ckpt_save_into` writes into a buffer the caller keeps across saves. Steady state on a
P-core: save ~18 us, load ~61 us, ~300 KB per checkpoint (first calls are slower: page faults).

## Layout tables

`src/nh_ckpt_layout.c` (pointer words, fields, copied slots) is generated from a `-g` build:

    NH_SRC=. NH_ONAMES=<build>/include/onames.h \
        gdb -batch -ex 'set max-value-size unlimited' -ex 'source util/gen_ckpt_layout.py' <build>/libpufferhack.so

It static-asserts every field's offset and size, so a struct change without regeneration fails
the build. A new lazy slot needs a policy: copy (add it to `SLOTS` in the generator) or keep.

## Checking

`nle_ckpt_diff` lists the struct fields and slots whose saved bytes differ between two
checkpoints; a load followed by a save should differ only in kept state and invalidated caches.
The checkpoint gate (pufferlib experiments, `ckpt_gate`) replays recorded RL actions, saves at
step k, and checks three runs for n steps: the original, the same game replayed without saving
(saving is side-effect free), and a different env that loaded the checkpoint (loading is exact),
in-process and across processes.
