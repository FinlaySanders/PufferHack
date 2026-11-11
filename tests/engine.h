/* the engine's own interface (nle_*), for tests of internals; programs use pufferhack.h */
#ifndef PH_TESTS_ENGINE_H
#define PH_TESTS_ENGINE_H
#include <stdio.h>
#include "nletypes.h"
nle_ctx_t *nle_start(nle_obs *, FILE *, nle_settings *);
nle_ctx_t *nle_step(nle_ctx_t *, nle_obs *);
nle_ctx_t *nle_obs_refresh(nle_ctx_t *, nle_obs *);
void nle_end(nle_ctx_t *);
void *nle_ckpt_save(nle_ctx_t *, size_t *);
size_t nle_ckpt_save_into(nle_ctx_t *, void **, size_t *);
int nle_ckpt_load(nle_ctx_t *, const void *, size_t);
void nle_ckpt_free(void *);
#endif
