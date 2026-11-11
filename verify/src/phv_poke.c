/* phv_poke.so: the state-perturbation negative control (REVIEW M2). phv loads it next to the engine library it was built for
   (build.sh poke LIBDIR: against that library's own headers) and, at a planted key, changes one field of the live game after the
   step and before the digest; the game carries on with it. The verifier must then report the field's section at exactly that
   key, which a digest that stopped reading the section would not. Fields from the review's poke.c (review/poke.c), one or more
   per section; every one is a field today's state_dump.c compares. */
#include "hack.h"
#include "timeout.h"
#include "lev.h"
#include "region.h"
#include "nletypes.h"

/* the layout the poke was compiled against (phv refuses a library whose nh_ckpt_ctx_size differs) */
unsigned long phv_poke_ctx_size(void)
{
    return sizeof(struct nh_ctx);
}

/* names and the section each one changes, for certify.sh's draw */
const char *const phv_poke_table[][2] = {
    { "u", "u" }, { "attr", "attr" }, { "prop", "prop" }, { "skill", "skill" }, { "spell", "spell" }, { "inv", "inv" },
    { "invname", "inv" }, { "mon", "mon" }, { "monext", "mon" }, { "floor", "floor" }, { "buried", "buried" },
    { "migr", "migr" }, { "trap", "trap" }, { "engr", "engr" }, { "timer", "timer" }, { "light", "light" }, { "cell", "cell" },
    { "lastseen", "cell" }, { "vis", "vis" }, { "room", "room" }, { "oc", "oc" }, { "disco", "disco" }, { "ctx", "ctx" },
    { "lflags", "lflags" }, { "mv", "mv" }, { "linfo", "linfo" }, { NULL, NULL }
};

/* returns the section poked, or NULL when the game has no such record now (or the name is unknown) */
const char *phv_poke(void *nle, const char *w)
{
    struct nh_ctx *saved = nh_cur;
    const char *sec = NULL;
    nh_cur = (struct nh_ctx *) ((nle_ctx_t *) nle)->nh;
#define IS(s) (!strcmp(w, s))
    if (IS("trap") && ftrap)
        ftrap->once ^= 1, sec = "trap";
    else if (IS("engr") && nh_cur->g_engrave_c_head_engr && nh_cur->g_engrave_c_head_engr->engr_txt[0])
        nh_cur->g_engrave_c_head_engr->engr_txt[0] ^= 1, sec = "engr";
    else if (IS("timer") && nh_cur->g_timeout_c_timer_base)
        ((timer_element *) nh_cur->g_timeout_c_timer_base)->timeout += 1000, sec = "timer";
    else if (IS("light") && nh_cur->g_light_c_light_base)
        ((light_source *) nh_cur->g_light_c_light_base)->range ^= 1, sec = "light";
    else if (IS("ctx"))
        context.stethoscope_move ^= 1, sec = "ctx";
    else if (IS("lflags"))
        level.lflags.has_zoo ^= 1, sec = "lflags";
    else if (IS("mv"))
        mvitals[PM_NEWT].born ^= 1, sec = "mv";
    else if (IS("linfo"))
        level_info[2].linfo_flags ^= 0x40, sec = "linfo";
    else if (IS("floor") && fobj)
        fobj->was_thrown ^= 1, sec = "floor";
    else if (IS("inv") && invent)
        invent->lknown ^= 1, sec = "inv";
    else if (IS("invname") && invent && has_oname(invent) && ONAME(invent)[0])
        ONAME(invent)[0] ^= 1, sec = "inv";
    else if (IS("room") && nroom)
        rooms[0].rlit ^= 1, sec = "room";
    else if (IS("oc"))
        objects[ELVEN_DAGGER].oc_pre_discovered ^= 1, sec = "oc";
    else if (IS("u"))
        u.uconduct.literate += 1, sec = "u";
    else if (IS("mon") && fmon)
        fmon->mstrategy ^= 0x100, sec = "mon";
    else if (IS("monext")) {
        for (struct monst *m = fmon; m && !sec; m = m->nmon)
            if (has_edog(m))
                EDOG(m)->apport ^= 1, sec = "mon";
    } else if (IS("vis") && viz_array)
        viz_array[5][5] ^= 0x10, sec = "vis";
    else if (IS("cell"))
        level.locations[10][5].seenv ^= 0x01, sec = "cell";
    else if (IS("spell") && spl_book[0].sp_id != NO_SPELL)
        spl_book[0].sp_know += 1, sec = "spell";
    else if (IS("skill"))
        u.weapon_skills[P_DAGGER].advance += 1, sec = "skill";
    else if (IS("prop"))
        u.uprops[STEALTH].blocked ^= 0x2, sec = "prop";
    else if (IS("attr"))
        u.atime.a[A_CON] ^= 1, sec = "attr";
    else if (IS("disco"))
        nh_cur->g_artifact_c_artiexist[5] ^= 1, sec = "disco";
    else if (IS("lastseen"))
        lastseentyp[20][10] ^= 1, sec = "cell";
    else if (IS("buried") && level.buriedobjlist)
        level.buriedobjlist->spe ^= 1, sec = "buried";
    else if (IS("migr") && migrating_objs)
        migrating_objs->spe ^= 1, sec = "migr";
#undef IS
    nh_cur = saved;
    return sec;
}
