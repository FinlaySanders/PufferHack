#ifdef __SSE2__
#include <emmintrin.h>
#ifdef __SSE4_1__
#include <smmintrin.h>
#endif
#ifdef __AVX2__
#include <immintrin.h>
#endif
#endif
/* NetHack 3.6	monmove.c	$NHDT-Date: 1575245074 2019/12/02 00:04:34 $  $NHDT-Branch: NetHack-3.6 $:$NHDT-Revision: 1.116 $ */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Michael Allison, 2006. */
/* NetHack may be freely redistributed.  See license for details. */

#include "hack.h"
#include "mfndpos.h"
#include "nh_ctx_files/engrave_c.h"
#include "nh_ctx_files/region_c.h"
#include <pthread.h> /* v1.8: eligibility mask calibration */
extern int nh_mt2_fast;
static inline int FDECL(dochug_fast, (struct monst *, unsigned long long)) __attribute__((always_inline)); /* inlined into movemon through dochugw */
static inline int mt2_eligible_at(struct monst *, unsigned long long, int, int) __attribute__((always_inline));
#define MT2_MAXOBJ 16
static int mt2_box_objects(int, int, int, int, struct obj **, unsigned char);
extern unsigned char nh_objcat(const struct obj *);
#include "artifact.h"


STATIC_DCL void FDECL(watch_on_duty, (struct monst *));
STATIC_DCL int FDECL(disturb, (struct monst *));
STATIC_DCL void FDECL(release_hero, (struct monst *));
STATIC_DCL void FDECL(distfleeck, (struct monst *, int *, int *, int *)) __attribute__((always_inline));
/* per-move helpers: inline into the monster-turn pass (their out-of-line copies stay for other callers) */
STATIC_DCL void FDECL(set_apparxy_hot, (struct monst *)) __attribute__((always_inline));
STATIC_DCL void FDECL(set_apparxy_disp, (struct monst *, int, int)) __attribute__((noinline));
extern boolean FDECL(should_displace, (struct monst *, coord *, long *, int, XCHAR_P, XCHAR_P)) __attribute__((always_inline));
extern boolean FDECL(m_digweapon_check, (struct monst *, XCHAR_P, XCHAR_P)) __attribute__((always_inline));
extern boolean FDECL(monhaskey, (struct monst *, BOOLEAN_P)) __attribute__((always_inline));
extern int FDECL(dochugw, (struct monst *)) __attribute__((always_inline));
extern int FDECL(mt2_eligible, (struct monst *)) __attribute__((always_inline));
STATIC_DCL int FDECL(m_arrival, (struct monst *));
STATIC_DCL boolean FDECL(stuff_prevents_passage, (struct monst *));
STATIC_DCL int FDECL(vamp_shift, (struct monst *, struct permonst *,
                                  BOOLEAN_P));

/* the hero-track bit of a cell; NH_CELL_VERIFY compares it with the counter plane it mirrors */
static inline int
nh_cell_track(int x, int y)
{
    return NH_SUMMARY(x, y).move_track;
}
#define NH_CELL_TRACK(x, y) nh_cell_track((x), (y))

static inline boolean
nh_move_obj_at(int x, int y)
{
    boolean present = NH_SUMMARY(x, y).move_obj_present;
    return present;
}

/* True if mtmp died */
boolean
mb_trapped(mtmp)
struct monst *mtmp;
{
    if (flags.verbose) {
        if (cansee(mtmp->mx, mtmp->my) && !Unaware)
            pline("KABOOM!!  You see a door explode.");
        else if (!Deaf)
            You_hear("a distant explosion.");
    }
    wake_nearto(mtmp->mx, mtmp->my, 7 * 7);
    mtmp->mstun = 1;
    mtmp->mhp -= rnd(15);
    if (DEADMONSTER(mtmp)) {
        mondied(mtmp);
        if (!DEADMONSTER(mtmp)) /* lifesaved */
            return FALSE;
        else
            return TRUE;
    }
    return FALSE;
}

/* check whether a monster is carrying a locking/unlocking tool */
boolean
monhaskey(mon, for_unlocking)
struct monst *mon;
boolean for_unlocking; /* true => credit card ok, false => not ok */
{
    if (for_unlocking && m_carrying_hot(mon, CREDIT_CARD))
        return TRUE;
    return m_carrying_hot(mon, SKELETON_KEY) || m_carrying_hot(mon, LOCK_PICK);
}

void
mon_yells(mon, shout)
struct monst *mon;
const char *shout;
{
    if (Deaf) {
        if (canspotmon(mon))
            /* Sidenote on "A watchman angrily waves her arms!"
             * Female being called watchman is correct (career name).
             */
            pline("%s angrily %s %s %s!",
                Amonnam(mon),
                nolimbs(mon->data) ? "shakes" : "waves",
                mhis(mon),
                nolimbs(mon->data) ? mbodypart(mon, HEAD)
                                   : makeplural(mbodypart(mon, ARM)));
    } else {
        if (canspotmon(mon))
            pline("%s yells:", Amonnam(mon));
        else
            You_hear("someone yell:");
        verbalize1(shout);
    }
}

STATIC_OVL void
watch_on_duty(mtmp)
register struct monst *mtmp;
{
    int x, y;

    if (mtmp->mpeaceful && in_town(u.ux + u.dx, u.uy + u.dy)
        && mtmp->mcansee && m_canseeu(mtmp) && !rn2(3)) {
        if (picking_lock(&x, &y) && IS_DOOR(TYP_AT(x, y))
            && (NH_TILE(x, y).doormask & D_LOCKED)) {
            if (couldsee(mtmp->mx, mtmp->my)) {
                if (NH_TILE(x, y).looted & D_WARNED) {
                    mon_yells(mtmp, "Halt, thief!  You're under arrest!");
                    (void) angry_guards(!!Deaf);
                } else {
                    mon_yells(mtmp, "Hey, stop picking that lock!");
                    NH_TILE(x, y).looted |= D_WARNED;
                }
                stop_occupation();
            }
        } else if (is_digging()) {
            /* chewing, wand/spell of digging are checked elsewhere */
            watch_dig(mtmp, context.digging.pos.x, context.digging.pos.y,
                      FALSE);
        }
    }
}

/* canspotmon(): neither in sight nor in line of sight, a monster is spotted only through the four rare senses of the movemon()
   sense word, as nh_sensed_mon() reads it; fast: wormno is 0 */
static inline boolean __attribute__((always_inline))
nh_canspotmon(struct monst *mtmp, int fast)
{
    if (!(viz_array[mtmp->my][mtmp->mx] & (IN_SIGHT | COULD_SEE)) && (fast || !mtmp->wormno)
        && (nh_display_sense & (NH_SENSE_VALID | NH_SENSE_TPBLIND | NH_SENSE_TP | NH_SENSE_WARNMON | NH_SENSE_DETECT))
               == NH_SENSE_VALID)
        return FALSE;
    return canspotmon(mtmp);
}

int
NH_CYC(dochugw)(mtmp)
register struct monst *mtmp;
{
    int x = mtmp->mx, y = mtmp->my, dx = x - u.ux, dy = y - u.uy;
    unsigned long long spw = nh_spw[mtmp->mnum]; /* elig: mnum */
    int fast = nh_mt2_fast && mt2_eligible_at(mtmp, spw, dx, dy); /* pure, so ahead of canspotmon() */
    boolean already_saw_mon = !occupation ? 0 : nh_canspotmon(mtmp, fast); /* at any range: the monster's turn can move the hero */
    int rd = fast ? dochug_fast(mtmp, spw) : dochug(mtmp); /* dochug()'s own dispatch, without its frame */

    /* a similar check is in monster_nearby() in hack.c */
    /* check whether hero notices monster and stops current activity */
    if (occupation && distu(x, y) <= (BOLT_LIM + 1) * (BOLT_LIM + 1) && !rd && !Confusion && (!mtmp->mpeaceful || Hallucination) /* read after dochug() (it can end the occupation or move the hero) */
        /* and either couldn't see it before, or it was too far away */
        && (!already_saw_mon || !couldsee(x, y)
            || distu(x, y) > (BOLT_LIM + 1) * (BOLT_LIM + 1))
        /* can see it now, or sense it and would normally see it */
        && (canseemon(mtmp) || (sensemon(mtmp) && couldsee(x, y)))
        && mtmp->mcanmove && !noattacks(mtmp->data)
        && !onscary(u.ux, u.uy, mtmp))
        stop_occupation();

    return rd;
}

static inline boolean nh_onscary(int, int, struct monst *) __attribute__((always_inline));
static inline boolean
nh_onscary(int x, int y, struct monst *mtmp)
{
    /* creatures who are directly resistant to magical scaring:
     * Rodney, lawful minions, Angels, the Riders, shopkeepers
     * inside their own shop, priests inside their own temple */
    if (mtmp->iswiz || is_lminion(mtmp) || mtmp->data == &mons[PM_ANGEL]
        || is_rider(mtmp->data)
        || (mtmp->isshk && inhishop(mtmp))
        || (mtmp->ispriest && inhistemple(mtmp)))
        return FALSE;

    /* <0,0> is used by musical scaring to check for the above;
     * it doesn't care about scrolls or engravings or dungeon branch */
    if (x == 0 && y == 0)
        return TRUE;

    /* should this still be true for defiled/molochian altars? */
    if (IS_ALTAR(TYP_AT(x, y))
        && (mtmp->data->mlet == S_VAMPIRE || is_vampshifter(mtmp)))
        return TRUE;

    /* the scare monster scroll doesn't have any of the below
     * restrictions, being its own source of power; the cell's pile summary says whether one is there */
    {
        boolean scroll = (NH_SUMMARY(x, y).move_pile & 4U) != 0;
        if (scroll)
            return TRUE;
    }

    /*
     * Creatures who don't (or can't) fear a written Elbereth:
     * all the above plus shopkeepers (even if poly'd into non-human),
     * vault guards (also even if poly'd), blind or peaceful monsters,
     * humans and elves, and minotaurs.
     *
     * If the player isn't actually on the square OR the player's image
     * isn't displaced to the square, no protection is being granted.
     *
     * Elbereth doesn't work in Gehennom, the Elemental Planes, or the
     * Astral Plane; the influence of the Valar only reaches so far.
     */
    /* NLE: all three clauses are pure predicates; test the cheap position
     * and monster-type exclusions before sengr_at, whose engraving-chain
     * walk + fuzzymatch dominated this function (mfndpos calls onscary for
     * every candidate cell of every monster; the position clause fails for
     * all but the hero's own square). Same result, no side effects. */
    return (((u.ux == x && u.uy == y)
             || (Displaced && mtmp->mux == x && mtmp->muy == y))
            && !(mtmp->isshk || mtmp->isgd || !mtmp->mcansee
                 || mtmp->mpeaceful || mtmp->data->mlet == S_HUMAN
                 || mtmp->data == &mons[PM_MINOTAUR]
                 || Inhell || In_endgame(&u.uz))
            && sengr_at("Elbereth", x, y, TRUE));
}

boolean
onscary(x, y, mtmp)
int x, y;
struct monst *mtmp;
{
    return nh_onscary(x, y, mtmp);
}


/* regenerate lost hit points */
void
mon_regen(mon, digest_meal)
struct monst *mon;
boolean digest_meal;
{
    if (mon->mhp < mon->mhpmax && (moves % 20 == 0 || regenerates(mon->data)))
        mon->mhp++;
    if (mon->mspec_used)
        mon->mspec_used--;
    if (digest_meal) {
        if (mon->meating) {
            mon->meating--;
            if (mon->meating <= 0)
                finish_meating(mon);
        }
    }
}

/*
 * Possibly awaken the given monster.  Return a 1 if the monster has been
 * jolted awake.
 */
STATIC_OVL int
disturb(mtmp)
register struct monst *mtmp;
{
    /*
     * + Ettins are hard to surprise.
     * + Nymphs, jabberwocks, and leprechauns do not easily wake up.
     *
     * Wake up if:
     *  in direct LOS                                           AND
     *  within 10 squares                                       AND
     *  not stealthy or (mon is an ettin and 9/10)              AND
     *  (mon is not a nymph, jabberwock, or leprechaun) or 1/50 AND
     *  Aggravate or mon is (dog or human) or
     *      (1/7 and mon is not mimicing furniture or object)
     */
    if (couldsee(mtmp->mx, mtmp->my) && distu(mtmp->mx, mtmp->my) <= 100
        && (!Stealth || (mtmp->data == &mons[PM_ETTIN] && rn2(10)))
        && (!(mtmp->data->mlet == S_NYMPH
              || mtmp->data == &mons[PM_JABBERWOCK]
#if 0 /* DEFERRED */
              || mtmp->data == &mons[PM_VORPAL_JABBERWOCK]
#endif
              || mtmp->data->mlet == S_LEPRECHAUN) || !rn2(50))
        && (Aggravate_monster
            || (mtmp->data->mlet == S_DOG || mtmp->data->mlet == S_HUMAN)
            || (!rn2(7) && M_AP_TYPE(mtmp) != M_AP_FURNITURE
                && M_AP_TYPE(mtmp) != M_AP_OBJECT))) {
        mtmp->msleeping = 0;
        return 1;
    }
    return 0;
}

/* ungrab/expel held/swallowed hero */
STATIC_OVL void
release_hero(mon)
struct monst *mon;
{
    if (mon == u.ustuck) {
        if (u.uswallow) {
            expels(mon, mon->data, TRUE);
        } else if (!sticks(youmonst.data)) {
            unstuck(mon); /* let go */
            You("get released!");
        }
    }
}

#define flees_light(mon) ((mon)->data == &mons[PM_GREMLIN]     \
                          && (uwep && artifact_light(uwep) && uwep->lamplit))
/* we could include this in the above macro, but probably overkill/overhead */
/*      && (!(which_armor((mon), W_ARMC) != 0                               */
/*            && which_armor((mon), W_ARMH) != 0))                          */

/* monster begins fleeing for the specified time, 0 means untimed flee
 * if first, only adds fleetime if monster isn't already fleeing
 * if fleemsg, prints a message about new flight, otherwise, caller should */
void
monflee(mtmp, fleetime, first, fleemsg)
struct monst *mtmp;
int fleetime;
boolean first;
boolean fleemsg;
{
    /* shouldn't happen; maybe warrants impossible()? */
    if (DEADMONSTER(mtmp))
        return;

    if (mtmp == u.ustuck)
        release_hero(mtmp); /* expels/unstuck */

    if (!first || !mtmp->mflee) {
        /* don't lose untimed scare */
        if (!fleetime)
            mtmp->mfleetim = 0;
        else if (!mtmp->mflee || mtmp->mfleetim) {
            fleetime += (int) mtmp->mfleetim;
            /* ensure monster flees long enough to visibly stop fighting */
            if (fleetime == 1)
                fleetime++;
            mtmp->mfleetim = (unsigned) min(fleetime, 127);
        }
        if (!mtmp->mflee && fleemsg && canseemon(mtmp)
            && M_AP_TYPE(mtmp) != M_AP_FURNITURE
            && M_AP_TYPE(mtmp) != M_AP_OBJECT) {
            /* unfortunately we can't distinguish between temporary
               sleep and temporary paralysis, so both conditions
               receive the same alternate message */
            if (!mtmp->mcanmove || !mtmp->data->mmove) {
                pline("%s seems to flinch.", Adjmonnam(mtmp, "immobile"));
            } else if (flees_light(mtmp)) {
                if (rn2(10) || Deaf)
                    pline("%s flees from the painful light of %s.",
                          Monnam(mtmp), bare_artifactname(uwep));
                else
                    verbalize("Bright light!");
            } else
                pline("%s turns to flee.", Monnam(mtmp));
        }
        mtmp->mflee = 1;
    }
    /* ignore recently-stepped spaces when made to flee */
    memset(mtmp->mtrack, 0, sizeof(mtmp->mtrack));
}

STATIC_OVL void
distfleeck(mtmp, inrange, nearby, scared)
register struct monst *mtmp;
int *inrange, *nearby, *scared;
{
    int seescaryx, seescaryy;
    boolean sawscary = FALSE, bravegremlin = (rn2_hot(5) == 0);

    *inrange = (dist2(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy)
                <= (BOLT_LIM * BOLT_LIM));
    *nearby = *inrange && monnear(mtmp, mtmp->mux, mtmp->muy);

    /* Note: if your image is displaced, the monster sees the Elbereth
     * at your displaced position, thus never attacking your displaced
     * position, but possibly attacking you by accident.  If you are
     * invisible, it sees the Elbereth at your real position, thus never
     * running into you by accident but possibly attacking the spot
     * where it guesses you are.
     */
    if (!mtmp->mcansee || (Invis && !SPWQ(mtmp->data, SPW_PERCEIVES, perceives(mtmp->data)))) {
        seescaryx = mtmp->mux;
        seescaryy = mtmp->muy;
    } else {
        seescaryx = u.ux;
        seescaryy = u.uy;
    }

    sawscary = (*nearby || mtmp->isshk || mtmp->ispriest)
        && onscary(seescaryx, seescaryy, mtmp);
    if (*nearby && (sawscary
                    || (flees_light(mtmp) && !bravegremlin)
                    || (!mtmp->mpeaceful && in_your_sanctuary(mtmp, 0, 0)))) {
        *scared = 1;
        monflee(mtmp, rnd_hot(rn2_hot(7) ? 10 : 100), TRUE, TRUE);
    } else
        *scared = 0;
}

#undef flees_light

/* perform a special one-time action for a monster; returns -1 if nothing
   special happened, 0 if monster uses up its turn, 1 if monster is killed */
STATIC_OVL int
m_arrival(mon)
struct monst *mon;
{
    mon->mstrategy &= ~STRAT_ARRIVE; /* always reset */

    return -1;
}

static inline int FDECL(dochug_tail, (struct monst *, struct permonst *, unsigned long long, int, int, int, int)) __attribute__((always_inline));

/* returns 1 if monster died moving, 0 otherwise */
/* The whole dochugw/m_move/distfleeck/mfndpos section is serious spaghetti
 * code. --KAA
 */
