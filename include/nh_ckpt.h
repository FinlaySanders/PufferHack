/* checkpoint layout tables, generated into src/nh_ckpt_layout.c by tools/gen_ckpt_layout.py */
#ifndef NH_CKPT_H
#define NH_CKPT_H

struct nh_ckpt_field {
    unsigned off, size;
    const char *name;
};

struct nh_ckpt_slot {
    unsigned slot, bytes, first_ptr, nptr;
};

extern const unsigned nh_ckpt_ctx_size, nh_ckpt_nptr, nh_ckpt_nfield, nh_ckpt_nslot;
extern const unsigned nh_ckpt_ptr[], nh_ckpt_slot_ptr[];
extern const struct nh_ckpt_field nh_ckpt_field[];
extern const struct nh_ckpt_slot nh_ckpt_slot[];

#endif
