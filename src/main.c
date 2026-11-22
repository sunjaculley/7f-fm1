/* SPDX-License-Identifier: GPL-3.0-only */
/* 7F: an adaptation of Fors FMS (the Game Boy Advance FM groovebox) for the M-VAVE FM-1. Unofficial: not by Fors.
 *
 *   4 FM tracks    2-op: a modulator with self-feedback into a carrier, or both in parallel (+)
 *   1 noise track  GB, WH (LFSRs), ML (metallic), GR (grainy), RS (resonant bandpass)
 *   16 steps       each step is its own sound: there are no instruments
 *   per pattern    length, rate, direction, shuffle, note echo, a step modulator
 *   global         transpose sequence, scale and key, palette (12 sounds a kind), snapshot, 5 pattern slots a track
 *
 * Integers only (the chip has no FPU). The sound runs in audio(); loop() reads the panel and draws. */
#include "fm1.h"

#define STEPS 16
#define TRK 5
#define NFM 4
#define SLOTS 5
#define PAL 12
#define SUB 16
#define EVQ 112
#define NZ 4 /* the noise track */
#define IS_NZ(t) ((t) == NZ)

/* ================================================================ data */

/* a step: 21 values and the flags */
enum { S_NOTE, S_LEVEL, S_PAN, S_PORT, S_COND, S_WAIT, S_CH1, S_CH2, S_CH3, S_MULT, S_FRAC, S_DEPTH, S_FDBK,
       S_ATK, S_HOLD, S_REL, S_MATK, S_MREL, S_MEND, S_SWD, S_SWR, NS };
#define S_SOUND S_MULT /* S_MULT..S_SWR (and the mode flag) are the step's sound: what the palette keeps */
#define NSOUND (NS - S_SOUND)
enum { FL_ON = 1, FL_TL = 2, FL_ECHO = 4, FL_TSP = 8, FL_PARA = 16, FL_USED = 32 };
typedef struct {
  uint8_t v[NS], fl;
} step_t;

enum { K_HEX, K_NOTE, K_PAN, K_COND, K_WAIT, K_CH, K_MULT, K_FRAC, K_NTYPE, K_OFF };
typedef struct {
  uint8_t max, kind;
  const char *name, *nname; /* on FM tracks, on the noise track (0: not there) */
  uint8_t nmax, nkind;
} fdef_t;
static const fdef_t FD[NS] = {
  {127, K_NOTE, "NOTE", "NOTE", 127, K_NOTE},  {127, K_HEX, "LEVEL", "LEVEL", 127, K_HEX},
  {2, K_PAN, "PAN", "PAN", 2, K_PAN},          {127, K_HEX, "PORT", "PORT", 127, K_HEX},
  {15, K_COND, "COND", "COND", 15, K_COND},    {5, K_WAIT, "WAIT", "WAIT", 5, K_WAIT},
  {24, K_CH, "CHORD 1", 0, 0, 0},              {24, K_CH, "CHORD 2", 0, 0, 0},
  {24, K_CH, "CHORD 3", 0, 0, 0},              {15, K_MULT, "MULT", "TYPE", 4, K_NTYPE},
  {7, K_FRAC, "FRAC", "RATE", 127, K_HEX},     {127, K_HEX, "DEPTH", "FILT", 127, K_HEX},
  {127, K_HEX, "FDBK", 0, 0, 0},               {127, K_HEX, "ATTACK", "ATTACK", 127, K_HEX},
  {127, K_HEX, "HOLD", "HOLD", 127, K_HEX},    {127, K_HEX, "RELEASE", "RELEASE", 127, K_HEX},
  {127, K_HEX, "M.ATK", 0, 0, 0},              {127, K_HEX, "M.REL", 0, 0, 0},
  {127, K_HEX, "M.END", 0, 0, 0},              {127, K_HEX, "SWEEP", 0, 0, 0},
  {127, K_HEX, "SW.REL", 0, 0, 0},
};
static int fmaxv(int t, int f) { return IS_NZ(t) ? FD[f].nmax : FD[f].max; }
static int fkind(int t, int f) { return IS_NZ(t) ? FD[f].nkind : FD[f].kind; }
static const char *fname(int t, int f) { return IS_NZ(t) ? FD[f].nname : FD[f].name; }

typedef struct {
  step_t st[STEPS];
  uint8_t len, rate, dir, shuf;                            /* len 1-16, rate 0-7, dir 0-3, shuffle 0-127 */
  uint8_t erep, espd, evol, emod, efbk, epan, etsp, emodu; /* note echo */
  uint8_t mtrk, mdst, mspd, mwav, moff, mdep;              /* the step modulator */
} pat_t;

#define MAGIC 0x37460002u /* "7F" + layout 1; 2: the rebalanced starter loads once */
typedef struct {
  uint32_t magic;
  uint16_t bpm;
  uint8_t scale, key, sync, dac, theme, lock;
  uint8_t tsp[8], tlen, trate, tadv, pad;
  uint8_t loaded[TRK], used[TRK]; /* the slot each track came from (0: none, 1-5); bit n: slot n+1 holds a pattern */
  pat_t live[TRK];
  pat_t slot[TRK][SLOTS];
  uint8_t pal[2][PAL][NSOUND + 1]; /* [FM, noise][slot]: the sound, then its flags; flags 0: empty */
} store_t;
static store_t S;
typedef char store_fits[sizeof(store_t) <= SETTINGS_MAX ? 1 : -1];

static const uint8_t RATE_TICKS[8] = {1, 2, 3, 4, 6, 8, 12, 24};
static const char *const RATE_NAME[8] = {"6X", "3X", "2X", "1.5X", "1X", "3/4", "1/2", "1/4"};
static const char *const DIR_NAME[4] = {"FWD", "PING", "BACK", "RAND"};
static const char *const NTYPE_NAME[5] = {"GB", "WH", "ML", "GR", "RS"};
static const char *const NTYPE_LONG[5] = {"GAME BOY", "WHITE-ISH", "METALLIC", "GRAINY", "RESONANT"};
static const char *const WAVE_NAME[5] = {"RAMP DN", "RAMP UP", "TRI", "SQUARE", "RANDOM"};
static const char *const EPAN_NAME[4] = {"C", "L", "R", "LR"};
static const char *const ADV_NAME[3] = {"PATTERN", "STEP", "TRIG"};
static const char *const SYNC_NAME[3] = {"INT", "OUT", "IN"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
/* modulator destinations: step fields */
static const uint8_t MDEST[] = {255, S_NOTE, S_LEVEL, S_PAN, S_PORT, S_MULT, S_FRAC, S_DEPTH, S_FDBK, S_ATK, S_HOLD, S_REL, S_MATK, S_MREL, S_MEND, S_SWD, S_SWR, S_WAIT};
#define NMDEST ((int)sizeof MDEST)

typedef struct {
  const char *name;
  uint16_t mask; /* bit n: n semitones above the key */
} scale_t;
static const scale_t SCALE[16] = {
  {"CHROMA", 0xFFF},  {"MAJOR", 0xAB5},   {"MINOR", 0x5AD},   {"DORIAN", 0x6AD},  {"PHRYG", 0x5AB},  {"LYDIAN", 0xAD5},
  {"MIXO", 0x6B5},    {"LOCRIAN", 0x56B}, {"HARMMIN", 0x9AD}, {"MELMIN", 0xAAD},  {"MAJPENT", 0x295}, {"MINPENT", 0x4A9},
  {"BLUES", 0x4E9},   {"WHOLE", 0x555},   {"HIRAJO", 0x18D},  {"IN SEN", 0x4A3},
};
static int in_scale(int n) {
  int r = (n - S.key) % 12;
  if (r < 0) r += 12;
  return (SCALE[S.scale].mask >> r) & 1;
}
static int quantize(int n) {
  for (int i = 0; i < 12 && !in_scale(n); i++) n--;
  return n;
}

static void fill(void *p, uint8_t v, uint32_t n) { /* no C library here */
  uint8_t *d = (uint8_t *)p;
  while (n--) *d++ = v;
}
static void copy(void *dst, const void *src, uint32_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n--) *d++ = *s++;
}
static int same(const void *a, const void *b, uint32_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  while (n--)
    if (*x++ != *y++) return 0;
  return 1;
}
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the clean slate: what a step sounds like before anyone touches it */
static void step_default(int t, step_t *st) {
  fill(st, 0, sizeof *st);
  uint8_t *v = st->v;
  v[S_LEVEL] = 0x60, v[S_PAN] = 1, v[S_ATK] = 0, v[S_HOLD] = 0x20, v[S_REL] = 0x50;
  if (IS_NZ(t)) {
    v[S_NOTE] = 84, v[S_MULT] = 0, v[S_FRAC] = 0, v[S_DEPTH] = 0x20, v[S_REL] = 0x48, v[S_HOLD] = 0;
  } else {
    v[S_NOTE] = 60, v[S_MULT] = 1, v[S_FRAC] = 0, v[S_DEPTH] = 0x30, v[S_FDBK] = 0;
    v[S_MATK] = 0, v[S_MREL] = 0x58, v[S_MEND] = 0x20;
  }
  st->fl = FL_TSP;
}
static void pat_default(int t, pat_t *p) {
  fill(p, 0, sizeof *p);
  for (int s = 0; s < STEPS; s++) step_default(t, &p->st[s]);
  p->len = 16, p->rate = 4, p->dir = 0, p->shuf = 0;
  p->erep = 3, p->espd = 3, p->evol = 16 - 4, p->emod = 16, p->efbk = 16, p->epan = 0, p->etsp = 12, p->emodu = 0;
  p->mtrk = (uint8_t)t, p->mdst = 0, p->mspd = 15, p->mwav = 2, p->moff = 0, p->mdep = 64;
}

static void set_step(pat_t *p, int s, int note, int lvl) {
  step_t *st = &p->st[s];
  st->v[S_NOTE] = (uint8_t)note, st->v[S_LEVEL] = (uint8_t)lvl;
  st->fl |= FL_ON | FL_USED;
}
static void starter(void) { /* something to hear at first power-on */
  pat_t *k = &S.live[0], *b = &S.live[1], *e = &S.live[2], *l = &S.live[3], *n = &S.live[4];
  for (int s = 0; s < STEPS; s++) { /* a kick: low, swept, short */
    uint8_t *v = k->st[s].v;
    v[S_NOTE] = 36, v[S_MULT] = 1, v[S_FRAC] = 0, v[S_DEPTH] = 0x18, v[S_FDBK] = 0x10, v[S_SWD] = 0x30, v[S_SWR] = 0x44;
    v[S_HOLD] = 0x30, v[S_REL] = 0x52, v[S_MREL] = 0x30, v[S_MEND] = 0, v[S_LEVEL] = 0x58;
  }
  for (int s = 0; s < 16; s += 4) set_step(k, s, 36, 0x58);
  static const int8_t bass[16] = {0, -1, 12, -1, 0, -1, 3, 15, 0, -1, 12, -1, 7, -1, 10, -1};
  for (int s = 0; s < STEPS; s++) { /* a bass: ratio 1, some feedback, mod envelope closing */
    uint8_t *v = b->st[s].v;
    v[S_MULT] = 1, v[S_FRAC] = 0, v[S_DEPTH] = 0x3C, v[S_FDBK] = 0x28, v[S_MREL] = 0x58, v[S_MEND] = 0x10;
    v[S_HOLD] = 0x40, v[S_REL] = 0x54, v[S_PORT] = 0;
    if (bass[s] >= 0) set_step(b, s, 36 + bass[s], s & 2 ? 0x38 : 0x48);
  }
  b->st[7].v[S_PORT] = 0x50;
  for (int s = 0; s < STEPS; s++) { /* a bell: ratio 3.5 */
    uint8_t *v = e->st[s].v;
    v[S_MULT] = 3, v[S_FRAC] = 4, v[S_DEPTH] = 0x2C, v[S_MREL] = 0x64, v[S_MEND] = 0x00, v[S_HOLD] = 0, v[S_REL] = 0x6A;
    v[S_LEVEL] = 0x78, v[S_PAN] = 2;
  }
  set_step(e, 2, 72, 0x78), set_step(e, 11, 79, 0x70);
  e->st[2].fl |= FL_ECHO, e->st[11].fl |= FL_ECHO;
  e->erep = 4, e->espd = 9, e->epan = 3, e->etsp = 12 + 7, e->emodu = 12;
  for (int s = 0; s < STEPS; s++) { /* a pad-ish lead in parallel mode, a chord */
    uint8_t *v = l->st[s].v;
    v[S_MULT] = 2, v[S_FRAC] = 7, v[S_DEPTH] = 0x50, v[S_FDBK] = 0x08, v[S_ATK] = 0x5A, v[S_HOLD] = 0x6C, v[S_REL] = 0x6C;
    v[S_MATK] = 0x58, v[S_MREL] = 0x68, v[S_MEND] = 0x40, v[S_LEVEL] = 0x70, v[S_PAN] = 1;
    l->st[s].fl |= FL_PARA;
  }
  set_step(l, 0, 60, 0x70);
  l->st[0].v[S_CH1] = 3, l->st[0].v[S_CH2] = 7;
  l->len = 16, l->rate = 6; /* half speed: once every two bars */
  for (int s = 0; s < STEPS; s++) { /* hats from the metallic noise */
    uint8_t *v = n->st[s].v;
    v[S_MULT] = 2, v[S_FRAC] = 0x10, v[S_DEPTH] = 0x60, v[S_HOLD] = 0x08, v[S_REL] = 0x50, v[S_NOTE] = 84, v[S_LEVEL] = 0x7F;
  }
  for (int s = 2; s < 16; s += 4) set_step(n, s, 84, 0x7F);
  set_step(n, 15, 84, 0x60);
  n->st[14].v[S_REL] = 0x5C;
  n->shuf = 0x30;
  /* a modulator on track 2: its level follows a triangle over 8 steps */
  b->mtrk = 1, b->mdst = 7, b->mspd = 7, b->mwav = 2, b->mdep = 64 + 24; /* DEPTH, +24 over 8 steps */
}

static void init_all(void) {
  fill(&S, 0, sizeof S);
  S.magic = MAGIC;
  S.bpm = 120;
  S.scale = 0, S.key = 0, S.sync = 1, S.dac = 0, S.theme = 0;
  for (int i = 0; i < 8; i++) S.tsp[i] = 24;
  S.tlen = 1, S.trate = 1, S.tadv = 0;
  for (int t = 0; t < TRK; t++) pat_default(t, &S.live[t]);
  for (int t = 0; t < TRK; t++)
    for (int i = 0; i < SLOTS; i++) pat_default(t, &S.slot[t][i]);
  starter();
}

/* ================================================================ shared: loop() <-> audio() */

