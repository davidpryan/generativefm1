/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 GEN contributors. Built on Felucca, Copyright (C) 2026 Leo Kuroshita (@kurogedelic). */
/* GEN: a generative sequencer that writes into Felucca's own step format (core.h step_t) and nothing else.
 *
 * Every decision is a hash of (seed, track, lane, step, salt) against a threshold (gen_rand), so a part is reproducible
 * from a few bytes and every knob sweeps without reshuffling what it does not touch. Three seeds per track: RHYTHM
 * (where things happen), PITCH (what plays there) and VAR (which steps bar 4 and MUTATE re-decide). Sections A..D of a
 * song are the same seeds under a different ENERGY and role mask, so they stay recognisably one piece.
 *
 * Roles (one per track; the engine decides which are offered): DRUMS (a style's probability grid, 8 lanes x 32 steps,
 * MORPHed between two styles, with a nudge bias per lane for the late snare and the early hat), BASS (follows the
 * DRUMS track's kick, root / fifth / octave, 808 slides), CHORDS (the harmony bus's chord, voiced with the least
 * movement from the last one, at most 4 notes and never more than the voice budget), LEAD (a 2..4 note cell,
 * transformed across the phrase: repeat, transpose to a chord tone, invert, truncate; on a SLICE / SAMPLE track in
 * CHOP mode the notes are slices, so a vocal chop costs no lock record).
 * The harmony bus: one project-wide ROOT / SCALE (pushed to the pitched tracks' P_ROOT / P_SCALE, fix 3), a weighted
 * Markov table over scale degrees (PROG) stepped every RATE bars, and TENSION (0..255) rising over the 4-bar phrase that
 * the roles read for fills, leaps and rests.
 * Budgets (fixes 1 and 4): per-step chance, ratchet, nudge, accent, slide and velocity are fields of step_t and cost
 * nothing. The 128 motion records are shared by every track; GEN writes at most its budget of them per track
 * (GEN_LOCKS[]), tags its own (motion event ids above P_COUNT are impossible, so GEN keeps a bitmap of the steps it
 * wrote instead), and drops its lowest-priority ones first when the pool is short. CHORDS never writes more than 4
 * notes on a step and never more than its voice share (GEN_VOICES[]); DRUMS never more than 3 lanes on one step.
 *
 * LIVE: the generator re-renders the track every bar from the seeds; DRIFT advances a seed now and then. COMMIT (OCT+ in
 * the layer) writes the render into the steps for keeps; the hand editor takes over from there. Generation runs on the
 * UI side, once per bar at most, never in the audio ISR. The layer, the pages and the storage: gen_ui.c. */

/* ----------------------------------------------------------------- hashing --- */
static inline uint32_t gen_mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
/* one decision, 0..255, stable for its inputs */
static inline uint32_t gen_rand(uint32_t seed, uint32_t track, uint32_t lane, uint32_t step, uint32_t salt)
{
    return gen_mix(seed ^ (track << 24 | lane << 16 | step << 8 | salt)) & 255u;
}
static inline uint32_t gen_derive(uint32_t seed, uint32_t tag) { return gen_mix(seed * 0x9E3779B1U + tag + 1u); }
/* a fresh seed: the device's RNG (libc.c rng) */
static uint32_t gen_fresh(void) { uint32_t s = rng() ^ gen_mix(rng() << 1); return s ? s : 0xC0FFEEu; }

/* ------------------------------------------------------------------- sizes --- */
#define GEN_PHRASE 64u                   /* a phrase: 4 bars of 16 steps, NSTEP */
#define GEN_BAR 16u
#define GEN_NSEC 4u                      /* sections A..D, the project slots */
#define GEN_HIST 8u                      /* seeds PREV / NEXT walk back through */
#define GEN_NSTYLE 8u                    /* drum styles in flash (5 factory + 3 user) */
#define GEN_NPROG 8u                     /* progressions (6 factory + 2 user) */
#define GEN_NCELL 4u                     /* LEAD: notes of a cell */

enum { GR_OFF, GR_DRUMS, GR_BASS, GR_CHORDS, GR_LEAD, GR_N };   /* ROLE */
/* METHOD (GEN METHOD): the generator a role uses; 0 is always the one described with the role above */
enum { GMD_STYLE, GMD_EUCLID, GMD_CA, GMD_POLY, GMD_TURING, GMD_CALL, GMD_N };   /* DRUMS: style grid, Euclidean, cellular automaton,
                                                                                  * polymeter, Turing shift register, cross-track logic */
enum { GMB_FOLLOW, GMB_WALK, GMB_ARP, GMB_DRONE, GMB_MARKOV, GMB_CHAOS, GMB_CALL, GMB_N };   /* BASS: + Markov (learned), logistic map, cross-track */
enum { GMC_VOICE, GMC_PULSE, GMC_ARP, GMC_TURING, GMC_N };          /* CHORDS: voiced pad, Euclidean stabs, broken chord, Turing rhythm */
enum { GML_CELL, GML_WALK, GML_EUCLID, GML_LSYS, GML_TURING, GML_MARKOV, GML_CHAOS, GML_PINK, GML_CALL, GML_N };
                                                                    /* LEAD: + Turing, Markov (learned), logistic map, 1/f pink dice, cross-track */
#define GEN_METHODS_MAX 9u
enum { GS_INTRO, GS_VERSE, GS_BUILD, GS_DROP };                 /* sections' default meaning */
enum { GV_PITCH, GV_CHOP };                                     /* LEAD on a slice-mapped engine: VOX mode */
enum { GC_TRIAD, GC_SUS, GC_ADD9, GC_SEVENTH, GC_N };           /* CHORDS COLOR */
enum { GRH_PAD, GRH_HALF, GRH_STAB, GRH_ARP, GRH_N };          /* CHORDS RHYTHM */
#define GF_LIVE 1u                       /* gen_track_t.flags */
#define GF_LOCK 2u
#define GF_CHOP 4u                       /* VOX mode CHOP (else PITCH) */
#define GF_DIRTY 8u                      /* the project's key was edited by hand on SCL since the last push */
#define GM_TRK(i) (1u << (i))            /* role mask bits: tracks 0..3 */
#define GM_TEXTURE 16u                   /*   .. the texture bed (gen_ui.c: only when a bed exists) */
#define GM_NOKICK 32u                    /*   .. DRUMS without its KICK lane (BUILD) */

/* ------------------------------------------------------------------- state --- */
typedef struct {                         /* 24 bytes: the project's */
    uint32_t master_seed;
    uint8_t root, scale, prog, rate;     /* the harmony bus (fix 3); rate: 0 one chord per bar, 1 per 2, 2 per 4 */
    uint8_t energy[GEN_NSEC];            /* per section, 0..100 */
    uint8_t mask[GEN_NSEC];              /* per section: GM_TRK | GM_TEXTURE | GM_NOKICK */
    uint8_t tex_level, tex_rate;         /* the texture bed, when gen_ui.c has one to drive */
    uint8_t lock_scale;                  /* GEN LOCKS: 0 LOW (half), 1 NORM, 2 HIGH (double) */
    uint8_t version;
    uint8_t form, song_len, grow, tail;  /* 3: the song's FORM (GEN_FORM / GFM_*), LEN 0 HALF 1 NORM 2 LONG, GROW the grammar's
                                          * depth 1..4, TAIL 0 NONE 1 REPRISE 2 PEEL */
} gen_project_t;
typedef struct {                         /* 96 bytes: a track's */
    uint8_t role, style, morph_to, morph;
    uint8_t macro[4];                    /* the role's four knobs, 0..100 */
    uint8_t variation, drift, fill;      /* 0..100 */
    uint8_t flags;                       /* GF_* */
    uint8_t lane_lock;                   /* DRUMS: bit l = lane l keeps its steps */
    uint8_t hist_pos, hist_len;
    uint8_t locks_used;                  /* motion records GEN wrote on the last COMMIT */
    uint32_t seed_rhythm, seed_pitch, seed_var;
    struct { uint32_t rhythm, pitch; } history[GEN_HIST];
    uint8_t method, mp[3];               /* the role's generator (GM_*) and its three parameters, 0..100 (GEN METHOD) */
} gen_track_t;
_Static_assert(sizeof(gen_project_t) == 24u && sizeof(gen_track_t) == 96u, "GEN store layout");
#define GEN_VERSION 3u
typedef struct { gen_project_t p; gen_track_t t[NTRK]; } gen_state_t;
static gen_state_t gen;                  /* the working state (gen_ui.c keeps the flash copy per project slot) */
static uint64_t gen_wrote[NTRK];         /* COMMIT: bit s = GEN's locks stand on step s (its own, to replace) */
static uint8_t gen_section;              /* the section being rendered / committed, 0..3 */
static uint32_t gen_render_count;        /* bumped by every track render (gen_ui.c graph_gen_sig: the panel follows) */
static uint32_t gen_live_cycles;         /* LIVE: bars played since LIVE went on: the Turing registers run on it */

/* the voice share and the lock budget of each role (fixes 4 and 1) */
static const uint8_t GEN_VOICES[GR_N] = {0, 3, 1, 3, 1};
static const uint8_t GEN_LOCKS[GR_N] = {0, 8, 4, 4, 16};
static uint32_t gen_lock_budget(uint32_t role)
{
    uint32_t b = GEN_LOCKS[role % GR_N];
    return gen.p.lock_scale == 0u ? b / 2u : gen.p.lock_scale == 2u ? b * 2u : b;
}

/* ----------------------------------------------------------------- styles --- */
/* a drum style: 8 lanes x 32 steps of weights (the step's chance of a hit, 0..255, over 2 bars; bars 3..4 repeat
 * with the phrase's variation), a nudge bias per lane (-8..7 sixteenths of a step), a ghost weight and a swing
 * hint. Lanes: KICK SNARE CLAP HATCL HATOP TOM RIM BELL (eng_drum.c DRUM_LANE_NOTE) */
