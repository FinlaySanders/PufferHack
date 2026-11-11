#ifndef OBJ_SAVE_H
#define OBJ_SAVE_H
#include <stddef.h>
_Static_assert(sizeof(struct obj)==96,"compact object size");
_Static_assert(sizeof(struct obj_legacy)==96,"legacy object size");
_Static_assert(offsetof(struct obj,nobj)==32 && offsetof(struct obj,oclass)==59 && offsetof(struct obj,corpsenm)==64,"export fields in bytes 32-63");
/* saved objects keep the legacy layout; copied by name (nh_fidx is not saved and restore leaves it alone) */
#define NH_OBJ_SAVED_FIELDS(X) \
    X(nobj) X(v) X(cobj) X(o_id) X(ox) X(oy) X(otyp) X(owt) X(quan) X(spe) X(oclass) X(invlet) X(oartifact) X(where) X(timed) \
    X(cursed) X(blessed) X(unpaid) X(no_charge) X(known) X(dknown) X(bknown) X(rknown) X(oeroded) X(oeroded2) X(oerodeproof) \
    X(olocked) X(obroken) X(otrapped) X(recharged) X(lamplit) X(globby) X(greased) X(nomerge) X(was_thrown) X(in_use) X(bypass) \
    X(cknown) X(lknown) X(corpsenm) X(usecount) X(oeaten) X(age) X(owornmask) X(oextra)
#define NH_OBJ_COPY_FIELD(f) out->f = in->f;
static inline void obj_to_legacy(struct obj_legacy *out,const struct obj *in) {
    memset(out,0,sizeof(*out));
    NH_OBJ_SAVED_FIELDS(NH_OBJ_COPY_FIELD)
}
static inline void obj_from_legacy(struct obj *out,const struct obj_legacy *in) {
    NH_OBJ_SAVED_FIELDS(NH_OBJ_COPY_FIELD)
}
#endif