static volatile uint8_t running, ready;
static volatile int8_t play_pos[TRK] = {-1, -1, -1, -1, -1};
static volatile uint8_t muted, soloed;           /* bit t */
static volatile int8_t cue[TRK] = {-1, -1, -1, -1, -1}; /* slot 0-4 waiting for the end of the track's pattern */
static volatile uint8_t reset_req;               /* bit t: restart the track at the next beat */
static volatile uint8_t prev_req;                /* loop -> audio: 1 + track: sound the cursor step */
static volatile uint8_t prev_step;
static volatile uint8_t tpos_out, flash_track;   /* audio -> loop */
static volatile uint8_t vstate[NFM + 1];         /* audio -> loop: voice busy (owner + 1) */
static volatile uint32_t gstep_out;
static volatile uint16_t load_pct;
static volatile int8_t nudge; /* tempo nudge while held, BPM */

/* ================================================================ the sound */

static const uint32_t EXP2[257] = {32768,32857,32946,33035,33125,33215,33305,33395,33486,33576,33667,33759,33850,33942,34034,34126,34219,34312,34405,34498,34591,34685,34779,34874,34968,35063,35158,35253,35349,35445,35541,35637,35734,35831,35928,36025,36123,36221,36319,36417,36516,36615,36715,36814,36914,37014,37114,37215,37316,37417,37518,37620,37722,37824,37927,38030,38133,38236,38340,38444,38548,38653,38757,38863,38968,39074,39180,39286,39392,39499,39606,39714,39821,39929,40037,40146,40255,40364,40473,40583,40693,40804,40914,41025,41136,41248,41360,41472,41584,41697,41810,41923,42037,42151,42265,42380,42495,42610,42726,42841,42958,43074,43191,43308,43425,43543,43661,43780,43898,44017,44137,44256,44376,44497,44617,44738,44859,44981,45103,45225,45348,45471,45594,45718,45842,45966,46091,46216,46341,46467,46593,46719,46846,46973,47100,47228,47356,47484,47613,47742,47871,48001,48131,48262,48393,48524,48655,48787,48920,49052,49185,49319,49452,49586,49721,49856,49991,50126,50262,50399,50535,50672,50810,50947,51085,51224,51363,51502,51642,51782,51922,52063,52204,52346,52488,52630,52773,52916,53059,53203,53347,53492,53637,53782,53928,54074,54221,54368,54515,54663,54811,54960,55109,55258,55408,55558,55709,55860,56012,56163,56316,56468,56622,56775,56929,57083,57238,57393,57549,57705,57861,58018,58176,58333,58491,58650,58809,58968,59128,59289,59449,59611,59772,59934,60097,60260,60423,60587,60751,60916,61081,61247,61413,61579,61746,61914,62081,62250,62419,62588,62757,62928,63098,63269,63441,63613,63785,63958,64132,64306,64480,64655,64830,65006,65182,65359,65536}; /* 2^(i / 256), Q15 */
static uint32_t exp2_i(int32_t l) { /* 2^(l / 65536), l >= 0 */
  int i = l >> 16;
  uint32_t v = EXP2[(l >> 8) & 255];
  return i >= 15 ? v << (i - 15) : v >> (15 - i);
}
#define REL_K 1854286438u /* -ln(0.001), Q28: fall 60 dB over the time given */
/* the fractional part of MULT, Q16: 0, 1/8, 1/4, 1/3, 1/2, 2/3, 3/4, 0.975 */
static const uint32_t FRAC_Q16[8] = {0, 8192, 16384, 21845, 32768, 43691, 49152, 63898};
static const uint32_t METAL_INC[6] = {19994485u, 29645987u, 35995917u, 50906563u, 52591437u, 77913240u}; /* 808 hats */
static int32_t ENVF[128];                                                                                /* frames */

/* phase step per frame of a note in 1/256 semitones: 440 Hz x 2^32 / 44100 x 2^((n - 69) / 12) */
static uint32_t inc_of(int32_t nq8) {
  nq8 = clampi(nq8, 0, 135 * 256);
  int32_t l = (nq8 - 69 * 256) * 64 / 3 + (16 << 16); /* x 65536 / (12 x 256), + 16 octaves to stay positive */
  return (uint32_t)(((uint64_t)42852282u * exp2_i(l)) >> 16);
}
static int32_t coef_n(int32_t n) { /* falls to 0.001 over n steps of itself, Q28 */
  if (n < 1) n = 1;
  return (int32_t)((1u << 28) - REL_K / (uint32_t)n);
}

typedef struct {
  uint8_t stage, para, owner, mstage; /* stage: 0 attack 1 hold 2 release 3 idle */
  uint32_t cph, mph, cinc, minc, ratio;
  int32_t note, target, glide, gslices; /* 1/256 semitones */
  int32_t sw, swcoef;                   /* sweep offset, 1/256 semitones (Q8 x 256), per slice coef Q28 */
  int32_t amp, ainc, hold, rcoef;       /* Q24 */
  int32_t menv, mainc, mend, mcoef;     /* Q24 */
  int32_t depth, fb, y1, y2;
  int32_t gl, gr; /* Q15 */
} fv_t;
static fv_t FV[NFM];

typedef struct {
  uint8_t stage, type, width;
  uint32_t lfsr, ph, inc, mph[6], minc[6];
  int32_t note, target, glide, gslices;
  int32_t amp, ainc, hold, rcoef;
  int32_t hpa, hpl;                   /* one-pole highpass */
  int32_t f, q, low, band, sh;        /* RS: bandpass; sample and hold */
  int32_t lpa, lpl;                   /* GR: impulses through a lowpass */
  uint32_t dens;                      /* GR */
  uint8_t npart;
  int32_t gl, gr;
} nv_t;
static nv_t NV;

static int32_t last_note[TRK]; /* for portamento, 1/256 semitones */

static void gains(int32_t *gl, int32_t *gr, int lvl, int pan) {
  int32_t g = lvl * 258; /* Q15, linear like FMS's hex levels: 40 is -6 dB, not -12 */
  /* L / R lean hard but keep a quarter in the other side, so a panned sound never vanishes on one speaker */
  *gl = pan == 2 ? g >> 2 : g;
  *gr = pan == 0 ? g >> 2 : g;
}

static void amp_start(int32_t *amp, int32_t *ainc, int32_t *hold, int32_t *rcoef, uint8_t *stage, const uint8_t *v) {
  int32_t a = ENVF[v[S_ATK]];
  if (a < 24) a = 24;
  *amp = 0;
  *ainc = (1 << 24) / a;
  *hold = ENVF[v[S_HOLD]];
  int32_t r = ENVF[v[S_REL]];
  if (r < 64) r = 64;
  *rcoef = coef_n(r);
  *stage = 0;
}

static void fm_sound(fv_t *f, const uint8_t *v, int para) { /* the parts a trigless step changes too */
  f->para = (uint8_t)para;
  f->ratio = ((uint32_t)v[S_MULT] << 16) + FRAC_Q16[v[S_FRAC] & 7];
  f->depth = v[S_DEPTH];
  f->fb = v[S_FDBK];
}

static void glide_to(int32_t *note, int32_t *target, int32_t *glide, int32_t *gslices, int32_t nq8, int port, int from_last, int32_t last) {
  *target = nq8;
  if (port && from_last) {
    *note = last;
    int32_t sl = ENVF[port] / SUB;
    if (sl < 1) sl = 1;
    *gslices = sl;
    *glide = (nq8 - last) / sl;
  } else {
    *note = nq8, *glide = 0, *gslices = 0;
  }
}

static void fm_note(int vi, int t, const uint8_t *v, int fl, int nq8, int glide_ok) {
  fv_t *f = &FV[vi];
  f->owner = (uint8_t)t;
  fm_sound(f, v, fl & FL_PARA);
  glide_to(&f->note, &f->target, &f->glide, &f->gslices, nq8, v[S_PORT], glide_ok, last_note[t]);
  f->sw = v[S_SWD] ? (int32_t)v[S_SWD] * 128 : 0; /* half a semitone a unit */
  f->swcoef = coef_n(ENVF[v[S_SWR]] / SUB + 1);
  amp_start(&f->amp, &f->ainc, &f->hold, &f->rcoef, &f->stage, v);
  int32_t ma = ENVF[v[S_MATK]];
  f->mend = (int32_t)v[S_MEND] << 17; /* 0..127 -> Q24 */
  if (ma < SUB) f->menv = 1 << 24, f->mstage = 1;
  else f->menv = 0, f->mainc = (1 << 24) / (ma / SUB), f->mstage = 0;
  f->mcoef = coef_n(ENVF[v[S_MREL]] / SUB + 1);
  f->cph = f->mph = 0; /* FMS resets the oscillators on a trig */
  f->y1 = f->y2 = 0;
  gains(&f->gl, &f->gr, v[S_LEVEL], v[S_PAN]);
}

static void nz_sound(nv_t *n, const uint8_t *v) {
  int type = v[S_MULT] > 4 ? 4 : v[S_MULT], rate = v[S_FRAC], filt = v[S_DEPTH];
  n->type = (uint8_t)type;
  if (type == 0) n->width = (uint8_t)(15 - rate * 12 / 127);
  else if (type == 1) n->width = (uint8_t)(31 - rate * 28 / 127);
  n->npart = (uint8_t)(6 - rate * 4 / 127);
  n->dens = 65536u >> (rate / 16);
  /* highpass: a = 2 pi fc / 44100, fc = 20 x 2^(filt / 14) (off at 0) */
  uint32_t fc = filt ? 20u * exp2_i(filt * 65536 / 14) : 0;
  n->hpa = (int32_t)clampi((int32_t)(fc * 467u / 100u), 0, 32000);
  /* RS: Q from 0.7 to 25 */
  int32_t qx10 = 7 + filt * 2;
  n->q = clampi(163840 / qx10, 400, 31130);
}

static void nz_pitch(nv_t *n) {
  uint32_t inc = inc_of(n->note);
  /* GB / WH: the LFSR clock, 32x the note; RS: the bandpass at the note; ML: the 808 bank around note 84 */
  n->inc = inc << 5 < inc ? 0xFFFFFFFFu : inc << 5;
  if (n->type == 4 || n->type == 3) {
    uint32_t fc = (uint32_t)(((uint64_t)inc * 44100u) >> 32); /* Hz */
    if (fc > 15000) fc = 15000;
    n->f = sine(fc * 24348u); /* 2 sin(pi fc / 88200), Q14 */
    n->lpa = (int32_t)clampi((int32_t)(fc * 467u / 100u), 200, 32000);
  }
  if (n->type == 2) {
    uint32_t k = exp2_i((n->note - 84 * 256) * 64 / 3 + (16 << 16)); /* Q16 */
    for (int i = 0; i < 6; i++) n->minc[i] = (uint32_t)(((uint64_t)METAL_INC[i] * k) >> 16);
  }
}

static void nz_note(int t, const uint8_t *v, int nq8, int glide_ok) {
  nv_t *n = &NV;
  nz_sound(n, v);
  glide_to(&n->note, &n->target, &n->glide, &n->gslices, nq8, v[S_PORT], glide_ok, last_note[t]);
  nz_pitch(n);
  amp_start(&n->amp, &n->ainc, &n->hold, &n->rcoef, &n->stage, v);
  if (!n->lfsr) n->lfsr = 0x7FFFu;
  n->low = n->band = 0;
  gains(&n->gl, &n->gr, v[S_LEVEL], v[S_PAN]);
}

/* a step reaches the voices. kind 0: a trig, 1: trigless (move it, don't restart it) */
static void play(int t, const uint8_t *v, int fl, int note, int kind) {
  if (soloed ? !((soloed >> t) & 1) : (muted >> t) & 1) return;
  int32_t nq8 = note << 8;
  if (IS_NZ(t)) {
    if (kind == 1) {
      if (NV.stage == 3) return;
      nz_sound(&NV, v);
      glide_to(&NV.note, &NV.target, &NV.glide, &NV.gslices, nq8, v[S_PORT], 1, NV.note);
      nz_pitch(&NV);
      gains(&NV.gl, &NV.gr, v[S_LEVEL], v[S_PAN]);
    } else {
      nz_note(t, v, nq8, 1);
    }
    last_note[t] = nq8;
    flash_track |= (uint8_t)(1 << t);
    return;
  }
  if (kind == 1) { /* trigless: the voice keeps its phase and envelopes */
    fv_t *f = &FV[t];
    if (f->stage == 3 || f->owner != t) return;
    fm_sound(f, v, fl & FL_PARA);
    glide_to(&f->note, &f->target, &f->glide, &f->gslices, nq8, v[S_PORT], 1, f->note);
    gains(&f->gl, &f->gr, v[S_LEVEL], v[S_PAN]);
    last_note[t] = nq8;
    return;
  }
  fm_note(t, t, v, fl, nq8, FV[t].owner == t);
  last_note[t] = nq8;
  flash_track |= (uint8_t)(1 << t);
  /* chord: each offset above 0 takes a voice, top-down by track: idle first, then a releasing one */
  for (int c = 0; c < 3; c++) {
    int off = v[S_CH1 + c];
    if (!off) continue;
    int pick = -1;
    for (int i = 0; i < NFM && pick < 0; i++)
      if (i != t && FV[i].stage == 3) pick = i;
    for (int i = 0; i < NFM && pick < 0; i++)
      if (i != t && FV[i].stage == 2 && FV[i].owner != t) pick = i;
    if (pick < 0) break;
    int n2 = (fl & FL_TSP) ? quantize(note + off) : note + off;
    if (n2 > 127) n2 = 127;
    fm_note(pick, t, v, fl, n2 << 8, 0);
  }
}