int
dochug(mtmp)
register struct monst *mtmp;
{
    register struct permonst *mdat;
    register int tmp = 0;
    int inrange, nearby, scared;

    /*  Pre-movement adjustments
     */

    mdat = mtmp->data;

    
    nh_mt2_hero_valid = 0; /* the original path may change hero properties (attacks, theft, engulfing): recompute before the next fast move */

    if (mtmp->mstrategy & STRAT_ARRIVE) {
        
        int res = m_arrival(mtmp);
        if (res >= 0)
            return res;
    }

    /* check for waitmask status change */
    if ((mtmp->mstrategy & STRAT_WAITFORU)
        && (m_canseeu(mtmp) || mtmp->mhp < mtmp->mhpmax)) {
        
        mtmp->mstrategy &= ~STRAT_WAITFORU;
    }

    /* update quest status flags */
    quest_stat_check(mtmp);

    if (!mtmp->mcanmove || (mtmp->mstrategy & STRAT_WAITMASK)) {
        
        if (Hallucination)
            newsym(mtmp->mx, mtmp->my);
        if (mtmp->mcanmove && (mtmp->mstrategy & STRAT_CLOSE)
            && !mtmp->msleeping && monnear(mtmp, u.ux, u.uy))
            { quest_talk(mtmp); } /* give the leaders a chance to speak */
        return 0;             /* other frozen monsters can't do anything */
    }

    /* there is a chance we will wake it */
    if (mtmp->msleeping) {
        if (!disturb(mtmp)) {
            
            if (Hallucination)
                newsym(mtmp->mx, mtmp->my);
            return 0;
        }
        
    }

    /* not frozen or sleeping: wipe out texts written in the dust */
    if (nh_engr_here(mtmp->mx, mtmp->my))
        wipe_engr_at(mtmp->mx, mtmp->my, 1, FALSE);

    /* confused monsters get unconfused with small probability */
    if (mtmp->mconf) {
        
        if (!rn2(50))
            mtmp->mconf = 0;
    }

    /* stunned monsters get un-stunned with larger probability */
    if (mtmp->mstun) {
        
        if (!rn2(10))
            mtmp->mstun = 0;
    }

    /* some monsters teleport */
    if (mtmp->mflee && !rn2(40) && SPWQ(mdat, SPW_TPORT, can_teleport(mdat)) && !mtmp->iswiz
        && !level.lflags.noteleport) {
        
        (void) rloc(mtmp, TRUE);
        return 0;
    }
    if (mdat->msound == MS_SHRIEK && !um_dist(mtmp->mx, mtmp->my, 1)) {
        
        m_respond(mtmp);
    }
    if (mdat == &mons[PM_MEDUSA] && couldsee(mtmp->mx, mtmp->my)) {
        
        m_respond(mtmp);
    }
    if (DEADMONSTER(mtmp)) {
        
        return 1; /* m_respond gaze can kill medusa */
    }

    /* fleeing monsters might regain courage */
    if (mtmp->mflee && !mtmp->mfleetim && mtmp->mhp == mtmp->mhpmax
        && !rn2(25)) {
        
        mtmp->mflee = 0;
    }

    /* cease conflict-induced swallow/grab if conflict has ended */
    if (mtmp == u.ustuck && mtmp->mpeaceful && !mtmp->mconf && !Conflict) {
        
        release_hero(mtmp);
        return 0; /* uses up monster's turn */
    }

    set_apparxy(mtmp);
    /* Must be done after you move and before the monster does.  The
     * set_apparxy() call in m_move() doesn't suffice since the variables
     * inrange, etc. all depend on stuff set by set_apparxy().
     */

    /* Monsters that want to acquire things */
    /* may teleport, so do it before inrange is set */
    if (SPWQ(mdat, SPW_COVETOUS, is_covetous(mdat)) || is_qnemesis(mdat)) {
        
        (void) tactics(mtmp);
    }

    /* check distance and scariness of attacks */
    distfleeck(mtmp, &inrange, &nearby, &scared);

    if (find_defensive(mtmp)) {
        
        if (use_defensive(mtmp) != 0) {
            
            return 1;
        }
    } else if (find_misc(mtmp)) {
        
        if (use_misc(mtmp) != 0) {
            
            return 1;
        }
    }

    /* Demonic Blackmail! */
    if (nearby && mdat->msound == MS_BRIBE && mtmp->mpeaceful && !mtmp->mtame
        && !u.uswallow) {
        
        if (mtmp->mux != u.ux || mtmp->muy != u.uy) {
            pline("%s whispers at thin air.",
                  cansee(mtmp->mux, mtmp->muy) ? Monnam(mtmp) : "It");

            if (is_demon(youmonst.data)) {
                /* "Good hunting, brother" */
                if (!tele_restrict(mtmp))
                    (void) rloc(mtmp, TRUE);
            } else {
                mtmp->minvis = mtmp->perminvis = 0;
                /* Why?  For the same reason in real demon talk */
                pline("%s gets angry!", Amonnam(mtmp));
                mtmp->mpeaceful = 0;
                set_malign(mtmp);
                /* since no way is an image going to pay it off */
            }
        } else if (demon_talk(mtmp)) {
            
            return 1; /* you paid it off */
        }
    }

    /* the watch will look around and see if you are up to no good :-) */
    if (SPWQ(mdat, SPW_WATCH, is_watch(mdat))) {
        
        watch_on_duty(mtmp);

    } else if (SPWQ(mdat, SPW_MINDFLAYER, is_mind_flayer(mdat)) && !rn2(20)) {
        struct monst *m2, *nmon = (struct monst *) 0;
        

        if (canseemon(mtmp))
            pline("%s concentrates.", Monnam(mtmp));
        if (distu(mtmp->mx, mtmp->my) > BOLT_LIM * BOLT_LIM) {
            You("sense a faint wave of psychic energy.");
            goto toofar;
        }
        pline("A wave of psychic energy pours over you!");
        if (mtmp->mpeaceful
            && (!Conflict || resist(mtmp, RING_CLASS, 0, 0))) {
            pline("It feels quite soothing.");
        } else if (!u.uinvulnerable) {
            register boolean m_sen = sensemon(mtmp);

            if (m_sen || (Blind_telepat && rn2(2)) || !rn2(10)) {
                int dmg;
                pline("It locks on to your %s!",
                      m_sen ? "telepathy" : Blind_telepat ? "latent telepathy"
                                                          : "mind");
                dmg = rnd(15);
                if (Half_spell_damage)
                    dmg = (dmg + 1) / 2;
                losehp(dmg, "psychic blast", KILLED_BY_AN);
            }
        }
        for (m2 = fmon; m2; m2 = nmon) {
            nmon = m2->nmon;
            if (DEADMONSTER(m2))
                continue;
            if (m2->mpeaceful == mtmp->mpeaceful)
                continue;
            if (SPWQ(m2->data, SPW_MINDLESS, mindless(m2->data)))
                continue;
            if (m2 == mtmp)
                continue;
            if ((telepathic(m2->data) && (rn2(2) || m2->mblinded))
                || !rn2(10)) {
                if (cansee(m2->mx, m2->my))
                    pline("It locks on to %s.", mon_nam(m2));
                m2->mhp -= rnd(15);
                if (DEADMONSTER(m2))
                    monkilled(m2, "", AD_DRIN);
                else
                    m2->msleeping = 0;
            }
        }
    }
 toofar:

    return dochug_tail(mtmp, mdat, 0ULL, 0, inrange, nearby, scared);
}

#define MT2C(k) do { } while (0)
static NEARDATA const char practical[] = { WEAPON_CLASS, ARMOR_CLASS,
                                           GEM_CLASS, FOOD_CLASS, 0 };
static NEARDATA const char magical[] = { AMULET_CLASS, POTION_CLASS,
                                         SCROLL_CLASS, WAND_CLASS,
                                         RING_CLASS,   SPBOOK_CLASS, 0 };
static NEARDATA const char indigestion[] = { BALL_CLASS, ROCK_CLASS, 0 };
/* index(set, oclass) != 0 for the sets above: one bit per class, bit 0 the terminator */
#define OCLASS_IN(mask, c) ((uchar) (c) < 32 && ((mask) >> (uchar) (c) & 1))
#define PRACTICAL_MASK (1U | 1U << WEAPON_CLASS | 1U << ARMOR_CLASS | 1U << GEM_CLASS | 1U << FOOD_CLASS)
#define MAGICAL_MASK (1U | 1U << AMULET_CLASS | 1U << POTION_CLASS | 1U << SCROLL_CLASS | 1U << WAND_CLASS \
                      | 1U << RING_CLASS | 1U << SPBOOK_CLASS)
#define INDIGESTION_MASK (1U | 1U << BALL_CLASS | 1U << ROCK_CLASS)
static NEARDATA const char boulder_class[] = { ROCK_CLASS, 0 };
static NEARDATA const char gem_class[] = { GEM_CLASS, 0 };

boolean
itsstuck(mtmp)
register struct monst *mtmp;
{
    if (mtmp == u.ustuck && !u.uswallow && sticks(youmonst.data)) { /* same predicate; the identity test first skips the attack-table walk */
        pline("%s cannot escape from you!", Monnam(mtmp));
        return TRUE;
    }
    return FALSE;
}

/*
 * should_displace()
 *
 * Displacement of another monster is a last resort and only
 * used on approach. If there are better ways to get to target,
 * those should be used instead. This function does that evaluation.
 */
boolean
should_displace(mtmp, poss, info, cnt, gx, gy)
struct monst *mtmp;
coord *poss; /* coord poss[9] */
long *info;  /* long info[9] */
int cnt;
xchar gx, gy;
{
    int shortest_with_displacing = -1;
    int shortest_without_displacing = -1;
    int count_without_displacing = 0;
    register int i, nx, ny;
    int ndist;

    for (i = 0; i < cnt && !((info[i] & ALLOW_MDISP) && !(info[i] & ALLOW_M)); i++)
        ;
    if (i == cnt) /* no displacement candidate: shortest_with_displacing stays -1 */
        return FALSE;
    for (i = 0; i < cnt; i++) {
        nx = poss[i].x;
        ny = poss[i].y;
        ndist = dist2(nx, ny, gx, gy);
        if ((info[i] & ALLOW_MDISP) && !(info[i] & ALLOW_M) /* ALLOW_MDISP is only set with a monster there */
            && !undesirable_disp(mtmp, nx, ny)) {
            if (shortest_with_displacing == -1
                || (ndist < shortest_with_displacing))
                shortest_with_displacing = ndist;
        } else {
            int upd = (shortest_without_displacing == -1) | (ndist < shortest_without_displacing);
            int m = -upd;
            shortest_without_displacing = (ndist & m) | (shortest_without_displacing & ~m);
            count_without_displacing++;
        }
    }
    if (shortest_with_displacing > -1
        && (shortest_with_displacing < shortest_without_displacing
            || !count_without_displacing))
        return TRUE;
    return FALSE;
}


/* ===================================================================================================================
 * Monster-turn v2 (mt2): exact fast path for an ordinary hostile monster far from (distmin > 9) and unseen by the hero.
 * mt2_eligible() excludes every monster, species, hero and level state under which any skipped original statement could
 * have an effect; what remains is evaluated in the original order with the original RNG draws. Runtime switch: NH_MT2=0.
 * =================================================================================================================== */
#include <stdlib.h>
int nh_mt2_fast = 1;
#define MT2_TRACE(...) do { } while (0)

/* hero-state half of the predicate: recomputed at each movemon() pass and after any monster move that was not a pure fast move
   (only hero actions, old-path monster moves and the fast path's near tail can change these properties) */
static inline int __attribute__((always_inline))
mt2_hero_bad(void)
{
    if (!nh_mt2_hero_valid) {
        nh_mt2_hero_bad = (u.uswallow || Underwater || Hallucination || Conflict || Reflecting
                           || u.uundetected || U_AP_TYPE != M_AP_NOTHING); /* Invis/Displaced: set_apparxy(); Upolyd: lined_up() draws */
        nh_mt2_hero_valid = 1;
    }
    return nh_mt2_hero_bad;
}

#define MT2WHY_SP(i) ((void) 0)
#define MT2WHY_H() ((void) 0)
#define MT2WHY(k) 0
/* v1.8: the per-monster status part of the predicate as masked 64-bit word compares. The masks are calibrated once by
   probing each field on a zeroed struct monst (set the field, record the bytes it occupies), so no layout is assumed; a
   field that must be zero contributes its bits to the mask only, a field that must be all-ones (mcansee, mcanmove,
   cham == NON_PM) to the mask and the wanted value. Pure, so the test order is irrelevant. */
struct nh_mt2_ew { int n, vec; unsigned off[8], base; unsigned long long mask[8], want[8];
                   unsigned char vm[32] __attribute__((aligned(32))), vw[32] __attribute__((aligned(32))); }; /* elig: vector window */
static struct nh_mt2_ew nh_mt2_ew;
static int nh_mt2_ew_ready;
static pthread_once_t nh_mt2_ew_once = PTHREAD_ONCE_INIT;
static void
nh_mt2_ew_probe(unsigned char *mb, unsigned char *wb, const struct monst *m, int want_ones)
{
    const unsigned char *b = (const unsigned char *) m;
    size_t i;
    for (i = 0; i < sizeof *m; i++)
        if (b[i]) { mb[i] |= b[i]; if (want_ones) wb[i] |= b[i]; }
}
#define NH_EW_ZERO(f) do { memset(&pm, 0, sizeof pm); pm.f = ~0; nh_mt2_ew_probe(mb, wb, &pm, 0); } while (0)
#define NH_EW_ONES(f) do { memset(&pm, 0, sizeof pm); pm.f = ~0; nh_mt2_ew_probe(mb, wb, &pm, 1); } while (0)
static void
nh_mt2_ew_init(void)
{
    struct monst pm;
    unsigned char mb[sizeof(struct monst)], wb[sizeof(struct monst)];
    size_t off;
    memset(mb, 0, sizeof mb); memset(wb, 0, sizeof wb);
    NH_EW_ZERO(mtame); NH_EW_ZERO(m_ap_type); /* hd: mundetected free; fl2: mflee free */ NH_EW_ZERO(msleeping);
    NH_EW_ZERO(mstun); NH_EW_ZERO(mconf); NH_EW_ZERO(mtrapped); NH_EW_ZERO(isshk); NH_EW_ZERO(isgd); NH_EW_ZERO(ispriest);
    NH_EW_ZERO(iswiz); NH_EW_ZERO(wormno); NH_EW_ZERO(mstrategy); NH_EW_ZERO(meating);
    NH_EW_ONES(mcansee); NH_EW_ONES(mcanmove); NH_EW_ONES(cham); /* NON_PM == -1: all ones */
    nh_mt2_ew.n = 0;
    for (off = 0; off + 8 <= sizeof(struct monst); off += 8) {
        unsigned long long mk = 0, wt = 0;
        int k;
        for (k = 7; k >= 0; k--) { mk = (mk << 8) | mb[off + k]; wt = (wt << 8) | wb[off + k]; }
        if (mk) {
            if (nh_mt2_ew.n == 8) panic("nh_mt2_ew_init: too many words");
            nh_mt2_ew.off[nh_mt2_ew.n] = (unsigned) off; nh_mt2_ew.mask[nh_mt2_ew.n] = mk; nh_mt2_ew.want[nh_mt2_ew.n] = wt;
            nh_mt2_ew.n++;
        }
    }
    { /* elig: every masked byte inside one 32-byte window -> the vector test */
        int lo = -1, hi = -1;
        for (off = 0; off < sizeof(struct monst); off++)
            if (mb[off]) { if (lo < 0) lo = (int) off; hi = (int) off; }
        if (lo >= 0 && hi - lo < 32 && sizeof(struct monst) >= 32) {
            unsigned base = (unsigned) lo + 32 <= sizeof(struct monst) ? (unsigned) lo : (unsigned) (sizeof(struct monst) - 32);
            memcpy(nh_mt2_ew.vm, mb + base, 32); memcpy(nh_mt2_ew.vw, wb + base, 32);
            nh_mt2_ew.base = base; __atomic_store_n(&nh_mt2_ew.vec, 1, __ATOMIC_RELEASE); /* last: a set vec implies the window */
        }
    }
    __atomic_store_n(&nh_mt2_ew_ready, 1, __ATOMIC_RELEASE);
}
static inline int
mt2_status_vec(const unsigned char *b) /* elig */
{
    const unsigned char *w = b + nh_mt2_ew.base;
#ifdef __AVX2__
    __m256i x = _mm256_xor_si256(_mm256_and_si256(_mm256_loadu_si256((const __m256i *) w), _mm256_load_si256((const __m256i *) nh_mt2_ew.vm)),
                                 _mm256_load_si256((const __m256i *) nh_mt2_ew.vw));
    return _mm256_testz_si256(x, x);
#else
    __m128i x0 = _mm_xor_si128(_mm_and_si128(_mm_loadu_si128((const __m128i *) w), _mm_load_si128((const __m128i *) nh_mt2_ew.vm)),
                               _mm_load_si128((const __m128i *) nh_mt2_ew.vw));
    __m128i x1 = _mm_xor_si128(_mm_and_si128(_mm_loadu_si128((const __m128i *) (w + 16)), _mm_load_si128((const __m128i *) (nh_mt2_ew.vm + 16))),
                               _mm_load_si128((const __m128i *) (nh_mt2_ew.vw + 16)));
    return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_or_si128(x0, x1), _mm_setzero_si128())) == 0xFFFF;