typedef struct {
    char name[8];
    uint8_t w[NLANE][32];
    int8_t nudge[NLANE];
    uint8_t ghost;                       /* the weight of a ghost (soft) copy of each lane's grid, 0..255 */
    uint8_t ratchet;                     /* HATCL ratchets at phrase ends, 0..255 */
    uint8_t triplet;                     /* HATOP / RIM bursts as x3 ratchets, 0..255 */
} gen_style_t;
#define _ 0
static const gen_style_t GEN_STYLE_FACTORY[5] = {
    {"2-STEP",                           /* Burial: the skipped second kick, the late snare, shuffled hats, ghosts */
     {{255, _, _, _, _, _, 90, _, _, _, 200, _, _, _, _, 60, 255, _, _, _, _, _, _, 120, _, _, 190, _, _, _, 40, _},
      {_, _, _, _, 255, _, _, _, _, _, _, _, 255, _, _, _, _, _, _, _, 255, _, _, _, _, _, _, _, 255, _, _, 70},
      {_, _, _, _, 40, _, _, _, _, _, _, _, 60, _, _, _, _, _, _, _, 40, _, _, _, _, _, _, _, 90, _, _, _},
      {_, _, 190, _, _, _, 200, _, _, _, 160, _, _, _, 220, _, _, _, 190, _, _, _, 200, _, _, _, 150, _, _, _, 230, _},
      {_, _, _, _, _, _, _, 110, _, _, _, _, _, _, _, 150, _, _, _, _, _, _, _, 120, _, _, _, _, _, _, _, 180},
      {_, _, _, _, _, _, _, _, _, 50, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 70, _, _, _, _, _, _},
      {_, 60, _, 90, _, _, _, 70, _, 120, _, _, _, 80, _, _, _, 60, _, 100, _, _, _, 60, _, 130, _, _, _, 90, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, 40, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 60, _}},
     {0, 2, 1, -1, 1, 0, -2, 0}, 110, 90, 60},
    {"HALF-T",                          /* trap / Purity Ring: snare on 3, sparse syncopated kick, hat rolls */
     {{255, _, _, _, _, _, _, 150, _, _, 120, _, _, _, _, _, 255, _, _, 90, _, _, _, 160, _, _, _, _, 110, _, _, _},
      {_, _, _, _, _, _, _, _, 255, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 255, _, _, _, _, _, 60, _},
      {_, _, _, _, _, _, _, _, 150, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 150, _, _, _, _, _, _, _},
      {230, _, 230, _, 230, _, 230, _, 230, _, 230, _, 230, 120, 230, _, 230, _, 230, _, 230, _, 230, _, 230, _, 230, _, 230, 160, 230, 200},
      {_, _, _, _, _, _, 90, _, _, _, _, _, _, _, 120, _, _, _, _, _, _, _, 90, _, _, _, _, _, _, _, 150, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 60, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 90},
      {_, _, _, _, 50, _, _, _, _, _, _, _, 50, _, _, _, _, _, _, _, 50, _, _, _, _, _, _, _, 50, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 50}},
     {0, 0, 0, 0, 0, 0, 1, 0}, 40, 230, 110},
    {"WITCH",                            /* witch house: very sparse, long gaps, triplet bursts, a bell */
     {{255, _, _, _, _, _, _, _, _, _, _, 140, _, _, _, _, 255, _, _, _, _, _, _, _, _, _, 110, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, 230, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 230, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, 200, _, _, _, _, _, _, 60, _, _, _, _, _, _, _, _, 200, _, _, _, _, _, _, _},
      {_, _, _, _, 90, _, _, _, _, _, _, _, 110, _, _, _, _, _, _, _, 90, _, _, _, _, _, _, _, 130, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, 80, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 140, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 90, _},
      {_, _, _, _, _, _, 60, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 60, _, _, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 120, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 160}},
     {0, 1, 0, 0, 0, 0, 0, 0}, 30, 60, 200},
    {"DNB",                              /* a broken 2-bar amen shape: snares on 2 and the 4-and */
     {{255, _, _, _, _, _, _, _, _, _, 220, _, _, _, _, _, 255, _, _, _, _, _, _, _, _, _, 200, _, 120, _, _, _},
      {_, _, _, _, 255, _, _, _, _, _, _, _, _, _, 230, _, _, _, _, _, 255, _, _, _, _, _, _, 90, _, _, 230, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _},
      {200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _, 200, _},
      {_, _, _, _, _, _, 70, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 70, _, _, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 90},
      {_, _, 80, _, _, _, _, 90, _, _, _, _, 80, _, _, _, _, _, 80, _, _, _, _, 90, _, _, _, _, 80, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _}},
     {0, 0, 0, 0, 0, 0, 0, 0}, 90, 160, 40},
    {"DRONE",                            /* ambient: a kick a bar, a bell, almost nothing else */
     {{255, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 255, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 90, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, 60, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 60, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 70, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _},
      {_, _, _, _, _, _, _, 150, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, 150, _, _, _, _, _, _, _, _}},
     {0, 0, 0, 0, 0, 0, 0, 0}, 20, 20, 30},
};
#undef _
static gen_style_t gen_style_user[3] __attribute__((section(".pool")));   /* the web editor fills these; empty = 2-STEP (the pool: .bss is tight) */
static const gen_style_t *gen_style(uint32_t i)
{
    i %= GEN_NSTYLE;
    if (i < 5u) return &GEN_STYLE_FACTORY[i];
    return gen_style_user[i - 5u].name[0] ? &gen_style_user[i - 5u] : &GEN_STYLE_FACTORY[0];
}

/* ----------------------------------------------------------- progressions --- */
/* a progression: 7 x 7 weights, row = the degree now (0 = the tonic), col = the next degree; the first chord is
 * row 7's pick from the tonic. Degrees are the scale's own, so a chord is diatonic in any scale */