static void render_fm(int32_t *out, uint32_t n) {
  for (int vi = 0; vi < NFM; vi++) {
    fv_t *f = &FV[vi];
    if (f->stage == 3) continue;
    /* once a slice: glide, sweep, the mod envelope */
    if (f->gslices > 0) {
      f->note += f->glide;
      if (--f->gslices == 0) f->note = f->target;
    }
    if (f->sw) {
      f->sw = (int32_t)(((int64_t)f->sw * f->swcoef) >> 28);
      if (f->sw < 16) f->sw = 0;
    }
    f->cinc = inc_of(f->note + f->sw);
    uint64_t mi = ((uint64_t)f->cinc * f->ratio) >> 16;
    f->minc = mi > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)mi;
    if (f->mstage == 0) {
      f->menv += f->mainc;
      if (f->menv >= 1 << 24) f->menv = 1 << 24, f->mstage = 1;
    } else {
      f->menv = f->mend + (int32_t)(((int64_t)(f->menv - f->mend) * f->mcoef) >> 28);
    }
    int32_t eff = (f->depth * (f->menv >> 9)) >> 15; /* depth x envelope, 0..127 */
    int32_t mq15 = f->menv >> 9;
    int32_t amp = f->amp;
    for (uint32_t k = 0; k < n; k++) {
      if (f->stage == 0) {
        amp += f->ainc;
        if (amp >= 1 << 24) amp = 1 << 24, f->stage = 1;
      } else if (f->stage == 1) {
        if (--f->hold <= 0) f->stage = 2;
      } else {
        amp = (int32_t)(((int64_t)amp * f->rcoef) >> 28);
        if (amp < 2048) {
          f->stage = 3;
          break;
        }
      }
      uint32_t fbt = (uint32_t)(((f->y1 + f->y2) >> 1) * f->fb) * 516u; /* up to pi of self-modulation */
      int32_t m = sine(f->mph + fbt);
      f->y2 = f->y1, f->y1 = m;
      f->mph += f->minc;
      int32_t c;
      if (!f->para) {
        c = sine(f->cph + (uint32_t)(m * eff) * 1314u); /* index up to 8 radians */
      } else {
        c = (sine(f->cph + fbt) + ((m * f->depth) >> 7)) >> 1; /* parallel: both, through the mod envelope */
        c = (c * mq15) >> 15;
      }
      f->cph += f->cinc;
      int32_t s = (c * (amp >> 9)) >> 15;
      out[2 * k] += (s * f->gl) >> 16;
      out[2 * k + 1] += (s * f->gr) >> 16;
    }
    f->amp = amp;
  }
}

static void render_nz(int32_t *out, uint32_t n) {
  nv_t *v = &NV;
  if (v->stage == 3) return;
  if (v->gslices > 0) {
    v->note += v->glide;
    if (--v->gslices == 0) v->note = v->target;
    nz_pitch(v);
  }
  int32_t amp = v->amp;
  for (uint32_t k = 0; k < n; k++) {
    if (v->stage == 0) {
      amp += v->ainc;
      if (amp >= 1 << 24) amp = 1 << 24, v->stage = 1;
    } else if (v->stage == 1) {
      if (--v->hold <= 0) v->stage = 2;
    } else {
      amp = (int32_t)(((int64_t)amp * v->rcoef) >> 28);
      if (amp < 2048) {
        v->stage = 3;
        break;
      }
    }
    int32_t x = 0;
    switch (v->type) {
      case 0:
      case 1: { /* an LFSR clocked at the note; RATE narrows it to a short loop */
        uint32_t np = v->ph + v->inc;
        if (np < v->ph || v->inc == 0xFFFFFFFFu) {
          uint32_t b = (v->lfsr ^ (v->lfsr >> 1)) & 1u;
          v->lfsr = (v->lfsr >> 1) | (b << (v->width - 1));
          if (!v->lfsr) v->lfsr = 1;
        }
        v->ph = np;
        x = (v->lfsr & 1u) ? 16000 : -16000;
      } break;
      case 2: { /* six square waves, inharmonic; RATE takes partials away */
        for (int i = 0; i < v->npart; i++) {
          v->mph[i] += v->minc[i];
          x += (v->mph[i] & 0x80000000u) ? 7000 : -7000;
        }
      } break;
      case 3: { /* sparse impulses through a lowpass at the note */
        uint32_t r = random32();
        int32_t imp = (r & 0xFFFFu) < v->dens ? ((r >> 16) & 1 ? 30000 : -30000) : 0;
        v->lpl += ((imp - v->lpl) * v->lpa) >> 15;
        x = v->lpl;
      } break;
      default: { /* noise held at RATE, through a resonant bandpass at the note */
        uint32_t np = v->ph + (v->dens << 15);
        if (np < v->ph || v->dens >= 65536u) v->sh = (int32_t)random32() >> 21;
        v->ph = np;
        for (int o = 0; o < 2; o++) {
          v->low += (v->f * v->band) >> 14;
          int32_t high = v->sh - v->low - ((v->q * v->band) >> 14);
          v->band += (v->f * high) >> 14;
        }
        x = clampi(((v->band * v->q) >> 14) * 16, -24000, 24000);
      } break;
    }
    if (v->hpa && v->type != 4) { /* FILT: a highpass (on RS it is the resonance) */
      v->hpl += ((x - v->hpl) * v->hpa) >> 15;
      x -= v->hpl;
    }
    int32_t s = (clampi(x, -32767, 32767) * (amp >> 9)) >> 15;
    out[2 * k] += (s * v->gl) >> 15; /* noise a little hotter than FM: it reads quieter */
    out[2 * k + 1] += (s * v->gr) >> 15;
  }
  v->amp = amp;
}

/* ================================================================ the sequencer */

typedef struct {
  int32_t wait; /* frames */
  uint8_t used, track, fl, kind, note;
  uint8_t v[NS];
} event_t;
static event_t Q[EVQ];

static int32_t frames_per_tick(void) {
  uint32_t b = clock_bpm();
  if (b < 2000) b = (uint32_t)S.bpm * 100u;
  return (int32_t)(11025000u / b); /* 44100 x 60 x 100 / 24 */
}

static void enqueue(int t, const uint8_t *v, int fl, int note, int kind, int32_t wait) {
  for (int i = 0; i < EVQ; i++) {
    event_t *e = &Q[i];
    if (e->used) continue;
    e->wait = wait, e->track = (uint8_t)t, e->fl = (uint8_t)fl, e->kind = (uint8_t)kind;
    e->note = (uint8_t)clampi(note, 0, 127);
    copy(e->v, v, NS);
    e->used = 1;
    return;
  }
}

static int32_t gtick;
static int16_t tcnt[TRK];
static int8_t pos[TRK], pdir[TRK];
static uint16_t visits[TRK][STEPS];
static uint16_t mphase[TRK];
static uint8_t tpos, tcount;

static int32_t wave(int w, uint32_t p, uint32_t n) { /* -32767..32767 at step p of n */
  int32_t u = (int32_t)((p * 65536u) / n);
  switch (w) {
    case 0: return 32767 - u;
    case 1: return u - 32767;
    case 2: return u < 32768 ? u * 2 - 32767 : 32767 - (u - 32768) * 2;
    case 3: return u < 32768 ? 32767 : -32767;
    default: return (int32_t)(random32() & 0xFFFFu) - 32768;
  }
}

/* the modulators aimed at track t: each one moves its value on, once per trig of t */
static void apply_mods(int t, uint8_t *v) {
  for (int m = 0; m < TRK; m++) {
    pat_t *p = &S.live[m];
    if (p->mtrk != t || !p->mdst || p->mdst >= NMDEST) continue;
    int f = MDEST[p->mdst];
    if (IS_NZ(t) && !FD[f].nname) continue;
    uint32_t n = (uint32_t)p->mspd + 1;
    uint32_t ph = (mphase[m] + (uint32_t)p->moff * n / 16u) % n;
    int32_t w = wave(p->mwav, ph, n);
    int32_t dep = (int32_t)p->mdep - 64;
    int mx = fmaxv(t, f);
    int32_t d = (w * dep) / 32767;
    if (mx < 127) d = d * (mx + 1) / 128;
    v[f] = (uint8_t)clampi(v[f] + d, 0, mx);
    mphase[m] = (uint16_t)((mphase[m] + 1) % n);
  }
}

static int out_note(int t, const uint8_t *v, int fl) {
  int n = v[S_NOTE];
  if (IS_NZ(t)) return n;
  if (fl & FL_TSP) n += (int)S.tsp[tpos % 8] - 24;
  return quantize(clampi(n, 0, 127));
}

static void tsp_advance(void) {
  if (++tcount >= (S.trate ? S.trate : 1)) {
    tcount = 0;
    tpos = (uint8_t)((tpos + 1) % (S.tlen ? S.tlen : 1));
    tpos_out = tpos;
  }
}

static void load_slot_now(int t, int sl) {
  copy(&S.live[t], &S.slot[t][sl], sizeof(pat_t));
  S.loaded[t] = (uint8_t)(sl + 1);
}

static void track_step(int t) {
  pat_t *p = &S.live[t];
  int len = clampi(p->len, 1, STEPS);
  int s = pos[t];
  int wrapped = 0;
  switch (p->dir) {
    case 0: s = s + 1 >= len ? (wrapped = 1, 0) : s + 1; break;
    case 2: s = s - 1 < 0 || s >= len ? (wrapped = 1, len - 1) : s - 1; break;
    case 1:
      if (len < 2) {
        s = 0, wrapped = 1;
        break;
      }
      if (s < 0) {
        s = 0, pdir[t] = 1, wrapped = 1;
        break;
      }
      s += pdir[t];
      if (s >= len) s = len - 2, pdir[t] = -1;
      if (s <= 0) s = 0, pdir[t] = 1, wrapped = 1;
      break;
    default:
      s = (int)(random32() % (uint32_t)len);
      if (++visits[t][0] % (uint16_t)len == 0) wrapped = 1; /* (a pattern's worth of random steps) */
      break;
  }
  if (pos[t] < 0) wrapped = 1;
  if (wrapped && cue[t] >= 0) { /* a queued pattern starts here */
    load_slot_now(t, cue[t]);
    cue[t] = -1;
    p = &S.live[t];
    len = clampi(p->len, 1, STEPS);
    if (s >= len) s = 0;
  }
  pos[t] = (int8_t)s;
  play_pos[t] = (int8_t)s;
  step_t *st = &p->st[s];
  if (!(st->fl & FL_ON)) return;
  uint16_t vis = visits[t][s]++;
  if (p->dir == 3) vis = 0;
  if (vis % (st->v[S_COND] + 1u)) return;
  uint8_t v[NS];
  copy(v, st->v, NS);
  if (!(st->fl & FL_TL)) apply_mods(t, v);
  int fl = st->fl;
  int32_t fpt = frames_per_tick();
  int32_t wait = v[S_WAIT] * fpt;
  if ((gtick / 6) & 1) wait += (int32_t)p->shuf * fpt * 3 / 127; /* shuffle: odd 16ths, up to half a step */
  int note = out_note(t, v, fl);
  enqueue(t, v, fl, note, (fl & FL_TL) ? 1 : 0, wait);
  if (t == 0 && S.tadv == 2) tsp_advance();
  if (!(fl & FL_ECHO) || !p->erep || (fl & FL_TL)) return;
  /* note echo: repeats of the step, each one changed a little more */
  int sp = p->espd ? p->espd : 1, pan = v[S_PAN];
  for (int r = 1; r <= p->erep; r++) {
    uint8_t e[NS];
    copy(e, v, NS);
    e[S_LEVEL] = (uint8_t)clampi(v[S_LEVEL] + ((int)p->evol - 16) * 4 * r, 0, 127);
    e[S_DEPTH] = (uint8_t)clampi(v[S_DEPTH] + ((int)p->emod - 16) * 4 * r, 0, IS_NZ(t) ? 127 : 127);
    if (!IS_NZ(t)) e[S_FDBK] = (uint8_t)clampi(v[S_FDBK] + ((int)p->efbk - 16) * 4 * r, 0, 127);
    if (e[S_LEVEL] == 0) break;
    e[S_PAN] = (uint8_t)(p->epan == 0 ? pan : p->epan == 1 ? 0 : p->epan == 2 ? 2 : (r & 1) ? 0 : 2);
    int ts = ((int)p->etsp - 12) * r;
    if (p->emodu) {
      ts %= (int)p->emodu;
    }
    int n2 = note + ts;
    if (!IS_NZ(t) && (fl & FL_TSP)) n2 = quantize(clampi(n2, 0, 127));
    enqueue(t, e, fl, n2, 0, wait + sp * fpt * r);
  }
}

static void on_clock(int what) {
  if (what == CLOCK_START) {
    gtick = 0;
    for (int t = 0; t < TRK; t++) {
      tcnt[t] = 0, pos[t] = -1, pdir[t] = 1, mphase[t] = 0;
      for (int s = 0; s < STEPS; s++) visits[t][s] = 0;
    }
    tpos = tcount = 0, tpos_out = 0;
    running = 1;
  } else if (what == CLOCK_CONTINUE) {
    running = 1;
  } else if (what == CLOCK_STOP) {
    running = 0;
    for (int t = 0; t < TRK; t++) play_pos[t] = -1, cue[t] = -1;
  } else if (what == CLOCK_TICK && running) {
    if (reset_req && gtick % 24 == 0) {
      for (int t = 0; t < TRK; t++)
        if ((reset_req >> t) & 1) tcnt[t] = 0, pos[t] = -1, pdir[t] = 1;
      reset_req = 0;
    }
    if (gtick % 6 == 0) {
      int32_t g = gtick / 6;
      gstep_out = (uint32_t)g;
      if (S.tadv == 1 && g > 0) tsp_advance();
      if (S.tadv == 0 && g > 0 && g % 16 == 0) tsp_advance();
    }
    for (int t = 0; t < TRK; t++) {
      if (tcnt[t] == 0) track_step(t);
      tcnt[t] = (int16_t)((tcnt[t] + 1) % RATE_TICKS[S.live[t].rate & 7]);
    }
    gtick++;
  }
}

static uint32_t dac_ph;
static int32_t dac_l, dac_r;

static int32_t soft(int32_t x) { /* soft clip above half scale */
  int32_t a = x < 0 ? -x : x;
  if (a > 16384) {
    int32_t e = a - 16384;
    a = 16384 + (int32_t)(((int64_t)e * 16383) / (e + 16383));
  }
  return x < 0 ? -a : a;
}

void audio(int32_t *out, uint32_t frames) {
  if (!ready) return;
  uint32_t t0 = micros();
  const clock_event_t *ev;
  int nev = clock_events(&ev), e = 0;
  uint8_t pr = prev_req;
  if (pr) {
    prev_req = 0;
    int t = pr - 1;
    step_t *st = &S.live[t].st[prev_step & 15];
    play(t, st->v, st->fl, out_note(t, st->v, st->fl), 0);
  }
  static int32_t mix[2 * SUB];
  for (uint32_t at = 0; at < frames; at += SUB) {
    uint32_t n = frames - at < SUB ? frames - at : SUB;
    while (e < nev && ev[e].at < at + n) on_clock(ev[e++].what);
    for (int i = 0; i < EVQ; i++) {
      event_t *q = &Q[i];
      if (!q->used) continue;
      if (q->wait < (int32_t)n) {
        play(q->track, q->v, q->fl, q->note, q->kind);
        q->used = 0;
      } else {
        q->wait -= (int32_t)n;
      }
    }
    for (uint32_t k = 0; k < 2 * n; k++) mix[k] = 0;
    render_fm(mix, n);
    render_nz(mix, n);
    for (uint32_t k = 0; k < n; k++) {
      int32_t l = soft(mix[2 * k]), r = soft(mix[2 * k + 1]);
      if (S.dac) { /* GBA: 8 bits at 32768 Hz */
        dac_ph += 3189796480u; /* 32768 / 44100 x 2^32 */
        if (dac_ph < 3189796480u) dac_l = l & ~0xFF, dac_r = r & ~0xFF;
        l = dac_l, r = dac_r;
      }
      out[2 * (at + k)] += l;
      out[2 * (at + k) + 1] += r;
    }
  }
  for (int i = 0; i < NFM; i++) vstate[i] = FV[i].stage == 3 ? 0 : (uint8_t)(FV[i].owner + 1);
  vstate[NFM] = NV.stage == 3 ? 0 : NZ + 1;
  uint32_t us = micros() - t0;
  load_pct = (uint16_t)((load_pct * 15u + us * 100u / (frames * 1000000u / SAMPLE_RATE)) / 16u);
}