#endif
}
static inline int
mt2_status_ok(const struct monst *m)
{
    const unsigned char *b = (const unsigned char *) m;
    int i;
    if (__atomic_load_n(&nh_mt2_ew.vec, __ATOMIC_ACQUIRE)) /* stored last by the init: the window is ready */
        return mt2_status_vec(b);
    if (!__atomic_load_n(&nh_mt2_ew_ready, __ATOMIC_ACQUIRE)) pthread_once(&nh_mt2_ew_once, nh_mt2_ew_init);
    if (nh_mt2_ew.vec)
        return mt2_status_vec(b);
    {
        unsigned long long bad = 0; /* v2.3d: every word, no early exit */
        for (i = 0; i < nh_mt2_ew.n; i++) {
            unsigned long long w;
            memcpy(&w, b + nh_mt2_ew.off[i], 8);
            bad |= (w & nh_mt2_ew.mask[i]) ^ nh_mt2_ew.want[i];
        }
        return bad == 0;
    }
}
/* mfndpos ALLOW_DIG's rockok/treeok sources: any pick or axe in the inventory (wielded cursed ones included) */
static inline int
nh_has_dig_tool(struct monst *m)
{
    struct obj *o;

    for (o = m->minvent; o; o = o->nobj)
        if (is_pick(o) || is_axe(o)) return 1;
    return 0;
}
/* spw: the monster's species word; dx, dy: its offset from the hero */
static inline int
mt2_eligible_at(struct monst *m, unsigned long long spw, int dx, int dy)
{
    unsigned int far = ((unsigned int) (dx + 1) > 2U) | ((unsigned int) (dy + 1) > 2U); /* not adjacent */
    return mt2_status_ok(m) & (m != u.ustuck) & !(spw & SPW_MT2_NOFAST) & !mt2_hero_bad()
           & !((spw & SPW_UNICORN) && level.lflags.noteleport) /* uni: noteleport levels keep the original avoid rule */
           & far; /* v2.3d: all terms pure, combined without short-circuit */
}
static inline int __attribute__((always_inline))
mt2_eligible_fast(struct monst *m)
{
    return mt2_eligible_at(m, nh_spw[m->mnum], m->mx - u.ux, m->my - u.uy); /* elig: mnum */
}
int
mt2_eligible(m)
struct monst *m;
{
    return mt2_eligible_fast(m);
}
static int
mt2_eligible_slow(struct monst *m) __attribute__((unused));
static int
mt2_eligible_slow(struct monst *m)
{
    const struct permonst *p = m->data;

    if (m->mtame) return MT2WHY(0);
    if (m->isshk || m->ispriest || m->isgd || m->iswiz || m->wormno) return MT2WHY(2);
    if (m->mstrategy) return MT2WHY(3);
    if (m->msleeping) return MT2WHY(4);
    if (m->mconf || m->mstun || m->mtrapped || m->meating || !m->mcanmove || !m->mcansee
        || M_AP_TYPE(m) != M_AP_NOTHING || m->cham != NON_PM || m == u.ustuck) /* hd: mundetected allowed */
        return MT2WHY(6);
    if ((nh_spw[p - mons] & SPW_MT2_NOFAST) || ((nh_spw[p - mons] & SPW_UNICORN) && level.lflags.noteleport)) {
        MT2WHY_SP(p - mons);
        return MT2WHY(7);
    }
    if (mt2_hero_bad()) {
        MT2WHY_H();
        return MT2WHY(8);
    }
    {
        int ddx = abs(m->mx - u.ux), ddy = abs(m->my - u.uy);
        if ((ddx > ddy ? ddx : ddy) <= 1) /* adjacent (nearby): the melee path stays original */
            return MT2WHY(9);
    }
    return 1;
}

/* window bits (3 * (x - omx + 1) + (y - omy + 1)) of the cells online2() with the hero's true position */
static inline unsigned int
nh_online_mask(int omx, int omy)
{
    unsigned int m = 0;
    int b;

    for (b = 0; b < 9; b++) {
        int dx = omx - 1 + b / 3 - u.ux, dy = omy - 1 + b % 3 - u.uy;
        m |= (unsigned int) (!dx || !dy || dx == dy || dx == -dy) << b;
    }
    return m;
}

/* linedup()'s pure geometric pre-tests: FALSE means lined_up() sets tbx/tby and returns FALSE without any draw; dx, dy = the
   believed hero position minus the monster's, dm = their distmin (0 < dm < BOLT_LIM: not on the hero, within range) */
static inline int
nh_lined_geom(int dx, int dy, int dm)
{
    return ((unsigned int) (dm - 1) < BOLT_LIM - 1) & (dx * dy * ((dx - dy) * (dx + dy)) == 0); /* mb; lg: dx, dy, dx - dy or dx + dy is 0 */
}

/* *in_rooms(x, y, SHOPBASE) != '\0' */
static inline int
mt2_in_shop(int x, int y)
{
    int rno = NH_TILE(x, y).roomno;
    if (__builtin_expect((unsigned) (rno - 1) < (unsigned) (ROOMOFFSET - 1), 0)) /* mb: shared cells keep in_rooms() */
        return *in_rooms(x, y, SHOPBASE) != '\0';
    return NH_TILE(x, y).msum.move_shop; /* the room's shop test, kept per cell by nh_shop_bits_sync */
}

/* any floor object inside the search box: the search loop body has no effect when it never runs */
static inline int
mt2_box_has_objects(int lx, int hx, int ly, int hy)
{
    unsigned int rows = (2U << hy) - (1U << ly), acc = 0;
    int x;
    for (x = lx; x <= hx; x++) acc |= level.move_columns[x + 1].obj;
    return (acc & rows) != 0;
}

/* win: nh_move_neighbours() of the (nonrock, occupied, bad, door) words of three adjacent columns, one lane each; rows y-1..y+1
   of a column are its word shifted left by 30 - y, then right by 29 */
static inline __m128i
nh_move_window4(const struct nh_move_column *c, int y)
{
    __m128i s = _mm_cvtsi32_si128(30 - y);
    __m128i l = _mm_srli_epi32(_mm_sll_epi32(_mm_loadu_si128((const __m128i *) &c[0].nonrock), s), 29);
    __m128i m = _mm_srli_epi32(_mm_sll_epi32(_mm_loadu_si128((const __m128i *) &c[1].nonrock), s), 29);
    __m128i r = _mm_srli_epi32(_mm_sll_epi32(_mm_loadu_si128((const __m128i *) &c[2].nonrock), s), 29);
    return _mm_and_si128(_mm_or_si128(l, _mm_slli_epi32(_mm_or_si128(m, _mm_slli_epi32(r, 3)), 3)), _mm_set1_epi32(0x1ef));
}

/* v2.3c: selection reduction helpers; lanes = window bits 0-3, 5-8 (bit 4 is the monster's own cell) */
static const short nh_sel_dx[8] __attribute__((aligned(16))) = { 0, 0, 0, 1, 1, 2, 2, 2 };
static const short nh_sel_dy[8] __attribute__((aligned(16))) = { 0, 1, 2, 0, 2, 0, 1, 2 };
#ifdef __SSE4_1__
/* sel: per direction (approach, flee) the key xor, the lane order and the window bit of each ordered lane */
static const struct __attribute__((aligned(64))) { unsigned short kx[8]; unsigned char ord[16]; unsigned char bit[8]; } nh_sel_dir[2] = {
    { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, { 0, 1, 2, 3, 5, 6, 7, 8 } },
    { { 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff }, { 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1 },
      { 8, 7, 6, 5, 3, 2, 1, 0 } } };
static const unsigned short nh_sel_lanebit[8] __attribute__((aligned(16))) = { 1, 2, 4, 8, 32, 64, 128, 256 };
#else
static const unsigned char nh_sel_bit[8] = { 0, 1, 2, 3, 5, 6, 7, 8 };
#endif
/* window bit b = 3 * (x - omx + 1) + (y - omy + 1): its offsets from the monster */
static const signed char nh_win_dx[9] = { -1, -1, -1, 0, 0, 0, 1, 1, 1 };
static const signed char nh_win_dy[9] = { -1, 0, 1, -1, 0, 1, -1, 0, 1 };
/* sq: the diagonal window bits whose two orthogonal neighbours are both rock, indexed by the rock bits 1, 3, 5, 7 shifted down by one */
static unsigned short nh_squeeze_diag[0x56];
static void __attribute__((constructor))
nh_squeeze_init(void)
{
    int i;
    for (i = 0; i < 0x56; i++)
        nh_squeeze_diag[i] = ((i & 0x05) == 0x05 ? 0x001 : 0) | ((i & 0x11) == 0x11 ? 0x004 : 0)
                             | ((i & 0x44) == 0x44 ? 0x040 : 0) | ((i & 0x50) == 0x50 ? 0x100 : 0);
}
#ifndef __SSE4_1__
static inline __m128i
nh_hmin16(__m128i x)
{
    x = _mm_min_epi16(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(1, 0, 3, 2)));
    x = _mm_min_epi16(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_min_epi16(x, _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1)));
}
static inline __m128i
nh_hmax16(__m128i x)
{
    x = _mm_max_epi16(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(1, 0, 3, 2)));
    x = _mm_max_epi16(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_max_epi16(x, _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1)));
}
#endif
#ifndef SQSRCHRADIUS
#define SQSRCHRADIUS 5
#endif

/* xt: mfndpos_masked()'s trap test outside Sokoban */
static boolean
nh_trap_harms(struct monst *mon, int tt)
{
    struct permonst *mdat = mon->data;
    return (tt != RUST_TRAP || mdat == &mons[PM_IRON_GOLEM]) && tt != STATUE_TRAP && tt != VIBRATING_SQUARE
           && ((!is_pit(tt) && !is_hole(tt)) || (!is_flyer(mdat) && !is_floater(mdat) && !is_clinger(mdat)))
           && (tt != SLP_GAS_TRAP || !resists_sleep(mon))
           && (tt != BEAR_TRAP
               || (mdat->msize > MZ_SMALL && !amorphous(mdat) && !is_flyer(mdat) && !is_floater(mdat) && !is_whirly(mdat)
                   && !unsolid(mdat)))
           && (tt != FIRE_TRAP || !resists_fire(mon))
           && (tt != SQKY_BOARD || !is_flyer(mdat))
           && (tt != WEB || (!amorphous(mdat) && !webmaker(mdat) && !is_whirly(mdat) && !unsolid(mdat)))
           && (tt != ANTI_MAGIC || !resists_magm(mon));
}

/* xt: the window cells in c that mfndpos() drops for a boulder, garlic or a known trap; 0 when a cell keeps the fallback */
static int
nh_xt_excl(struct monst *mtmp, unsigned long long spw, int omx, int omy, unsigned int c, unsigned int *excl)
{
    unsigned int e = 0x10U; /* the origin bit, never a candidate: marks the window as decided here */
    int nogarlic = (spw & SPW_UNDEAD) && mtmp->data->mlet != S_GHOST;
    while (c) {
        int b = __builtin_ctz(c), nx = omx - 1 + b / 3, ny = omy - 1 + b % 3, tt;
        unsigned int pl = NH_SUMMARY(nx, ny).move_pile;
        c &= c - 1;
        if (pl & 4U)
            return 0;
        if (((pl & 2U) && nogarlic) || ((pl & 1U) && !(spw & SPW_THROWSROCKS)))
            e |= 1U << b;
        if (NH_SUMMARY(nx, ny).move_trap_present) {
            tt = NH_SUMMARY(nx, ny).move_trap;
            if (tt == 0 || tt >= TRAPNUM)
                return 0;
            if ((mtmp->mtrapseen & (1L << (tt - 1))) && nh_trap_harms(mtmp, tt))
                e |= 1U << b;
        }
    }
    *excl = e;
    return 1;
}

#define NH_LIKES_MM ((unsigned char) ((likegold ? 1 : 0) | (likeobjs ? 2 : 0) | (likemagic ? 4 : 0) | (uses_items ? 8 : 0) \
                                     | (likerock ? 16 : 0) | (likegems ? 32 : 0) | (ptr == &mons[PM_GELATINOUS_CUBE] ? 64 : 0) | (conceals ? 128 : 0)))
/* m_move()/m_move_fast(): the first floor object in fobj order within the search box that the monster wants (mm = its interest
   categories: gold 1, practical 2, magic 4, items it uses 8, boulders 16, gems 32, a gelatinous cube 64, a hiding place 128) becomes
   its goal and shrinks the box; 1 when that object lies under the monster */
static inline int nh_mon_objsearch(struct monst *, struct permonst *, unsigned long long, int, int, int *, xchar *, xchar *, unsigned char, int) __attribute__((always_inline));
static inline int
nh_mon_objsearch(struct monst *mtmp, struct permonst *ptr, unsigned long long spw, int omx, int omy, int *pminr, xchar *pgx, xchar *pgy, unsigned char mm, int mload)
{
    int minr = *pminr, xx, yy, oomx, oomy, lmx, lmy;
    xchar gx = *pgx, gy = *pgy;
    struct obj *otmp;
    struct monst *mtoo;
    struct nh_fobj_cursor cursor;
    struct obj *mt2_list[MT2_MAXOBJ];
    int mt2_n, mt2_i = 0;

    MT2C(C_SEARCH);
    oomx = min(COLNO - 1, omx + minr);
    oomy = min(ROWNO - 1, omy + minr);
    lmx = max(1, omx - minr);
    lmy = max(0, omy - minr);
    mt2_n = mt2_box_objects(lmx, oomx, lmy, oomy, mt2_list, mm); /* -1: box too large or too full, walk the cursor */
    for (otmp = mt2_n >= 0 ? (mt2_n ? mt2_list[0] : (struct obj *) 0) : nh_fobj_next(&cursor, (struct obj *) 0, lmx, oomx, lmy, oomy);
         otmp;
         otmp = mt2_n >= 0 ? (++mt2_i < mt2_n ? mt2_list[mt2_i] : (struct obj *) 0) : nh_fobj_next(&cursor, otmp, lmx, oomx, lmy, oomy)) {
        /* monsters may pick rocks up, but won't go out of their way
           to grab them; this might hamper sling wielders, but it cuts
           down on move overhead by filtering out most common item */
        MT2C(C_SEARCHOBJ);
        if (otmp->otyp == ROCK)
            continue;
        if (mt2_n < 0 && !(nh_objcat(otmp) & mm)) /* the box gather already filtered by category */
            continue;
        xx = otmp->ox;
        yy = otmp->oy;
        /* Nymphs take everything.  Most other creatures should not
         * pick up corpses except as a special case like in
         * searches_for_item().  We need to do this check in
         * mpickstuff() as well.
         */
        if (xx >= lmx && xx <= oomx && yy >= lmy && yy <= oomy) {
            /* don't get stuck circling around object that's
               underneath an immobile or hidden monster;
               paralysis victims excluded */
            if ((mtoo = m_at(xx, yy)) != 0
                && (mtoo->msleeping || mtoo->mundetected
                    || (mtoo->mappearance && !mtoo->iswiz)
                    || !mtoo->data->mmove))
                continue;
            /* the mfndpos() test for whether to allow a move to a
               water location accepts flyers, but they can't reach
               underwater objects, so being able to move to a spot
               is insufficient for deciding whether to do so */
            if ((is_pool(xx, yy) && !(spw & SPW_SWIMMER))
                || (is_lava(xx, yy) && !(spw & SPW_LIKELAVA)))
                continue;

            if ((((mm & 1) && otmp->oclass == COIN_CLASS)
                 || ((mm & 2) && OCLASS_IN(PRACTICAL_MASK, otmp->oclass)
                     && (otmp->otyp != CORPSE
                         || (ptr->mlet == S_NYMPH
                             && !is_rider(&mons[otmp->corpsenm]))))
                 || ((mm & 4) && OCLASS_IN(MAGICAL_MASK, otmp->oclass))
                 || ((mm & 8) && searches_for_item(mtmp, otmp))
                 || ((mm & 16) && otmp->otyp == BOULDER)
                 || ((mm & 32) && otmp->oclass == GEM_CLASS
                     && objects[otmp->otyp].oc_material != MINERAL)
                 || ((mm & 128) && !cansee(otmp->ox, otmp->oy))
                 || (ptr == &mons[PM_GELATINOUS_CUBE]
                     && !OCLASS_IN(INDIGESTION_MASK, otmp->oclass)
                     && !(otmp->otyp == CORPSE
                          && touch_petrifies(&mons[otmp->corpsenm]))))
                && (otmp->oartifact ? touch_artifact(otmp, mtmp) : (nh_cur->g_artifact_c_touch_blasted = FALSE, 1))) { /* sc */
                if (can_carry_load(mtmp, otmp, mload) > 0
                    && ((spw & SPW_THROWSROCKS) || !(NH_SUMMARY(xx, yy).move_pile & 1U)) /* fl: boulder pile bit */
                    && (!(spw & SPW_UNICORN)
                        || objects[otmp->otyp].oc_material == GEMSTONE)
                    /* Don't get stuck circling an Elbereth */
                    && !nh_onscary(xx, yy, mtmp)) { /* sc */
                    minr = distmin(omx, omy, xx, yy);
                    oomx = min(COLNO - 1, omx + minr);
                    oomy = min(ROWNO - 1, omy + minr);
                    lmx = max(1, omx - minr);
                    lmy = max(0, omy - minr);
                    gx = otmp->ox;
                    gy = otmp->oy;
                    MT2C(C_TARGET);
                    if (gx == omx && gy == omy) {
                        *pminr = minr, *pgx = gx, *pgy = gy;
                        return 1;
                    }
                }
            }
        }
    }
    *pminr = minr, *pgx = gx, *pgy = gy;
    return 0;
}

/* m_move()/m_move_fast(): the chosen square holds another monster (confusion, or it is where the hero seems to be) */
static int __attribute__((noinline))
nh_mon_fight(struct monst *mtmp, xchar nix, xchar niy)
{
    struct monst *mtmp2;
    int mstatus;

    
    mtmp2 = m_at(nix, niy);

    notonhead = mtmp2 && (nix != mtmp2->mx || niy != mtmp2->my);
    /* note: mstatus returns 0 if mtmp2 is nonexistent */
    mstatus = mattackm(mtmp, mtmp2);

    if (mstatus & MM_AGR_DIED) /* aggressor died */
        return 2;

    if ((mstatus & MM_HIT) && !(mstatus & MM_DEF_DIED) && rn2(4)
        && mtmp2->movement >= NORMAL_SPEED) {
        mtmp2->movement -= NORMAL_SPEED;
        notonhead = 0;
        mstatus = mattackm(mtmp2, mtmp); /* return attack */
        if (mstatus & MM_DEF_DIED)
            return 2;
    }
    return 3;
}

/* m_move()/m_move_fast(): swap places with a displaceable monster */
static int __attribute__((noinline))
nh_mon_displace(struct monst *mtmp, xchar nix, xchar niy)
{
    struct monst *mtmp2;
    int mstatus;

    
    mtmp2 = m_at(nix, niy);
    mstatus = mdisplacem(mtmp, mtmp2, FALSE);
    if ((mstatus & MM_AGR_DIED) || (mstatus & MM_DEF_DIED))
        return 2;
    if (mstatus & MM_HIT)
        return 1;
    return 3;
}