typedef struct { char name[8]; uint8_t w[7][7]; uint8_t sevenths; } gen_prog_t;
static const gen_prog_t GEN_PROG_FACTORY[6] = {
    {"i-VI-III", {{0, 0, 0, 0, 0, 255, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 255}, {0, 0, 0, 0, 0, 0, 0},
                  {0, 0, 0, 0, 0, 0, 0}, {0, 0, 255, 0, 0, 0, 0}, {255, 0, 0, 0, 0, 0, 0}}, 0},
    {"i-iv-VI", {{0, 0, 0, 200, 60, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 255, 0},
                 {255, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 180, 0, 80}, {255, 0, 0, 0, 0, 0, 0}}, 0},
    {"i-VII-VI", {{0, 0, 0, 0, 0, 0, 255}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
                  {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 255}, {200, 0, 0, 0, 0, 80, 0}}, 0},
    {"SUSPEND", {{0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
                 {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}}, 1},   /* all zero: stay on i */
    {"WANDER", {{0, 20, 60, 80, 40, 90, 30}, {120, 0, 40, 60, 60, 20, 0}, {60, 20, 0, 60, 30, 120, 30},
                {120, 0, 30, 0, 90, 40, 20}, {160, 20, 0, 40, 0, 60, 30}, {60, 0, 80, 120, 40, 0, 50},
                {160, 0, 20, 30, 40, 60, 0}}, 0},
    {"PHRYGIA", {{0, 220, 0, 0, 0, 0, 60}, {200, 0, 0, 0, 0, 80, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
                 {0, 0, 0, 0, 0, 0, 0}, {160, 0, 0, 0, 0, 0, 90}, {255, 0, 0, 0, 0, 0, 0}}, 1},
};
static gen_prog_t gen_prog_user[2] __attribute__((section(".pool")));
static const gen_prog_t *gen_prog(uint32_t i)
{
    i %= GEN_NPROG;
    if (i < 6u) return &GEN_PROG_FACTORY[i];
    return gen_prog_user[i - 6u].name[0] ? &gen_prog_user[i - 6u] : &GEN_PROG_FACTORY[0];
}

/* ------------------------------------------------------------- harmony bus --- */
/* the scale's degrees as semitones from the root (7 entries, a scale with fewer repeats the last) */
static uint32_t gen_scale_notes(uint32_t scale, uint8_t *out)
{
    uint32_t m = SCALE_MASK[scale % (sizeof SCALE_MASK / sizeof SCALE_MASK[0])], i, n = 0;
    for (i = 0; i < 12u && n < 7u; i++)
        if ((m >> i) & 1u) out[n++] = (uint8_t)i;
    if (!n) { out[0] = 0; n = 1; }
    while (n < 7u) { out[n] = out[n - 1u]; n++; }
    return n;
}
/* the degree sounding on bar b of the phrase in section sec: the Markov walk from the start of the song, so bar
 * 1 of VERSE follows the chord INTRO ended on. seed: the project's */
static uint32_t gen_degree_at(uint32_t sec, uint32_t bar)
{
    const gen_prog_t *pg = gen_prog(gen.p.prog);
    uint32_t span = 1u << (gen.p.rate % 3u), chords = (sec * 4u + bar) / span, i, d = 0, c;
    for (c = 0; c < chords; c++) {
        uint32_t sum = 0, r, k, row = c == 0u ? 6u : d;
        for (k = 0; k < 7u; k++) sum += pg->w[row][k];
        if (!sum) { d = 0; continue; }                    /* a silent row: back to i */
        r = gen_rand(gen.p.master_seed, 7u, 0, c & 255u, 1u) * sum / 256u;
        for (k = 0, i = 0; k < 7u; k++) { i += pg->w[row][k]; if (r < i) break; }
        d = k < 7u ? k : 0u;
    }
    return d;
}
/* the chord of a degree: its 1 3 5 (and 7) as semitones from the root, in the scale */
static uint32_t gen_chord_of(uint32_t degree, uint32_t color, uint8_t *out)
{
    uint8_t s[7];
    uint32_t n = 0;
    gen_scale_notes(gen.p.scale, s);
    out[n++] = s[degree % 7u];
    if (color == GC_SUS) out[n++] = (uint8_t)(s[(degree + 3u) % 7u] + ((degree + 3u) >= 7u ? 12u : 0u));
    else out[n++] = (uint8_t)(s[(degree + 2u) % 7u] + ((degree + 2u) >= 7u ? 12u : 0u));
    out[n++] = (uint8_t)(s[(degree + 4u) % 7u] + ((degree + 4u) >= 7u ? 12u : 0u));
    if (color == GC_SEVENTH) out[n++] = (uint8_t)(s[(degree + 6u) % 7u] + ((degree + 6u) >= 7u ? 12u : 0u));
    else if (color == GC_ADD9) out[n++] = (uint8_t)(s[(degree + 1u) % 7u] + 12u + ((degree + 1u) >= 7u ? 12u : 0u));
    return n;
}
/* TENSION over the phrase: 0 at bar 1, climbing to 255 through bar 4, with a dip at each bar line */
static uint32_t gen_tension(uint32_t step)
{
    uint32_t bar = (step % GEN_PHRASE) / GEN_BAR, in = step % GEN_BAR;
    return (bar * 64u + in * 4u) > 255u ? 255u : bar * 64u + in * 4u;
}
static int gen_is_fill(uint32_t step, uint32_t fill)   /* the phrase's last beat(s), by FILL: 0 none .. 100 the last bar */
{
    uint32_t last = GEN_PHRASE - 1u - (fill * 15u) / 100u;
    return step % GEN_PHRASE >= last;
}

/* ------------------------------------------------------------------ helpers --- */
static void gen_step_clear(step_t *s) { memset(s, 0, sizeof *s); s->time = ST_REST; }
static void gen_step_note(step_t *s, uint32_t note, uint32_t vel)
{
    s->time = ST_NOTE;
    s->note[0] = (uint8_t)(note > 127u ? 127u : note);
    s->n = 1;
    s->vel = (uint8_t)(vel > 127u ? 127u : vel < 1u ? 1u : vel);
}
static void gen_step_add(step_t *s, uint32_t note)
{
    if (s->n >= 4u) return;
    s->note[s->n++] = (uint8_t)(note > 127u ? 127u : note);
}
/* how much of the phrase varies: bar 4 by VARIATION, and MUTATE's re-decided steps by seed_var */
static int gen_varied(const gen_track_t *g, uint32_t t, uint32_t step)
{
    uint32_t bar = (step % GEN_PHRASE) / GEN_BAR;
    if (bar == 3u && gen_rand(g->seed_var, t, 9u, step, 2u) < g->variation * 255u / 100u) return 1;
    return gen_rand(g->seed_var, t, 10u, step, 3u) < g->variation * 128u / 100u;
}
/* the seed for a decision at this step: the track's, or the variation's where the step is varied */
static uint32_t gen_seed(const gen_track_t *g, uint32_t t, uint32_t which, uint32_t step)
{
    uint32_t s = which ? g->seed_pitch : g->seed_rhythm;
    s = gen_derive(s, gen_section);
    return gen_varied(g, t, step) ? gen_derive(s, 0x5EC7u + step) : s;
}
static uint32_t gen_energy(void) { return gen.p.energy[gen_section % GEN_NSEC]; }

/* ------------------------------------------------------------ other generators --- */
/* Euclidean rhythm (Bjorklund): hit on step i of a cycle of n with k hits, turned by rot. The closed form
 * (j * k) mod n < k is Bjorklund's pattern for every k, n, with the first hit on step 0: E(3,8) = x..x..x. */
static int gen_euclid(uint32_t k, uint32_t n, uint32_t rot, uint32_t i)
{
    if (!n || !k) return 0;
    if (k >= n) return 1;
    return ((i + rot) % n) * k % n < k;
}
/* an elementary cellular automaton: row r of a 16-cell ring from a seeded row 0, under rule (Wolfram code) */
static uint32_t gen_ca_row(uint32_t seed, uint32_t lane, uint32_t rule, uint32_t r)
{
    uint32_t row = gen_mix(seed ^ (lane << 20) ^ 0xCA5Eu) & 0xFFFFu, g, i, nxt;
    if (!row) row = 1u << (lane & 15u);
    for (g = 0; g < r; g++) {
        nxt = 0;
        for (i = 0; i < 16u; i++) {
            uint32_t l = (row >> ((i + 15u) & 15u)) & 1u, c = (row >> i) & 1u, rr = (row >> ((i + 1u) & 15u)) & 1u;
            nxt |= ((rule >> (l << 2 | c << 1 | rr)) & 1u) << i;
        }
        row = nxt ? nxt : (gen_mix(seed + g) & 0xFFFFu);   /* a dead ring is reseeded, so it never falls silent */
    }
    return row;
}
static const uint8_t GEN_CA_RULES[8] = {30, 90, 110, 150, 73, 54, 126, 182};   /* GEN METHOD P1 picks one: chaotic, fractal, ... */
/* a Turing-machine shift register of len bits (8, 16 or 32), seeded, run `cycles` times round: each cycle the bit that
 * recirculates flips with chance flip (0..255). flip 0 is a locked loop, 128 is random, 255 flips every bit (a 2-cycle
 * loop). Deterministic in (seed, cycles), so a bar of LIVE is the register as it stands after that many cycles */
static uint32_t gen_turing(uint32_t seed, uint32_t lane, uint32_t len, uint32_t flip, uint32_t cycles)
{
    uint32_t reg = gen_mix(seed ^ (lane << 18) ^ 0x7124u), c, i, mask = len >= 32u ? 0xFFFFFFFFu : (1u << len) - 1u;
    reg &= mask;
    for (c = 0; c < cycles; c++)
        for (i = 0; i < len; i++) {
            uint32_t out = reg & 1u;
            if (gen_rand(seed, lane, c & 255u, i, 90u) < flip) out ^= 1u;
            reg = (reg >> 1) | (out << (len - 1u));
        }
    return reg;
}
/* MARKOV melodies: a transition table over the seven scale degrees and a rest (symbol 7), learned from a track's own
 * steps (gen_markov_learn: what you played in, as the layer opened) and blended with a seeded table that prefers small
 * intervals. One table per track, in RAM (the pool), not saved: learning again is one ROLL away */
typedef struct { uint8_t n[8][8]; } gen_markov_t;
static gen_markov_t gen_learned[NTRK] __attribute__((section(".pool")));
static uint32_t gen_degree_of(uint32_t note)               /* the scale degree nearest a note, 0..6 (the bus's key) */
{
    uint8_t sc[7]; uint32_t i, best = 0, bd = 99, pc = (note + 120u - gen.p.root) % 12u;
    gen_scale_notes(gen.p.scale, sc);
    for (i = 0; i < 7u; i++) { uint32_t d = pc > sc[i] ? pc - sc[i] : sc[i] - pc; if (d < bd) { bd = d; best = i; } }
    return best;
}
static void gen_markov_learn(uint32_t ti, const step_t *src)
{
    gen_markov_t *m = &gen_learned[ti % NTRK];
    uint32_t s, prev = 7;
    memset(m, 0, sizeof *m);
    for (s = 0; s < NSTEP; s++) {                     /* note to note; rests and ties are the rhythm's (REST, DENS), not learned */
        uint32_t sym;
        if (src[s].time != ST_NOTE || !src[s].n) continue;
        sym = gen_degree_of(src[s].note[0]);
        if (prev < 7u && m->n[prev][sym] < 255u) m->n[prev][sym]++;
        prev = sym;
    }
}
static uint32_t gen_markov_next(uint32_t ti, uint32_t prev, uint32_t learn, uint32_t rest_w, uint32_t seed, uint32_t step)
{
    const gen_markov_t *m = &gen_learned[ti % NTRK];
    uint32_t w[8], sum = 0, i, r;
    for (i = 0; i < 8u; i++) {                            /* the seeded table: small intervals likely, a rest by rest_w */
        uint32_t d = i == 7u ? 0u : (i > prev ? i - prev : prev - i), base = i == 7u ? rest_w : (d == 0u ? 60u : d == 1u ? 100u : d == 2u ? 70u : d == 3u ? 35u : 12u);
        base = base * (gen_rand(seed, ti, prev, i, 91u) + 128u) / 255u;   /* .. varied by the pitch seed */
        w[i] = base * (100u - learn) / 100u + (uint32_t)m->n[prev % 8u][i] * 40u * learn / 100u;
        sum += w[i];
    }
    if (!sum) return prev;
    r = gen_rand(seed, ti, 30u, step, 92u) * sum / 256u;
    for (i = 0; i < 8u; i++) { if (r < w[i]) return i; r -= w[i]; }
    return 7u;
}
/* CHAOS: the logistic map x' = r x (1 - x) in Q16, r = 3.5 .. 4.0 by P1: periodic at the bottom, chaotic at the top */
static uint32_t gen_logistic(uint32_t x16, uint32_t r100)    /* r100: 350..400 */
{
    uint64_t x = x16, one = 65536u;
    return (uint32_t)((x * (one - x) / one) * r100 / 100u);
}
/* PINK (1/f, Voss-McCartney): dice re-rolled when their bit of the step counter changes; the sum drifts like a melody */
static uint32_t gen_pink(uint32_t seed, uint32_t ti, uint32_t ndice, uint32_t step)
{
    uint32_t d, sum = 0;
    for (d = 0; d < ndice; d++) sum += gen_rand(seed, ti, d, step >> d, 93u) % 7u;   /* die d re-rolls every 2^d steps */
    return sum;
}
/* CALL: cross-track logic: did the reference track sound on this step? src 0 the DRUMS kick, 1 its snare, 2 any drum,
 * 3 the BASS, 4 the LEAD (a role's track, whichever it is; none: nothing sounded) */
static int gen_ref_sounded(uint32_t src, uint32_t step, uint32_t self)
{
    uint32_t i, role = src <= 2u ? GR_DRUMS : src == 3u ? GR_BASS : GR_LEAD;
    for (i = 0; i < NTRK; i++)
        if (i != self && gen.t[i].role == role) {
            const step_t *st = &trk[i].step[step];
            if (st->time != ST_NOTE) return 0;
            if (role == GR_DRUMS) return src == 0u ? (st->hit & 1u) != 0u : src == 1u ? (st->hit & 2u) != 0u : st->hit != 0u;
            return st->n > 0u;
        }
    return 0;
}
/* conditional trigs (Felucca 1.5.5 CONDITIONS, core.h): a varied step that plays on loop a of every b, so successive
 * passes of a committed pattern differ as Elektron-style trig conditions do; c = the condition index 1..NCOND */
static void gen_step_cond(step_t *s, uint32_t c) { if (c >= 1u && c <= NCOND) s->probability = (uint8_t)(COND_FIRST + c - 1u); }

/* ------------------------------------------------------------------- DRUMS --- */
/* macros: DENSITY SKIP GHOST ROLL. A hit lands when its weight (MORPHed, scaled by DENSITY and ENERGY, SKIP moving
 * weight off the beat onto the off-beats) beats the draw; at most 3 lanes a step (fix 4), KICK and SNARE kept first.
 * GHOST: soft hits (vel 40..70) on the style's ghost grid; ROLL: ratchets on the hats toward the fill; the nudge
 * biases: the lane's timing (QUANTIZE OFF plays it) */
/* the lanes' hits on a step by the other DRUMS methods (gen_render_drums fills in the rest). Each lane's base count of
 * hits per 16 (KICK 4 SNARE 2 CLAP 1 HATCL 8 HATOP 2 TOM 1 RIM 3 BELL 1), scaled by DENSITY and ENERGY:
 *   EUCLID  k hits in n: P1 scales k (50 = the base), P2 the cycle (16, 12, 8, 32 steps: cycles against the bar), P3 how
 *           far the lanes' rotations spread (0: all on the one; 100: each lane its own turn)
 *   CA      an automaton row per lane per bar: P1 the rule, P2 how many bars one row lasts, P3 the hit threshold
 *   POLY    each lane its own cycle length (KICK 16, SNARE 16, CLAP 12, HATCL 6, HATOP 10, TOM 7, RIM 5, BELL 9) with
 *           Euclidean hits in it: P1 scales k, P2 stretches the odd cycles (0: as listed, 100: all 16), P3 rotation spread */
static uint32_t gen_drum_hits_method(const gen_track_t *g, uint32_t ti, uint32_t step, uint32_t density, uint32_t e)
{
    static const uint8_t BASE_K[NLANE] = {4, 2, 1, 8, 2, 1, 3, 1}, POLY_N[NLANE] = {16, 16, 12, 6, 10, 7, 5, 9};
    uint32_t l, hits = 0, scale = (g->mp[0] + 50u) * density / 100u * (50u + e / 2u) / 100u;   /* 100 = the base counts */
    for (l = 0; l < NLANE; l++) {
        uint32_t k = (BASE_K[l] * scale + 50u) / 100u, n, rot, on;
        if (g->method == GMD_CA) {
            uint32_t rule = GEN_CA_RULES[g->mp[0] * 7u / 100u], span = 1u + g->mp[1] * 3u / 100u;
            uint32_t row = gen_ca_row(g->seed_rhythm, l, rule, step / (GEN_BAR * span) + ti), bit = (row >> (step % 16u)) & 1u;
            uint32_t w = gen_rand(g->seed_pitch, ti, l, step, 60u);
            on = bit && w < 64u + density * 191u / 100u * (100u - g->mp[2] * 60u / 100u) / 100u;   /* P3 thins */
            if (on) hits |= 1u << l;
            continue;
        }
        if (g->method == GMD_TURING) {                /* a register per lane: P1 FLIP, P2 LEN 8 / 16 / 32, P3 the fill threshold */
            uint32_t len = 8u << (g->mp[1] * 2u / 100u), reg = gen_turing(g->seed_rhythm + ti, l, len, g->mp[0] * 255u / 100u, step / len + gen_live_cycles);
            uint32_t bit = (reg >> (step % len)) & 1u, w = gen_rand(g->seed_pitch, ti, l, step % len, 64u);   /* (the gate loops with it) */
            on = bit && w < 40u + density * 2u * (BASE_K[l] * 16u) / 128u + g->mp[2];
            if (on) hits |= 1u << l;
            continue;
        }
        if (g->method == GMD_CALL) {                  /* P1 the reference (kick / snare / any drum / bass / lead), P2 AND or NOT, P3 density */
            uint32_t src = g->mp[0] * 4u / 100u, ref = (uint32_t)gen_ref_sounded(src, step, ti), want = g->mp[1] < 50u ? 1u : 0u;
            on = ref == want && gen_rand(g->seed_rhythm, ti, l, step, 65u) < (BASE_K[l] * 16u) * g->mp[2] / 100u;
            if (on) hits |= 1u << l;
            continue;
        }
        if (g->method == GMD_POLY) {
            n = POLY_N[l] + (16u - POLY_N[l]) * g->mp[1] / 100u;
            k = (BASE_K[l] * n / 16u * scale + 50u) / 100u;
        } else {
            static const uint8_t CYC[4] = {16, 12, 8, 32};
            n = CYC[g->mp[1] * 3u / 100u];
            k = (BASE_K[l] * n / 16u * scale + 50u) / 100u;
        }
        if (!k && BASE_K[l] * scale >= 25u) k = 1;
        rot = gen_rand(g->seed_rhythm, ti, l, 0, 61u) % n * g->mp[2] / 100u;
        if (l == 0u && g->mp[2] < 50u) rot = 0;               /* the kick on the one, the snare on the backbeat, unless the spread is wide */
        if (l == 1u && g->mp[2] < 50u) rot = n / 4u;
        if (gen_euclid(k, n, rot, step)) hits |= 1u << l;
    }
    return hits;
}
static void gen_render_drums(track_t *t, gen_track_t *g, uint32_t ti, uint32_t nokick)
{
    const gen_style_t *a = gen_style(g->style), *b = gen_style(g->morph_to);
    uint32_t density = g->macro[0], skip = g->macro[1], ghost = g->macro[2], roll = g->macro[3], e = gen_energy();
    uint32_t step, l, method = g->method % GMD_N;
    for (step = 0; step < GEN_PHRASE; step++) {
        step_t *s = &t->step[step];
        uint32_t grid = step % 32u, bar4 = step >= 48u, hits = 0, acc = 0, nlane = 0, keep = s->hit & g->lane_lock;
        uint32_t fill = gen_is_fill(step, g->fill), tens = gen_tension(step);
        int32_t nd = 0;
        gen_step_clear(s);
        if (method != GMD_STYLE) {                    /* EUCLID / CA / POLY: the hits, then the shared finishing below */
            uint32_t m = gen_drum_hits_method(g, ti, step, density, e);
            if (gen_varied(g, ti, step)) m ^= gen_rand(g->seed_var, ti, 14u, step, 62u) & 0x18u;   /* bar 4 / MUTATE: the hats differ */
            if (nokick) m &= ~1u;
            hits = (m & ~g->lane_lock) | keep;
            for (l = 0; l < NLANE; l++) if ((hits >> l) & 1u) { nlane++; if (l < 2u || (fill && l == 3u)) acc |= 1u << l; }
            if (ghost && gen_rand(g->seed_pitch, ti, 15u, step, 63u) < ghost * 255u / 100u / 3u && !(hits & 0x40u)) { hits |= 0x40u; nlane++; }
            goto finish;
        }
        for (l = 0; l < NLANE; l++) {
            uint32_t w = (a->w[l][grid] * (100u - g->morph) + b->w[l][grid] * g->morph) / 100u, r, thr, anchor = w == 255u;
            uint32_t off = (step & 3u) != 0u;         /* off the beat */
            if ((keep >> l) & 1u) { hits |= 1u << l; nlane++; continue; }
            if (nokick && l == 0u) continue;
            if (skip && l == 0u) w = off ? (w * (100u + skip) / 100u) : (w * (100u - skip / 2u) / 100u);
            if (!w) {                                 /* a ghost: the lane's grid, shifted one step, softly */
                uint32_t gw = (a->w[l][(grid + 31u) % 32u] * (100u - g->morph) + b->w[l][(grid + 31u) % 32u] * g->morph) / 100u;
                if (l != 0u && gw && gen_rand(gen_seed(g, ti, 0, step), ti, l, step, 4u) < gw * a->ghost * ghost / (255u * 100u)) {
                    hits |= 1u << l;                  /* (acc clear: soft) */
                    nlane++;
                }
                continue;
            }
            thr = w * density / 100u;
            thr = thr * (50u + e / 2u) / 100u;        /* ENERGY 0 halves, 100 keeps */
            if (anchor && density >= 30u && e >= 30u) thr = 256u;   /* a style's anchors (the one, the backbeat) always land */
            if (bar4 && fill) thr = thr + (255u - thr) * tens / 512u;   /* the fill leans in */
            r = gen_rand(gen_seed(g, ti, 0, step), ti, l, step, 5u);
            if (r < thr) {
                hits |= 1u << l;
                nlane++;
                if (w >= 200u || (fill && l < 2u)) acc |= 1u << l;
            }
        }
    finish:
        if (nlane > 3u) {                             /* at most 3 lanes a step (fix 4): KICK, SNARE kept, then the rest by lane */
            for (l = NLANE; l-- > 2u && nlane > 3u;)
                if ((hits >> l) & 1u && !((keep >> l) & 1u)) { hits &= ~(1u << l); acc &= ~(1u << l); nlane--; }
        }
        if (!hits) continue;
        s->time = ST_NOTE;
        s->hit = (uint8_t)hits;
        s->acc = (uint8_t)acc;
        s->vel = (uint8_t)(acc ? 112u : (hits & 3u) ? 96u : 48u + gen_rand(g->seed_pitch, ti, 11u, step, 6u) % 28u);
        /* timing: the loudest lane's bias, the hats' a touch of swing on the off-beats */
        for (l = 0; l < NLANE; l++)
            if ((hits >> l) & 1u) { nd = a->nudge[l]; break; }
        if ((hits & 8u) && (step & 1u)) nd += 1;
        step_set_nudge(s, nd);
        /* ratchets: hats toward the fill, a triplet burst on HATOP / RIM now and then */
        if ((hits & 8u) && roll && (fill || gen_rand(g->seed_rhythm, ti, 12u, step, 7u) < a->ratchet * roll / 400u))
            step_set_ratchet(s, fill && tens > 200u ? 4u : 2u);
        else if ((hits & 0x50u) && gen_rand(g->seed_rhythm, ti, 13u, step, 8u) < a->triplet * roll / 400u)
            step_set_ratchet(s, 3u);
        /* chance: ghosts and the quiet lanes play only sometimes; a varied quiet step instead takes a trig condition
         * (2:2, 1:3, 2:4 ..), so each pass of the committed pattern differs by design, not by chance */
        if (!(hits & 3u) && !acc) {
            if (gen_varied(g, ti, step) && gen_rand(g->seed_var, ti, 16u, step, 66u) < g->variation * 2u) gen_step_cond(s, 2u + gen_rand(g->seed_var, ti, 17u, step, 67u) % 7u);
            else step_set_chance(s, 55u + e / 3u);
        }
    }
}

/* -------------------------------------------------------------------- BASS --- */
/* macros: FOLLOW MOVE GLIDE LENGTH. The DRUMS track's kick steps are where bass most likely lands (FOLLOW), else a
 * sparse pulse; the note is the bus chord's root (MOVE adds fifths and octaves, and passing tones toward the next
 * chord as TENSION rises); GLIDE: slides (an 808's); LENGTH: ties after the note. One note a step (fix 4) */
static const track_t *gen_drums_track(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) if (gen.t[i].role == GR_DRUMS) return &trk[i];
    return 0;
}
/* BASS by the other methods. One note a step, every note in key (fix 3), MOVE and GLIDE as FOLLOW uses them:
 *   WALK   a random walk over the scale from the chord root: P1 the step size (degrees), P2 the Euclidean density of the
 *          rhythm, P3 the pull back toward the root
 *   ARP    the bus chord's tones in the bass octave, one a step: P1 the rate (every 1, 2 or 4 steps), P2 the shape (UP,
 *          DOWN, UP-DOWN, root-fifth), P3 the octave reach
 *   DRONE  the root held, retriggered at P1's rate, with an octave jump by P2 and a fifth by P3 at tension */
static void gen_render_bass_method(track_t *t, gen_track_t *g, uint32_t ti)
{
    uint32_t glide = g->macro[2], len = g->macro[3], e = gen_energy(), root = 36u + gen.p.root, step, last = 0, tie = 0;
    int32_t deg = 0;
    uint8_t sc[7], ch[4];
    gen_scale_notes(gen.p.scale, sc);
    for (step = 0; step < GEN_PHRASE; step++) {
        step_t *s = &t->step[step];
        uint32_t bar = step / GEN_BAR, cdeg = gen_degree_at(gen_section, bar), tens = gen_tension(step), note = 0, on = 0, vel = 90;
        gen_step_clear(s);
        if (tie) { s->time = ST_TIE; tie--; continue; }
        gen_chord_of(cdeg, GC_TRIAD, ch);
        if (g->method == GMB_WALK) {
            uint32_t k = 2u + (g->mp[1] * 10u / 100u) * (50u + e / 2u) / 100u;
            on = gen_euclid(k, 16u, gen_rand(g->seed_rhythm, ti, 0, 0, 70u) % 16u, step) || step % GEN_BAR == 0u;
            if (on) {
                int32_t d = (int32_t)(gen_rand(gen_seed(g, ti, 1, step), ti, 1, step, 71u) % (1u + g->mp[0] * 3u / 100u + 1u)) - (int32_t)(g->mp[0] * 3u / 200u + 1u) / 2;
                deg += d;
                if (gen_rand(g->seed_pitch, ti, 2u, step, 72u) < g->mp[2] * 255u / 100u) deg = deg > 0 ? deg - 1 : deg < 0 ? deg + 1 : 0;
                if (deg > 6) deg = 6; if (deg < -4) deg = -4;
                {   /* the scale walked from the chord's degree: in key by construction */
                    int32_t idx = (int32_t)cdeg + deg, oct = idx >= 0 ? idx / 7 : -1;
                    note = root + sc[((idx % 7) + 7) % 7] + (uint32_t)(oct * 12);
                }
                while (note < 29u) note += 12u;
            }
        } else if (g->method == GMB_ARP) {
            uint32_t rate = 1u << (g->mp[0] * 2u / 100u), cn = 3u, i = step / rate, shape = g->mp[1] * 3u / 100u, reach = 12u * (g->mp[2] * 2u / 100u);
            on = step % rate == 0u;
            if (on) {
                uint32_t idx = shape == 0u ? i % cn : shape == 1u ? (cn - 1u - i % cn) : shape == 2u ? (i % (2u * cn - 2u) < cn ? i % (2u * cn - 2u) : 2u * cn - 2u - i % (2u * cn - 2u)) : (i & 1u ? 2u : 0u);
                note = root + ch[idx] + (reach && ((i / cn) & 1u) ? 12u : 0u);
                vel = idx == 0u ? 100u : 80u;
            }
        } else if (g->method == GMB_MARKOV) {         /* P1 LEARN blend, P2 rhythm density, P3 rest weight */
            on = gen_euclid(2u + g->mp[1] * 10u / 100u, 16u, gen_rand(g->seed_rhythm, ti, 7u, 0, 94u) % 16u, step) || step % GEN_BAR == 0u;
            if (on) { uint32_t sym = gen_markov_next(ti, (uint32_t)(deg < 0 ? 0 : deg), g->mp[0], g->mp[2], g->seed_pitch, step);
                      if (sym == 7u) on = 0; else { deg = (int32_t)sym; note = root + sc[sym % 7u]; } }
        } else if (g->method == GMB_CHAOS) {          /* P1 R (3.50 .. 4.00), P2 rhythm density, P3 range */
            static uint32_t x; if (step == 0u) x = 16384u + (g->seed_pitch & 8191u);
            x = gen_logistic(x, 350u + g->mp[0] / 2u); if (x < 300u) x = 300u + (g->seed_pitch & 511u);
            on = gen_euclid(2u + g->mp[1] * 10u / 100u, 16u, 0, step);
            if (on) { uint32_t span = 3u + g->mp[2] * 9u / 100u, idx = (uint32_t)cdeg + x * span / 65536u; note = root + sc[idx % 7u] + 12u * (idx / 7u); vel = 70u + x * 50u / 65536u; }
        } else if (g->method == GMB_CALL) {           /* P1 reference, P2 AND / NOT, P3 density: the bass answers the drums or the lead */
            uint32_t src = g->mp[0] * 4u / 100u, want = g->mp[1] < 50u ? 1u : 0u;
            on = (uint32_t)gen_ref_sounded(src, step, ti) == want && gen_rand(g->seed_rhythm, ti, 8u, step, 95u) < 60u + g->mp[2] * 150u / 100u;
            if (on) note = root + ch[(step / 4u) % 3u == 0u ? 0u : (step & 4u) ? 2u : 0u];
        } else {                                      /* DRONE */
            uint32_t rate = 4u << (g->mp[0] * 2u / 100u);
            on = step % rate == 0u || (tens > 200u && step % 4u == 0u && gen_rand(g->seed_rhythm, ti, 3u, step, 73u) < g->mp[2] * 2u);
            if (on) { note = root + ch[0] + (step % GEN_BAR == 8u && gen_rand(g->seed_pitch, ti, 4u, step, 74u) < g->mp[1] * 2u ? 12u : 0u) + (tens > 200u && gen_rand(g->seed_pitch, ti, 5u, step, 75u) < g->mp[2] * 2u ? ch[2] : 0u); vel = 95; tie = rate > 2u ? rate - 2u : 0u; }
        }
        if (!on) continue;
        gen_step_note(s, note, vel);
        if (last && gen_rand(g->seed_rhythm, ti, 6u, step, 76u) < glide * 255u / 100u) s->flags |= SF_SLIDE;
        if (g->method != GMB_DRONE) { tie = (len * 3u) / 100u; if (tie > 3u) tie = 3u; }
        last = note;
    }
}
static void gen_render_bass(track_t *t, gen_track_t *g, uint32_t ti)
{
    const track_t *d = gen_drums_track();
    if (g->method % GMB_N != GMB_FOLLOW) { gen_render_bass_method(t, g, ti); return; }
    uint32_t follow = g->macro[0], move = g->macro[1], glide = g->macro[2], len = g->macro[3], e = gen_energy();
    uint32_t root = 36u + gen.p.root, step, last = 0, tie = 0;
    uint8_t ch[4];
    for (step = 0; step < GEN_PHRASE; step++) {
        step_t *s = &t->step[step];
        uint32_t bar = step / GEN_BAR, deg = gen_degree_at(gen_section, bar), ndeg = gen_degree_at(gen_section, bar + 1u);
        uint32_t kick = d && d->step[step].time == ST_NOTE && (d->step[step].hit & 1u), thr, note, tens = gen_tension(step);
        uint32_t r = gen_rand(gen_seed(g, ti, 0, step), ti, 0, step, 20u);
        gen_step_clear(s);
        if (tie) { s->time = ST_TIE; tie--; continue; }
        thr = kick ? 128u + follow * 127u / 100u : (step % 4u == 0u ? 60u : 25u) * (100u - follow) / 100u + 10u;
        thr = thr * (40u + e * 6u / 10u) / 100u;
        if (!kick && step == 0u) thr = 255u;                  /* always something on the one */
        if (r >= thr) continue;
        gen_chord_of(deg, GC_TRIAD, ch);
        note = root + ch[0];
        if (gen_rand(gen_seed(g, ti, 1, step), ti, 1, step, 21u) < move * 2u) {
            uint32_t pick = gen_rand(g->seed_pitch, ti, 2u, step, 22u) % 3u;
            note = pick == 0u ? root + ch[2] : pick == 1u ? root + 12u + ch[0] : root + ch[1];
        }
        if (tens > 190u && gen_rand(g->seed_pitch, ti, 3u, step, 23u) < move * 2u) {
            uint8_t nx[4]; gen_chord_of(ndeg, GC_TRIAD, nx);  /* a passing tone: the scale step under the next root */
            note = root + nx[0] + 12u - 1u;
            while (!((SCALE_MASK[gen.p.scale % 16u] >> ((note + 120u - gen.p.root) % 12u)) & 1u)) note--;
            if (note > root + 12u + 7u) note -= 12u;
        }
        gen_step_note(s, note, kick ? 110u : 85u);
        if (last && gen_rand(g->seed_rhythm, ti, 4u, step, 24u) < glide * 255u / 100u) s->flags |= SF_SLIDE;
        if (kick) s->flags |= SF_ACCENT;
        tie = (len * 3u) / 100u + (gen_rand(g->seed_rhythm, ti, 5u, step, 25u) < len * 2u ? 1u : 0u);
        if (tie > 3u) tie = 3u;
        last = note;
    }
}

/* ------------------------------------------------------------------ CHORDS --- */
/* macros: SPREAD COLOR RHYTHM STRUM. The bus chord, voiced from the last voicing with the least total movement
 * (each note goes to the nearest chord tone), at most 4 notes and never more than the role's voice share (fix 4:
 * budget, the freed voices of masked roles added); RHYTHM: PAD (one chord held per change), HALF (every half bar),
 * STAB (off-beat stabs), ARP (the notes one a step, up); STRUM: the notes spread over following steps as ties */
static uint32_t gen_voices_for(uint32_t role)
{
    uint32_t i, v = GEN_VOICES[role % GR_N], m = gen.p.mask[gen_section % GEN_NSEC];
    if (role != GR_CHORDS) return v;
    for (i = 0; i < NTRK; i++)                        /* a masked or OFF role hands its voices to CHORDS, up to 4 */
        if (gen.t[i].role != GR_CHORDS && (gen.t[i].role == GR_OFF || !((m >> i) & 1u)))
            v += GEN_VOICES[gen.t[i].role % GR_N];
    return v > 4u ? 4u : v;
}
static void gen_render_chords(track_t *t, gen_track_t *g, uint32_t ti)
{
    uint32_t spread = g->macro[0], color = g->macro[1] * (GC_N - 1u) / 100u, rhythm = g->macro[2] * (GRH_N - 1u) / 100u;
    uint32_t method = g->method % GMC_N;
    if (method == GMC_PULSE || method == GMC_TURING) rhythm = GRH_STAB;   /* PULSE: stabs on a Euclidean rhythm; TURING: on a register (mp[0..1]) */
    if (method == GMC_ARP) rhythm = GRH_ARP;          /* ARP: the voicing one note a step, mp[0] the rate, mp[1] the direction */
    uint32_t strum = g->macro[3], e = gen_energy(), nv = gen_voices_for(GR_CHORDS), base = 48u + gen.p.root;
    uint32_t step, lastdeg = 99u, n = 0, i, k, span = 1u << (gen.p.rate % 3u);
    uint8_t ch[4], voicing[4] = {0, 0, 0, 0};
    if (!nv) nv = 1u;
    for (step = 0; step < GEN_PHRASE; step++) {
        step_t *s = &t->step[step];
        uint32_t bar = step / GEN_BAR, deg = gen_degree_at(gen_section, bar), in = step % GEN_BAR;
        uint32_t change = deg != lastdeg, play = 0;
        gen_step_clear(s);
        if (change) {                                 /* re-voice: each old note to its nearest new chord tone */
            uint32_t cn = gen_chord_of(deg, color, ch);
            if (cn > nv) cn = nv;                     /* the cap: the doubled root and then the fifth go first */
            if (!n) {                                 /* the first voicing: close, from base, SPREAD opens it */
                for (i = 0; i < cn; i++) voicing[i] = (uint8_t)(base + ch[i] + (i == 2u && spread > 50u ? 12u : 0u) + (i == 3u && spread > 25u ? 12u : 0u));
                n = cn;
            } else {
                uint8_t used = 0;
                for (i = 0; i < n && i < cn; i++) {
                    int32_t best = 0, bd = 99;
                    for (k = 0; k < cn; k++) {
                        int32_t cand, d;
                        if ((used >> k) & 1u) continue;
                        cand = (int32_t)base + ch[k];
                        while (cand + 12 < (int32_t)voicing[i] + 7) cand += 12;
                        while (cand - 12 >= (int32_t)voicing[i] - 6) cand -= 12;
                        d = cand > (int32_t)voicing[i] ? cand - voicing[i] : voicing[i] - cand;
                        if (d < bd) { bd = d; best = cand; used |= 0; }
                    }
                    for (k = 0; k < cn; k++)          /* mark the tone taken */
                        if (!((used >> k) & 1u) && ((best - (int32_t)base - ch[k]) % 12 + 12) % 12 == 0) { used |= 1u << k; break; }
                    voicing[i] = (uint8_t)(best < 36 ? best + 12 : best > 84 ? best - 12 : best);
                }
                n = i;
                for (; i < cn; i++) { voicing[i] = (uint8_t)(base + ch[i] + 12u); n++; }   /* a tone with no voice yet */
            }
            lastdeg = deg;
        }
        switch (rhythm) {
        case GRH_PAD: play = (in == 0u && (bar % span) == 0u) || change; break;
        case GRH_HALF: play = in == 0u || in == 8u; break;
        case GRH_STAB:
            if (method == GMC_PULSE) play = gen_euclid(2u + g->mp[0] * 8u / 100u, 16u, g->mp[1] * 15u / 100u, step) && gen_rand(gen_seed(g, ti, 0, step), ti, 0, step, 30u) < 120u + e;
            else if (method == GMC_TURING) { uint32_t len = 8u << (g->mp[1] * 2u / 100u); play = (gen_turing(g->seed_rhythm + ti, 0, len, g->mp[0] * 255u / 100u, step / len + gen_live_cycles) >> (step % len)) & 1u && gen_rand(g->seed_rhythm, ti, 33u, step % len, 34u) < 100u + g->mp[2]; }
            else play = (in == 2u || in == 6u || in == 10u || in == 14u) && gen_rand(gen_seed(g, ti, 0, step), ti, 0, step, 30u) < 90u + e;
            break;
        default: play = method == GMC_ARP ? step % (1u << (g->mp[0] * 2u / 100u)) == 0u : (step & 1u) == 0u; break;
        }
        if (!play) { if (rhythm == GRH_PAD && step % GEN_BAR != 0u) s->time = ST_TIE; continue; }
        if (rhythm == GRH_ARP) {                      /* one note a step, up the voicing (ARP: by mp[1] up, down or up-down) */
            uint32_t i2 = method == GMC_ARP ? step / (1u << (g->mp[0] * 2u / 100u)) : step / 2u, dir = method == GMC_ARP ? g->mp[1] * 2u / 100u : 0u;
            uint32_t idx = dir == 0u ? i2 % n : dir == 1u ? n - 1u - i2 % n : (n > 1u ? (i2 % (2u * n - 2u) < n ? i2 % (2u * n - 2u) : 2u * n - 2u - i2 % (2u * n - 2u)) : 0u);
            gen_step_note(s, voicing[idx], 80u + e / 4u);
            if (method == GMC_ARP && g->mp[2] > 50u && gen_rand(g->seed_var, ti, 31u, step, 32u) < (g->mp[2] - 50u) * 5u) step_set_chance(s, 60u);   /* P3: gaps */
            continue;
        }
        gen_step_note(s, voicing[0], rhythm == GRH_STAB ? 100u : 72u + e / 5u);
        for (i = 1; i < n; i++) gen_step_add(s, voicing[i]);
        if (strum && n > 1u && rhythm != GRH_STAB) {  /* STRUM: the upper notes arrive on the next steps, tied in */
            uint32_t d = strum / 34u + 1u;
            for (i = 1; i < n && step + i * d < GEN_PHRASE; i++) {
                step_t *q = &t->step[step + i * d];
                if (q->time == ST_REST && (step + i * d) % GEN_BAR != 0u) { gen_step_clear(q); q->time = ST_TIE; }
            }
            s->n = 1;
            for (i = 1; i < n && step + i * d < GEN_PHRASE; i++) {
                step_t *q = &t->step[step + i * d];
                if (q->time == ST_TIE) { gen_step_note(q, voicing[i], 70u); for (k = 0; k < i; k++) gen_step_add(q, voicing[k]); }
            }
        }
        if (rhythm == GRH_PAD) {
            for (i = step + 1u; i < GEN_PHRASE && i % (GEN_BAR * span) != 0u; i++) { gen_step_clear(&t->step[i]); t->step[i].time = ST_TIE; }
            step = i - 1u;
        }
    }
}

/* -------------------------------------------------------------------- LEAD --- */
/* macros: CELL VARY REST RANGE. A cell of 2..4 notes (CELL: its length and shape) in the scale; each bar of the phrase
 * plays the cell through a transform picked by VARY (repeat, transpose to the bar's chord tone, invert, truncate,
 * reverse), on a rhythm from the cell's own durations; REST thins it (Burial's gaps); RANGE: the octaves it may climb
 * as TENSION rises (a leap into bar 4). On a CHOP track (fix 1): the note is a slice (one key per slice, as SLICE
 * maps them), the pitch is the track's, so no lock record is spent per step; the pitch, reverse and formant locks
 * (gen_locks) come only at phrase boundaries, within the budget. One note a step */
/* LEAD by the other methods. One note a step, in key (chop: slices), REST and RANGE as CELL uses them:
 *   WALK    a random walk: P1 the step size, P2 the rhythm's Euclidean density, P3 the pull toward chord tones
 *   EUCLID  a Euclidean rhythm of P1 hits in 16, turned by P2, with a melodic contour (rise / fall / arch by P3) over
 *           the chord's tones and their neighbours
 *   LSYS    an L-system: an axiom of two letters rewritten P1 times (A -> AB, B -> A: Fibonacci; P2 picks the rule set),
 *           each letter an interval step; P3 the note length */
static void gen_render_lead_method(track_t *t, gen_track_t *g, uint32_t ti)
{
    uint32_t rest = g->macro[2], range = g->macro[3], e = gen_energy(), chop = (g->flags & GF_CHOP) != 0u, base = chop ? 60u : 60u + gen.p.root;
    uint32_t step, m = g->method % GML_N;
    int32_t deg = 0;
    uint8_t sc[7], ch[4], ls[128], n = 0;
    gen_scale_notes(gen.p.scale, sc);
    for (step = 0; step < GEN_PHRASE; step++) gen_step_clear(&t->step[step]);
    if (m == GML_LSYS) {                              /* grow the string: A B / A AB / AB A / ABA AB ... up to 64 letters */
        static const char *const RULES[4][2] = {{"AB", "A"}, {"AB", "BA"}, {"ABB", "A"}, {"BA", "AAB"}};
        uint32_t gens = 2u + g->mp[0] * 5u / 100u, rs = g->mp[1] * 3u / 100u, gg, i, j;
        ls[0] = 'A'; n = 1;
        for (gg = 0; gg < gens && n < 64u; gg++) {
            uint8_t nx[128]; uint32_t nn = 0;
            for (i = 0; i < n && nn < 120u; i++) { const char *r = RULES[rs][ls[i] == 'B']; for (j = 0; r[j] && nn < 120u; j++) nx[nn++] = (uint8_t)r[j]; }
            memcpy(ls, nx, nn); n = (uint8_t)nn;
        }
    }
    for (step = 0; step < GEN_PHRASE; step++) {
        step_t *s = &t->step[step];
        uint32_t bar = step / GEN_BAR, cdeg = gen_degree_at(gen_section, bar), tens = gen_tension(step), on = 0, note, r;
        gen_chord_of(cdeg, GC_TRIAD, ch);
        r = gen_rand(gen_seed(g, ti, 0, step), ti, 22u, step, 44u);
        if (m == GML_WALK) {
            on = gen_euclid(3u + g->mp[1] * 10u / 100u, 16u, gen_rand(g->seed_rhythm, ti, 20u, 0, 80u) % 16u, step);
            if (on) {
                int32_t d = (int32_t)(gen_rand(gen_seed(g, ti, 1, step), ti, 21u, step, 81u) % (3u + g->mp[0] * 4u / 100u)) - (int32_t)(1u + g->mp[0] * 2u / 100u);
                deg += d; if (deg > 10) deg = 10; if (deg < -4) deg = -4;
                if (gen_rand(g->seed_pitch, ti, 23u, step, 82u) < g->mp[2] * 255u / 100u) deg = (int32_t)(cdeg + (gen_rand(g->seed_pitch, ti, 24u, step, 83u) % 3u) * 2u) % 7;   /* pulled to a chord tone */
            }
        } else if (m == GML_EUCLID) {
            uint32_t k = 2u + g->mp[0] * 10u / 100u, contour = g->mp[2] * 2u / 100u, pos = step % GEN_BAR;
            on = gen_euclid(k, 16u, g->mp[1] * 15u / 100u, step);
            deg = (int32_t)(contour == 0u ? pos * 7u / 16u : contour == 1u ? 6u - pos * 7u / 16u : (pos < 8u ? pos : 15u - pos) * 7u / 8u);
            deg = (int32_t)cdeg + deg;
        } else if (m == GML_TURING) {                 /* the classic: the top bits of the register are the pitch, a bit the gate */
            uint32_t len = 8u << (g->mp[1] * 2u / 100u), reg = gen_turing(g->seed_rhythm + ti, 3u, len, g->mp[0] * 255u / 100u, step / len + gen_live_cycles);
            uint32_t v = (reg >> (step % len)) & 0xFFu;
            on = (v & 1u) || (v & 2u);
            deg = (int32_t)(cdeg + (v >> 2) % (3u + g->mp[2] * 9u / 100u));
        } else if (m == GML_MARKOV) {                 /* P1 LEARN, P2 density, P3 rest weight */
            on = gen_euclid(3u + g->mp[1] * 10u / 100u, 16u, gen_rand(g->seed_rhythm, ti, 28u, 0, 96u) % 16u, step);
            if (on) { uint32_t sym = gen_markov_next(ti, (uint32_t)(deg < 0 ? 0 : deg % 7), g->mp[0], g->mp[2], g->seed_pitch, step); if (sym == 7u) on = 0; else deg = (int32_t)sym + (deg >= 7 ? 7 : 0); }
        } else if (m == GML_CHAOS) {                  /* P1 R, P2 density, P3 range */
            static uint32_t x; if (step == 0u) x = 16384u + (g->seed_pitch & 8191u);
            x = gen_logistic(x, 350u + g->mp[0] / 2u); if (x < 300u) x = 300u + (g->seed_pitch & 511u);
            on = gen_euclid(3u + g->mp[1] * 10u / 100u, 16u, 0, step);
            deg = (int32_t)(cdeg + x * (4u + g->mp[2] * 10u / 100u) / 65536u);
        } else if (m == GML_PINK) {                   /* P1 dice 2..5, P2 density, P3 range */
            on = gen_euclid(3u + g->mp[1] * 10u / 100u, 16u, 0, step) && gen_rand(g->seed_rhythm, ti, 29u, step, 97u) < 200u;
            deg = (int32_t)(cdeg + gen_pink(g->seed_pitch, ti, 2u + g->mp[0] * 3u / 100u, step) * (3u + g->mp[2] * 9u / 100u) / 20u);
        } else if (m == GML_CALL) {                   /* P1 reference, P2 AND / NOT, P3 density: fills the gaps the drums or bass leave */
            uint32_t src = g->mp[0] * 4u / 100u, want = g->mp[1] < 50u ? 1u : 0u;
            on = (uint32_t)gen_ref_sounded(src, step, ti) == want && gen_rand(g->seed_rhythm, ti, 32u, step, 98u) < 50u + g->mp[2] * 160u / 100u;
            if (on) deg = (int32_t)(cdeg + (gen_rand(g->seed_pitch, ti, 33u, step, 99u) % 3u) * 2u + ((step / GEN_BAR) & 1u ? 7u : 0u));
        } else {                                      /* LSYS: one letter a step; A steps up, B steps down (or rests at low energy) */
            uint32_t i = step % (n ? n : 1u), hold = 1u + g->mp[2] * 3u / 100u;
            on = step % hold == 0u;
            if (on) { if (ls[i] == 'A') deg += 1; else deg -= (e < 40u && gen_rand(g->seed_pitch, ti, 25u, step, 84u) < 100u) ? 0 : 1; if (deg > 9) deg = 2; if (deg < -3) deg = 4; }
        }
        if (!on || (r < rest * 255u / 100u * (120u - e) / 120u && step % GEN_BAR)) continue;
        if (chop) note = base + (uint32_t)((deg % 16 + 16) % 16);
        else {
            int32_t oct = deg >= 0 ? deg / 7 : -1, d7 = ((deg % 7) + 7) % 7;
            note = base + sc[d7] + (uint32_t)(oct * 12) + (range && tens > 160u && gen_rand(g->seed_pitch, ti, 26u, step, 85u) < range * tens / 120u ? 12u : 0u);
            if (note > 96u) note -= 12u; if (note < 48u) note += 12u;
        }
        gen_step_note(s, note, 72u + e / 4u + (step % GEN_BAR == 0u ? 18u : 0u));
        if (chop && gen_rand(g->seed_rhythm, ti, 27u, step, 86u) < 60u) step_set_ratchet(s, 2u);
    }
}
static void gen_render_lead(track_t *t, gen_track_t *g, uint32_t ti)
{
    uint32_t cellk = g->macro[0], vary = g->macro[1], rest = g->macro[2], range = g->macro[3], e = gen_energy();
    if (g->method % GML_N != GML_CELL) { gen_render_lead_method(t, g, ti); return; }
    uint32_t cn = 2u + cellk * 2u / 100u, chop = (g->flags & GF_CHOP) != 0u, base = chop ? 60u : 60u + gen.p.root;
    uint8_t sc[7], cell[GEN_NCELL], dur[GEN_NCELL];
    uint32_t i, step, bar, pos;
    gen_scale_notes(gen.p.scale, sc);
    for (i = 0; i < cn; i++) {                        /* the cell: degrees 0..6 (chop: slices 0..15), 1..4 steps each */
        uint32_t ps = gen_derive(g->seed_pitch, gen_section);
        cell[i] = (uint8_t)(chop ? gen_rand(ps, ti, i, 0, 40u) % 16u : gen_rand(ps, ti, i, 0, 40u) % 7u);
        dur[i] = (uint8_t)(1u + gen_rand(g->seed_rhythm, ti, i, 0, 41u) % (cellk > 60u ? 2u : 4u));
    }
    for (step = 0; step < GEN_PHRASE; step++) gen_step_clear(&t->step[step]);
    for (bar = 0; bar < 4u; bar++) {
        uint32_t tf = bar == 0u ? 0u : gen_rand(gen_seed(g, ti, 1, bar * GEN_BAR), ti, 20u, bar, 42u) * (1u + vary / 25u) / 256u;
        uint32_t deg = gen_degree_at(gen_section, bar), start = bar * GEN_BAR, n = cn, k;
        uint8_t c2[GEN_NCELL];
        uint8_t ch[4]; gen_chord_of(deg, GC_TRIAD, ch);
        for (i = 0; i < cn; i++) c2[i] = cell[i];
        switch (tf % 5u) {
        case 1: for (i = 0; i < cn; i++) c2[i] = (uint8_t)((c2[i] + deg) % (chop ? 16u : 7u)); break;   /* transpose to the chord */
        case 2: for (i = 0; i < cn; i++) c2[i] = (uint8_t)((chop ? 15u : 6u) - c2[i]); break;            /* invert */
        case 3: n = cn > 2u ? cn - 1u : cn; break;                                                         /* truncate */
        case 4: for (i = 0; i < cn / 2u; i++) { uint8_t x = c2[i]; c2[i] = c2[cn - 1u - i]; c2[cn - 1u - i] = x; } break;
        default: break;
        }
        pos = start + (bar > 0u && gen_rand(g->seed_rhythm, ti, 21u, bar, 43u) < 100u ? 2u : 0u);   /* a late entry */
        for (k = 0; k < 2u && pos < start + GEN_BAR; k++) {   /* the cell up to twice a bar */
            for (i = 0; i < n && pos < start + GEN_BAR; i++) {
                uint32_t tens = gen_tension(pos), oct = 0, note, r = gen_rand(gen_seed(g, ti, 0, pos), ti, 22u, pos, 44u);
                step_t *s = &t->step[pos];
                if (r < rest * 255u / 100u * (120u - e) / 120u) { pos += dur[i]; continue; }   /* REST: a gap */
                if (!chop) {
                    if (range && tens > 128u && gen_rand(g->seed_pitch, ti, 23u, pos, 45u) < range * tens / 100u) oct = 12u;
                    if (range > 60u && bar == 3u && i == 0u) oct = 12u;   /* the leap into the last bar */
                    note = base + sc[c2[i] % 7u] + oct;
                    if (tens > 220u && !((scale_mask(t) >> ((note - gen.p.root + 120u) % 12u)) & 1u)) note = base + ch[0] + oct;
                } else {
                    note = base + c2[i];              /* a slice: one key each, from middle C */
                }
                gen_step_note(s, note, 70u + e / 4u + (i == 0u ? 20u : 0u));
                if (i == 0u && k == 0u) s->flags |= SF_ACCENT;
                if (chop && gen_rand(g->seed_rhythm, ti, 24u, pos, 46u) < 40u + vary) step_set_ratchet(s, 2u);   /* a stutter */
                if (dur[i] > 1u && !chop) { uint32_t j; for (j = 1; j < dur[i] && pos + j < start + GEN_BAR; j++) t->step[pos + j].time = ST_TIE; }
                if (gen_rand(g->seed_var, ti, 25u, pos, 47u) < 30u) step_set_chance(s, 70u);
                pos += dur[i];
            }
            if (gen_rand(g->seed_rhythm, ti, 26u, bar, 48u) < rest * 2u) break;   /* once a bar when resting */
        }
    }
}

/* ------------------------------------------------------------ parameter locks --- */
/* GEN's locks on COMMIT (fix 1): within the track's budget, its own lowest-priority ones dropped first when the
 * pool is short. Priority high -> low: a slice change at a phrase boundary (CHOP: P_E0.. the engine's), reverse,
 * formant / brightness (P_ED_FLT), ratchet-adjacent tweaks. What exists on every engine is P_ED_FLT (a brightness
 * macro), so that is the lock every role can write: a dark bar 1, opening with TENSION. Role-specific ones are
 * added by gen_ui.c where it knows the engine's E ids */
static void gen_locks_clear(track_t *t, uint32_t ti)
{
    uint32_t s;
    for (s = 0; s < NSTEP; s++)
        if ((gen_wrote[ti] >> s) & 1u) motion_delete_event(t, s, P_ED_FLT);
    gen_wrote[ti] = 0;
    gen.t[ti].locks_used = 0;
}
static uint32_t gen_locks_write(track_t *t, gen_track_t *g, uint32_t ti)
{
    uint32_t budget = gen_lock_budget(g->role), used = 0, bar, dropped = 0;
    if (g->role == GR_DRUMS || g->role == GR_OFF) return 0;
    /* a brightness lock at each bar's first sounding step: TENSION opens the filter over the phrase */
    for (bar = 0; bar < 4u && used < budget; bar++) {
        uint32_t s;
        for (s = bar * GEN_BAR; s < (bar + 1u) * GEN_BAR; s++)
            if (t->step[s].time == ST_NOTE) {
                int16_t v = (int16_t)(40 + (int32_t)(gen_tension(s) * 80u / 255u) + (int32_t)gen_energy() / 10);
                int r = motion_set_lock(t, s, P_ED_FLT, v > 127 ? 127 : v);
                if (r == 2) dropped = 1;              /* the pool is full */
                else if (!r) { gen_wrote[ti] |= 1ull << s; used++; }
                break;
            }
    }
    g->locks_used = (uint8_t)used;
    return dropped ? 0x100u | used : used;
}

/* ------------------------------------------------------------------- render --- */
/* the track's engine offers which roles */
static int gen_role_ok(const track_t *t, uint32_t role)
{
    int dr = drum_track(t);
    if (role == GR_OFF) return 1;
    return dr ? role == GR_DRUMS : role != GR_DRUMS;
}
static int gen_chop_ok(const track_t *t)             /* an engine that maps slices to keys */
{
    uint32_t e = eng_idx(t->eng_req);
    return e == ENGI_SAMPLE || e == 8u || (FELUCCA_SLICE && e == 13u);   /* SAMPLE, GRAIN, SLICE (engines.c ENGINES[]) */
}
/* render one track's phrase into its steps (the harmony pushed first, fix 3). sec: the section */
static void gen_render_track(uint32_t ti, uint32_t sec)
{
    gen_render_count++;
    track_t *t = &trk[ti % NTRK];
    gen_track_t *g = &gen.t[ti % NTRK];
    uint32_t m = gen.p.mask[sec % GEN_NSEC];
    gen_section = (uint8_t)(sec % GEN_NSEC);
    if (g->role == GR_OFF || (g->flags & GF_LOCK) || !gen_role_ok(t, g->role)) return;
    if (!((m >> ti) & 1u)) {                          /* masked out of this section: rests */
        uint32_t s; for (s = 0; s < NSTEP; s++) gen_step_clear(&t->step[s]);
        return;
    }
    if (g->role != GR_DRUMS && !(g->role == GR_LEAD && (g->flags & GF_CHOP))) {
        t->p[P_ROOT] = (int16_t)(gen.p.root % 12u);  /* the harmony bus's key, pushed (fix 3) */
        t->p[P_SCALE] = (int16_t)(gen.p.scale % 16u);
        g->flags &= (uint8_t)~GF_DIRTY;
    } else if (g->role == GR_LEAD) {
        t->p[P_QUANT] = QN_OFF;                       /* CHOP: the notes are slices, never snapped */
    }
    t->p[P_SLEN] = (int16_t)GEN_PHRASE;
    switch (g->role) {
    case GR_DRUMS: gen_render_drums(t, g, ti, (m & GM_NOKICK) != 0u); break;
    case GR_BASS: gen_render_bass(t, g, ti); break;
    case GR_CHORDS: gen_render_chords(t, g, ti); break;
    default: gen_render_lead(t, g, ti); break;
    }
    t->seq_active = 1;
}
/* the whole section, DRUMS first (BASS follows its kicks) */
static void gen_render_section(uint32_t sec)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) if (gen.t[i].role == GR_DRUMS) gen_render_track(i, sec);
    for (i = 0; i < NTRK; i++) if (gen.t[i].role == GR_BASS) gen_render_track(i, sec);     /* (CALL methods read the tracks before them) */
    for (i = 0; i < NTRK; i++) if (gen.t[i].role == GR_CHORDS) gen_render_track(i, sec);
    for (i = 0; i < NTRK; i++) if (gen.t[i].role == GR_LEAD) gen_render_track(i, sec);
}