/* ================================================================ the panel */

static int8_t white_of[KEY_COUNT], black_of[KEY_COUNT];
static int white_key[16], black_key[11];
enum { BK_COPY = 5, BK_PASTE, BK_RAND, BK_CLEAR, BK_MUTE, BK_SOLO };

enum { V_GRID, V_DATA, V_PAL };
enum { G_EDIT, G_SEL, G_ENV, G_LFO, G_FX, G_SEQ, G_GLO, NG };
enum { PG_NOTE, PG_TRIG, PG_CHORD, PG_FM, PG_AMP, PG_MENV, PG_SWEEP, PG_MOD1, PG_MOD2, PG_ECHO1, PG_ECHO2, PG_TRACK,
       PG_GLOBAL, PG_TSP, PG_SYS, NPG };
static const char *const PAGE_NAME[NPG] = {"NOTE", "TRIG", "CHORD", "FM", "AMP", "MOD ENV", "SWEEP", "MODULATOR",
                                           "MOD 2", "ECHO", "ECHO 2", "TRACK", "GLOBAL", "TRANSPOSE", "SYSTEM"};
static const uint8_t GROUP_PAGES[NG][4] = {{PG_NOTE, PG_TRIG, PG_CHORD, 255}, {PG_FM, 255}, {PG_AMP, PG_MENV, PG_SWEEP, 255},
                                           {PG_MOD1, PG_MOD2, 255},           {PG_ECHO1, PG_ECHO2, 255},
                                           {PG_TRACK, 255},                   {PG_GLOBAL, PG_TSP, PG_SYS, 255}};
static const uint8_t GROUP_BTN[NG] = {BTN_EDIT, BTN_SEL, BTN_ENV, BTN_LFO, BTN_FX, BTN_SEQ, BTN_GLO};
/* knob slots: 0..20 step fields; the rest by id */
enum { X_ECHO = 32, X_TSP, X_MODE, T_LEN = 40, T_RATE, T_DIR, T_SHUF, E_REP, E_SPD, E_VOL, E_PAN, E_MOD, E_FBK, E_TSP,
       E_MODU, M_DST, M_SPD, M_WAV, M_DEP, M_TRK, M_OFF, G_BPM, G_SCALE, G_KEY, G_SYNC, G_TLEN, G_TRATE, G_TADV, G_TVAL,
       G_DAC, G_THEME, X_NONE = 255 };
static const uint8_t PAGE_SLOT[NPG][4] = {
  {S_NOTE, S_LEVEL, S_PAN, S_PORT}, {S_COND, S_WAIT, X_ECHO, X_TSP}, {S_CH1, S_CH2, S_CH3, X_MODE},
  {S_MULT, S_FRAC, S_DEPTH, S_FDBK}, {S_ATK, S_HOLD, S_REL, S_LEVEL}, {S_MATK, S_MREL, S_MEND, S_DEPTH},
  {S_SWD, S_SWR, S_PORT, S_NOTE},   {M_DST, M_SPD, M_WAV, M_DEP},    {M_TRK, M_OFF, X_NONE, X_NONE},
  {E_REP, E_SPD, E_VOL, E_PAN},     {E_MOD, E_FBK, E_TSP, E_MODU},   {T_LEN, T_RATE, T_DIR, T_SHUF},
  {G_BPM, G_SCALE, G_KEY, G_SYNC},  {G_TLEN, G_TRATE, G_TADV, G_TVAL}, {G_DAC, G_THEME, X_NONE, X_NONE},
};
static int page_fm_only(int pg) { return pg == PG_CHORD || pg == PG_MENV || pg == PG_SWEEP; }

static uint8_t view = V_GRID, group = G_EDIT, gpage[NG], sel, cursor, tcur, playing, lane_f = S_NOTE, last_knob;
static int8_t pal_browse = -3;
static uint16_t held;             /* white keys held, as steps */
static uint8_t held_turned, mod_used[BTN_COUNT];
static uint32_t press_ms[16], rel_ms[16], btn_ms[BTN_COUNT], knob_ms[4];
static uint8_t prev_fl[16];       /* a step's flags before its last tap: for double taps */
static int8_t last_tap = -1;
static uint8_t bk_used;           /* a black-key modifier did something while held */
static step_t pen[TRK];           /* the last step touched on each track: new steps take its sound */
static char msg[30];
static uint32_t msg_until, flash_until;
static pat_t snap[TRK];
static uint8_t snap_tsp[8], has_snap, on_snap;
static struct {
  uint8_t kind, n; /* 0 empty, 1 steps, 2 a track */
  uint8_t track;
  step_t st[STEPS];
  pat_t pat;
} clip;
static uint32_t taps[4], tap_n, last_edit, last_save, saved_sum, prev_ms;

enum { D_HEAD = 1, D_GRID = 2, D_LANE = 4, D_PIC = 8, D_STRIP = 16, D_CELLS = 32, D_ALL = 63 };
static uint8_t dirty = D_ALL, drawn_view = 255;
static int8_t drawn_pos[TRK] = {-2, -2, -2, -2, -2};

/* ---- colours: dark or light */
static uint16_t C_BG, C_PANEL, C_LINE, C_DIM, C_MUTE, C_TEXT, C_HI;
static const uint16_t TRACK_COL[TRK] = {RGB(139, 108, 255), RGB(84, 160, 255), RGB(255, 79, 154), RGB(255, 112, 80),
                                        RGB(196, 186, 168)};
static const uint16_t KC[4] = {RGB(77, 225, 240), RGB(200, 242, 90), RGB(242, 242, 245), RGB(255, 138, 61)};
static const uint16_t KC_LIGHT[4] = {RGB(0, 132, 160), RGB(88, 136, 0), RGB(40, 40, 52), RGB(214, 86, 0)};
static uint16_t kc(int i) { return S.theme ? KC_LIGHT[i] : KC[i]; }
static void theme(void) {
  if (S.theme) {
    C_BG = RGB(236, 234, 228), C_PANEL = RGB(216, 213, 205), C_LINE = RGB(190, 186, 176), C_DIM = RGB(120, 116, 108);
    C_MUTE = RGB(90, 88, 84), C_TEXT = RGB(20, 20, 26), C_HI = RGB(0, 0, 0);
  } else {
    C_BG = RGB(5, 5, 7), C_PANEL = RGB(18, 18, 25), C_LINE = RGB(38, 38, 50), C_DIM = RGB(96, 96, 112);
    C_MUTE = RGB(150, 150, 165), C_TEXT = RGB(242, 242, 245), C_HI = RGB(255, 255, 255);
  }
}
static uint16_t mix565(uint16_t a, uint16_t b, int t) { /* t 0..256: a -> b */
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + ((br - ar) * t >> 8)) << 11) | ((ag + ((bg - ag) * t >> 8)) << 5) | (ab + ((bb - ab) * t >> 8)));
}

/* ---- text */
static void say(const char *s) {
  int i = 0;
  for (; s[i] && i < 29; i++) msg[i] = s[i];
  msg[i] = 0;
  msg_until = millis() + 1300;
  dirty |= D_HEAD;
}
static char *put(char *d, const char *s) {
  while (*s) *d++ = *s++;
  *d = 0;
  return d;
}
static char *put_int(char *d, int32_t v) {
  char b[12];
  format_int(b, v);
  return put(d, b);
}
static char *put_hex2(char *d, int v) {
  static const char H[] = "0123456789ABCDEF";
  *d++ = H[(v >> 4) & 15];
  *d++ = H[v & 15];
  *d = 0;
  return d;
}
static char *put_signed(char *d, int32_t v) {
  if (v > 0) *d++ = '+';
  return put_int(d, v);
}
static char *put_note(char *d, int n) {
  d = put(d, NOTE_NAME[n % 12]);
  return put_int(d, n / 12 - 1);
}
static void center(int x, int w, int y, const char *s, uint16_t c, int font) {
  text(x + (w - text_width(s, font)) / 2, y, s, c, font);
}
static int msg_live(void) { return msg[0] && (int32_t)(msg_until - millis()) > 0; }

static pat_t *P(void) { return &S.live[sel]; }
static step_t *CS(void) { return &P()->st[cursor]; }
static int cur_page(void) { return GROUP_PAGES[group][gpage[group]]; }
static int slot_of(int i) {
  int pg = cur_page();
  int id = PAGE_SLOT[pg][i];
  if (IS_NZ(sel)) {
    if (page_fm_only(pg)) return X_NONE;
    if (id < NS && !FD[id].nname) return X_NONE;
    if (pg == PG_FM && i == 3) return X_NONE;
  }
  return id;
}

/* ---- values by id: the cursor step's, the track's or the global ones */
static int id_max(int id) {
  if (id < NS) return fmaxv(sel, id);
  switch (id) {
    case X_ECHO: case X_TSP: case X_MODE: return 1;
    case T_LEN: return 16;
    case T_RATE: return 7;
    case T_DIR: return 3;
    case E_REP: case M_SPD: case M_OFF: return 15;
    case E_SPD: return 95;
    case E_VOL: case E_MOD: case E_FBK: return 31;
    case E_PAN: return 3;
    case E_TSP: return 24;
    case E_MODU: return 24;
    case M_DST: return NMDEST - 1;
    case M_WAV: return 4;
    case M_DEP: return 127;
    case M_TRK: return TRK - 1;
    case G_BPM: return 300;
    case G_SCALE: return 15;
    case G_KEY: return 11;
    case G_SYNC: return 2;
    case G_TLEN: return 8;
    case G_TRATE: return 16;
    case G_TADV: return 2;
    case G_TVAL: return 48;
    case G_DAC: case G_THEME: return 1;
  }
  return 127;
}
static int id_min(int id) {
  if (id == T_LEN || id == G_TLEN || id == G_TRATE) return 1;
  if (id == G_BPM) return 30;
  return 0;
}
static uint8_t *id_ptr(pat_t *p, int id) {
  switch (id) {
    case T_LEN: return &p->len;
    case T_RATE: return &p->rate;
    case T_DIR: return &p->dir;
    case T_SHUF: return &p->shuf;
    case E_REP: return &p->erep;
    case E_SPD: return &p->espd;
    case E_VOL: return &p->evol;
    case E_PAN: return &p->epan;
    case E_MOD: return &p->emod;
    case E_FBK: return &p->efbk;
    case E_TSP: return &p->etsp;
    case E_MODU: return &p->emodu;
    case M_DST: return &p->mdst;
    case M_SPD: return &p->mspd;
    case M_WAV: return &p->mwav;
    case M_DEP: return &p->mdep;
    case M_TRK: return &p->mtrk;
    case M_OFF: return &p->moff;
    case G_SCALE: return &S.scale;
    case G_KEY: return &S.key;
    case G_SYNC: return &S.sync;
    case G_TLEN: return &S.tlen;
    case G_TRATE: return &S.trate;
    case G_TADV: return &S.tadv;
    case G_TVAL: return &S.tsp[tcur & 7];
    case G_DAC: return &S.dac;
    case G_THEME: return &S.theme;
  }
  return 0;
}
static int step_get(const step_t *st, int id) {
  if (id < NS) return st->v[id];
  if (id == X_ECHO) return (st->fl & FL_ECHO) != 0;
  if (id == X_TSP) return (st->fl & FL_TSP) != 0;
  if (id == X_MODE) return (st->fl & FL_PARA) != 0;
  return 0;
}
static int is_step_id(int id) { return id < NS || id == X_ECHO || id == X_TSP || id == X_MODE; }
static int id_get(int id) {
  if (is_step_id(id)) return step_get(CS(), id);
  if (id == G_BPM) return S.bpm;
  uint8_t *p = id_ptr(P(), id);
  return p ? *p : 0;
}

static void fmt_id(int t, int id, int v, char *out) { /* how a value reads, FMS-style hex where it is a level */
  char *d = out;
  *d = 0;
  int kind = id < NS ? fkind(t, id) : K_OFF;
  if (id < NS) {
    switch (kind) {
      case K_NOTE: put_note(d, IS_NZ(t) ? v : quantize(v)); return;
      case K_PAN: put(d, v == 0 ? "L" : v == 1 ? "C" : "R"); return;
      case K_COND: d = put(d, "1:"), put_int(d, v + 1); return;
      case K_WAIT: d = put_int(d, v), put(d, "/6"); return;
      case K_CH: if (!v) put(d, "--"); else put_signed(d, v); return;
      case K_MULT: put_hex2(d, v), d[0] = d[1], d[1] = 0; return;
      case K_FRAC: { static const char *const F[8] = {".0", "1/8", "1/4", "1/3", "1/2", "2/3", "3/4", ".97"}; put(d, F[v & 7]); } return;
      case K_NTYPE: put(d, NTYPE_NAME[v > 4 ? 4 : v]); return;
      default: put_hex2(d, v); return;
    }
  }
  switch (id) {
    case X_ECHO: case X_TSP: put(d, v ? "ON" : "OFF"); return;
    case X_MODE: put(d, v ? "+" : "FM"); return;
    case T_LEN: put_int(d, v); return;
    case T_RATE: put(d, RATE_NAME[v & 7]); return;
    case T_DIR: put(d, DIR_NAME[v & 3]); return;
    case E_REP: put_hex2(d, v), d[0] = d[1], d[1] = 0; return;
    case E_SPD: d = put_int(d, v % 6), *d++ = '.', put_int(d, v / 6); return; /* sub-step, then whole steps */
    case E_VOL: case E_MOD: case E_FBK: put_signed(d, v - 16); return;
    case E_PAN: put(d, EPAN_NAME[v & 3]); return;
    case E_TSP: put_signed(d, v - 12); return;
    case E_MODU: if (!v) put(d, "OFF"); else put_int(d, v); return;
    case M_DST: put(d, v ? (fname(S.live[sel].mtrk, MDEST[v]) ? fname(S.live[sel].mtrk, MDEST[v]) : "--") : "OFF"); return;
    case M_SPD: put_int(d, v + 1); return;
    case M_WAV: put(d, WAVE_NAME[v > 4 ? 4 : v]); return;
    case M_DEP: put_signed(d, v - 64); return;
    case M_TRK: if (v == NZ) put(d, "N"); else put_int(d, v + 1); return;
    case M_OFF: put_int(d, v); return;
    case G_BPM: put_int(d, v); return;
    case G_SCALE: put(d, SCALE[v & 15].name); return;
    case G_KEY: put(d, NOTE_NAME[v % 12]); return;
    case G_SYNC: put(d, SYNC_NAME[v % 3]); return;
    case G_TLEN: case G_TRATE: put_int(d, v); return;
    case G_TADV: put(d, ADV_NAME[v % 3]); return;
    case G_TVAL: put_signed(d, v - 24); return;
    case G_DAC: put(d, v ? "GBA" : "CLEAN"); return;
    case G_THEME: put(d, v ? "LIGHT" : "DARK"); return;
  }
  put_hex2(d, v);
}
static const char *id_name(int t, int id) {
  if (id < NS) return fname(t, id);
  if (id == S_DEPTH && IS_NZ(t)) return "FILT";
  switch (id) {
    case X_ECHO: return "ECHO";
    case X_TSP: return "TSP";
    case X_MODE: return "MODE";
    case T_LEN: return "LENGTH";
    case T_RATE: return "RATE";
    case T_DIR: return "DIR";
    case T_SHUF: return "SHUFFLE";
    case E_REP: return "REP";
    case E_SPD: return "SPD";
    case E_VOL: return "VOL";
    case E_PAN: return "PAN";
    case E_MOD: return "MOD";
    case E_FBK: return "FBK";
    case E_TSP: return "TSP";
    case E_MODU: return "MODULO";
    case M_DST: return "DEST";
    case M_SPD: return "SPEED";
    case M_WAV: return "WAVE";
    case M_DEP: return "DEPTH";
    case M_TRK: return "TRACK";
    case M_OFF: return "OFFSET";
    case G_BPM: return "TEMPO";
    case G_SCALE: return "SCALE";
    case G_KEY: return "KEY";
    case G_SYNC: return "SYNC";
    case G_TLEN: return "LENGTH";
    case G_TRATE: return "RATE";
    case G_TADV: return "ADV";
    case G_TVAL: return "VALUE";
    case G_DAC: return "DAC";
    case G_THEME: return "THEME";
  }
  return "";
}