/* m_move()/m_move_fast(): a monster standing in a doorway opens, unlocks or breaks the door if it can; 2 = a door trap killed it */
static int __attribute__((noinline))
nh_mon_door(struct monst *mtmp, struct permonst *ptr, boolean can_open, boolean can_unlock, boolean canseeit)
{
    struct rm *here = &NH_TILE(mtmp->mx, mtmp->my);
    boolean btrapped = (here->doormask & D_TRAPPED) != 0,
            observeit = canseeit && canspotmon(mtmp);

    

    /* if mon has MKoT, disarm door trap; no message given */
    if (btrapped && has_magic_key(mtmp)) {
        /* BUG: this lets a vampire or blob or a doorbuster
           holding the Key disarm the trap even though it isn't
           using that Key when squeezing under or smashing the
           door.  Not significant enough to worry about; perhaps
           the Key's magic is more powerful for monsters? */
        here->doormask &= ~D_TRAPPED;
        btrapped = FALSE;
    }
    if ((here->doormask & (D_LOCKED | D_CLOSED)) != 0
        && SPWQ(ptr, SPW_AMORPHOUS, amorphous(ptr))) {
        if (flags.verbose && canseemon(mtmp))
            pline("%s %s under the door.", Monnam(mtmp),
                  (ptr == &mons[PM_FOG_CLOUD]
                   || ptr->mlet == S_LIGHT) ? "flows" : "oozes");
    } else if (here->doormask & D_LOCKED && can_unlock) {
        if (btrapped) {
            here->doormask = D_NODOOR;
            newsym(mtmp->mx, mtmp->my);
            unblock_point(mtmp->mx, mtmp->my); /* vision */
            if (mb_trapped(mtmp)) {
                
                return 2;
            }
        } else {
            if (flags.verbose) {
                if (observeit)
                    pline("%s unlocks and opens a door.",
                          Monnam(mtmp));
                else if (canseeit)
                    You_see("a door unlock and open.");
                else if (!Deaf)
                    You_hear("a door unlock and open.");
            }
            here->doormask = D_ISOPEN;
            /* newsym(mtmp->mx, mtmp->my); */
            unblock_point(mtmp->mx, mtmp->my); /* vision */
        }
    } else if (here->doormask == D_CLOSED && can_open) {
        if (btrapped) {
            here->doormask = D_NODOOR;
            newsym(mtmp->mx, mtmp->my);
            unblock_point(mtmp->mx, mtmp->my); /* vision */
            if (mb_trapped(mtmp)) {
                
                return 2;
            }
        } else {
            if (flags.verbose) {
                if (observeit)
                    pline("%s opens a door.", Monnam(mtmp));
                else if (canseeit)
                    You_see("a door open.");
                else if (!Deaf)
                    You_hear("a door open.");
            }
            here->doormask = D_ISOPEN;
            /* newsym(mtmp->mx, mtmp->my); */  /* done below */
            unblock_point(mtmp->mx, mtmp->my); /* vision */
        }
    } else if (here->doormask & (D_LOCKED | D_CLOSED)) {
        /* mfndpos guarantees this must be a doorbuster */
        if (btrapped) {
            here->doormask = D_NODOOR;
            newsym(mtmp->mx, mtmp->my);
            unblock_point(mtmp->mx, mtmp->my); /* vision */
            if (mb_trapped(mtmp)) {
                
                return 2;
            }
        } else {
            if (flags.verbose) {
                if (observeit)
                    pline("%s smashes down a door.",
                          Monnam(mtmp));
                else if (canseeit)
                    You_see("a door crash open.");
                else if (!Deaf)
                    You_hear("a door crash open.");
            }
            if ((here->doormask & D_LOCKED) != 0 && !rn2(2))
                here->doormask = D_NODOOR;
            else
                here->doormask = D_BROKEN;
            /* newsym(mtmp->mx, mtmp->my); */  /* done below */
            unblock_point(mtmp->mx, mtmp->my); /* vision */
        }
        /* if it's a shop door, schedule repair */
        if (mt2_in_shop(mtmp->mx, mtmp->my))
            add_damage(mtmp->mx, mtmp->my, 0L);
    }
    return 0;
}

static const unsigned int nh_k4_lane[5][4] __attribute__((aligned(16))) = { /* k4: track entries j < min(MTSZ, cnt - 1) */
    { 0, 0, 0, 0 }, { ~0U, 0, 0, 0 }, { ~0U, ~0U, 0, 0 }, { ~0U, ~0U, ~0U, 0 }, { ~0U, ~0U, ~0U, ~0U } };

/* the fast move's choice among the candidate window bits, in the original order of draws: reservoir when wandering, else the
   track draws in candidate order, then the first nearest (approach) or last farthest (flee) of what is left; -1: none */
static inline int nh_k4_pick(struct monst *, unsigned int, int, int, int, int, int) __attribute__((always_inline));
static inline int
nh_k4_pick(struct monst *mtmp, unsigned int cand, int appr, int omx, int omy, int gx, int gy)
{
    int k4a = appr, best = -1, cnt = __builtin_popcount(cand);
    unsigned int v = cand;
    if (level.lflags.shortsighted && k4a == 1 && !mtmp->mpeaceful && dist2(omx, omy, gx, gy) > (couldsee(omx, omy) ? 144 : 36))
        k4a = 0; /* k4c: the never-true level flag first (appr == 1 first mispredicted ~17 times per step) */
    if (!k4a) { /* the original reservoir: one draw per candidate, in candidate order (none without a candidate) */
        unsigned int c;
        int k = 0;
        for (c = cand; c; c &= c - 1) { int bt = __builtin_ctz(c), r = rn2_hot(++k); best = r ? best : bt; } /* mb: cmov */
    } else if (cand) {
        if (cnt > 1) { /* track entries j < min(MTSZ, cnt - 1); on-track candidates draw in candidate order, entries in order */
            unsigned int th, tm;
#ifdef __AVX2__
            unsigned long long mt;
            __m128i D, in8, bits;
            memcpy(&mt, &mtmp->mtrack[0], 8);
            D = _mm_sub_epi8(_mm_cvtsi64_si128((long long) mt), _mm_set1_epi16((short) ((((omy - 1) & 0xff) << 8) | ((omx - 1) & 0xff))));
            in8 = _mm_cmpeq_epi8(_mm_subs_epu8(D, _mm_set1_epi8(2)), _mm_setzero_si128());
            bits = _mm_sllv_epi32(_mm_set1_epi32(1), _mm_cvtepu16_epi32(_mm_maddubs_epi16(D, _mm_set1_epi16(0x0103))));
            bits = _mm_and_si128(bits, _mm_and_si128(_mm_cvtepi16_epi32(_mm_cmpeq_epi16(in8, _mm_set1_epi16(-1))),
                                                     _mm_load_si128((const __m128i *) nh_k4_lane[cnt - 1 < MTSZ ? cnt - 1 : MTSZ])));
            {
                __m128i t2 = _mm_or_si128(bits, _mm_shuffle_epi32(bits, _MM_SHUFFLE(1, 0, 3, 2)));
                tm = (unsigned int) _mm_cvtsi128_si32(_mm_or_si128(t2, _mm_shuffle_epi32(t2, _MM_SHUFFLE(2, 3, 0, 1))));
            }
#else
            unsigned int pb[MTSZ];
            int jj, jc = min(MTSZ, cnt - 1);
            tm = 0;
            for (jj = 0; jj < MTSZ; jj++) {
                pb[jj] = nh_move_position_bit(omx, omy, mtmp->mtrack[jj].x, mtmp->mtrack[jj].y) & (0U - (unsigned int) (jj < jc));
                tm |= pb[jj];
            }
#endif
            th = cand & tm;
            while (th) {
                unsigned int bb = th & (0U - th), hits;
#ifdef __AVX2__
                hits = (unsigned int) _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(bits, _mm_set1_epi32((int) bb))));
#else
                hits = (pb[0] == bb) | ((pb[1] == bb) << 1) | ((pb[2] == bb) << 2) | ((pb[3] == bb) << 3);
#endif
                th &= th - 1;
                while (hits) {
                    int jh = __builtin_ctz(hits);
                    hits &= hits - 1;
                    if (jh >= cnt - 1) /* hits holds track entries below cnt - 1: rn2's argument check folds away */
                        __builtin_unreachable();
                    if (rn2_hot(4 * (cnt - jh))) { v &= ~bb; break; }
                }
            }
        }
        if (v & (v - 1)) { /* first minimum (approach) or last maximum (flee) of dist2 over the remaining bits */
            __m128i X = _mm_add_epi16(_mm_set1_epi16((short) (omx - 1 - gx)), _mm_load_si128((const __m128i *) nh_sel_dx));
            __m128i Y = _mm_add_epi16(_mm_set1_epi16((short) (omy - 1 - gy)), _mm_load_si128((const __m128i *) nh_sel_dy));
            __m128i Dd = _mm_add_epi16(_mm_mullo_epi16(X, X), _mm_mullo_epi16(Y, Y));
#ifdef __SSE4_1__
            /* sel: phminposuw takes the first minimum (lanes off v are 0xffff); fleeing keys are 0x7fff - d in reversed lanes,
               whose first minimum is the last maximum of d: no branch on the direction */
            unsigned int fl = (unsigned int) k4a >> 31; /* k4a is 1 or -1 here */
            __m128i K = _mm_or_si128(_mm_xor_si128(Dd, _mm_load_si128((const __m128i *) nh_sel_dir[fl].kx)),
                                     _mm_cmpeq_epi16(_mm_and_si128(_mm_set1_epi16((short) v), _mm_load_si128((const __m128i *) nh_sel_lanebit)),
                                                     _mm_setzero_si128()));
            K = _mm_shuffle_epi8(K, _mm_load_si128((const __m128i *) nh_sel_dir[fl].ord));
            best = nh_sel_dir[fl].bit[(unsigned int) _mm_cvtsi128_si32(_mm_minpos_epu16(K)) >> 16];
#else
            __m128i LB = _mm_set_epi16(128, 64, 32, 16, 8, 4, 2, 1), VM;
            unsigned int lm = (v & 0xFU) | ((v >> 1) & 0xF0U);
            int l, l1, l2; /* mb: both reductions, the approach/flee choice by cmov */
            __m128i K1, K2;
            VM = _mm_cmpeq_epi16(_mm_and_si128(_mm_set1_epi16((short) lm), LB), LB);
            K1 = _mm_or_si128(_mm_and_si128(VM, Dd), _mm_andnot_si128(VM, _mm_set1_epi16(0x7fff)));
            K2 = _mm_or_si128(_mm_and_si128(VM, Dd), _mm_andnot_si128(VM, _mm_set1_epi16(-1)));
            l1 = __builtin_ctz((unsigned int) _mm_movemask_epi8(_mm_cmpeq_epi16(K1, nh_hmin16(K1)))) >> 1;
            l2 = (31 - __builtin_clz((unsigned int) _mm_movemask_epi8(_mm_cmpeq_epi16(K2, nh_hmax16(K2))))) >> 1;
            l = k4a > 0 ? l1 : l2;
            best = nh_sel_bit[l];
#endif
        } else if (v)
            best = __builtin_ctz(v);
    }
    return best;
}