/* ------------------------------------------------------------------- seeds --- */
static void gen_hist_push(gen_track_t *g)
{
    g->history[g->hist_pos % GEN_HIST].rhythm = g->seed_rhythm;
    g->history[g->hist_pos % GEN_HIST].pitch = g->seed_pitch;
    g->hist_pos = (uint8_t)((g->hist_pos + 1u) % GEN_HIST);
    if (g->hist_len < GEN_HIST) g->hist_len++;
}
/* BREED (black key 9): a child of this roll and the last one: each bit of the seeds from one parent or the other, by a
 * seeded mask, so a part keeps traits of both. The parents stay in the history */
static void gen_breed(uint32_t ti)
{
    gen_track_t *g = &gen.t[ti % NTRK];
    uint32_t i, mask;
    if ((g->flags & GF_LOCK) || !g->hist_len) return;
    i = (g->hist_pos + GEN_HIST - 1u) % GEN_HIST;
    mask = gen_mix(g->seed_rhythm ^ g->history[i].rhythm ^ 0xB4EEDu);
    gen_hist_push(g);
    g->seed_rhythm = (g->seed_rhythm & mask) | (g->history[i].rhythm & ~mask);
    g->seed_pitch = (g->seed_pitch & ~mask) | (g->history[i].pitch & mask);
}
static void gen_roll(uint32_t ti, uint32_t rhythm, uint32_t pitch)
{
    gen_track_t *g = &gen.t[ti % NTRK];
    if (g->flags & GF_LOCK) return;
    gen_hist_push(g);
    if (rhythm) g->seed_rhythm = gen_fresh();
    if (pitch) g->seed_pitch = gen_fresh();
}
static void gen_mutate(uint32_t ti)
{
    gen_track_t *g = &gen.t[ti % NTRK];
    if (g->flags & GF_LOCK) return;
    g->seed_var = gen_derive(g->seed_var, 0x3u);
}
/* PREV / NEXT: through the history (the current seeds are pushed on the first PREV) */
static void gen_hist_step(uint32_t ti, int dir)
{
    gen_track_t *g = &gen.t[ti % NTRK];
    uint32_t i;
    if (!g->hist_len) return;
    i = (g->hist_pos + GEN_HIST + (dir < 0 ? GEN_HIST - 1u : 1u)) % GEN_HIST;
    if (dir < 0) {
        uint32_t r = g->seed_rhythm, p = g->seed_pitch;
        g->seed_rhythm = g->history[i].rhythm; g->seed_pitch = g->history[i].pitch;
        g->history[i].rhythm = r; g->history[i].pitch = p;
        g->hist_pos = (uint8_t)i;
    } else {
        uint32_t r = g->seed_rhythm, p = g->seed_pitch;
        i = g->hist_pos % GEN_HIST;
        g->seed_rhythm = g->history[i].rhythm; g->seed_pitch = g->history[i].pitch;
        g->history[i].rhythm = r; g->history[i].pitch = p;
        g->hist_pos = (uint8_t)((i + 1u) % GEN_HIST);
    }
}
/* LIVE: at a bar line, DRIFT advances one seed sometimes; the track is re-rendered */
static void gen_live_bar(uint32_t ti, uint32_t bar)
{
    gen_track_t *g = &gen.t[ti % NTRK];
    if (!(g->flags & GF_LIVE) || (g->flags & GF_LOCK)) return;
    gen_live_cycles = bar;                            /* the Turing registers recirculate once a bar */
    if (g->drift && gen_rand(g->seed_var, ti, 30u, bar & 255u, 50u) < g->drift * 255u / 100u) {
        if (bar & 1u) g->seed_pitch = gen_derive(g->seed_pitch, bar); else g->seed_rhythm = gen_derive(g->seed_rhythm, bar);
    }
    gen_render_track(ti, gen_section);
}

