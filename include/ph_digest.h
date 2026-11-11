/* ph_digest.h: the per-key state digest of the stock differential (verify/). One definition, src/ph_digest.c: compiled into
   libnethack.so (exported ph_digest*) and, unchanged, against stock NLE 0.9.0's headers by the verifier's stock driver. */
#ifndef PH_DIGEST_H
#define PH_DIGEST_H

/* components, in record order: the per-key digest line (every key, the done key included) and the state line (the start and
   every key the game is not done after): 24 snapshot sections and 16 bound observation buffers */
enum {
    PHD_CORE, PHD_DISP, PHD_MSG, PHD_XYT, PHD_DONE, PHD_KEY,
    PHD_SEC0,
    PHD_OBS0 = PHD_SEC0 + 24,
    PHD_NCOMP = PHD_OBS0 + 16
};
#define PHD_NDIG PHD_SEC0 /* digest-line components */
#define PHD_NSEC 24
#define PHD_NOBS 16
#define PHD_UNBOUND 0x756e626f756e64ULL /* an observation buffer the caller left unbound */

/* observation buffers in canonical order and size (bytes): internal is its first 9 ints with [6] (the fork's
   engraving-underfoot channel) zeroed, misc its 3 ints */
#define PHD_OBS_SIZES                                                                                                 \
    { 21 * 79 * 2, 21 * 79, 21 * 79, 21 * 79, 27 * 8, 256, 6 * 4, 9 * 4, 55 * 2, 55 * 80, 55, 55, 24 * 80, 24 * 80, 2, \
      3 * 4 }

/* PHD_STATE: also the state line (else only the digest line); the key component is the caller's */
#define PHD_STATE 1u

#ifndef PHD_STOCK
#include "nletypes.h"
/* definition id: covers the sha256 of src/ph_digest.c and this header (from the build), so any edit to either changes it */
unsigned long long ph_digest_id(void);
const char *ph_digest_name(int comp);
/* the workspace size: a caller that zeroes this many bytes before a game and passes them with every key of that game gets the
   same digest faster (parts whose memory did not change are not recomputed) */
unsigned long ph_digest_ws_size(void);
/* out[PHD_NCOMP]; the bound buffers of obs are hashed (inv_state and inv_true_glyphs have no stock counterpart: never); ws:
   NULL or a workspace; returns PHD_NCOMP */
int ph_digest(nle_ctx_t *nle, const nle_obs *obs, unsigned mode, void *ws, unsigned long long *out);
/* the same fields as text, one record per line ("section record name=value ..."), then every bound buffer in hex rows */
int ph_digest_text(nle_ctx_t *nle, const nle_obs *obs, FILE *f);
#endif
#endif