static int
m_move_fast(mtmp)
register struct monst *mtmp;
{
    struct permonst *ptr = mtmp->data;
    unsigned long long spw = nh_spw[mtmp->mnum]; /* elig: mnum */
    register int appr;
    xchar gx, gy, nix, niy, chcnt = 0;
    int chi, cnt;
    boolean likegold = 0, likegems = 0, likeobjs = 0, likemagic = 0, conceals = 0;
    boolean likerock = 0, can_tunnel = 0;
    int mload = -1; /* the monster's load once computed for the likes (the object search reuses it) */
    boolean can_open = 0, can_unlock = 0, doorbuster = 0;
    boolean uses_items = 0, setlikes = 0;
    unsigned int lk = 0; /* lk: the likes before the move, as NH_LIKES_MM bits (postmov recomputes the booleans) */
    boolean plain;
    struct monst *mtoo;
    schar mmoved = 0;
    long info[9];
    long flag;
    int omx = mtmp->mx, omy = mtmp->my;
    struct nh_move_column *cols = &level.move_columns[omx];
    coord poss[9];
    unsigned int door_n, origin_door;
    unsigned long long nw; /* win: (nonrock, occupied) window */
    boolean unlock_known = 0; /* can_unlock is three inventory walks: computed only when a locked door is in reach */
    unsigned int xt_excl = 0; /* xt */
    boolean should_see;

    /* mtrapped and meating are excluded; sawmon (vampshifter) is excluded */
    if ((spw & SPW_HIDESUNDER) && NH_SUMMARY(omx, omy).move_obj_present && rn2_hot(10)) {
        
        return 0; /* do not leave hiding place */
    }

    set_apparxy_hot(mtmp); /* second call of the original: re-guesses for Invis/Displaced */
    int nh_dmu = distmin(omx, omy, mtmp->mux, mtmp->muy); /* dc */
    can_open = !(spw & (SPW_NOHANDS | SPW_VERYSMALL));
    doorbuster = (spw & SPW_GIANT) != 0;
    /* can_unlock (three inventory walks) is only read with a door in reach: computed on the fallback path */

    gx = mtmp->mux;
    gy = mtmp->muy;
    appr = mtmp->mflee ? -1 : 1; /* mflee/mconf were 0 at the eligibility test but distfleeck()/item use in this dochug may set them */
    should_see = ((dist2(omx, omy, gx, gy) <= 36) /* fl: pure terms, the cheap one first */
                  && couldsee(omx, omy)
                  && (NH_TILE(gx, gy).lit || !NH_TILE(omx, omy).lit));
    if (mtmp->mconf || (u.uswallow && mtmp == u.ustuck)) {
        appr = 0;
    } else {
        /* the original OR chain minus its excluded terms (!mcansee, disguise, uundetected), same evaluation order and draws */
        if ((should_see && Invis && !(spw & SPW_PERCEIVES) && rn2(11))
            || mtmp->mpeaceful
            || ((spw & SPW_RANDAPPR) && !rn2_hot(3)))
            appr = 0;
    }
    if (!(mtmp->mconf || (u.uswallow && mtmp == u.ustuck))) { /* the original's else branch continues */
        if (((spw & SPW_LEPRECHAUN) != 0) & (appr == 1)) { /* mb */
            struct obj *lepgold, *ygold;
            if ((lepgold = findgold(mtmp->minvent))
                && (lepgold->quan > (ygold = 0, nh_money_invent()) /* lg */)) {
                
                appr = -1;
            }
        }
        if (NH_CELL_TRACK(omx, omy) && !should_see && ((spw & SPW_HASEYES) || (uwep && uwep->oartifact == ART_EXCALIBUR))) { /* the rare cell bit first: all terms pure */
            register coord *cp = gettrack(omx, omy);
            if (cp) {
                
                gx = cp->x;
                gy = cp->y;
            }
        }
    }

    if ((!mtmp->mpeaceful || !rn2_hot(10)) && !Is_rogue_level(&u.uz)) { /* the draw precedes the level test in the original */
        boolean in_line = FALSE;
        /* v1.8: in_line is only read when appr == 1. Otherwise lined_up() is still called when it could draw (Upolyd) or
           reach linedup()'s path tests (geometrically lined up within BOLT_LIM); in the remaining case its only effect is
           tbx/tby, set here exactly as linedup() sets them before returning FALSE. */
        if (Upolyd || nh_lined_geom(mtmp->mux - omx, mtmp->muy - omy, nh_dmu)) { /* v2.0: otherwise lined_up() is FALSE without draws, whatever appr */
            in_line = (lined_up(mtmp)
                   && (distmin(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy)
                       <= (throws_rocks(youmonst.data) ? 20 : acurrstr_hot() / 2 + 1)));
        } else {
            tbx = mtmp->mux - mtmp->mx;
            tby = mtmp->muy - mtmp->my;
        }
        if (appr != 1 || !in_line) {
            unsigned int sl = nh_spw_likes[mtmp->mnum]; /* lk; elig: mnum */
            if (sl & 64U) {
                register int load100 = (mload = curr_mon_load(mtmp)) * 100, ml = nh_spw_maxload[mtmp->mnum]; /* mi: pctload < k <=> load * 100 < k * maxload; elig: mnum */
                /* lk: the thresholds nest (50 < 75 < 85 < 95); each opens its bits by a mask, not a branch */
                lk = sl & (128U | ((0U - (unsigned int) (load100 < 95 * ml)) & 1U) | ((0U - (unsigned int) (load100 < 85 * ml)) & 0x24U)
                           | ((0U - (unsigned int) (load100 < 75 * ml)) & 0x0aU) | ((0U - (unsigned int) ((load100 < 50 * ml) & !Sokoban)) & 0x10U));
            } else /* no bit that reads the load: uses items alone opens no search, so the hiding bit is the only term left */
                lk = sl & 128U;
            setlikes = TRUE;
        }
    }

    {
        int minr = SQSRCHRADIUS; /* not too far away */

        /* cut down the search radius if it thinks character is closer. */
        if (nh_dmu < SQSRCHRADIUS /* dc */
            && !mtmp->mpeaceful)
            minr--;
        /* guards shouldn't get too distracted */
        if (!mtmp->mpeaceful && (spw & SPW_MERC))
            minr = 1;

        if ((lk & 0xb7U) /* any like but uses_items */
            && (!mt2_in_shop(omx, omy) || (!rn2_hot(25) && !mtmp->isshk))) {
            if (nh_mon_objsearch(mtmp, ptr, spw, omx, omy, &minr, &gx, &gy,
                                 (unsigned char) (lk | (ptr == &mons[PM_GELATINOUS_CUBE] ? 64U : 0U)), mload)) {
                
                mmoved = 3; /* actually unnecessary */
                goto postmov;
            }
        } else if (lk & 1U) {
            /* don't try to pick up anything else, but use the same loop */
            if (nh_mon_objsearch(mtmp, ptr, spw, omx, omy, &minr, &gx, &gy,
                                 (unsigned char) (1U | (ptr == &mons[PM_GELATINOUS_CUBE] ? 64U : 0U)), mload)) {
                
                mmoved = 3; /* actually unnecessary */
                goto postmov;
            }
        }

        if (minr < SQSRCHRADIUS && appr == -1) {
            if (distmin(omx, omy, mtmp->mux, mtmp->muy) <= 3) {
                gx = mtmp->mux;
                gy = mtmp->muy;
            } else
                appr = 1;
        }
    }

    if ((spw & SPW_TUNNELS) && !Is_rogue_level(&u.uz)) /* dw: pickless */
        can_tunnel = !((spw & SPW_NEEDSPICK) && (!mtmp->mpeaceful || Conflict)
                       && dist2(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy) <= 8);
    nix = omx;
    niy = omy;
    {
        /* doors are the one special terrain handled here (their rules below); a door cell keeps its pile/trap/gas bits (a scare scroll
           in a doorway must still reach the fallback); any other special cell, origin gas or temple → fallback */
        unsigned int bad;
        { /* win: the (nonrock, occupied) and (bad, door) windows in one pass over the three columns */
            __m128i w4 = nh_move_window4(cols, omy);
            unsigned long long bd = (unsigned long long) _mm_cvtsi128_si64(_mm_unpackhi_epi64(w4, w4));
            nw = (unsigned long long) _mm_cvtsi128_si64(w4);
            bad = (unsigned int) bd;
            door_n = (unsigned int) (bd >> 32);
        }
        if (bad) { /* xt */
            unsigned int pn = nh_move_neighbours(cols[0].pile, cols[1].pile, cols[2].pile, omy);
            unsigned int tn = nh_move_neighbours(cols[0].trap, cols[1].trap, cols[2].trap, omy);
            /* pile/trap must be the only reason a cell is bad: a gas or liquid/bars cell that also holds a trap or pile takes mfndpos */
            unsigned int sg = nh_move_neighbours((cols[0].special & ~cols[0].door) | cols[0].gas, (cols[1].special & ~cols[1].door) | cols[1].gas,
                                                 (cols[2].special & ~cols[2].door) | cols[2].gas, omy);
            if (!(bad & ~(pn | tn)) && !(bad & sg) && nh_xt_excl(mtmp, spw, omx, omy, pn | tn, &xt_excl))
                bad = 0;
        }
        origin_door = (cols[1].door >> omy) & 1U;
        plain = !bad && !((((cols[1].special & ~cols[1].door) | cols[1].gas) >> omy) & 1U) && !level.lflags.has_temple && !Sokoban
                && nh_dmu > 1; /* k4: distmin to the hero > 1 since eligibility; dc */
        if (plain && can_tunnel && nh_has_dig_tool(mtmp)) /* dd: a digger with a pick or axe can enter rock and pass doors: mfndpos decides */
            plain = 0;
    }
    if (plain) {
        
        /* mfndpos() for a window of plain floor: every per-cell test of the original is decided by the masks */
        unsigned int nonrock_n = (unsigned int) nw;
        unsigned int occ = (unsigned int) (nw >> 32);
        unsigned int cand = nonrock_n & ~occ & ~xt_excl; /* xt */
        if ((spw & SPW_UNICORN) && (!Invis || (spw & SPW_PERCEIVES))) /* uni: mfndpos NOTONL, cells in line with the hero */
            cand &= ~nh_online_mask(omx, omy);
        if (cand & door_n) {
            /* mfndpos(): a closed door needs OPENDOOR, a locked one UNLOCKDOOR, unless amorphous (can_fog excluded) or a doorbuster */
            unsigned int dbits = cand & door_n;
            if (!(spw & SPW_AMORPHOUS) && !doorbuster) {
                while (dbits) {
                    int b = __builtin_ctz(dbits), dm;
                    dbits &= dbits - 1;
                    dm = NH_TILE(omx - 1 + b / 3, omy - 1 + b % 3).rmflags;
                    if (dm & D_LOCKED) {
                        if (!unlock_known) { can_unlock = (can_open && monhaskey(mtmp, TRUE)); unlock_known = 1; }
                        if (!can_unlock) { cand &= ~(1U << b); continue; }
                    }
                    if ((dm & D_CLOSED) && !can_open)
                        cand &= ~(1U << b);
                }
            }
        }
        cand &= (unsigned int) (spw >> SPW_WINDOW_SHIFT); /* v2.3d: NODIAG = no diagonal candidates, the blocks below then see none; nd: a mask, not a branch */
        if ((cand & 0x145U) & ((0U - (unsigned int) origin_door) | door_n)) { /* v2.3d: one test, same as (diag && (origin door || diag door)) */
            /* mfndpos(): no diagonal move out of or into a door that is not broken away (or any door on the Rogue level) */
            boolean rogue = Is_rogue_level(&u.uz);
            if (origin_door && (rogue || (NH_TILE(omx, omy).rmflags & ~D_BROKEN))) {
                cand &= ~0x145U; /* dd2: no diagonal out of such a door */
            } else {
                unsigned int dbits = cand & 0x145U & door_n; /* dd2: otherwise only the diagonal doors decide */
                while (dbits) {
                    int b = __builtin_ctz(dbits);
                    dbits &= dbits - 1;
                    if (rogue || (NH_TILE(omx - 1 + b / 3, omy - 1 + b % 3).rmflags & ~D_BROKEN))
                        cand &= ~(1U << b);
                }
            }
        }
        { /* mb: no guard on diagonal candidates; the squeeze test fires on its own bits (sq) */
            unsigned int squeeze_n = nh_squeeze_diag[(~nonrock_n & 0xAAU) >> 1];
            unsigned int O = occ & 0xAAU, worm_n; /* win: a diagonal bit when both its orthogonal neighbours are occupied */
            if ((cand & squeeze_n) && cant_squeeze_thru(mtmp))
                cand &= ~squeeze_n;
            if (nh_any_worm) { /* without a long worm on the level worm_cross() is FALSE everywhere */
                worm_n = (((O >> 1) & 0x41U) | ((O << 1) & 0x104U)) & (((O >> 3) & 0x5U) | ((O << 3) & 0x140U)) & cand;
                while (worm_n) {
                    int b = __builtin_ctz(worm_n);
                    worm_n &= worm_n - 1;
                    if (worm_cross(omx, omy, omx - 1 + b / 3, omy - 1 + b % 3))
                        cand &= ~(1U << b);
                }
            }
        }
        cnt = __builtin_popcount(cand);
        {
            int best = nh_k4_pick(mtmp, cand, appr, omx, omy, gx, gy);
            if (best >= 0) {
                nix = (xchar) (omx + nh_win_dx[best]);
                niy = (xchar) (omy + nh_win_dy[best]);
                mmoved = 1;
            }
        }
        flag = ALLOW_U;
    } else {
        
        can_unlock = (can_open && monhaskey(mtmp, TRUE));
        unlock_known = 1;
        flag = mtmp->mpeaceful ? (ALLOW_SANCT | ALLOW_SSM) : ALLOW_U; /* Conflict is excluded */
        if (spw & SPW_MINION)
            flag |= ALLOW_SANCT;
        if (spw & SPW_PASSBARS)
            flag |= ALLOW_BARS;
        if ((spw & SPW_HUMAN) || ptr == &mons[PM_MINOTAUR])
            flag |= ALLOW_SSM;
        if ((spw & SPW_UNDEAD) && ptr->mlet != S_GHOST)
            flag |= NOGARLIC;
        if (spw & SPW_THROWSROCKS)
            flag |= ALLOW_ROCK;
        if (can_open)
            flag |= OPENDOOR;
        if (can_unlock)
            flag |= UNLOCKDOOR;
        if (doorbuster)
            flag |= BUSTDOOR;
        if (spw & SPW_UNICORN) /* uni: eligible only off noteleport levels, where mfndpos excludes cells in line with the hero */
            flag |= NOTONL;
        if (can_tunnel) /* dd */
            flag |= ALLOW_DIG;
        cnt = mfndpos(mtmp, poss, info, flag);
        {
            unsigned int fc = 0;
            int best;
            for (chi = 0; chi < cnt; chi++)
                fc |= 1U << (3 * (poss[chi].x - omx + 1) + (poss[chi].y - omy + 1));
            best = nh_k4_pick(mtmp, fc, appr, omx, omy, gx, gy);
            chi = -1;
            if (best >= 0) { /* mfndpos lists the window in bit order: the choice's rank is its index; its info bits stay */
                nix = (xchar) (omx + nh_win_dx[best]);
                niy = (xchar) (omy + nh_win_dy[best]);
                chi = __builtin_popcount(fc & ((1U << best) - 1U));
                mmoved = 1;
            }
        }
    }
    MT2_TRACE(mtmp, omx, omy, gx, gy, appr, cnt, poss, info, chi, nix, niy, mmoved);

    if (mmoved) {
        /* itsstuck(): not u.ustuck */
        if (mmoved == 1 && (spw & SPW_TUNNELS) && m_digweapon_check(mtmp, nix, niy)) {
            
            return 3;
        }
        if (!plain) { /* a plain window's info bits are 0 and it holds neither the hero's cell nor the believed one (both farther) */
            if (info[chi] & ALLOW_U) {
                
                nix = mtmp->mux;
                niy = mtmp->muy;
            }
            if (nix == u.ux && niy == u.uy) {
                
                mtmp->mux = u.ux;
                mtmp->muy = u.uy;
                return 0;
            }
            /* The monster may attack another based on 1 of 2 conditions:
             * 1 - It may be confused.
             * 2 - It may mistake the monster for your (displaced) image.
             * Pets get taken care of above and shouldn't reach this code.
             * Conflict gets handled even farther away (movemon()).
             */
            if ((info[chi] & ALLOW_M) || (nix == mtmp->mux && niy == mtmp->muy))
                return nh_mon_fight(mtmp, nix, niy);

            if ((info[chi] & ALLOW_MDISP))
                return nh_mon_displace(mtmp, nix, niy);
        }

        if (n_regions && !m_in_out_region(mtmp, nix, niy)) {
            
            return 3;
        }
        remove_monster(omx, omy);
        mtmp->mx = nix, mtmp->my = niy; /* place_monster() without its consistency diagnostics */
        NH_SET_MON(nix, niy, mtmp);
        mtmp->mstate &= ~(MON_OFFMAP | MON_MIGRATING | MON_LIMBO | MON_BUBBLEMOVE | MON_ENDGAME_FREE | MON_ENDGAME_MIGR);
        { /* mtrack as one little-endian word (as k4 reads it): the entries move up one, the old cell goes in front */
            unsigned long long mt;
            memcpy(&mt, &mtmp->mtrack[0], 8);
            mt = (mt << 16) | (unsigned long long) (((unsigned int) omy & 0xffU) << 8 | ((unsigned int) omx & 0xffU));
            memcpy(&mtmp->mtrack[0], &mt, 8);
        }
    } else if ((spw & SPW_UNICORN) && rn2(2) && !tele_restrict(mtmp)) { /* uni: a unicorn that cannot move teleports */
        
        (void) rloc(mtmp, TRUE);
        return 1;
    }
    /* !mmoved: worms are excluded */
 postmov:
    if (mmoved == 1 || mmoved == 3) {
        boolean canseeit = 0, trapped = 0; /* canseeit: cansee() here, read only at a door; nothing but mintrap() changes it before */

        if (mmoved == 1) {
            /* vampshifter door shift: excluded; nix, niy: the monster's cell until mintrap() */
            nh_newsym_moved_far(omx, omy, (struct monst *) 0); /* update the old position (remove_monster() emptied it) */
            if (NH_SUMMARY(nix, niy).move_trap_present) { /* mintrap() without a trap only clears mtrapped (already 0) */
                
                canseeit = cansee(nix, niy);
                trapped = 1;
                if (mintrap(mtmp) >= 2) {
                    
                    if (mtmp->mx)
                        newsym(mtmp->mx, mtmp->my);
                    return 2; /* it died */
                }
                ptr = mtmp->data; /* in case mintrap() caused polymorph */
                spw = nh_spw[mtmp->mnum];
                nix = mtmp->mx, niy = mtmp->my;
            }

            /* open a door, or crash through it, if 'mtmp' can */
            if (IS_DOOR(TYP_AT(nix, niy))
                && !(spw & SPW_PASSWALL) /* doesn't need to open doors */
                && !can_tunnel) {
                if (NH_TILE(nix, niy).doormask & (D_LOCKED | D_CLOSED | D_TRAPPED)) { /* od: nh_mon_door() is a no-op otherwise */
                    if (!unlock_known) { can_unlock = (can_open && monhaskey(mtmp, TRUE)); unlock_known = 1; }
                    if (!trapped)
                        canseeit = cansee(nix, niy);
                    if (nh_mon_door(mtmp, ptr, can_open, can_unlock, canseeit))
                        return 2;
                }
            } else if (TYP_AT(nix, niy) == IRONBARS) {
                
                /* As of 3.6.2: was using may_dig() but it doesn't handle bars */
                if (!(NH_TILE(mtmp->mx, mtmp->my).wall_info & W_NONDIGGABLE)
                    && (dmgtype(ptr, AD_RUST) || dmgtype(ptr, AD_CORR))) {
                    if (canseemon(mtmp))
                        pline("%s eats through the iron bars.", Monnam(mtmp));
                    
                    dissolve_bars(mtmp->mx, mtmp->my);
                    return 3;
                } else if (flags.verbose && canseemon(mtmp))
                    Norep("%s %s %s the iron bars.", Monnam(mtmp),
                          /* pluralization fakes verb conjugation */
                          makeplural(locomotion(ptr, "pass")),
                          (spw & SPW_PASSWALL) ? "through" : "between");
            }


            if (can_tunnel && (mdig_tunnel(mtmp)))
                return 2;
            /* u.uswallow: excluded */
            nh_newsym_moved_far(mtmp->mx, mtmp->my, mtmp); /* still on the level here (mintrap() < 2), so its cell holds it */
        }
        if (nh_move_obj_at(mtmp->mx, mtmp->my) && mtmp->mcanmove) {
            /* recompute the likes tests, in case we polymorphed
             * or if the "likegold" case got taken above */
            if (setlikes) {
                int pctload = (curr_mon_load(mtmp) * 100) / max_mon_load(mtmp);

                /* look for gold or jewels nearby */
                likegold = ((spw & SPW_LIKEGOLD) && pctload < 95);
                likegems = ((spw & SPW_LIKEGEMS) && pctload < 85);
                uses_items = (!(spw & (SPW_MINDLESS | SPW_ANIMAL)) && pctload < 75);
                likeobjs = ((spw & SPW_LIKEOBJS) && pctload < 75);
                likemagic = ((spw & SPW_LIKEMAGIC) && pctload < 85);
                likerock = ((spw & SPW_THROWSROCKS) && pctload < 50 && !Sokoban);
                conceals = (spw & SPW_HIDESUNDER) != 0;
            }

            /* Maybe a rock mole just ate some metal object */
            if (metallivorous(ptr)) {
                
                if (meatmetal(mtmp) == 2)
                    return 2; /* it died */
            }

            if (likegold && g_at(mtmp->mx, mtmp->my)) {
                
                mpickgold(mtmp);
            }

            /* Maybe a cube ate just about anything */
            if (ptr == &mons[PM_GELATINOUS_CUBE]) {
                
                if (meatobj(mtmp) == 2)
                    return 2; /* it died */
            }

            if (!mt2_in_shop(mtmp->mx, mtmp->my) || !rn2_hot(25)) { /* v1.9: same predicate */
                boolean picked = FALSE;

                

                if (likeobjs)
                    picked |= mpickstuff(mtmp, practical);
                if (likemagic)
                    picked |= mpickstuff(mtmp, magical);
                if (likerock)
                    picked |= mpickstuff(mtmp, boulder_class);
                if (likegems)
                    picked |= mpickstuff(mtmp, gem_class);
                if (uses_items)
                    picked |= mpickstuff(mtmp, (char *) 0);
                if (picked) {
                    
                    mmoved = 3;
                }
            }

            if (mtmp->minvis) {
                
                newsym(mtmp->mx, mtmp->my);
                if (mtmp->wormno)
                    see_wsegs(mtmp);
            }
        }
        if (spw & SPW_HIDEMOVE) { /* hd: one bit (spw follows a polymorph above) */
            /* Always set--or reset--mundetected if it's already hidden
               (just in case the object it was hiding under went away);
               usually set mundetected unless monster can't move.  */
            if (mtmp->mundetected
                || (mtmp->mcanmove && !mtmp->msleeping && rn2_hot(5)))
                { (void) hideunder(mtmp); }
            newsym(mtmp->mx, mtmp->my);
        }
        /* isshk: excluded */
    }
    return mmoved;
}

/* dochug() for an eligible monster: the pre-move chain is inert at this distance (PLAN.md KERNEL DECISIONS 2) */
static inline int
dochug_fast(mtmp, spw)
register struct monst *mtmp;
unsigned long long spw; /* nh_spw[] of mtmp's species, read by the eligibility test */
{
    register struct permonst *mdat = mtmp->data;
    register int tmp = 0;
    int inrange = 0, nearby = 0, scared = 0;

    
    if (head_engr) { /* wipe_engr_at() is a no-op without an engraving under the monster */
        
        if ((level.move_columns[mtmp->mx + 1].engr >> mtmp->my) & 1U) /* nh_engr_here() without its bounds test: the monster is on the map */
            wipe_engr_at(mtmp->mx, mtmp->my, 1, FALSE);
    }
    if (mtmp->mflee) { /* fl2: dochug's flee teleport and courage regain, in its order (conf/stun, m_respond species excluded) */
        if (!rn2_hot(40) && (spw & SPW_TPORT) && !mtmp->iswiz && !level.lflags.noteleport) {
            
            (void) rloc(mtmp, TRUE);
            return 0;
        }
        if (!mtmp->mfleetim && mtmp->mhp == mtmp->mhpmax && !rn2_hot(25)) {
            
            mtmp->mflee = 0;
        }
    }
    set_apparxy_hot(mtmp); /* believed hero position (original function: exact for Invis/Displaced) */
    int nh_d2mu = dist2(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy); /* dc */
    if (nh_d2mu > (BOLT_LIM * BOLT_LIM)) { /* fl: out of range, distfleeck() is its draw and three zeros */
        
        (void) rn2_hot(5);
        inrange = nearby = scared = 0;
    } else {
        
        distfleeck(mtmp, &inrange, &nearby, &scared); /* not adjacent: nearby = scared = 0; inrange feeds the wield block */
    }
    if (inrange)
        nh_mt2_hero_valid = 0; /* items, spells and attacks below may change hero properties */
    if (nh_d2mu <= 36) { /* dc */
        if (find_defensive(mtmp)) {
            
            if (use_defensive(mtmp) != 0) {
                
                return 1;
            }
        } else if (find_misc(mtmp)) {
            
            if (use_misc(mtmp) != 0) {
                
                return 1;
            }
        }
    } else {
        
        nh_muse_far_misc();
    } /* find_defensive() FALSE before any effect (dist2 > 25); find_misc() clears m.misc then FALSE (dist2 > 36) */
    if (spw & SPW_WATCH) { /* the watch may see the hero from afar: dochug()'s order (bribe cannot fire here) */
        
        watch_on_duty(mtmp);
    }

    return dochug_tail(mtmp, mdat, spw, 1, inrange, nearby, scared);
}