static void apply_sync(void) {
  clock_send(S.sync != 0);
  clock_follow(S.sync == 2 ? MIDI_ANY : 0);
}
static void apply_tempo(void) { clock_tempo((uint32_t)clampi(S.bpm + nudge, 30, 300) * 100u); }

/* the steps an edit lands on: held steps (two or more: everything between), else the cursor; SEQ held: all of them */
static uint16_t targets(void) {
  if (button_held(BTN_SEQ)) return 0xFFFF;
  if (held) {
    int lo = 0, hi = 15;
    while (!((held >> lo) & 1)) lo++;
    while (!((held >> hi) & 1)) hi--;
    return (uint16_t)(((1u << (hi + 1)) - 1) & ~((1u << lo) - 1));
  }
  return (uint16_t)(1u << cursor);
}

static void step_nudge(int t, step_t *st, int id, int d) {
  if (id == X_ECHO || id == X_TSP || id == X_MODE) {
    uint8_t b = id == X_ECHO ? FL_ECHO : id == X_TSP ? FL_TSP : FL_PARA;
    if (d > 0) st->fl |= b;
    else st->fl &= (uint8_t)~b;
    st->fl |= FL_USED;
    return;
  }
  if (IS_NZ(t) && !FD[id].nname) return;
  int mx = fmaxv(t, id), v = st->v[id];
  if (id == S_NOTE && !IS_NZ(t)) { /* scale filters note entry */
    int dir = d > 0 ? 1 : -1, n = d > 0 ? d : -d;
    while (n-- > 0) {
      do v += dir;
      while (v > 0 && v < 127 && !in_scale(v));
    }
  } else {
    v += d;
  }
  st->v[id] = (uint8_t)clampi(v, 0, mx);
  st->fl |= FL_USED;
}

static void edit(int i, int d) {
  int id = slot_of(i);
  if (id == X_NONE) return;
  last_knob = (uint8_t)i;
  last_edit = millis();
  if (is_step_id(id)) {
    if (id < NS) lane_f = (uint8_t)id;
    if (held) held_turned = 1;
    if (button_held(BTN_SEQ)) mod_used[BTN_SEQ] = 1;
    int all = button_held(BTN_REC);
    for (int t = 0; t < TRK; t++) {
      if (!all && t != sel) continue;
      uint16_t m = all ? 0xFFFF : targets();
      for (int s = 0; s < STEPS; s++)
        if ((m >> s) & 1) step_nudge(t, &S.live[t].st[s], id, d);
    }
    copy(&pen[sel], CS(), sizeof(step_t));
    if (!playing && id < NS && id != S_COND && id != S_WAIT) prev_step = cursor, prev_req = (uint8_t)(sel + 1);
    dirty |= D_GRID | D_LANE | D_PIC | D_STRIP;
    return;
  }
  if (id == G_BPM) {
    S.bpm = (uint16_t)clampi(S.bpm + d, 30, 300);
    apply_tempo();
  } else {
    int all = button_held(BTN_REC) && id >= T_LEN && id <= T_SHUF; /* REC: direction, rate, length, shuffle on every track */
    for (int t = 0; t < TRK; t++) {
      if (!all && t != sel) continue;
      uint8_t *p = id_ptr(&S.live[t], id);
      if (!p) continue;
      int mx = id_max(id);
      if (id == M_DST) mx = NMDEST - 1;
      *p = (uint8_t)clampi(*p + d, id_min(id), mx);
    }
    if (id == G_SYNC) apply_sync();
    if (id == G_THEME) theme(), drawn_view = 255;
  }
  dirty |= D_GRID | D_LANE | D_PIC | D_STRIP | D_HEAD;
}

/* ---- the step tools */
static void random_sound(int t, step_t *st) {
  uint8_t *v = st->v;
  if (IS_NZ(t)) {
    v[S_MULT] = (uint8_t)(random32() % 5u), v[S_FRAC] = (uint8_t)(random32() % 128u), v[S_DEPTH] = (uint8_t)(random32() % 100u);
    v[S_NOTE] = (uint8_t)(60 + random32() % 48u);
  } else {
    v[S_MULT] = (uint8_t)(random32() % 9u), v[S_FRAC] = (uint8_t)(random32() & 7u), v[S_DEPTH] = (uint8_t)(random32() % 110u);
    v[S_FDBK] = (uint8_t)(random32() & 1 ? random32() % 70u : 0), v[S_MATK] = (uint8_t)(random32() & 3 ? 0 : random32() % 60u);
    v[S_MREL] = (uint8_t)(30 + random32() % 70u), v[S_MEND] = (uint8_t)(random32() % 80u);
    v[S_SWD] = (uint8_t)(random32() & 3 ? 0 : random32() % 60u), v[S_SWR] = (uint8_t)(20 + random32() % 50u);
    if (random32() % 6u == 0) st->fl ^= FL_PARA;
  }
  v[S_ATK] = (uint8_t)(random32() & 3 ? 0 : random32() % 60u), v[S_HOLD] = (uint8_t)(random32() % 40u);
  v[S_REL] = (uint8_t)(20 + random32() % 80u);
  st->fl |= FL_USED;
}
static void cook(int t, step_t *st) { /* an incremental random edit: a few values move a little */
  for (int k = 0; k < 3; k++) {
    int f = S_SOUND + (int)(random32() % NSOUND);
    if (IS_NZ(t) && !FD[f].nname) continue;
    int mx = fmaxv(t, f);
    int r = mx < 16 ? 1 : 2 + (int)(random32() % 7u);
    step_nudge(t, st, f, random32() & 1 ? r : -r);
  }
  if (random32() % 5u == 0) step_nudge(t, st, S_NOTE, random32() & 1 ? 1 : -1);
}
static void pal_load(int t, step_t *st, int slot) { /* -2: X (clean), -1: ? (random), 0..11 */
  if (slot == -2) {
    uint8_t keep_note = st->v[S_NOTE], fl = st->fl;
    step_t d;
    step_default(t, &d);
    copy(&st->v[S_SOUND], &d.v[S_SOUND], NSOUND);
    st->v[S_NOTE] = keep_note, st->fl = (uint8_t)((fl & ~FL_PARA) | FL_USED);
  } else if (slot == -1) {
    random_sound(t, st);
  } else {
    const uint8_t *p = S.pal[IS_NZ(t)][slot];
    if (!p[NSOUND]) return;
    copy(&st->v[S_SOUND], p, NSOUND);
    st->fl = (uint8_t)((st->fl & ~FL_PARA) | (p[NSOUND] & FL_PARA) | FL_USED);
  }
}
static void rotate(int t, int dir) {
  pat_t *p = &S.live[t];
  int len = clampi(p->len, 1, STEPS);
  step_t tmp;
  if (dir > 0) {
    copy(&tmp, &p->st[len - 1], sizeof tmp);
    for (int s = len - 1; s > 0; s--) copy(&p->st[s], &p->st[s - 1], sizeof tmp);
    copy(&p->st[0], &tmp, sizeof tmp);
  } else {
    copy(&tmp, &p->st[0], sizeof tmp);
    for (int s = 0; s < len - 1; s++) copy(&p->st[s], &p->st[s + 1], sizeof tmp);
    copy(&p->st[len - 1], &tmp, sizeof tmp);
  }
}
static int pat_empty(const pat_t *p) {
  for (int s = 0; s < STEPS; s++)
    if (p->st[s].fl & FL_ON) return 0;
  return 1;
}
static void random_pattern(int t) {
  pat_t *p = &S.live[t];
  step_t base;
  copy(&base, &p->st[cursor], sizeof base);
  random_sound(t, &base);
  int root = IS_NZ(t) ? 84 : t == 0 ? 36 : t == 1 ? 36 : 60;
  for (int s = 0; s < STEPS; s++) {
    step_t *st = &p->st[s];
    copy(st, &base, sizeof base);
    st->fl &= (uint8_t)~(FL_ON | FL_TL);
    int dens = t == 0 ? 30 : IS_NZ(t) ? 55 : 35;
    if ((int)(random32() % 100u) < dens || (t == 0 && s % 4 == 0)) st->fl |= FL_ON;
    st->v[S_NOTE] = (uint8_t)(IS_NZ(t) ? root : quantize(root + (int)(random32() % 13u)));
    if (random32() % 4u == 0) cook(t, st);
  }
}

static void snapshot_take(void) {
  copy(snap, S.live, sizeof snap);
  copy(snap_tsp, S.tsp, 8);
  has_snap = 1, on_snap = 0;
  flash_until = millis() + 160;
  say("SNAPSHOT");
  dirty = D_ALL;
}
static void snapshot_swap(void) { /* the snapshot and the live state trade places */
  for (int t = 0; t < TRK; t++)
    for (uint32_t i = 0; i < sizeof(pat_t); i++) {
      uint8_t *a = (uint8_t *)&S.live[t] + i, *b = (uint8_t *)&snap[t] + i, x = *a;
      *a = *b, *b = x;
    }
  for (int i = 0; i < 8; i++) {
    uint8_t x = S.tsp[i];
    S.tsp[i] = snap_tsp[i], snap_tsp[i] = x;
  }
  on_snap ^= 1;
  say(on_snap ? "SNAPSHOT" : "LIVE");
  dirty = D_ALL;
}

/* ================================================================ drawing */

#define GX 17
#define GY 20
#define RH 15
static int cell_x(int s) { return GX + s * 13 + (s >> 2) * 3; }

static void draw_head(void) {
  rect(0, 0, SCREEN_W, 18, (int32_t)(flash_until - millis()) > 0 ? C_HI : C_BG);
  if (msg_live()) {
    text(4, 1, msg, kc(1), FONT_SMALL);
  } else {
    char b[32], *d = b;
    int pg = cur_page();
    if (view == V_DATA) d = put(d, "PATTERNS");
    else if (view == V_PAL) d = put(d, "PALETTE");
    else d = put(d, PAGE_NAME[pg]);
    text(4, 1, b, C_TEXT, FONT_SMALL);
    int x = 4 + text_width(b, FONT_SMALL) + 8;
    d = b;
    *d++ = IS_NZ(sel) ? 'N' : (char)('1' + sel);
    *d = 0;
    rect(x - 2, 1, 12, 16, TRACK_COL[sel]);
    text(x, 1, b, C_BG, FONT_SMALL);
    x += 14;
    if (S.loaded[sel]) {
      d = b, d = put(d, "S"), put_int(d, S.loaded[sel]);
      text(x, 1, b, C_DIM, FONT_SMALL);
    }
    if (button_held(BTN_REC)) text(x + 22, 1, "ALL", kc(3), FONT_SMALL);
    else if (button_held(BTN_SEQ)) text(x + 22, 1, "TRK", kc(3), FONT_SMALL);
  }
  char b[16], *d = b;
  if (has_snap) text(150, 1, on_snap ? "SNP" : "LIV", on_snap ? kc(3) : C_DIM, FONT_SMALL);
  d = put_int(d, clampi(S.bpm + nudge, 30, 300));
  text(SCREEN_W - 12 - text_width(b, FONT_SMALL), 1, b, nudge ? kc(3) : C_TEXT, FONT_SMALL);
  if (playing) {
    for (int j = 0; j < 9; j++) rect(SCREEN_W - 9, 4 + j, (j < 5 ? j : 8 - j) + 1, 1, kc(1));
  } else {
    rect(SCREEN_W - 9, 5, 6, 7, C_DIM);
  }
}