/* -------------------------------------------------------------------- song --- */
/* SONG: the four sections from the master seed: each track's seeds derived from it, the sections' masks and ENERGY
 * as set (the defaults below), rendered one by one into the live tracks for gen_ui.c to save into slots A..D */
static const uint8_t GEN_MASK_DEFAULT[GEN_NSEC] = {GM_TEXTURE | 0 /* set by role below */, 15u | GM_TEXTURE, 15u | GM_TEXTURE | GM_NOKICK, 15u};
static const uint8_t GEN_ENERGY_DEFAULT[GEN_NSEC] = {25, 55, 75, 100};
static void gen_song_defaults(void)
{
    uint32_t i, s;
    for (s = 0; s < GEN_NSEC; s++) { gen.p.energy[s] = GEN_ENERGY_DEFAULT[s]; gen.p.mask[s] = GEN_MASK_DEFAULT[s]; }
    for (i = 0; i < NTRK; i++)                        /* INTRO: CHORDS only (and whatever is OFF anyway) */
        if (gen.t[i].role == GR_CHORDS) gen.p.mask[GS_INTRO] |= GM_TRK(i);
    for (i = 0; i < NTRK; i++)                        /* VERSE: LEAD sparse (its REST up, done by ENERGY); BUILD: no BASS */
        if (gen.t[i].role == GR_BASS) gen.p.mask[GS_BUILD] &= (uint8_t)~GM_TRK(i);
}
static void gen_song_seed(uint32_t master)
{
    uint32_t i;
    gen.p.master_seed = master ? master : gen_fresh();
    for (i = 0; i < NTRK; i++) {
        gen_track_t *g = &gen.t[i];
        if (g->flags & GF_LOCK) continue;
        gen_hist_push(g);
        g->seed_rhythm = gen_derive(gen.p.master_seed, 0x100u + i);
        g->seed_pitch = gen_derive(gen.p.master_seed, 0x200u + i);
        g->seed_var = gen_derive(gen.p.master_seed, 0x300u + i);
    }
}
/* ---------------------------------------------------------------------- FORM --- */
/* The song's form: which sections (A INTRO, B VERSE, C BUILD, D DROP) in what order, how many times each (song_chain.c
 * chain_config_t: a row is a section and its repeats, every track on that slot). Named forms, GRAMMAR (a form grown
 * by rewriting rules from the master seed, so SONG with a new seed gives a new shape too) and CUSTOM (the chain as
 * you built it in SONG mode stays; SONG re-renders only the four slots). LEN halves or doubles the repeats; TAIL
 * ends the song with a reprise of the INTRO, or peels the tracks off one by one (CHAIN_SILENT) so it falls apart */