/* dochug() from the wield step on, shared by both paths; fast: a monster dochug_fast() took (it never has nearby set,
   so the bribe block, which dochug() runs earlier, cannot fire there) */
static inline int
dochug_tail(struct monst *mtmp, struct permonst *mdat, unsigned long long spw, int fast, int inrange, int nearby, int scared)
{
    register int tmp = 0;

#define TAIL_SPWQ(bit, orig) (fast ? (spw & (bit)) != 0 : SPWQ(mdat, bit, orig)) /* fast: the species word dochug_fast() carries */

    /* If monster is nearby you, and has to wield a weapon, do so.   This
     * costs the monster a move, of course.
     */
    if (inrange && (!mtmp->mpeaceful || Conflict) /* mb: the rarely true term first */
        && dist2(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy) <= 8
        && TAIL_SPWQ(SPW_WEAPATTK, attacktype(mdat, AT_WEAP))) {
        struct obj *mw_tmp;

        /* The scared check is necessary.  Otherwise a monster that is
         * one square near the player but fleeing into a wall would keep
         * switching between pick-axe and weapon.  If monster is stuck
         * in a trap, prefer ranged weapon (wielding is done in thrwmu).
         * This may cost the monster an attack, but keeps the monster
         * from switching back and forth if carrying both.
         */
        
        mw_tmp = MON_WEP(mtmp);
        if (!(scared && mw_tmp && is_pick(mw_tmp))
            && mtmp->weapon_check == NEED_WEAPON
            && !(mtmp->mtrapped && !nearby && select_rwep(mtmp))) {
            mtmp->weapon_check = NEED_HTH_WEAPON;
            if (mon_wield_item(mtmp) != 0) {
                
                return 0;
            }
        }
    }


    /*  Now the actual movement phase
     */

    if (!nearby || mtmp->mflee || scared || mtmp->mconf || mtmp->mstun
        || (mtmp->minvis && !rn2(3))
        || (mdat->mlet == S_LEPRECHAUN && !nh_money_invent() /* lg */
            && (findgold(mtmp->minvent) || rn2(2)))
        || (TAIL_SPWQ(SPW_WANDER, is_wanderer(mdat)) && !rn2(4)) || (Conflict && !mtmp->iswiz)
        || (!mtmp->mcansee && !rn2(4)) || mtmp->mpeaceful) {
        /* Possibly cast an undirected spell if not attacking you */
        /* note that most of the time castmu() will pick a directed
           spell and do nothing, so the monster moves normally */
        /* arbitrary distance restriction to keep monster far away
           from you from having cast dozens of sticks-to-snakes
           or similar spells by the time you reach it */
        if (((fast ? spw : nh_spw[mdat - mons]) & SPW_CASTER) && !mtmp->mspec_used /* mi: pure tests, the rarely true one first */
            && dist2(mtmp->mx, mtmp->my, u.ux, u.uy) <= 49) {
            struct attack *a;

            
            for (a = &mdat->mattk[0]; a < &mdat->mattk[NATTK]; a++) {
                if (a->aatyp == AT_MAGC
                    && (a->adtyp == AD_SPEL || a->adtyp == AD_CLRC)) {
                    if (castmu(mtmp, a, FALSE, FALSE)) {
                        
                        tmp = 3;
                        break;
                    }
                }
            }
        }

        tmp = fast ? m_move_fast(mtmp) : m_move(mtmp, 0);
        if (tmp != 2) {
            if (dist2(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy) > (BOLT_LIM * BOLT_LIM)) {
                
                (void) rn2_hot(5); /* distfleeck() recalc out of range: its draw and its three results */
                inrange = nearby = scared = 0;
            } else {
                
                nh_mt2_hero_valid = 0; /* the near tail (attacks) may change hero properties */
                distfleeck(mtmp, &inrange, &nearby, &scared); /* recalc */
            }
        }

        switch (tmp) { /* for pets, cases 0 and 3 are equivalent */
        case 0: /* no movement, but it can still attack you */
        case 3: /* absolutely no movement */
            /* vault guard might have vanished */
            if (mtmp->isgd && (DEADMONSTER(mtmp) || mtmp->mx == 0)) {
                
                return 1; /* behave as if it died */
            }
            /* During hallucination, monster appearance should
             * still change - even if it doesn't move.
             */
            if (Hallucination)
                newsym(mtmp->mx, mtmp->my);
            break;
        case 1: /* monster moved */
            /* Maybe it stepped on a trap and fell asleep... */
            if (mtmp->msleeping || !mtmp->mcanmove) {
                
                return 0;
            }
            /* Monsters can move and then shoot on same turn;
               our hero can't.  Is that fair? */
            if (!nearby && (TAIL_SPWQ(SPW_RANGED, ranged_attk(mdat))
                            || ((mtmp->mpeaceful || (nh_spw[mtmp->mnum] & (SPW_ANIMAL | SPW_MINDLESS | SPW_NOHANDS))) /* elig: mnum */
                                ? nh_find_offensive_none() : find_offensive(mtmp)))) { /* fo2: current form */
                
                break;
            }
            /* engulfer/grabber checks */
            if (mtmp == u.ustuck) {
                
                /* a monster that's digesting you can move at the
                 * same time -dlc
                 */
                if (u.uswallow)
                    return mattacku(mtmp);
                /* if confused grabber has wandered off, let go */
                if (distu(mtmp->mx, mtmp->my) > 2)
                    unstuck(mtmp);
            }
            return 0;
        case 2: /* monster died */
            return 1;
        }
    }

    if (!mtmp->mpeaceful || (Conflict && (!resist(mtmp, RING_CLASS, 0, 0)))) {
        if (inrange && !TAIL_SPWQ(SPW_NOATTACKS, noattacks(mdat))
            && (Upolyd ? u.mh : u.uhp) > 0 && !scared && tmp != 3) {
            
            if (mattacku(mtmp))
                return 1;
        }
        if (mtmp->wormno) {
            
            wormhitu(mtmp);
        }
    }
    if (!mtmp->msleeping && mtmp->mcanmove && nearby) {
        
        quest_talk(mtmp);
    }
    if (inrange && mtmp->data->msound == MS_CUSS && !mtmp->mpeaceful
        && couldsee(mtmp->mx, mtmp->my) && !mtmp->minvis && !rn2(5)) {
        
        cuss(mtmp);
    }

    return (tmp == 2);
#undef TAIL_SPWQ
}

boolean
m_digweapon_check(mtmp, nix, niy)
struct monst *mtmp;
xchar nix,niy;
{
    boolean can_tunnel = 0;
    struct obj *mw_tmp = MON_WEP(mtmp);

    if (!Is_rogue_level(&u.uz))
        can_tunnel = tunnels(mtmp->data);

    if (can_tunnel && needspick(mtmp->data) && !mwelded(mw_tmp)
        && (may_dig(nix, niy) || closed_door(nix, niy))) {
        /* may_dig() is either IS_STWALL or IS_TREE */
        if (closed_door(nix, niy)) {
            if (!mw_tmp
                || !is_pick(mw_tmp)
                || !is_axe(mw_tmp))
                mtmp->weapon_check = NEED_PICK_OR_AXE;
        } else if (IS_TREE(TYP_AT(nix, niy))) {
            if (!(mw_tmp = MON_WEP(mtmp)) || !is_axe(mw_tmp))
                mtmp->weapon_check = NEED_AXE;
        } else if (IS_STWALL(TYP_AT(nix, niy))) {
            if (!(mw_tmp = MON_WEP(mtmp)) || !is_pick(mw_tmp))
                mtmp->weapon_check = NEED_PICK_AXE;
        }
        if (mtmp->weapon_check >= NEED_PICK_AXE && mon_wield_item(mtmp))
            return TRUE;
    }
    return FALSE;
}

/* Return values:
 * 0: did not move, but can still attack and do other stuff.
 * 1: moved, possibly can attack.
 * 2: monster died.
 * 3: did not move, and can't do anything else either.
 */

/* Preserve fobj order. A mutation during iteration switches that iterator to
 * the original linked-list walk from prev->nobj; never rebuild under a cursor. */
void
nh_fobj_invalidate(void)
{
    nh_fobj_valid = 0;
    nh_fobj_generation++;
}

/* whether any byte of v & m is set */
static inline int
nh_any_byte(__m128i v, __m128i m)
{
#ifdef __SSE4_1__
    return !_mm_testz_si128(v, m);
#else
    return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(v, m), _mm_setzero_si128())) != 0xFFFF;
#endif
}

/* The index lives in the window [nh_fobj_lo, nh_fobj_lo + nh_fobj_count) parked
 * at the top of the arrays, so place_object (a chain prepend) is one store and
 * remove_object shifts only the newer prefix. Other chain edits still invalidate. */
static void
nh_fobj_build(void)
{
    struct obj *o;
    int n = 0;
    for (o = fobj; o; o = o->nobj) {
        if (n == 2048) { nh_fobj_count = -1; nh_fobj_valid = 1; return; }
        nh_fobj_ptr[n] = o;
        nh_fobj_x[n] = o->ox; nh_fobj_y[n] = o->oy;
        n++;
    }
    if (n && n < 2048) {
        memmove(nh_fobj_ptr + 2048 - n, nh_fobj_ptr, n * sizeof nh_fobj_ptr[0]);
        memmove(nh_fobj_x + 2048 - n, nh_fobj_x, n * sizeof nh_fobj_x[0]);
        memmove(nh_fobj_y + 2048 - n, nh_fobj_y, n * sizeof nh_fobj_y[0]);
    }
    nh_fobj_lo = 2048 - n; nh_fobj_count = n; nh_fobj_valid = 1;
    for (n = nh_fobj_lo; n < 2048; n++) nh_fobj_ptr[n]->nh_fidx = n;
}

/* Floor objects inside the box, in fobj order (ascending index): the search loop can then run its original body over them.
   Returns -1 when the index is unavailable (too many objects) or the box holds more than MT2_MAXOBJ objects: use the cursor. */
static int
mt2_box_objects(int lx, int hx, int ly, int hy, struct obj **list, unsigned char mm)
{
    static const unsigned char rows16[32] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    unsigned int cols = 0;
    int x, c, n = 0, i, j;
    struct obj *o, *t;
    const unsigned char *p = &nh_move_objcat[lx][ly];
    __m128i rm, z = _mm_setzero_si128(), any = z;
    if (!nh_fobj_valid) nh_fobj_build();
    if (nh_fobj_count < 0 || hx - lx > 2 * SQSRCHRADIUS || hy - ly > 15) return -1; /* v2.3a: larger boxes use the cursor */
    rm = _mm_and_si128(_mm_loadu_si128((const __m128i *) &rows16[15 - (hy - ly)]), _mm_set1_epi8((char) mm)); /* mm on the box's rows */
    _Static_assert(2 * SQSRCHRADIUS == 10, "the cases below cover every box width the guard above lets through");
#define NH_BOX_COL(k) case k: any = _mm_or_si128(any, _mm_loadu_si128((const __m128i *) (p + (k) * ROWNO)))
    switch (hx - lx) { /* the box's columns ORed first, one load each: most boxes hold no wanted category at all */
    NH_BOX_COL(10); /* FALLTHROUGH */
    NH_BOX_COL(9); /* FALLTHROUGH */
    NH_BOX_COL(8); /* FALLTHROUGH */
    NH_BOX_COL(7); /* FALLTHROUGH */
    NH_BOX_COL(6); /* FALLTHROUGH */
    NH_BOX_COL(5); /* FALLTHROUGH */
    NH_BOX_COL(4); /* FALLTHROUGH */
    NH_BOX_COL(3); /* FALLTHROUGH */
    NH_BOX_COL(2); /* FALLTHROUGH */
    NH_BOX_COL(1); /* FALLTHROUGH */
    NH_BOX_COL(0);
    }
#undef NH_BOX_COL
    if (!nh_any_byte(any, rm))
        return 0;
    /* then one test per column, highest first, shifting each column's bit in: bit c of cols is column lx + c */
#define NH_BOX_COL(k) case k: cols = (cols << 1) | (unsigned int) nh_any_byte(_mm_loadu_si128((const __m128i *) (p + (k) * ROWNO)), rm)
    switch (hx - lx) {
    NH_BOX_COL(10); /* FALLTHROUGH */
    NH_BOX_COL(9); /* FALLTHROUGH */
    NH_BOX_COL(8); /* FALLTHROUGH */
    NH_BOX_COL(7); /* FALLTHROUGH */
    NH_BOX_COL(6); /* FALLTHROUGH */
    NH_BOX_COL(5); /* FALLTHROUGH */
    NH_BOX_COL(4); /* FALLTHROUGH */
    NH_BOX_COL(3); /* FALLTHROUGH */
    NH_BOX_COL(2); /* FALLTHROUGH */
    NH_BOX_COL(1); /* FALLTHROUGH */
    NH_BOX_COL(0);
    }
#undef NH_BOX_COL
    while (cols) {
        unsigned int bits;
        c = __builtin_ctz(cols);
        cols &= cols - 1;
        x = lx + c;
        bits = 0xFFFFU & ~(unsigned int) _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(_mm_loadu_si128((const __m128i *) (p + c * ROWNO)), rm), z));
        while (bits) {
            int y = ly + __builtin_ctz(bits);
            bits &= bits - 1;
            o = level.objs[x][y];
            if (o && !o->nexthere) { /* a lone object is the cell's category byte, matched by the column compare */
                if (n == MT2_MAXOBJ) return -1;
                list[n++] = o;
                continue;
            }
            for (; o; o = o->nexthere) {
                if (!(nh_objcat(o) & mm)) continue;
                if (n == MT2_MAXOBJ) return -1;
                list[n++] = o;
            }
        }
    }
    for (i = 1; i < n; i++) { /* insertion sort by fobj position */
        t = list[i];
        for (j = i; j > 0 && list[j - 1]->nh_fidx > t->nh_fidx; j--) list[j] = list[j - 1];
        list[j] = t;
    }
    return n;
}

void __attribute__((noinline))
nh_fobj_placed(struct obj *o)
{
    nh_fobj_generation++;
    if (!nh_fobj_valid || nh_fobj_count < 0) return;
    if (nh_fobj_lo == 0) { nh_fobj_valid = 0; return; }
    nh_fobj_lo--;
    nh_fobj_ptr[nh_fobj_lo] = o;
    o->nh_fidx = nh_fobj_lo;
    nh_fobj_x[nh_fobj_lo] = o->ox; nh_fobj_y[nh_fobj_lo] = o->oy;
    nh_fobj_count++;
}

void
nh_fobj_removed(struct obj *o)
{
    int i, lo = nh_fobj_lo, hi;
    nh_fobj_generation++;
    if (!nh_fobj_valid || nh_fobj_count < 0) return;
    hi = lo + nh_fobj_count;
    for (i = lo; i < hi; i++)
        if (nh_fobj_ptr[i] == o) break;
    if (i == hi) { nh_fobj_valid = 0; return; }
    memmove(nh_fobj_ptr + lo + 1, nh_fobj_ptr + lo, (i - lo) * sizeof nh_fobj_ptr[0]);
    memmove(nh_fobj_x + lo + 1, nh_fobj_x + lo, (i - lo) * sizeof nh_fobj_x[0]);
    memmove(nh_fobj_y + lo + 1, nh_fobj_y + lo, (i - lo) * sizeof nh_fobj_y[0]);
    for (hi = lo + 1; hi <= i; hi++) nh_fobj_ptr[hi]->nh_fidx = hi;
    nh_fobj_lo++; nh_fobj_count--;
}

struct obj *
nh_fobj_next(struct nh_fobj_cursor *cursor, struct obj *prev,
             int lx, int hx, int ly, int hy)
{
    struct obj *o;
    if (!prev) {
        if (!nh_fobj_valid) nh_fobj_build();
        cursor->generation = nh_fobj_generation;
        cursor->index = nh_fobj_lo;
        cursor->mask = 0;
        cursor->fallback = nh_fobj_count < 0;
    } else if (cursor->generation != nh_fobj_generation) {
        cursor->fallback = 1;
    }
    if (cursor->fallback) {
        for (o = prev ? prev->nobj : fobj; o; o = o->nobj)
            if (o->ox >= lx && o->ox <= hx && o->oy >= ly && o->oy <= hy)
                return o;
        return (struct obj *) 0;
    }
#ifdef __SSE2__
    /* Reject sixteen byte coordinates at once; the first matching entry still wins. */
    if (lx >= -128 && hx <= 127 && ly >= -128 && hy <= 127
        && lx <= hx && ly <= hy) {
        if (cursor->mask && cursor->lx == lx && cursor->hx == hx
            && cursor->ly == ly && cursor->hy == hy) {
            int bit = __builtin_ctz(cursor->mask);
            int i = cursor->base + bit;
            cursor->mask &= ~(1U << bit);
            cursor->index = i + 1;
            return nh_fobj_ptr[i];
        }
        cursor->mask = 0;
        __m128i xl = _mm_set1_epi8((char) lx), xh = _mm_set1_epi8((char) hx);
        __m128i yl = _mm_set1_epi8((char) ly), yh = _mm_set1_epi8((char) hy);
        while (cursor->index + 16 <= nh_fobj_lo + nh_fobj_count) {
            int i = cursor->index;
            __m128i x = _mm_loadu_si128((const __m128i *) (nh_fobj_x + i));
            __m128i y = _mm_loadu_si128((const __m128i *) (nh_fobj_y + i));
            __m128i reject = _mm_or_si128(
                _mm_or_si128(_mm_cmpgt_epi8(xl, x), _mm_cmpgt_epi8(x, xh)),
                _mm_or_si128(_mm_cmpgt_epi8(yl, y), _mm_cmpgt_epi8(y, yh)));
            unsigned int mask = (~(unsigned int) _mm_movemask_epi8(reject)) & 65535U;
            if (mask) {
                int bit = __builtin_ctz(mask);
                cursor->base = i;
                cursor->mask = mask & ~(1U << bit);
                cursor->lx = lx; cursor->hx = hx;
                cursor->ly = ly; cursor->hy = hy;
                i += bit;
                cursor->index = i + 1;
                return nh_fobj_ptr[i];
            }
            cursor->index += 16;
        }
    }
#endif
    cursor->mask = 0;
    while (cursor->index < nh_fobj_lo + nh_fobj_count) {
        int i = cursor->index++;
        if (nh_fobj_x[i] >= lx && nh_fobj_x[i] <= hx
            && nh_fobj_y[i] >= ly && nh_fobj_y[i] <= hy)
            return nh_fobj_ptr[i];
    }
    return (struct obj *) 0;
}

