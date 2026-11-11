#ifndef PUFFERHACK_H
#define PUFFERHACK_H

/* PufferHack: NetHack 3.6.6 as stock NLE 0.9.0 plays it, many games per process. Plain C types only. */

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PH_ROWS 21
#define PH_COLS 79
#define PH_MESSAGE_SIZE 256
#define PH_BLSTATS_SIZE 27
#define PH_PROGRAM_STATE_SIZE 6
#define PH_INTERNAL_SIZE 11
#define PH_MISC_SIZE 3
#define PH_INVENTORY_SIZE 55
#define PH_INV_STATE_FIELDS 8
#define PH_INVENTORY_STR_LENGTH 80
#define PH_SCREEN_DESCRIPTION_LENGTH 80
#define PH_TERM_CO 80
#define PH_TERM_LI 24
#define PH_BL_X 0
#define PH_BL_Y 1
#define PH_BL_STR25 2
#define PH_BL_STR125 3
#define PH_BL_DEX 4
#define PH_BL_CON 5
#define PH_BL_INT 6
#define PH_BL_WIS 7
#define PH_BL_CHA 8
#define PH_BL_SCORE 9
#define PH_BL_HP 10
#define PH_BL_HPMAX 11
#define PH_BL_DEPTH 12
#define PH_BL_GOLD 13
#define PH_BL_ENE 14
#define PH_BL_ENEMAX 15
#define PH_BL_AC 16
#define PH_BL_HD 17
#define PH_BL_XP 18
#define PH_BL_EXP 19
#define PH_BL_TIME 20
#define PH_BL_HUNGER 21
#define PH_BL_CAP 22
#define PH_BL_DNUM 23
#define PH_BL_DLEVEL 24
#define PH_BL_CONDITION 25
#define PH_BL_ALIGN 26

typedef struct ph_game ph_game;

/* the observation buffers a caller binds (NULL: not exported); same layout as the engine's nle_obs */
typedef struct ph_obs {
    int action;
    int done;
    char in_normal_game;
    int how_done;
    short *glyphs;                      /* PH_ROWS * PH_COLS */
    unsigned char *chars;               /* PH_ROWS * PH_COLS */
    unsigned char *colors;              /* PH_ROWS * PH_COLS */
    unsigned char *specials;            /* PH_ROWS * PH_COLS */
    long *blstats;                      /* PH_BLSTATS_SIZE, indices PH_BL_* */
    unsigned char *message;             /* PH_MESSAGE_SIZE */
    int *prog_state;                    /* PH_PROGRAM_STATE_SIZE */
    int *internal;                      /* PH_INTERNAL_SIZE */
    short *inv_glyphs;                  /* PH_INVENTORY_SIZE */
    unsigned char *inv_strs;            /* PH_INVENTORY_SIZE * PH_INVENTORY_STR_LENGTH */
    unsigned char *inv_letters;         /* PH_INVENTORY_SIZE */
    unsigned char *inv_oclasses;        /* PH_INVENTORY_SIZE */
    unsigned char *screen_descriptions; /* PH_ROWS * PH_COLS * PH_SCREEN_DESCRIPTION_LENGTH */
    unsigned char *tty_chars;           /* PH_TERM_LI * PH_TERM_CO */
    signed char *tty_colors;            /* PH_TERM_LI * PH_TERM_CO */
    unsigned char *tty_cursor;          /* 2 */
    int *misc;                          /* PH_MISC_SIZE */
    signed char *inv_state;             /* PH_INVENTORY_SIZE * PH_INV_STATE_FIELDS */
    short *inv_true_glyphs;             /* PH_INVENTORY_SIZE */
    char partial;                       /* in-game fills export only program_state, misc and message */
} ph_obs;

typedef struct ph_config {
    const char *data_dir;       /* game data, only read; many games can share it (default: this build's build/dat) */
    const char *var_dir;        /* writable files (record, logfile, xlogfile, saves, bones): NULL gives each game a
                                   private directory removed by ph_end; set it to keep the record across games */
    const char *options;        /* NetHack options, "name:Agent,role:Val,..." (NETHACKOPTIONS format); default: NLE's
                                   options and character (name:Agent-mon-hum-neu-mal) */
    const char *options_file;   /* or a NetHack options file; used when options is NULL */
    const char *wizkit;         /* wizard-mode starting items, or NULL */
    int seeded;                 /* seeds below are used */
    unsigned long seed_core, seed_disp;
    int reseed;                 /* NetHack reseeds from the system after the first level, as stock NLE does */
    int seeded_clock;           /* moon phase, Friday 13th, night and the birthday come from clock_seed, not the time */
    unsigned long clock_seed;
    int spawn_monsters;         /* random monster generation (stock: on) */
    int underfoot_glyphs;       /* the hero's map cell shows the top object underneath */
    int true_glyphs;            /* identified object types show their true glyphs on the map */
    int skip_gamelogs;          /* no logfile or xlogfile appends */
} ph_config;

void ph_config_default(ph_config *cfg); /* everything a game needs; then set what you want to change */

ph_game *ph_start(ph_obs *obs, FILE *ttyrec, const ph_config *cfg); /* NULL on a bad config */
ph_game *ph_step(ph_game *game, ph_obs *obs);
ph_game *ph_refresh(ph_game *game, ph_obs *obs);
void ph_end(ph_game *game);

/* 0 at a normal prompt; 1 getpos needs a position ('@' then '.' picks the hero's square); 2 ESC aborts it */
int ph_in_getpos(ph_game *game);

void ph_set_seed(ph_game *game, unsigned long core, unsigned long disp, int reseed, unsigned long lgen);
void ph_get_seed(ph_game *game, unsigned long *core, unsigned long *disp, int *reseed, unsigned long *lgen, int *use_lgen);

/* checkpoints: save at a command prompt, load into a started game with the same options */
void *ph_ckpt_save(ph_game *game, size_t *len);
size_t ph_ckpt_save_into(ph_game *game, void **buf, size_t *cap);
int ph_ckpt_load(ph_game *game, const void *buf, size_t len);
void ph_ckpt_free(void *buf);
int ph_ckpt_diff(const void *a, size_t alen, const void *b, size_t blen);

/* probes: read-only queries of the current game for an agent's observation */
long ph_shop_price(ph_game *game);
int ph_inside_shop(ph_game *game);
int ph_terrain_underfoot(ph_game *game);
int ph_container_at(ph_game *game);
int ph_food_underfoot(ph_game *game);
int ph_discoveries(ph_game *game);
int ph_peaceful_at(ph_game *game, int x, int y);
int ph_lnc_bits(ph_game *game);
void ph_weight(ph_game *game, int *carried, int *capacity);
int ph_spells(ph_game *game, short *ids, signed char *levels, signed char *fails, int *known, int cap);
int ph_cast_blocked(ph_game *game);
int ph_intrinsics(ph_game *game);
void ph_identity(ph_game *game, int *role, int *race, int *gender, int *alignment);
int ph_path_drain(ph_game *game, short *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
