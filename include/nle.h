#ifndef NLE_H
#define NLE_H

#define NLE_RL_STATUS_SIG 26 /* the status pipeline's previous evaluation, kept by the rl port (and checkpoints) */


#include <stdio.h>

#include <fcontext/fcontext.h>

#include "nletypes.h"

/* the rl port's inventory caches -- update_inventory's last result -- kept by checkpoints, so a load needs no resync (which
   would run obj_to_glyph's display-RNG draws and doname's flag writes at a moment the game never did) */
struct nle_rl_inv_ckpt {
    int n;
    short glyph[NLE_INVENTORY_SIZE];
    unsigned char letter[NLE_INVENTORY_SIZE], oclass[NLE_INVENTORY_SIZE];
    char str[NLE_INVENTORY_SIZE][NLE_INVENTORY_STR_LENGTH];
};

/* TODO: Fix this. */
#undef SIG_RET_TYPE
#define SIG_RET_TYPE void (*)(int)

/*
 * Would like to annotate this with __thread, but that causes
 * the MacOS dynamic linker to not unload the library on dlclose().
 */
extern NH_THREAD_LOCAL nle_ctx_t *current_nle_ctx; /* defined in nle.c;
    NH_THREAD_LOCAL comes from nh_ctx_gen.h (via hack.h) */

/* fast-nle: per-env settings (field on nle_ctx_t). Consumers that used the
 * old process global just work; the ctx pointer is anchored at every API
 * entry before any settings access. */
#define settings (current_nle_ctx->settings)
#define configfile (current_nle_ctx->configfile) /* files.c, options.c: per game */
#define wiz_error_flag (current_nle_ctx->wiz_error_flag) /* unixmain.c: per game */

nle_ctx_t *nle_start(nle_obs *, FILE *, nle_settings *);
nle_ctx_t *nle_step(nle_ctx_t *, nle_obs *);
nle_ctx_t *nle_obs_refresh(nle_ctx_t *, nle_obs *);
/* Checkpoints (src/nle_ckpt.c): save at the command prompt (NULL elsewhere); load into any env
 * whose game is started and waiting at its prompt, built from the same layout and options. A
 * blob is position independent: it loads in another env, process or machine. */
void *nle_ckpt_save(nle_ctx_t *, size_t *);
size_t nle_ckpt_save_into(nle_ctx_t *, void **, size_t *);
int nle_ckpt_load(nle_ctx_t *, const void *, size_t);
void nle_ckpt_free(void *);
int nle_ckpt_diff(const void *, size_t, const void *, size_t);
void nle_end(nle_ctx_t *);

#endif /* NLE_H */