/* Every floor object inside the (on-map) box in fobj order, from one pass over the index; the last block is read ending at the
   window's end with the entries already seen masked off. -1 when the index is unavailable or the box holds more than cap
   objects (walk the cursor instead) */
int
nh_fobj_box(int lx, int hx, int ly, int hy, struct obj **list, int cap)
{
#ifdef __SSE2__
    int i, b, end, n = 0;
    unsigned int mask;
    __m128i xl = _mm_set1_epi8((char) lx), xh = _mm_set1_epi8((char) hx);
    __m128i yl = _mm_set1_epi8((char) ly), yh = _mm_set1_epi8((char) hy);

    if (!nh_fobj_valid) nh_fobj_build();
    if (nh_fobj_count < 0) return -1;
    end = nh_fobj_lo + nh_fobj_count; /* the window is parked at the top of the 2048-entry arrays */
    for (i = nh_fobj_lo; i < end; i += 16) {
        b = i + 16 <= end ? i : end - 16;
        __m128i x = _mm_loadu_si128((const __m128i *) (nh_fobj_x + b));
        __m128i y = _mm_loadu_si128((const __m128i *) (nh_fobj_y + b));
        __m128i reject = _mm_or_si128(_mm_or_si128(_mm_cmpgt_epi8(xl, x), _mm_cmpgt_epi8(x, xh)),
                                      _mm_or_si128(_mm_cmpgt_epi8(yl, y), _mm_cmpgt_epi8(y, yh)));
        for (mask = ~(unsigned int) _mm_movemask_epi8(reject) & (0xffffU << (i - b)) & 0xffffU; mask; mask &= mask - 1) {
            if (n == cap) return -1;
            list[n++] = nh_fobj_ptr[b + __builtin_ctz(mask)];
        }
    }
    return n;
#else
    return -1;
#endif
}

int
m_move(mtmp, after)
register struct monst *mtmp;
register int after;
{
    register int appr;
    xchar gx, gy, nix, niy, chcnt;
    int chi; /* could be schar except for stupid Sun-2 compiler */
    boolean likegold = 0, likegems = 0, likeobjs = 0, likemagic = 0,
            conceals = 0;
    boolean likerock = 0, can_tunnel = 0;
    boolean can_open = 0, can_unlock = 0, doorbuster = 0;
    boolean uses_items = 0, setlikes = 0;
    boolean avoid = FALSE;
    boolean better_with_displacing = FALSE;
    boolean sawmon = is_vampshifter(mtmp) ? canspotmon(mtmp) : FALSE; /* before it moved; only the fog-cloud shift reads it */
    struct permonst *ptr;
    struct monst *mtoo;
    schar mmoved = 0; /* not strictly nec.: chi >= 0 will do */
    long info[9];
    long flag;
    int omx = mtmp->mx, omy = mtmp->my;

    if (mtmp->mtrapped) {
        int i = mintrap(mtmp);

        
        if (i >= 2) {
            
            newsym(mtmp->mx, mtmp->my);
            return 2;
        } /* it died */
        if (i == 1) {
            
            return 0; /* still in trap, so didn't move */
        }
    }
    ptr = mtmp->data; /* mintrap() can change mtmp->data -dlc */

    if (mtmp->meating) {
        
        mtmp->meating--;
        if (mtmp->meating <= 0)
            finish_meating(mtmp);
        return 3; /* still eating */
    }
    if (SPWQ(ptr, SPW_HIDESUNDER, hides_under(ptr)) && OBJ_AT(mtmp->mx, mtmp->my) && rn2(10)) {
        
        return 0; /* do not leave hiding place */
    }

    set_apparxy(mtmp);
    /* where does mtmp think you are? */
    /* Not necessary if m_move called from this file, but necessary in
     * other calls of m_move (ex. leprechauns dodging)
     */
    if (!Is_rogue_level(&u.uz))
        can_tunnel = SPWQ(ptr, SPW_TUNNELS, tunnels(ptr));
    can_open = !(SPWQ(ptr, SPW_NOHANDS, nohands(ptr)) || SPWQ(ptr, SPW_VERYSMALL, verysmall(ptr)));
    can_unlock =
        ((can_open && monhaskey(mtmp, TRUE)) || mtmp->iswiz || SPWQ(ptr, SPW_RIDER, is_rider(ptr)));
    doorbuster = SPWQ(ptr, SPW_GIANT, is_giant(ptr));
    if (mtmp->wormno) {
        
        goto not_special;
    }
    /* my dog gets special treatment */
    if (mtmp->mtame) {
        
        mmoved = dog_move(mtmp, after);
        goto postmov;
    }

    /* likewise for shopkeeper */
    if (mtmp->isshk) {
        
        mmoved = shk_move(mtmp);
        if (mmoved == -2)
            return 2;
        if (mmoved >= 0)
            goto postmov;
        
        mmoved = 0; /* follow player outside shop */
    }

    /* and for the guard */
    if (mtmp->isgd) {
        
        mmoved = gd_move(mtmp);
        if (mmoved == -2)
            return 2;
        if (mmoved >= 0)
            goto postmov;
        mmoved = 0;
    }

    /* and the acquisitive monsters get special treatment */
    if (SPWQ(ptr, SPW_COVETOUS, is_covetous(ptr)) || is_qnemesis(ptr)) {
        xchar tx = STRAT_GOALX(mtmp->mstrategy),
              ty = STRAT_GOALY(mtmp->mstrategy);
        struct monst *intruder = m_at(tx, ty);

        
        /*
         * if there's a monster on the object or in possession of it,
         * attack it.
         */
        if ((dist2(mtmp->mx, mtmp->my, tx, ty) < 2) && intruder
            && (intruder != mtmp)) {
            notonhead = (intruder->mx != tx || intruder->my != ty);
            
            if (mattackm(mtmp, intruder) == 2)
                return 2;
            mmoved = 1;
        } else
            mmoved = 0;
        goto postmov;
    }

    /* and for the priest */
    if (mtmp->ispriest) {
        
        mmoved = pri_move(mtmp);
        if (mmoved == -2)
            return 2;
        if (mmoved >= 0)
            goto postmov;
        mmoved = 0;
    }

#ifdef MAIL
    if (ptr == &mons[PM_MAIL_DAEMON]) {
        if (!Deaf && canseemon(mtmp))
            verbalize("I'm late!");
        mongone(mtmp);
        return 2;
    }
#endif

    /* teleport if that lies in our nature */
    if (ptr == &mons[PM_TENGU] && !rn2(5) && !mtmp->mcan
        && !tele_restrict(mtmp)) {
        
        if (mtmp->mhp < 7 || mtmp->mpeaceful || rn2(2))
            (void) rloc(mtmp, TRUE);
        else
            mnexto(mtmp);
        mmoved = 1;
        goto postmov;
    }
 not_special:
    if (u.uswallow && !mtmp->mflee && u.ustuck != mtmp) {
        
        return 1;
    }
    omx = mtmp->mx;
    omy = mtmp->my;
    gx = mtmp->mux;
    gy = mtmp->muy;
    appr = mtmp->mflee ? -1 : 1;
    if (mtmp->mconf || (u.uswallow && mtmp == u.ustuck)) {
        
        appr = 0;
    } else {
        struct obj *lepgold, *ygold;
        boolean should_see = (couldsee(omx, omy)
                              && (NH_TILE(gx, gy).lit || !NH_TILE(omx, omy).lit)
                              && (dist2(omx, omy, gx, gy) <= 36));

        if ((!mtmp->mcansee)
            || (should_see && Invis && !SPWQ(ptr, SPW_PERCEIVES, perceives(ptr)) && rn2(11))
            || ((is_obj_mappear(&youmonst,STRANGE_OBJECT) || u.uundetected
                 || (is_obj_mappear(&youmonst,GOLD_PIECE) && !SPWQ(ptr, SPW_LIKEGOLD, likes_gold(ptr)))))
            || (mtmp->mpeaceful && !mtmp->isshk) /* allow shks to follow */
            || ((monsndx(ptr) == PM_STALKER || ptr->mlet == S_BAT
                 || ptr->mlet == S_LIGHT) && !rn2(3)))
            appr = 0;

        if (monsndx(ptr) == PM_LEPRECHAUN && (appr == 1)
            && ((lepgold = findgold(mtmp->minvent))
                && (lepgold->quan
                    > (ygold = 0, nh_money_invent()) /* lg */))) {
            
            appr = -1;
        }

        if (!should_see && can_track(ptr)) {
            register coord *cp;

            cp = gettrack(omx, omy);
            if (cp) {
                
                gx = cp->x;
                gy = cp->y;
            }
        }
    }

    if ((!mtmp->mpeaceful || !rn2_hot(10)) && (!Is_rogue_level(&u.uz))) {
        boolean in_line = (lined_up(mtmp)
               && (distmin(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy)
                   <= (throws_rocks(youmonst.data) ? 20 : ACURRSTR / 2 + 1)));

        if (appr != 1 || !in_line) {
            /* Monsters in combat won't pick stuff up, avoiding the
             * situation where you toss arrows at it and it has nothing
             * better to do than pick the arrows up.
             */
            register int pctload =
                (curr_mon_load(mtmp) * 100) / max_mon_load(mtmp);

            /* look for gold or jewels nearby */
            likegold = (SPWQ(ptr, SPW_LIKEGOLD, likes_gold(ptr)) && pctload < 95);
            likegems = (SPWQ(ptr, SPW_LIKEGEMS, likes_gems(ptr)) && pctload < 85);
            uses_items = (!SPWQ(ptr, SPW_MINDLESS, mindless(ptr)) && !SPWQ(ptr, SPW_ANIMAL, is_animal(ptr)) && pctload < 75);
            likeobjs = (SPWQ(ptr, SPW_LIKEOBJS, likes_objs(ptr)) && pctload < 75);
            likemagic = (SPWQ(ptr, SPW_LIKEMAGIC, likes_magic(ptr)) && pctload < 85);
            likerock = (SPWQ(ptr, SPW_THROWSROCKS, throws_rocks(ptr)) && pctload < 50 && !Sokoban);
            conceals = SPWQ(ptr, SPW_HIDESUNDER, hides_under(ptr));
            setlikes = TRUE;
        }
    }
    if (appr == 0) MT2C(C_APPR0);
    if (likegold || likegems || likeobjs || likemagic || likerock || conceals) MT2C(C_INTEREST);
    if (uses_items) MT2C(C_USESITEMS);

#define SQSRCHRADIUS 5

    {
        int minr = SQSRCHRADIUS; /* not too far away */

        /* cut down the search radius if it thinks character is closer. */
        if (distmin(mtmp->mux, mtmp->muy, omx, omy) < SQSRCHRADIUS
            && !mtmp->mpeaceful)
            minr--;
        /* guards shouldn't get too distracted */
        if (!mtmp->mpeaceful && SPWQ(ptr, SPW_MERC, is_mercenary(ptr)))
            minr = 1;

        if ((likegold || likegems || likeobjs || likemagic || likerock
             || conceals) && (!*in_rooms(omx, omy, SHOPBASE)
                              || (!rn2(25) && !mtmp->isshk))) {
            if (nh_mon_objsearch(mtmp, ptr, nh_spw[ptr - mons], omx, omy, &minr, &gx, &gy, NH_LIKES_MM, -1)) {
                
                mmoved = 3; /* actually unnecessary */
                goto postmov;
            }
        } else if (likegold) {
            /* don't try to pick up anything else, but use the same loop */
            uses_items = 0;
            likegems = likeobjs = likemagic = likerock = conceals = 0;
            if (nh_mon_objsearch(mtmp, ptr, nh_spw[ptr - mons], omx, omy, &minr, &gx, &gy, NH_LIKES_MM, -1)) {
                
                mmoved = 3; /* actually unnecessary */
                goto postmov;
            }
        }

        if (minr < SQSRCHRADIUS && appr == -1) {
            if (distmin(omx, omy, mtmp->mux, mtmp->muy) <= 3) {
                gx = mtmp->mux;
                gy = mtmp->muy;
            } else
                appr = 1;
        }
    }

    /* don't tunnel if hostile and close enough to prefer a weapon */
    if (can_tunnel && SPWQ(ptr, SPW_NEEDSPICK, needspick(ptr))
        && ((!mtmp->mpeaceful || Conflict)
            && dist2(mtmp->mx, mtmp->my, mtmp->mux, mtmp->muy) <= 8))
        can_tunnel = FALSE;

    nix = omx;
    niy = omy;
    flag = 0L;
    if (mtmp->mpeaceful && (!Conflict || (resist(mtmp, RING_CLASS, 0, 0))))
        flag |= (ALLOW_SANCT | ALLOW_SSM);
    else
        flag |= ALLOW_U;
    if (SPWQ(ptr, SPW_MINION, is_minion(ptr)) || SPWQ(ptr, SPW_RIDER, is_rider(ptr)))
        flag |= ALLOW_SANCT;
    /* unicorn may not be able to avoid hero on a noteleport level */
    if (SPWQ(ptr, SPW_UNICORN, is_unicorn(ptr)) && !level.lflags.noteleport)
        flag |= NOTONL;
    if (SPWQ(ptr, SPW_PASSWALL, passes_walls(ptr)))
        flag |= (ALLOW_WALL | ALLOW_ROCK);
    if (SPWQ(ptr, SPW_PASSBARS, passes_bars(ptr)))
        flag |= ALLOW_BARS;
    if (can_tunnel)
        flag |= ALLOW_DIG;
    if (SPWQ(ptr, SPW_HUMAN, is_human(ptr)) || ptr == &mons[PM_MINOTAUR])
        flag |= ALLOW_SSM;
    if ((SPWQ(ptr, SPW_UNDEAD, is_undead(ptr)) && ptr->mlet != S_GHOST) || is_vampshifter(mtmp))
        flag |= NOGARLIC;
    if (SPWQ(ptr, SPW_THROWSROCKS, throws_rocks(ptr)))
        flag |= ALLOW_ROCK;
    if (can_open)
        flag |= OPENDOOR;
    if (can_unlock)
        flag |= UNLOCKDOOR;
    if (doorbuster)
        flag |= BUSTDOOR;
    {
        register int i, j, nx, ny, nearer;
        unsigned int trackmask;
        int jcnt, cnt;
        int ndist, nidist;
        register coord *mtrk;
        coord poss[9];

        cnt = mfndpos(mtmp, poss, info, flag);
        MT2C(C_CNT0 + (cnt > 8 ? 8 : cnt));
        chcnt = 0;
        jcnt = min(MTSZ, cnt - 1);
        trackmask = 0; /* neighbourhood bits of the recent track: the avoidance loop (and its RNG draw) only runs on a hit */
        for (j = 0; j < jcnt; j++)
            trackmask |= nh_move_position_bit(omx, omy, mtmp->mtrack[j].x, mtmp->mtrack[j].y);
        chi = -1;
        nidist = dist2(nix, niy, gx, gy);
        /* allow monsters be shortsighted on some levels for balance */
        if (!mtmp->mpeaceful && level.lflags.shortsighted
            && nidist > (couldsee(nix, niy) ? 144 : 36) && appr == 1)
            appr = 0;
        if (SPWQ(ptr, SPW_UNICORN, is_unicorn(ptr)) && level.lflags.noteleport) {
            /* on noteleport levels, perhaps we cannot avoid hero */
            for (i = 0; i < cnt; i++)
                if (!(info[i] & NOTONL)) {
                    
                    avoid = TRUE;
                }
        }
        better_with_displacing =
            should_displace(mtmp, poss, info, cnt, gx, gy);
        for (i = 0; i < cnt; i++) {
            if (avoid && (info[i] & NOTONL))
                continue;
            nx = poss[i].x;
            ny = poss[i].y;

            if ((info[i] & ALLOW_MDISP)
                && !(info[i] & ALLOW_M) && !better_with_displacing) {
                
                continue;
            }
            if (appr != 0 && (trackmask & nh_move_position_bit(omx, omy, nx, ny))) {
                MT2C(C_TRACKHIT);
                /* same draws in the same order: one rn2 per matching track entry, ascending, until the first nonzero */
                unsigned int hits = 0;
                mtrk = &mtmp->mtrack[0];
                for (j = 0; j < MTSZ; j++)
                    hits |= (unsigned int) ((j < jcnt) & (nx == mtrk[j].x) & (ny == mtrk[j].y)) << j;
                while (hits) {
                    j = __builtin_ctz(hits);
                    hits &= hits - 1;
                    if (rn2_hot(4 * (cnt - j)))
                        goto nxti;
                }
            }

            ndist = dist2(nx, ny, gx, gy);
            nearer = (ndist < nidist);
            {
                /* The original predicate without its data-dependent branch: appr 1 takes a nearer cell, -1 a farther one,
                   0 draws rn2(++chcnt) for every candidate exactly as before, and the first candidate is always taken. */
                int take = appr ? ((nearer != (appr < 0)) | !mmoved) : (!rn2_hot(++chcnt) | !mmoved);
                int m = -take;
                nix = (xchar) ((nx & m) | (nix & ~m));
                niy = (xchar) ((ny & m) | (niy & ~m));
                nidist = (ndist & m) | (nidist & ~m);
                chi = (i & m) | (chi & ~m);
                mmoved |= (schar) take;
            }
 nxti:
            ;
        }
        MT2_TRACE(mtmp, omx, omy, gx, gy, appr, cnt, poss, info, chi, nix, niy, mmoved);
    }

    if (mmoved) MT2C(C_MOVED); else MT2C(C_NOMOVE);
    if (mmoved) {
        register int j;

        if (mmoved == 1 && (u.ux != nix || u.uy != niy) && itsstuck(mtmp)) {
            
            return 3;
        }

        if (mmoved == 1 && m_digweapon_check(mtmp, nix,niy)) {
            
            return 3;
        }

        /* If ALLOW_U is set, either it's trying to attack you, or it
         * thinks it is.  In either case, attack this spot in preference to
         * all others.
         */
        /* Actually, this whole section of code doesn't work as you'd expect.
         * Most attacks are handled in dochug().  It calls distfleeck(), which
         * among other things sets nearby if the monster is near you--and if
         * nearby is set, we never call m_move unless it is a special case
         * (confused, stun, etc.)  The effect is that this ALLOW_U (and
         * mfndpos) has no effect for normal attacks, though it lets a
         * confused monster attack you by accident.
         */
        if (info[chi] & ALLOW_U) {
            
            nix = mtmp->mux;
            niy = mtmp->muy;
        }
        if (nix == u.ux && niy == u.uy) {
            
            mtmp->mux = u.ux;
            mtmp->muy = u.uy;
            return 0;
        }
        /* The monster may attack another based on 1 of 2 conditions:
         * 1 - It may be confused.
         * 2 - It may mistake the monster for your (displaced) image.
         * Pets get taken care of above and shouldn't reach this code.
         * Conflict gets handled even farther away (movemon()).
         */
        if ((info[chi] & ALLOW_M) || (nix == mtmp->mux && niy == mtmp->muy))
            return nh_mon_fight(mtmp, nix, niy);

        if ((info[chi] & ALLOW_MDISP))
            return nh_mon_displace(mtmp, nix, niy);

        if (!m_in_out_region(mtmp, nix, niy)) {
            
            return 3;
        }

        remove_monster(omx, omy);
        place_monster(mtmp, nix, niy);
        for (j = MTSZ - 1; j > 0; j--)
            mtmp->mtrack[j] = mtmp->mtrack[j - 1];
        mtmp->mtrack[0].x = omx;
        mtmp->mtrack[0].y = omy;
        /* Place a segment at the old position. */
        if (mtmp->wormno) {
            
            worm_move(mtmp);
        }
    } else {
        if (SPWQ(ptr, SPW_UNICORN, is_unicorn(ptr)) && rn2(2) && !tele_restrict(mtmp)) {
            
            (void) rloc(mtmp, TRUE);
            return 1;
        }
        if (mtmp->wormno) {
            
            worm_nomove(mtmp);
        }
    }
 postmov:
    if (mmoved == 3) MT2C(C_MMOVED3);
    if (mmoved == 1 || mmoved == 3) {
        boolean canseeit = cansee(mtmp->mx, mtmp->my);

        if (mmoved == 1) {
            /* normal monster move will already have <nix,niy>,
               but pet dog_move() with 'goto postmov' won't */
            nix = mtmp->mx, niy = mtmp->my;
            /* sequencing issue:  when monster movement decides that a
               monster can move to a door location, it moves the monster
               there before dealing with the door rather than after;
               so a vampire/bat that is going to shift to fog cloud and
               pass under the door is already there but transformation
               into fog form--and its message, when in sight--has not
               happened yet; we have to move monster back to previous
               location before performing the vamp_shift() to make the
               message happen at right time, then back to the door again
               [if we did the shift above, before moving the monster,
               we would need to duplicate it in dog_move()...] */
            if (is_vampshifter(mtmp) && !SPWQ(mtmp->data, SPW_AMORPHOUS, amorphous(mtmp->data))
                && IS_DOOR(TYP_AT(nix, niy))
                && ((NH_TILE(nix, niy).doormask & (D_LOCKED | D_CLOSED)) != 0)
                && can_fog(mtmp)) {
                
                if (sawmon) {
                    remove_monster(nix, niy);
                    place_monster(mtmp, omx, omy);
                    newsym(nix, niy), newsym(omx, omy);
                }
                if (vamp_shift(mtmp, &mons[PM_FOG_CLOUD], sawmon)) {
                    ptr = mtmp->data; /* update cached value */
                }
                if (sawmon) {
                    remove_monster(omx, omy);
                    place_monster(mtmp, nix, niy);
                    newsym(omx, omy), newsym(nix, niy);
                }
            }

            if (NH_SUMMARY(mtmp->mx, mtmp->my).move_trap_present) MT2C(C_DESTTRAP);
            if (IS_DOOR(TYP_AT(mtmp->mx, mtmp->my))) MT2C(C_DESTDOOR);
            if (nh_move_obj_at(mtmp->mx, mtmp->my)) MT2C(C_DESTOBJ);
            nh_newsym_moved(omx, omy); /* update the old position */
            if (mintrap(mtmp) >= 2) {
                
                if (mtmp->mx)
                    newsym(mtmp->mx, mtmp->my);
                return 2; /* it died */
            }
            ptr = mtmp->data; /* in case mintrap() caused polymorph */

            /* open a door, or crash through it, if 'mtmp' can */
            if (IS_DOOR(TYP_AT(mtmp->mx, mtmp->my))
                && !SPWQ(ptr, SPW_PASSWALL, passes_walls(ptr)) /* doesn't need to open doors */
                && !can_tunnel) {
                if (nh_mon_door(mtmp, ptr, can_open, can_unlock, canseeit))
                    return 2;
            } else if (TYP_AT(mtmp->mx, mtmp->my) == IRONBARS) {
                
                /* As of 3.6.2: was using may_dig() but it doesn't handle bars */
                if (!(NH_TILE(mtmp->mx, mtmp->my).wall_info & W_NONDIGGABLE)
                    && (dmgtype(ptr, AD_RUST) || dmgtype(ptr, AD_CORR))) {
                    if (canseemon(mtmp))
                        pline("%s eats through the iron bars.", Monnam(mtmp));
                    
                    dissolve_bars(mtmp->mx, mtmp->my);
                    return 3;
                } else if (flags.verbose && canseemon(mtmp))
                    Norep("%s %s %s the iron bars.", Monnam(mtmp),
                          /* pluralization fakes verb conjugation */
                          makeplural(locomotion(ptr, "pass")),
                          SPWQ(ptr, SPW_PASSWALL, passes_walls(ptr)) ? "through" : "between");
            }

            /* possibly dig */
            if (can_tunnel && (mdig_tunnel(mtmp)))
                return 2; /* mon died (position already updated) */

            /* set also in domove(), hack.c */
            if (u.uswallow && mtmp == u.ustuck
                && (mtmp->mx != omx || mtmp->my != omy)) {
                /* If the monster moved, then update */
                u.ux0 = u.ux;
                u.uy0 = u.uy;
                u.ux = mtmp->mx;
                u.uy = mtmp->my;
                
                swallowed(0);
            } else
                nh_newsym_moved(mtmp->mx, mtmp->my);
        }
        if (nh_move_obj_at(mtmp->mx, mtmp->my) && mtmp->mcanmove) {
            /* recompute the likes tests, in case we polymorphed
             * or if the "likegold" case got taken above */
            if (setlikes) {
                int pctload = (curr_mon_load(mtmp) * 100) / max_mon_load(mtmp);

                /* look for gold or jewels nearby */
                likegold = (SPWQ(ptr, SPW_LIKEGOLD, likes_gold(ptr)) && pctload < 95);
                likegems = (SPWQ(ptr, SPW_LIKEGEMS, likes_gems(ptr)) && pctload < 85);
                uses_items =
                    (!SPWQ(ptr, SPW_MINDLESS, mindless(ptr)) && !SPWQ(ptr, SPW_ANIMAL, is_animal(ptr)) && pctload < 75);
                likeobjs = (SPWQ(ptr, SPW_LIKEOBJS, likes_objs(ptr)) && pctload < 75);
                likemagic = (SPWQ(ptr, SPW_LIKEMAGIC, likes_magic(ptr)) && pctload < 85);
                likerock = (SPWQ(ptr, SPW_THROWSROCKS, throws_rocks(ptr)) && pctload < 50 && !Sokoban);
                conceals = SPWQ(ptr, SPW_HIDESUNDER, hides_under(ptr));
            }

            /* Maybe a rock mole just ate some metal object */
            if (metallivorous(ptr)) {
                
                if (meatmetal(mtmp) == 2)
                    return 2; /* it died */
            }

            if (likegold && g_at(mtmp->mx, mtmp->my)) {
                
                mpickgold(mtmp);
            }

            /* Maybe a cube ate just about anything */
            if (ptr == &mons[PM_GELATINOUS_CUBE]) {
                
                if (meatobj(mtmp) == 2)
                    return 2; /* it died */
            }

            if (!*in_rooms(mtmp->mx, mtmp->my, SHOPBASE) || !rn2(25)) {
                boolean picked = FALSE;

                

                if (likeobjs)
                    picked |= mpickstuff(mtmp, practical);
                if (likemagic)
                    picked |= mpickstuff(mtmp, magical);
                if (likerock)
                    picked |= mpickstuff(mtmp, boulder_class);
                if (likegems)
                    picked |= mpickstuff(mtmp, gem_class);
                if (uses_items)
                    picked |= mpickstuff(mtmp, (char *) 0);
                if (picked) {
                    
                    mmoved = 3;
                }
            }

            if (mtmp->minvis) {
                
                newsym(mtmp->mx, mtmp->my);
                if (mtmp->wormno)
                    see_wsegs(mtmp);
            }
        }

        if (SPWQ(ptr, SPW_HIDESUNDER, hides_under(ptr)) || ptr->mlet == S_EEL) {
            /* Always set--or reset--mundetected if it's already hidden
               (just in case the object it was hiding under went away);
               usually set mundetected unless monster can't move.  */
            if (mtmp->mundetected
                || (mtmp->mcanmove && !mtmp->msleeping && rn2(5)))
                { (void) hideunder(mtmp); }
            newsym(mtmp->mx, mtmp->my);
        }
        if (mtmp->isshk) {
            
            after_shk_move(mtmp);
        }
    }
    return mmoved;
}