typedef struct { uint8_t sec, rep; } gen_form_row_t;
typedef struct { const char *name; uint8_t n; gen_form_row_t row[12]; } gen_form_t;
enum { GFM_CLASSIC, GFM_POP, GFM_AABA, GFM_RISE, GFM_ARCH, GFM_THROUGH, GFM_LOOP, GFM_LONG, GFM_GRAMMAR, GFM_CUSTOM, GFM_N };
#define A GS_INTRO
#define B GS_VERSE
#define C GS_BUILD
#define D GS_DROP
static const gen_form_t GEN_FORM[GFM_N] = {
    {"CLASSIC", 7, {{A, 2}, {B, 4}, {C, 2}, {D, 4}, {B, 2}, {D, 4}, {A, 2}}},          /* the starter: in, verse, build, drop, again, out */
    {"POP", 9, {{A, 2}, {B, 4}, {D, 4}, {B, 4}, {D, 4}, {C, 2}, {D, 4}, {D, 4}, {A, 2}}}, /* verse chorus verse chorus bridge chorus chorus */
    {"AABA", 5, {{A, 1}, {B, 4}, {B, 4}, {D, 4}, {B, 4}}},                               /* the 32-bar song */
    {"RISE", 6, {{A, 4}, {C, 4}, {D, 8}, {B, 4}, {C, 4}, {D, 8}}},                       /* two builds, two drops, the second longer */
    {"ARCH", 7, {{A, 2}, {B, 2}, {C, 2}, {D, 4}, {C, 2}, {B, 2}, {A, 2}}},                /* up and back down */
    {"THROUGH", 4, {{A, 2}, {B, 2}, {C, 2}, {D, 2}}},                                     /* each once: through-composed */
    {"LOOP", 1, {{D, 16}}},                                                               /* the drop only, for jamming */
    {"LONG", 8, {{A, 4}, {B, 4}, {A, 2}, {B, 4}, {C, 4}, {D, 4}, {B, 4}, {A, 4}}},         /* slow, a Burial side: verses, one drop */
    {"GROWN", 0, {{0, 0}}},
    {"CUSTOM", 0, {{0, 0}}}};