static void draw_cell(int t, int s) {
  pat_t *p = &S.live[t];
  step_t *st = &p->st[s];
  int x = cell_x(s), y = GY + t * RH;
  int len = clampi(p->len, 1, STEPS);
  int play = playing && play_pos[t] == s;
  int mute = soloed ? !((soloed >> t) & 1) : (muted >> t) & 1;
  uint16_t col = TRACK_COL[t];
  if (mute) col = mix565(col, C_BG, 170);
  rect(x - 1, y - 1, 14, 14, C_BG);
  if (s >= len) {
    rect(x + 5, y + 5, 2, 2, C_LINE);
  } else if (!(st->fl & FL_ON)) {
    rect(x, y, 12, 12, play ? C_LINE : C_PANEL);
    if (s % 4 == 0) rect(x + 5, y + 5, 2, 2, C_LINE);
  } else if (st->fl & FL_TL) {
    frame(x, y, 12, 12, col);
    frame(x + 1, y + 1, 10, 10, play ? C_HI : mix565(col, C_BG, 120));
  } else {
    int lv = st->v[S_LEVEL];
    rect(x, y, 12, 12, play ? mix565(col, C_HI, 150) : mix565(C_BG, col, 90 + lv * 166 / 127));
    if (!IS_NZ(t) && (st->v[S_CH1] | st->v[S_CH2] | st->v[S_CH3])) rect(x + 8, y + 1, 3, 3, C_BG);
    if (st->fl & FL_ECHO) rect(x + 1, y + 9, 5, 2, C_BG);
    if (st->v[S_COND]) rect(x + 1, y + 1, 2, 5, C_BG);
  }
  if (t == sel && view == V_GRID) {
    uint16_t m = targets();
    if (s == cursor) frame(x - 1, y - 1, 14, 14, C_HI);
    else if (((held != 0 || button_held(BTN_SEQ)) && ((m >> s) & 1))) frame(x - 1, y - 1, 14, 14, kc(3));
  }
}
static void draw_row_label(int t) {
  int y = GY + t * RH;
  int mute = soloed ? !((soloed >> t) & 1) : (muted >> t) & 1;
  rect(0, y - 1, GX - 3, 14, t == sel ? TRACK_COL[t] : C_BG);
  char b[2] = {IS_NZ(t) ? 'N' : (char)('1' + t), 0};
  text(3, y - 2, b, t == sel ? C_BG : mute ? C_LINE : TRACK_COL[t], FONT_SMALL);
  if (vstate[t < NFM ? t : NFM]) rect(GX - 4, y + 3, 2, 6, t == sel ? C_BG : TRACK_COL[t]);
}
static void draw_grid(void) {
  rect(0, GY - 2, SCREEN_W, TRK * RH + 1, C_BG);
  for (int t = 0; t < TRK; t++) {
    draw_row_label(t);
    for (int s = 0; s < STEPS; s++) draw_cell(t, s);
    drawn_pos[t] = playing ? play_pos[t] : -1;
  }
}
static void draw_cells(void) { /* only the playheads that moved */
  for (int t = 0; t < TRK; t++) {
    int p = playing ? play_pos[t] : -1;
    if (p == drawn_pos[t]) continue;
    if (drawn_pos[t] >= 0) draw_cell(t, drawn_pos[t]);
    if (p >= 0) draw_cell(t, p);
    drawn_pos[t] = (int8_t)p;
    draw_row_label(t);
  }
}