void
dissolve_bars(x, y)
register int x, y;
{
    SET_TYP_XY(x, y, (Is_special(&u.uz) || *in_rooms(x, y, 0)) ? ROOM : CORR);
    NH_TILE(x, y).rmflags = 0;
    newsym(x, y);
}

boolean
closed_door(x, y)
register int x, y;
{
    return (boolean) (IS_DOOR(TYP_AT(x, y))
                      && (NH_TILE(x, y).doormask & (D_LOCKED | D_CLOSED)));
}

boolean
accessible(x, y)
register int x, y;
{
    int levtyp = TYP_AT(x, y);

    /* use underlying terrain in front of closed drawbridge */
    if (levtyp == DRAWBRIDGE_UP)
        levtyp = db_under_typ(NH_TILE(x, y).drawbridgemask);

    return (boolean) (ACCESSIBLE(levtyp) && !closed_door(x, y));
}

/* set_apparxy()'s body, inlined in the movemon and m_move_fast paths */
STATIC_OVL void
set_apparxy_hot(mtmp)
register struct monst *mtmp;
{
    boolean notseen;
    register int disp, mx = mtmp->mux, my = mtmp->muy;

    /*
     * do cheapest and/or most likely tests first
     */

    /* monsters which know where you are don't suddenly forget,
       if you haven't moved away (first: found_you would store the same position, as it does for the two tests below) */
    if (mx == u.ux && my == u.uy)
        return;

    /* pet knows your smell; grabber still has hold of you */
    if (mtmp->mtame || mtmp == u.ustuck)
        goto found_you;

    notseen = (!mtmp->mcansee || (Invis && !SPWQ(mtmp->data, SPW_PERCEIVES, perceives(mtmp->data))));
    /* add cases as required.  eg. Displacement ... */
    if (notseen || Underwater) {
        /* Xorns can smell quantities of valuable metal
            like that in solid gold coins, treat as seen */
        if ((mtmp->data == &mons[PM_XORN]) && money_cnt(invent) && !Underwater)
            disp = 0;
        else
            disp = 1;
    } else if (Displaced) {
        disp = couldsee(mx, my) ? 2 : 1;
    } else
        disp = 0;
    if (disp) {
        set_apparxy_disp(mtmp, disp, notseen);
        return;
    }
 found_you:
    mtmp->mux = u.ux;
    mtmp->muy = u.uy;
}

/* set_apparxy()'s guess for an unseen or displaced hero: one shared copy out of line */
STATIC_OVL void
set_apparxy_disp(struct monst *mtmp, int disp, int notseen)
{
    boolean gotu;
    int mx, my;

    /* without something like the following, invisibility and displacement
       are too powerful */
    gotu = notseen ? !rn2_hot(3) : Displaced ? !rn2_hot(4) : FALSE;

    if (!gotu) {
        register int try_cnt = 0;

        do {
            if (++try_cnt > 200)
                goto found_you; /* punt */
            mx = u.ux - disp + rn2_hot(2 * disp + 1);
            my = u.uy - disp + rn2_hot(2 * disp + 1);
        } while (!isok(mx, my)
                 || (disp != 2 && mx == mtmp->mx && my == mtmp->my)
                 || ((mx != u.ux || my != u.uy) && !SPWQ(mtmp->data, SPW_PASSWALL, passes_walls(mtmp->data))
                     && !(accessible(mx, my)
                          || (closed_door(mx, my)
                              && (can_ooze(mtmp) || can_fog(mtmp)))))
                 || !couldsee(mx, my));
    } else {
 found_you:
        mx = u.ux;
        my = u.uy;
    }

    mtmp->mux = mx;
    mtmp->muy = my;
}

/* decide where the monster thinks you are standing */
void __attribute__((noinline))
set_apparxy(mtmp)
register struct monst *mtmp;
{
    set_apparxy_hot(mtmp);
}

/*
 * mon-to-mon displacement is a deliberate "get out of my way" act,
 * not an accidental bump, so we don't consider mstun or mconf in
 * undesired_disp().
 *
 * We do consider many other things about the target and its
 * location however.
 */
boolean
undesirable_disp(mtmp, x, y)
struct monst *mtmp; /* barging creature */
xchar x, y; /* spot 'mtmp' is considering moving to */
{
    boolean is_pet = (mtmp && mtmp->mtame && !mtmp->isminion);
    struct trap *trap = t_at(x, y);

    if (is_pet) {
        /* Pets avoid a trap if you've seen it usually. */
        if (trap && trap->tseen && rn2(40))
            return TRUE;
        /* Pets avoid cursed locations */
        if (cursed_object_at(x, y))
            return TRUE;

    /* Monsters avoid a trap if they've seen that type before */
    } else if (trap && rn2(40)
               && (mtmp->mtrapseen & (1 << (trap->ttyp - 1))) != 0) {
        return TRUE;
    }

    /* oversimplification:  creatures that bargethrough can't swap places
       when target monster is in rock or closed door or water (in particular,
       avoid moving to spots where mondied() won't leave a corpse; doesn't
       matter whether barger is capable of moving to such a target spot if
       it were unoccupied) */
    if (!accessible(x, y)
        /* mondied() allows is_pool() as an exception to !accessible(),
           but we'll only do that if 'mtmp' is already at a water location
           so that we don't swap a water critter onto land */
        && !(is_pool(x, y) && is_pool(mtmp->mx, mtmp->my)))
        return TRUE;

    return FALSE;
}

/*
 * Inventory prevents passage under door.
 * Used by can_ooze() and can_fog().
 */
STATIC_OVL boolean
stuff_prevents_passage(mtmp)
struct monst *mtmp;
{
    struct obj *chain, *obj;

    if (mtmp == &youmonst) {
        chain = invent;
    } else {
        chain = mtmp->minvent;
    }
    for (obj = chain; obj; obj = obj->nobj) {
        int typ = obj->otyp;

        if (typ == COIN_CLASS && obj->quan > 100L)
            return TRUE;
        if (obj->oclass != GEM_CLASS && !(typ >= ARROW && typ <= BOOMERANG)
            && !(typ >= DAGGER && typ <= CRYSKNIFE) && typ != SLING
            && !is_cloak(obj) && typ != FEDORA && !is_gloves(obj)
            && typ != LEATHER_JACKET && typ != CREDIT_CARD && !is_shirt(obj)
            && !(typ == CORPSE && verysmall(&mons[obj->corpsenm]))
            && typ != FORTUNE_COOKIE && typ != CANDY_BAR && typ != PANCAKE
            && typ != LEMBAS_WAFER && typ != LUMP_OF_ROYAL_JELLY
            && obj->oclass != AMULET_CLASS && obj->oclass != RING_CLASS
            && obj->oclass != VENOM_CLASS && typ != SACK
            && typ != BAG_OF_HOLDING && typ != BAG_OF_TRICKS
            && !Is_candle(obj) && typ != OILSKIN_SACK && typ != LEASH
            && typ != STETHOSCOPE && typ != BLINDFOLD && typ != TOWEL
            && typ != TIN_WHISTLE && typ != MAGIC_WHISTLE
            && typ != MAGIC_MARKER && typ != TIN_OPENER && typ != SKELETON_KEY
            && typ != LOCK_PICK)
            return TRUE;
        if (Is_container(obj) && obj->cobj)
            return TRUE;
    }
    return FALSE;
}

boolean
can_ooze(mtmp)
struct monst *mtmp;
{
    if (!amorphous(mtmp->data) || stuff_prevents_passage(mtmp))
        return FALSE;
    return TRUE;
}

/* monster can change form into a fog if necessary */
boolean
can_fog(mtmp)
struct monst *mtmp;
{
    if (!(mvitals[PM_FOG_CLOUD].mvflags & G_GENOD) && is_vampshifter(mtmp)
        && !Protection_from_shape_changers && !stuff_prevents_passage(mtmp))
        return TRUE;
    return FALSE;
}

STATIC_OVL int
vamp_shift(mon, ptr, domsg)
struct monst *mon;
struct permonst *ptr;
boolean domsg;
{
    int reslt = 0;
    char oldmtype[BUFSZ];

    /* remember current monster type before shapechange */
    Strcpy(oldmtype, domsg ? noname_monnam(mon, ARTICLE_THE) : "");

    if (mon->data == ptr) {
        /* already right shape */
        reslt = 1;
        domsg = FALSE;
    } else if (is_vampshifter(mon)) {
        reslt = newcham(mon, ptr, FALSE, FALSE);
    }

    if (reslt && domsg) {
        pline("You %s %s where %s was.",
              !canseemon(mon) ? "now detect" : "observe",
              noname_monnam(mon, ARTICLE_A), oldmtype);
        /* this message is given when it turns into a fog cloud
           in order to move under a closed door */
        display_nhwindow(WIN_MESSAGE, FALSE);
    }

    return reslt;
}

/*monmove.c*/