#undef A
#undef B
#undef C
#undef D
static const char *gen_form_name(uint32_t f) { return GEN_FORM[f % GFM_N].name; }
/* GRAMMAR: S -> I M O; M -> V C D | V D V C D | M M (by GROW); V -> V V | V (at depth); O -> I | V I | (nothing).
 * Rewritten GROW times from the master seed, then the letters become rows (a run of one letter: its repeats) */
static uint32_t gen_form_grammar(gen_form_row_t *out, uint32_t seed, uint32_t grow)
{
    uint8_t str[48] = "IMO", nx[48];
    uint32_t g, i, j, n = 3, nn, cnt = 0, rows = 0, prev = 255;
    static const uint8_t SEC_OF[256] = {['I'] = GS_INTRO, ['V'] = GS_VERSE, ['C'] = GS_BUILD, ['D'] = GS_DROP};
    for (g = 0; g < grow; g++) {
        for (i = 0, nn = 0; i < n && nn < 40u; i++) {
            const char *r; uint32_t pick = gen_rand(seed, g, i, str[i], 120u) % 3u;
            switch (str[i]) {
            case 'M': r = pick == 0u ? "VCD" : pick == 1u ? "VDVCD" : g + 1u < grow ? "MM" : "VCD"; break;
            case 'O': r = pick == 0u ? "I" : pick == 1u ? "VI" : "D"; break;
            case 'V': r = pick == 0u && g + 1u < grow ? "VV" : "V"; break;
            case 'D': r = pick == 2u && g + 1u == grow ? "DD" : "D"; break;
            default: r = 0;
            }
            if (!r) { nx[nn++] = str[i]; continue; }
            for (j = 0; r[j] && nn < 40u; j++) nx[nn++] = (uint8_t)r[j];
        }
        memcpy(str, nx, nn); n = nn;
    }
    for (i = 0; i < n; i++) {                         /* letters -> rows; M or O left over (GROW 0) count as a verse / the intro */
        uint32_t sec = str[i] == 'M' ? GS_VERSE : str[i] == 'O' ? GS_INTRO : SEC_OF[str[i]];
        uint32_t rep = sec == GS_DROP ? 4u : sec == GS_INTRO ? 2u : sec == GS_BUILD ? 2u : 4u;
        if (sec == prev && rows) { if (out[rows - 1u].rep + rep <= 16u) out[rows - 1u].rep = (uint8_t)(out[rows - 1u].rep + rep); continue; }
        if (rows >= CHAIN_ROWS) break;
        out[rows].sec = (uint8_t)sec; out[rows].rep = (uint8_t)rep; rows++; prev = sec; cnt++;
    }
    return rows;
}
/* the chain for the form as set (0 rows written: CUSTOM, the chain stays). Rows past CHAIN_ROWS are dropped from
 * the middle, never the ends, so the song still opens and closes as the form does */