/* the value of one step setting across the 16 steps, under the grid's columns */
static void draw_lane(void) {
  int y0 = 98;
  rect(0, y0, SCREEN_W, 34, C_BG);
  int f = lane_f, step_page = 0;
  for (int i = 0; i < 4; i++) step_page |= slot_of(i) < NS;
  if (!step_page || (IS_NZ(sel) && !FD[f].nname)) f = S_NOTE;
  int ki = -1;
  for (int i = 0; i < 4; i++)
    if (slot_of(i) == f) ki = i;
  uint16_t c = ki >= 0 ? kc(ki) : C_MUTE;
  char b[24], *d = b;
  d = put(d, fname(sel, f));
  text(GX, y0, b, c, FONT_SMALL);
  fmt_id(sel, f, CS()->v[f], b);
  text(SCREEN_W - 6 - text_width(b, FONT_SMALL), y0, b, C_TEXT, FONT_SMALL);
  pat_t *p = P();
  int mx = fmaxv(sel, f);
  int lo = 0, hi = mx;
  if (f == S_NOTE) { /* notes: fit the range in use */
    lo = 127, hi = 0;
    for (int s = 0; s < STEPS; s++) {
      int v = p->st[s].v[S_NOTE];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (hi - lo < 12) lo = lo - (12 - (hi - lo)) / 2, hi = lo + 12;
  }
  for (int s = 0; s < STEPS; s++) {
    int v = p->st[s].v[f], x = cell_x(s);
    int h = hi > lo ? (v - lo) * 15 / (hi - lo) + 1 : 1;
    h = clampi(h, 1, 16);
    int on = (p->st[s].fl & FL_ON) && s < p->len;
    rect(x + 2, y0 + 33 - h, 8, h, on ? c : mix565(C_BG, c, 70));
    if (s == cursor) rect(x, y0 + 33, 12, 1, C_HI);
  }
}

/* ---- the picture of the page */
#define PY 134
#define PH 60
static void env_shape(int x, int w, int y, int h, int a, int hl, int r, int end, uint16_t c) { /* attack, hold, release */
  int tot = a + hl + r + 1;
  int xa = x + a * w / tot, xh = xa + hl * w / tot, xr = x + w;
  int ye = y + h - end * h / 127;
  line(x, y + h, xa, y, c);
  line(xa, y, xh, y, c);
  int px = xh, py = y;
  for (int i = 1; i <= 12; i++) { /* exponential fall */
    int xx = xh + (xr - xh) * i / 12;
    int k = 256 >> (i * 8 / 12);
    int yy = ye - (ye - y) * k / 256;
    line(px, py, xx, yy, c);
    px = xx, py = yy;
  }
}
static void draw_wave_dots(int x, int y, int w, int h, int wav, int n, int off, int ph, uint16_t c) {
  for (int i = 0; i < 16; i++) {
    int p = (int)(((uint32_t)i + (uint32_t)off * (uint32_t)n / 16u) % (uint32_t)n);
    int32_t v = wav == 4 ? (int32_t)((i * 40503 + 7919) & 0xFFFF) - 32768 : wave(wav, (uint32_t)p, (uint32_t)n);
    int yy = y + h / 2 - v * (h / 2 - 2) / 32767;
    int xx = x + i * w / 16;
    rect(xx, yy - 1, 6, 3, i == ph ? C_HI : c);
  }
}
static void op_box(int x, int y, const char *name, const char *val, uint16_t c) {
  frame(x, y, 56, 34, c);
  text(x + 4, y + 1, name, c, FONT_SMALL);
  text(x + 4, y + 17, val, C_TEXT, FONT_SMALL);
}
static void draw_pic(void) {
  rect(0, PY, SCREEN_W, PH, C_BG);
  int pg = cur_page();
  step_t *st = CS();
  const uint8_t *v = st->v;
  char b[32], *d;
  int nz = IS_NZ(sel);
  if (view == V_DATA) {
    text(4, PY, "1-5 CUE  6 CLEAR  7 RANDOM", C_MUTE, FONT_SMALL);
    text(4, PY + 15, "SEQ+ NOW  SEL+ NOW, RESET", C_DIM, FONT_SMALL);
    text(4, PY + 30, "SAVE+ STORE  REC+ COLUMN", C_DIM, FONT_SMALL);
    text(4, PY + 45, "16 LOCK", C_DIM, FONT_SMALL);
    return;
  }
  if (view == V_PAL) {
    text(4, PY, nz ? "NOISE SOUNDS" : "FM SOUNDS", C_TEXT, FONT_SMALL);
    text(4, PY + 15, "1 X CLEAN  2 ? RANDOM", C_MUTE, FONT_SMALL);
    text(4, PY + 30, "3-14 SLOTS 0-B  SAVE+ STORE", C_DIM, FONT_SMALL);
    text(4, PY + 45, "SEQ+ TRACK  REC+ ALL", C_DIM, FONT_SMALL);
    return;
  }
  switch (pg) {
    case PG_NOTE: {
      d = b;
      put_note(d, nz ? v[S_NOTE] : out_note(sel, v, st->fl));
      text(8, PY + 8, b, kc(0), FONT_BIG);
      for (int i = 0; i < 3; i++) { /* L C R */
        static const char *const LCR[3] = {"L", "C", "R"};
        int x = 120 + i * 20;
        if (v[S_PAN] == i) rect(x - 2, PY + 8, 14, 18, kc(2));
        text(x + 1, PY + 9, LCR[i], v[S_PAN] == i ? C_BG : C_DIM, FONT_SMALL);
      }
      rect(120, PY + 34, 100, 6, C_PANEL);
      rect(120, PY + 34, v[S_LEVEL] * 100 / 127, 6, kc(1));
      rect(120, PY + 46, 100, 6, C_PANEL);
      rect(120, PY + 46, v[S_PORT] * 100 / 127, 6, kc(3));
      text(8, PY + 42, st->fl & FL_TL ? "TRIGLESS" : st->fl & FL_ON ? "TRIG" : "OFF", C_DIM, FONT_SMALL);
    } break;
    case PG_TRIG: {
      d = b, d = put(d, "1:"), put_int(d, v[S_COND] + 1);
      text(8, PY + 8, b, kc(0), FONT_BIG);
      for (int i = 0; i < 6; i++) { /* a step in sixths: where the trig falls */
        int x = 96 + i * 22;
        rect(x, PY + 14, 18, 12, i == v[S_WAIT] ? kc(1) : C_PANEL);
      }
      text(96, PY - 1, "WAIT", C_DIM, FONT_SMALL);
      int x = 96;
      rect(x, PY + 36, 56, 18, st->fl & FL_ECHO ? kc(2) : C_PANEL);
      center(x, 56, PY + 37, "ECHO", st->fl & FL_ECHO ? C_BG : C_DIM, FONT_SMALL);
      rect(x + 64, PY + 36, 56, 18, st->fl & FL_TSP ? kc(3) : C_PANEL);
      center(x + 64, 56, PY + 37, "TSP", st->fl & FL_TSP ? C_BG : C_DIM, FONT_SMALL);
    } break;
    case PG_CHORD: { /* two octaves of keys from the step's note */
      int base = quantize(v[S_NOTE]) % 12;
      for (int pass = 0; pass < 2; pass++) /* white keys, then the black ones over them */
        for (int k = 0; k < 25; k++) {
          int black = (0x54A >> ((base + k) % 12)) & 1;
          if (black != pass) continue;
          int x = 8 + k * 9;
          int c = k == 0 ? 2 : k == v[S_CH1] ? 0 : k == v[S_CH2] ? 1 : k == v[S_CH3] ? 3 : -1;
          uint16_t col = c >= 0 ? kc(c) : black ? C_PANEL : C_LINE;
          if (black) rect(x - 1, PY + 4, 10, 22, C_BG), rect(x, PY + 4, 8, 21, col);
          else rect(x, PY + 4, 8, 34, col);
        }
      text(8, PY + 42, st->fl & FL_PARA ? "MODE + (PARALLEL)" : "MODE FM", kc(3), FONT_SMALL);
    } break;
    case PG_FM: {
      if (nz) {
        int ty = v[S_MULT] > 4 ? 4 : v[S_MULT];
        text(8, PY + 4, NTYPE_NAME[ty], kc(0), FONT_BIG);
        text(56, PY + 4, NTYPE_LONG[ty], C_TEXT, FONT_SMALL);
        text(56, PY + 20, ty <= 1 ? "RATE: LFSR LENGTH" : ty == 2 ? "RATE: PARTIALS" : ty == 3 ? "RATE: DENSITY" : "RATE: CLOCK",
             kc(1), FONT_SMALL);
        text(56, PY + 36, ty == 4 ? "FILT: RESONANCE" : "FILT: HIGHPASS", kc(2), FONT_SMALL);
        break;
      }
      static const char *const F[8] = {"", ".125", ".25", ".333", ".5", ".667", ".75", ".975"};
      d = b, d = put_int(d, v[S_MULT]), put(d, F[v[S_FRAC] & 7]);
      char dv[8];
      put_hex2(dv, v[S_DEPTH]);
      if (st->fl & FL_PARA) {
        op_box(8, PY + 4, "MOD", b, kc(0));
        text(70, PY + 12, "+", C_TEXT, FONT_BIG);
        op_box(92, PY + 4, "CAR", "1", kc(2));
      } else {
        op_box(8, PY + 4, "MOD", b, kc(0));
        line(64, PY + 21, 92, PY + 21, kc(2));
        line(86, PY + 17, 92, PY + 21, kc(2));
        line(86, PY + 25, 92, PY + 21, kc(2));
        op_box(92, PY + 4, "CAR", "1", kc(2));
        text(66, PY + 26, dv, kc(2), FONT_SMALL);
      }
      int fb = v[S_FDBK]; /* the feedback loop, thicker with more */
      uint16_t c = fb ? kc(3) : C_LINE;
      line(20, PY + 4, 20, PY - 0, c), line(20, PY, 52, PY, c), line(52, PY, 52, PY + 4, c);
      if (fb > 40) line(21, PY + 4, 21, PY + 1, c), line(51, PY + 1, 51, PY + 4, c);
      put_hex2(b, fb);
      text(160, PY + 4, "FDBK", kc(3), FONT_SMALL);
      text(200, PY + 4, b, C_TEXT, FONT_SMALL);
      text(160, PY + 22, "DEPTH", kc(2), FONT_SMALL);
      text(208, PY + 22, dv, C_TEXT, FONT_SMALL);
      rect(160, PY + 44, 72, 6, C_PANEL);
      rect(160, PY + 44, v[S_DEPTH] * 72 / 127, 6, kc(2));
    } break;
    case PG_AMP:
      env_shape(12, 210, PY + 6, 44, v[S_ATK] + 2, v[S_HOLD], v[S_REL] + 4, 0, kc(2));
      line(12, PY + 6, 12 + (v[S_ATK] + 2) * 210 / (v[S_ATK] + v[S_HOLD] + v[S_REL] + 7), PY + 6, kc(0));
      break;
    case PG_MENV:
      env_shape(12, 210, PY + 6, 44, v[S_MATK] + 1, 0, v[S_MREL] + 4, v[S_MEND], kc(1));
      rect(12, PY + 54, v[S_DEPTH] * 210 / 127, 3, kc(3));
      break;
    case PG_SWEEP: {
      int dep = v[S_SWD] * 40 / 127;
      env_shape(12, 210, PY + 50 - dep, dep + 1, 0, 0, v[S_SWR] + 4, 0, kc(0));
      line(12, PY + 50, 222, PY + 50, C_LINE);
      d = b, d = put(d, "+"), put_int(d, v[S_SWD] / 2);
      text(160, PY + 4, b, kc(0), FONT_SMALL);
      text(196, PY + 4, "ST", C_DIM, FONT_SMALL);
    } break;
    case PG_MOD1:
    case PG_MOD2: {
      pat_t *p = P();
      int n = p->mspd + 1;
      draw_wave_dots(12, PY + 2, 210, 36, p->mwav, n, p->moff, -1, kc(2));
      d = b, d = put(d, "TO "), *d++ = p->mtrk == NZ ? 'N' : (char)('1' + p->mtrk), *d = 0;
      d = put(d, " ");
      put(d, p->mdst ? (fname(p->mtrk, MDEST[p->mdst]) ? fname(p->mtrk, MDEST[p->mdst]) : "--") : "OFF");
      text(12, PY + 42, b, p->mdst ? kc(0) : C_DIM, FONT_SMALL);
      d = b, put_signed(d, p->mdep - 64);
      text(180, PY + 42, b, kc(3), FONT_SMALL);
    } break;
    case PG_ECHO1:
    case PG_ECHO2: { /* the repeats: height its level, place its pan */
      pat_t *p = P();
      int lv = v[S_LEVEL] ? v[S_LEVEL] : 96, sp = p->espd ? p->espd : 1;
      for (int r = 0; r <= p->erep; r++) {
        int l = r ? clampi(lv + ((int)p->evol - 16) * 4 * r, 0, 127) : lv;
        int x = 12 + r * sp * 3;
        if (x > 226) break;
        int pan = r == 0 ? 1 : p->epan == 0 ? v[S_PAN] : p->epan == 1 ? 0 : p->epan == 2 ? 2 : (r & 1) ? 0 : 2;
        int h = l * 40 / 127 + 1;
        rect(x, PY + 44 - h, 4, h, r ? kc(1) : kc(2));
        rect(x, PY + 48 + pan * 3, 4, 2, kc(3));
        int ts = ((int)p->etsp - 12) * r;
        if (p->emodu) ts %= (int)p->emodu;
        if (ts) {
          d = b, put_signed(d, ts);
          if (sp * 3 >= 18) text(x - 2, PY - 1, b, C_DIM, FONT_SMALL);
        }
      }
      if (!(st->fl & FL_ECHO)) text(124, PY + 44, "STEP: ECHO OFF", C_DIM, FONT_SMALL);
    } break;
    case PG_TRACK: {
      pat_t *p = P();
      d = b, d = put_int(d, p->len), d = put(d, " STEPS  "), d = put(d, RATE_NAME[p->rate & 7]);
      text(8, PY + 2, b, C_TEXT, FONT_SMALL);
      text(8, PY + 20, DIR_NAME[p->dir & 3], kc(2), FONT_BIG);
      for (int s = 0; s < 8; s++) { /* shuffle: every other step, later */
        int x = 110 + s * 15 + ((s & 1) ? p->shuf * 7 / 127 : 0);
        rect(x, PY + 30, 6, 14, s & 1 ? kc(3) : C_LINE);
      }
    } break;
    case PG_GLOBAL: {
      d = b, put_int(d, S.bpm);
      text(8, PY + 4, b, kc(0), FONT_BIG);
      text(8, PY + 40, "BPM", C_DIM, FONT_SMALL);
      text(80, PY + 4, SCALE[S.scale].name, kc(1), FONT_SMALL);
      for (int i = 0; i < 12; i++) { /* the scale on a ring of 12 */
        int on = (SCALE[S.scale].mask >> i) & 1;
        rect(80 + i * 12, PY + 24, 10, 10, on ? (i == 0 ? kc(2) : kc(1)) : C_PANEL);
      }
      d = b, d = put(d, "KEY "), d = put(d, NOTE_NAME[S.key % 12]), d = put(d, "  SYNC "), put(d, SYNC_NAME[S.sync % 3]);
      text(80, PY + 40, b, C_MUTE, FONT_SMALL);
    } break;
    case PG_TSP: {
      for (int i = 0; i < 8; i++) {
        int x = 12 + i * 27, on = i < S.tlen, val = (int)S.tsp[i] - 24;
        int h = val * 20 / 24;
        rect(x, PY + 1, 22, 50, i == tcur ? C_PANEL : C_BG);
        rect(x + 2, PY + 26 - (h > 0 ? h : 0), 18, h > 0 ? h : -h ? -h : 1, on ? (i == tcur ? kc(3) : kc(1)) : C_LINE);
        if (playing && i == tpos_out && on) rect(x, PY + 52, 22, 3, C_HI);
        d = b, put_signed(d, val);
        if (on) center(x, 22, PY + 34, b, i == tcur ? C_TEXT : C_DIM, FONT_SMALL);
      }
    } break;
    case PG_SYS: {
      d = b, d = put(d, "CPU "), d = put_int(d, load_pct), put(d, "%");
      text(8, PY + 4, b, C_TEXT, FONT_SMALL);
      text(8, PY + 22, "7F: AFTER FMS, UNOFFICIAL", C_DIM, FONT_SMALL);
      text(8, PY + 38, "FMS IS BY FORS", C_DIM, FONT_SMALL);
    } break;
  }
}

static void draw_strip(void) {
  for (int i = 0; i < 4; i++) {
    int x = i * 60, y = 196;
    rect(x, y, 59, 44, C_PANEL);
    int id = view == V_GRID ? slot_of(i) : X_NONE;
    if (id == X_NONE) {
      rect(x, y, 59, 3, C_LINE);
      continue;
    }
    rect(x, y, 59, 3, kc(i));
    text(x + 3, y + 5, id_name(sel, id), kc(i), FONT_SMALL);
    char b[16];
    int v = id_get(id);
    if (id == M_DST) {
      int m = P()->mtrk;
      put(b, v ? (fname(m, MDEST[v]) ? fname(m, MDEST[v]) : "--") : "OFF");
    } else {
      fmt_id(sel, id, v, b);
    }
    text(x + 3, y + 22, b, C_TEXT, FONT_SMALL);
    int mx = id_max(id), mn = id_min(id);
    rect(x + 3, y + 39, 53, 2, C_LINE);
    if (mx > mn) rect(x + 3, y + 39, (v - mn) * 53 / (mx - mn), 2, kc(i));
  }
}

static void draw_data(void) { /* five tracks, the live pattern and five slots each */
  rect(0, GY - 2, SCREEN_W, 112, C_BG);
  text(GX + 2, GY - 2, "LIVE", C_DIM, FONT_SMALL);
  for (int i = 0; i < SLOTS; i++) {
    char b[4] = {(char)('1' + i), 0};
    text(GX + 58 + i * 34, GY - 2, b, C_DIM, FONT_SMALL);
  }
  for (int t = 0; t < TRK; t++) {
    int y = GY + 16 + t * 18;
    char b[2] = {IS_NZ(t) ? 'N' : (char)('1' + t), 0};
    rect(0, y - 1, GX - 3, 15, t == sel ? TRACK_COL[t] : C_BG);
    text(3, y - 2, b, t == sel ? C_BG : TRACK_COL[t], FONT_SMALL);
    for (int i = -1; i < SLOTS; i++) { /* a pattern drawn as 16 tiny steps */
      const pat_t *p = i < 0 ? &S.live[t] : &S.slot[t][i];
      int x = i < 0 ? GX + 2 : GX + 52 + i * 34;
      int has = i < 0 || ((S.used[t] >> i) & 1);
      int w = i < 0 ? 3 : 2;
      for (int s = 0; s < STEPS; s++) {
        int on = has && (p->st[s].fl & FL_ON) && s < p->len;
        rect(x + s * w, y + (s & 1) * 0, w - (w > 2), 12, on ? TRACK_COL[t] : C_PANEL);
      }
      if (i >= 0) {
        int cued = cue[t] == i && ((millis() >> 7) & 1);
        if (S.loaded[t] == i + 1 || cued) frame(x - 2, y - 2, 36, 16, cued ? kc(3) : C_HI);
      }
    }
  }
  if (S.lock) text(SCREEN_W - 40, GY - 2, "LOCK", kc(3), FONT_SMALL);
}

static void draw_pal(void) {
  rect(0, GY - 2, SCREEN_W, 112, C_BG);
  int k = IS_NZ(sel);
  for (int i = -2; i < PAL; i++) {
    int col = (i + 2) % 7, row = (i + 2) / 7;
    int x = 6 + col * 33, y = GY + row * 50;
    int sel_now = pal_browse == i;
    rect(x, y, 30, 44, sel_now ? kc(3) : C_PANEL);
    char b[4];
    if (i == -2) put(b, "X");
    else if (i == -1) put(b, "?");
    else put_hex2(b, i), b[0] = b[1], b[1] = 0;
    text(x + 11, y + 2, b, sel_now ? C_BG : C_TEXT, FONT_SMALL);
    if (i >= 0 && S.pal[k][i][NSOUND]) { /* a glimpse: depth and release as two bars */
      const uint8_t *p = S.pal[k][i];
      rect(x + 4, y + 40 - p[S_DEPTH - S_SOUND] * 20 / 127, 8, p[S_DEPTH - S_SOUND] * 20 / 127 + 1, TRACK_COL[sel]);
      rect(x + 16, y + 40 - p[S_REL - S_SOUND] * 20 / 127, 8, p[S_REL - S_SOUND] * 20 / 127 + 1, C_MUTE);
    }
  }
}

static void draw_all(void) {
  if (drawn_view != view) {
    clear(C_BG);
    drawn_view = view;
    dirty = D_ALL;
  }
  uint8_t d = dirty;
  dirty = 0;
  if (d & D_HEAD) draw_head();
  if (view == V_GRID) {
    if (d & D_GRID) draw_grid();
    else draw_cells();
    if (d & D_LANE) draw_lane();
  } else if (view == V_DATA) {
    if (d & (D_GRID | D_CELLS)) draw_data();
  } else {
    if (d & D_GRID) draw_pal();
  }
  if (d & D_PIC) draw_pic();
  if (d & D_STRIP) draw_strip();
}

/* ================================================================ setup and the loop */

void setup(void) {
  static const uint8_t IS_BLACK[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0}; /* from F */
  int w = 0, b = 0;
  for (int k = 0; k < KEY_COUNT; k++) {
    white_of[k] = black_of[k] = -1;
    if (IS_BLACK[k % 12]) black_key[b] = k, black_of[k] = (int8_t)b++;
    else white_key[w] = k, white_of[k] = (int8_t)w++;
  }
  for (int i = 0; i < 128; i++) ENVF[i] = i ? 22 * (int32_t)exp2_i(i * 6554) : 0; /* 1 ms .. ~3.3 s */
  if (settings_load(&S, sizeof S) != 0 || S.magic != MAGIC) init_all();
  else say("RESTORED");
  for (int t = 0; t < TRK; t++) copy(&pen[t], &S.live[t].st[0], sizeof(step_t));
  for (int i = 0; i < NFM; i++) FV[i].stage = 3, FV[i].owner = (uint8_t)i;
  NV.stage = 3, NV.lfsr = 0x7FFF;
  theme();
  clear(C_BG);
  apply_tempo();
  apply_sync();
  led_backlight(1);
  saved_sum = 0;
  ready = 1;
  print("7F ready");
}

static uint32_t sum_store(void) {
  uint32_t h = 2166136261u;
  const uint8_t *p = (const uint8_t *)&S;
  for (uint32_t i = 0; i < sizeof S; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

static void white_grid(int s, int pressed) {
  uint32_t now = millis();
  pat_t *p = P();
  step_t *st = &p->st[s];
  if (pressed) {
    if (held == 0) held_turned = 0;
    else held_turned = 1; /* two keys: a range, not a tap */
    held |= (uint16_t)(1u << s);
    cursor = (uint8_t)s;
    copy(&pen[sel], st, sizeof *st);
    press_ms[s] = now;
    if (last_tap == s && now - rel_ms[s] < 300) { /* a double tap: trigless on or off */
      st->fl = prev_fl[s];
      if ((st->fl & FL_ON) && (st->fl & FL_TL)) st->fl &= (uint8_t)~FL_TL;
      else {
        if (!(st->fl & FL_USED)) {
          uint8_t keep = st->v[S_NOTE];
          copy(st, &pen[sel], sizeof *st);
          st->v[S_NOTE] = keep;
        }
        st->fl |= FL_ON | FL_TL | FL_USED;
      }
      held_turned = 1; /* the release toggles nothing */
      last_tap = -1;
      say(st->fl & FL_TL ? "TRIGLESS" : "TRIG");
    }
    dirty |= D_GRID | D_LANE | D_PIC | D_STRIP;
    return;
  }
  held &= (uint16_t)~(1u << s);
  int multi = held != 0;
  if (!held_turned && !multi && now - press_ms[s] < 600) { /* a tap: the step on or off */
    prev_fl[s] = st->fl;
    if (st->fl & FL_ON) st->fl &= (uint8_t)~(FL_ON | FL_TL);
    else {
      if (!(st->fl & FL_USED)) { /* a fresh step takes the sound of the last one touched, like a pen */
        copy(st, &pen[sel], sizeof *st);
        st->fl &= (uint8_t)~FL_TL;
      }
      st->fl |= FL_ON | FL_USED;
    }
    last_tap = (int8_t)s;
    rel_ms[s] = now;
    last_edit = now;
  }
  if (!held) held_turned = 0;
  dirty |= D_GRID | D_LANE | D_PIC | D_STRIP;
}

static void white_data(int k) {
  int col = button_held(BTN_REC);
  if (k < SLOTS) {
    if (button_held(BTN_SAVE)) {
      mod_used[BTN_SAVE] = 1;
      if (S.lock) {
        say("LOCKED");
        return;
      }
      for (int t = 0; t < TRK; t++) {
        if (!col && t != sel) continue;
        if (pat_empty(&S.live[t])) { /* saving an empty track clears the slot */
          S.used[t] &= (uint8_t)~(1 << k);
          pat_default(t, &S.slot[t][k]);
        } else {
          copy(&S.slot[t][k], &S.live[t], sizeof(pat_t));
          S.used[t] |= (uint8_t)(1 << k);
          S.loaded[t] = (uint8_t)(k + 1);
        }
      }
      say(col ? "COLUMN SAVED" : "SLOT SAVED");
    } else {
      int now = !playing || button_held(BTN_SEQ) || button_held(BTN_SEL);
      if (button_held(BTN_SEQ)) mod_used[BTN_SEQ] = 1;
      for (int t = 0; t < TRK; t++) {
        if (!col && t != sel) continue;
        if (!((S.used[t] >> k) & 1)) continue;
        if (now) load_slot_now(t, k), cue[t] = -1;
        else cue[t] = (int8_t)k;
        if (button_held(BTN_SEL) && playing) reset_req |= (uint8_t)(1 << t);
      }
      say(now ? (button_held(BTN_SEL) ? "LOADED, RESET" : "LOADED") : "CUED");
      for (int t = 0; t < TRK; t++) copy(&pen[t], &S.live[t].st[0], sizeof(step_t));
    }
  } else if (k == 5) {
    for (int t = 0; t < TRK; t++)
      if (col || t == sel) pat_default(t, &S.live[t]), S.loaded[t] = 0;
    say("CLEARED");
  } else if (k == 6) {
    for (int t = 0; t < TRK; t++)
      if (col || t == sel) random_pattern(t), S.loaded[t] = 0;
    say("RANDOM");
  } else if (k == 15) {
    S.lock ^= 1;
    say(S.lock ? "LOCKED" : "UNLOCKED");
  }
  last_edit = millis();
  dirty = D_ALL;
}

static void white_pal(int k) {
  if (k > 13) return;
  int slot = k - 2, kind = IS_NZ(sel);
  if (button_held(BTN_SAVE)) {
    mod_used[BTN_SAVE] = 1;
    if (slot < 0) return;
    copy(S.pal[kind][slot], &CS()->v[S_SOUND], NSOUND);
    S.pal[kind][slot][NSOUND] = (uint8_t)(0x80 | (CS()->fl & FL_PARA));
    say("SOUND STORED");
  } else {
    if (slot >= 0 && !S.pal[kind][slot][NSOUND]) {
      say("EMPTY SLOT");
      return;
    }
    int all = button_held(BTN_SEQ), tracks = button_held(BTN_REC);
    if (all) mod_used[BTN_SEQ] = 1;
    for (int t = 0; t < TRK; t++) {
      if (tracks ? IS_NZ(t) != kind : t != sel) continue;
      for (int s = 0; s < STEPS; s++)
        if (all || tracks || s == cursor) pal_load(t, &S.live[t].st[s], slot);
    }
    pal_browse = (int8_t)slot;
    copy(&pen[sel], CS(), sizeof(step_t));
    if (!playing) prev_step = cursor, prev_req = (uint8_t)(sel + 1);
    say(slot == -2 ? "CLEAN" : slot == -1 ? "RANDOM SOUND" : "LOADED");
  }
  last_edit = millis();
  dirty = D_ALL;
}

static void black_down(int b) {
  if (b < TRK) {
    if (key_held(black_key[BK_MUTE])) {
      muted ^= (uint8_t)(1 << b), bk_used = 1;
    } else if (key_held(black_key[BK_SOLO])) {
      soloed ^= (uint8_t)(1 << b), bk_used = 1;
    } else {
      sel = (uint8_t)b;
      if (IS_NZ(sel) && page_fm_only(cur_page())) gpage[group] = 0;
      if (!playing) prev_step = cursor, prev_req = (uint8_t)(sel + 1);
    }
    dirty = D_ALL;
    return;
  }
  int all = button_held(BTN_REC);
  uint16_t m = held ? targets() : 0;
  pat_t *p = P();
  switch (b) {
    case BK_COPY:
      if (m) {
        clip.kind = 1, clip.track = sel, clip.n = 0;
        for (int s = 0; s < STEPS; s++)
          if ((m >> s) & 1) copy(&clip.st[clip.n++], &p->st[s], sizeof(step_t));
        say("STEPS COPIED");
      } else {
        clip.kind = 2, clip.track = sel;
        copy(&clip.pat, p, sizeof(pat_t));
        say("TRACK COPIED");
      }
      held_turned = 1;
      break;
    case BK_PASTE:
      if (clip.kind == 1) {
        int at = m ? 0 : cursor;
        if (m)
          while (!((m >> at) & 1)) at++;
        for (int i = 0; i < clip.n && at + i < STEPS; i++) copy(&p->st[at + i], &clip.st[i], sizeof(step_t));
        say("PASTED");
      } else if (clip.kind == 2) {
        copy(p, &clip.pat, sizeof(pat_t));
        if (IS_NZ(sel) != IS_NZ(clip.track)) /* FM into noise or back: keep the rhythm and notes, not the sound */
          for (int s = 0; s < STEPS; s++) {
            step_t d;
            step_default(sel, &d);
            copy(&p->st[s].v[S_SOUND], &d.v[S_SOUND], NSOUND);
          }
        if (p->mtrk == clip.track) p->mtrk = sel;
        say("PASTED");
      }
      held_turned = 1;
      break;
    case BK_RAND:
      for (int t = 0; t < TRK; t++) {
        if (!all && t != sel) continue;
        uint16_t mm = (!all && m) ? m : 0xFFFF;
        for (int s = 0; s < STEPS; s++)
          if ((mm >> s) & 1) cook(t, &S.live[t].st[s]);
      }
      held_turned = 1;
      say("COOKED");
      break;
    case BK_CLEAR:
      for (int t = 0; t < TRK; t++) {
        if (!all && t != sel) continue;
        uint16_t mm = (!all && m) ? m : 0xFFFF;
        for (int s = 0; s < STEPS; s++)
          if ((mm >> s) & 1) step_default(t, &S.live[t].st[s]);
      }
      held_turned = 1;
      say(m ? "STEPS CLEARED" : "TRACK CLEARED");
      break;
    case BK_MUTE:
    case BK_SOLO:
      bk_used = 0;
      break;
  }
  last_edit = millis();
  dirty = D_ALL;
}

static void group_press(int g) {
  if (view != V_GRID) {
    view = V_GRID;
    group = (uint8_t)g;
    return;
  }
  if (group == g) { /* again: the next page of the group (skipping FM-only pages on the noise track) */
    for (int n = 0; n < 4; n++) {
      gpage[g] = (uint8_t)(gpage[g] + 1);
      if (GROUP_PAGES[g][gpage[g]] == 255) gpage[g] = 0;
      if (!(IS_NZ(sel) && page_fm_only(cur_page()))) break;
    }
  }
  group = (uint8_t)g;
  int pg = cur_page();
  if (is_step_id(PAGE_SLOT[pg][0]) && PAGE_SLOT[pg][0] < NS && !(IS_NZ(sel) && !FD[PAGE_SLOT[pg][0]].nname)) lane_f = PAGE_SLOT[pg][0];
  dirty = D_ALL;
}

static int accel(int i, int d) {
  uint32_t now = millis(), dt = now - knob_ms[i];
  knob_ms[i] = now;
  int id = view == V_GRID ? slot_of(i) : X_NONE;
  if (id == X_NONE || id_max(id) - id_min(id) < 32) return d;
  return dt < 25 ? d * 4 : dt < 60 ? d * 2 : d;
}

void loop(void) {
  uint32_t now = millis();
  /* ---- buttons */
  if (button_pressed(BTN_PLAY)) {
    if (playing) clock_stop();
    else clock_start();
  }
  playing = running;
  for (int b = 0; b < BTN_COUNT; b++)
    if (button_pressed(b)) btn_ms[b] = now, mod_used[b] = 0;
  for (int g = 0; g < NG; g++) {
    int btn = GROUP_BTN[g];
    if (btn == BTN_SEQ || btn == BTN_GLO) { /* also held as layers: the page changes on a tap */
      if (button_released(btn) && !mod_used[btn] && now - btn_ms[btn] < 500) group_press(g);
    } else if (button_pressed(btn)) {
      group_press(g);
    }
  }
  if (button_pressed(BTN_SEQ) || button_released(BTN_SEQ) || button_pressed(BTN_REC) || button_released(BTN_REC)) dirty |= D_HEAD | D_GRID;
  if (button_pressed(BTN_HOME)) {
    view = view == V_DATA ? V_GRID : V_DATA;
    dirty = D_ALL;
  }
  if (button_pressed(BTN_ARP)) {
    view = view == V_PAL ? V_GRID : V_PAL;
    dirty = D_ALL;
  }
  if ((button_pressed(BTN_OCT_DOWN) || button_pressed(BTN_OCT_UP)) && button_held(BTN_SEQ)) { /* SEQ + OCT: rotate */
    int dir = button_pressed(BTN_OCT_UP) ? 1 : -1;
    for (int t = 0; t < TRK; t++)
      if (button_held(BTN_REC) || t == sel) rotate(t, dir);
    mod_used[BTN_SEQ] = 1;
    say(dir > 0 ? "ROTATE >" : "< ROTATE");
    last_edit = now;
    dirty = D_ALL;
  } else if (button_pressed(BTN_OCT_DOWN) || button_pressed(BTN_OCT_UP)) {
    sel = (uint8_t)((sel + (button_pressed(BTN_OCT_UP) ? 1 : TRK - 1)) % TRK);
    if (IS_NZ(sel) && page_fm_only(cur_page())) gpage[group] = 0;
    dirty = D_ALL;
  }
  if (view == V_GRID) { /* SAVE: a tap swaps the snapshot and the live state; held 0.8 s takes a snapshot */
    if (button_held(BTN_SAVE) && !mod_used[BTN_SAVE] && now - btn_ms[BTN_SAVE] > 800) {
      snapshot_take();
      mod_used[BTN_SAVE] = 1;
    }
    if (button_released(BTN_SAVE) && !mod_used[BTN_SAVE]) {
      if (has_snap) snapshot_swap();
      else snapshot_take();
    }
  }
  /* ---- the GLO layer: white keys 1 tap tempo, 2 / 3 nudge -10 / +10 while held, 4 restart */
  int glo = button_held(BTN_GLO);
  int8_t nd = 0;
  if (glo) {
    for (int k = 0; k < 4; k++) {
      int key = white_key[k];
      if (key_pressed(key)) mod_used[BTN_GLO] = 1;
      if (k == 0 && key_pressed(key)) {
        if (tap_n && now - taps[(tap_n - 1) & 3] > 2000) tap_n = 0;
        taps[tap_n++ & 3] = now;
        if (tap_n >= 4) {
          uint32_t span = taps[(tap_n - 1) & 3] - taps[(tap_n - 4) & 3];
          if (span) S.bpm = (uint16_t)clampi((int32_t)(180000u / span), 30, 300), apply_tempo();
        }
        say("TAP");
      }
      if (k == 1 && key_held(key)) nd = -10;
      if (k == 2 && key_held(key)) nd = 10;
      if (k == 3 && key_pressed(key) && playing) clock_stop(), clock_start();
    }
  }
  if (nd != nudge) nudge = nd, apply_tempo(), dirty |= D_HEAD;
  /* ---- keys */
  for (int k = 0; k < KEY_COUNT; k++) {
    int w = white_of[k], b = black_of[k];
    if (w >= 0) {
      if (glo && w < 4) continue;
      if (view == V_GRID) {
        if (key_pressed(k)) white_grid(w, 1);
        if (key_released(k)) white_grid(w, 0);
      } else if (key_pressed(k)) {
        if (view == V_DATA) white_data(w);
        else white_pal(w);
      }
    } else if (b >= 0) {
      if (key_pressed(k)) black_down(b);
      if (key_released(k) && b == BK_MUTE && !bk_used) muted = 0, soloed = 0, say("ALL ON"), dirty = D_ALL;
    }
  }
  /* ---- encoders */
  int32_t e = encoder(ENC_SELECT);
  if (e) {
    if (view == V_GRID && cur_page() == PG_TSP) tcur = (uint8_t)((tcur + 8 + (e > 0 ? 1 : -1)) % 8);
    else cursor = (uint8_t)((cursor + 16 + (e > 0 ? 1 : -1)) % 16), copy(&pen[sel], CS(), sizeof(step_t));
    dirty |= D_GRID | D_LANE | D_PIC | D_STRIP;
  }
  e = encoder(ENC_ALGORITHM);
  if (e) S.bpm = (uint16_t)clampi(S.bpm + e, 30, 300), apply_tempo(), dirty |= D_HEAD | D_PIC | D_STRIP;
  e = encoder(ENC_PRESETS); /* browse the palette into the cursor step (or held steps; SEQ: the track) */
  if (e) {
    int kind = IS_NZ(sel), n = pal_browse;
    for (int i = 0; i < PAL + 2; i++) {
      n += e > 0 ? 1 : -1;
      if (n > PAL - 1) n = -2;
      if (n < -2) n = PAL - 1;
      if (n < 0 || S.pal[kind][n][NSOUND]) break;
    }
    pal_browse = (int8_t)n;
    uint16_t m = view == V_GRID ? targets() : (uint16_t)(1u << cursor);
    if (held) held_turned = 1;
    for (int s = 0; s < STEPS; s++)
      if ((m >> s) & 1) pal_load(sel, &P()->st[s], n);
    copy(&pen[sel], CS(), sizeof(step_t));
    if (!playing) prev_step = cursor, prev_req = (uint8_t)(sel + 1);
    char b[20], *d = b;
    d = put(d, "PALETTE ");
    if (n == -2) put(d, "X");
    else if (n == -1) put(d, "?");
    else put_hex2(d, n), d[0] = d[1], d[1] = 0;
    say(b);
    last_edit = now;
    dirty |= D_GRID | D_LANE | D_PIC | D_STRIP;
  }
  for (int i = 0; i < 4; i++) {
    e = encoder(ENC_KNOB1 + i);
    if (e && view == V_GRID) edit(i, accel(i, (int)e));
  }
  /* ---- the screen */
  static uint8_t was_playing, was_tpos, was_flash;
  if (was_playing != playing) was_playing = playing, dirty |= D_HEAD | D_GRID;
  if (view == V_GRID && cur_page() == PG_TSP && was_tpos != tpos_out) was_tpos = tpos_out, dirty |= D_PIC;
  int any_cue = 0;
  for (int t = 0; t < TRK; t++) any_cue |= cue[t] >= 0;
  if (view == V_DATA && any_cue && ((now >> 7) & 1) != was_flash) was_flash = (uint8_t)((now >> 7) & 1), dirty |= D_CELLS;
  static uint32_t sys_ms;
  if (view == V_GRID && cur_page() == PG_SYS && now - sys_ms > 500) sys_ms = now, dirty |= D_PIC;
  if (msg[0] && !msg_live()) msg[0] = 0, dirty |= D_HEAD;
  if (flash_until && (int32_t)(flash_until - now) <= 0) flash_until = 0, dirty |= D_HEAD;
  draw_all();
  /* ---- LEDs: white keys show the selected track's steps, the playhead blinks */
  pat_t *p = P();
  int ph = playing ? play_pos[sel] : -1;
  for (int w = 0; w < 16; w++) {
    int on;
    if (view == V_DATA) on = w < SLOTS ? ((S.used[sel] >> w) & 1) && !(cue[sel] == w && ((now >> 7) & 1)) : w == 15 ? S.lock : 0;
    else if (view == V_PAL) on = w < 2 || (w < 14 && S.pal[IS_NZ(sel)][w - 2][NSOUND]);
    else {
      step_t *st = &p->st[w];
      on = (st->fl & FL_ON) && w < p->len;
      if (on && (st->fl & FL_TL)) on = (now >> 7) & 1;
      if (w == ph) on = !on;
    }
    led_key(white_key[w], on);
  }
  for (int b = 0; b < 11; b++) {
    int on = 0;
    if (b < TRK) on = b == sel;
    else if (b == BK_MUTE) on = muted != 0 || soloed != 0;
    led_key(black_key[b], on);
  }
  led_button(BTN_PLAY, playing);
  led_button(BTN_REC, button_held(BTN_REC));
  led_button(BTN_HOME, view == V_DATA);
  led_button(BTN_ARP, view == V_PAL);
  led_button(BTN_SAVE, on_snap ? (now >> 8) & 1 : has_snap);
  for (int g = 0; g < NG; g++) led_button(GROUP_BTN[g], view == V_GRID && group == g);
  /* ---- autosave: when stopped and left alone for 10 s, at most once a minute */
  if (!playing && now - last_edit > 10000 && now - last_save > 60000 && now - prev_ms > 1000) {
    prev_ms = now;
    uint32_t h = sum_store();
    if (h != saved_sum) {
      if (settings_save(&S, sizeof S) == 0) saved_sum = h, last_save = now, say("SAVED");
    }
  }
}