static uint32_t gen_chain_write(chain_config_t *c)
{
    gen_form_row_t rows[CHAIN_ROWS + 4];
    uint32_t n = 0, r, i, f = gen.p.form % GFM_N, lead = 255, chords = 255;
    if (f == GFM_CUSTOM) return 0;
    if (f == GFM_GRAMMAR) n = gen_form_grammar(rows, gen.p.master_seed, 1u + gen.p.grow % 4u);
    else { n = GEN_FORM[f].n; memcpy(rows, GEN_FORM[f].row, n * sizeof rows[0]); }
    if (!n) { rows[0].sec = GS_DROP; rows[0].rep = 4; n = 1; }
    for (r = 0; r < n; r++) {                         /* LEN: the repeats halved or doubled, 1..16 */
        uint32_t rep = rows[r].rep;
        rep = gen.p.song_len == 0u ? (rep + 1u) / 2u : gen.p.song_len == 2u ? rep * 2u : rep;
        rows[r].rep = (uint8_t)(rep < 1u ? 1u : rep > 16u ? 16u : rep);
    }
    if (gen.p.tail == 1u && n < CHAIN_ROWS + 4u) { rows[n].sec = GS_INTRO; rows[n].rep = 2; n++; }   /* REPRISE */
    while (n > CHAIN_ROWS) { memmove(&rows[n / 2u], &rows[n / 2u + 1u], (n - n / 2u - 1u) * sizeof rows[0]); n--; }
    memset(c, 0, sizeof *c);
    for (i = 0; i < NTRK; i++) { if (gen.t[i].role == GR_LEAD) lead = i; if (gen.t[i].role == GR_CHORDS) chords = i; }
    for (r = 0; r < n; r++) {
        for (i = 0; i < NTRK; i++) c->row[r].slot[i] = rows[r].sec;
        c->row[r].repeat = rows[r].rep;
    }
    if (gen.p.tail == 2u) {                           /* PEEL: the last section again without the LEAD, then without CHORDS */
        uint32_t last = rows[n - 1u].sec, k;
        for (k = 0; k < 2u && n < CHAIN_ROWS; k++, n++) {
            for (i = 0; i < NTRK; i++) c->row[n].slot[i] = (uint8_t)((i == lead || (k == 1u && i == chords)) ? CHAIN_SILENT : last);
            c->row[n].repeat = 2;
        }
    }
    c->count = (uint8_t)n;
    return n;
}

/* ------------------------------------------------------------------ defaults --- */
static void gen_init_track(gen_track_t *g, uint32_t role)
{
    static const uint8_t M[GR_N][4] = {{0, 0, 0, 0}, {70, 30, 40, 40}, {80, 30, 60, 50}, {50, 40, 0, 30}, {50, 50, 50, 40}};
    memset(g, 0, sizeof *g);
    g->role = (uint8_t)role;
    g->style = role == GR_DRUMS ? 0u : 0u;
    g->morph_to = 1;
    memcpy(g->macro, M[role % GR_N], 4);
    g->variation = 35; g->drift = 10; g->fill = 60;
    g->method = 0; g->mp[0] = 50; g->mp[1] = 25; g->mp[2] = 50;
    g->seed_rhythm = gen_fresh(); g->seed_pitch = gen_fresh(); g->seed_var = gen_fresh();
}
static void gen_init(void)
{
    static const uint8_t ROLE[NTRK] = {GR_DRUMS, GR_BASS, GR_CHORDS, GR_LEAD};
    uint32_t i;
    memset(&gen, 0, sizeof gen);
    gen.p.version = GEN_VERSION;
    gen.p.form = GFM_CLASSIC; gen.p.song_len = 1; gen.p.grow = 1; gen.p.tail = 0;                                /* 2: methods (GEN METHOD); a version-1 store starts afresh */
    gen.p.root = 9;                                   /* A minor: the default of the references */
    gen.p.scale = 2;                                  /* MIN (seq.c SCALE_MASK order: CHR MAJ MIN DOR ..) */
    gen.p.prog = 0; gen.p.rate = 0; gen.p.lock_scale = 1;
    gen.p.tex_level = 40; gen.p.tex_rate = 30;
    for (i = 0; i < NTRK; i++) gen_init_track(&gen.t[i], ROLE[i]);
    gen_song_defaults();
    gen.p.master_seed = gen_fresh();
    memset(gen_wrote, 0, sizeof gen_wrote);
}
