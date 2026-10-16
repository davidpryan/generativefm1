/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 GEN contributors. Built on Felucca, Copyright (C) 2026 Leo Kuroshita (@kurogedelic). */
/* GEN's panel: the ARP-held layer, its three pages, LIVE at the bar line, COMMIT, SONG and the flash copy.
 *
 * The layer (LAYER_GEN, ARP held or double-tapped to lock, as the other layers: ui_layer.c):
 *   white keys  1 ROLL  2 ROLL RHYTHM  3 ROLL PITCH  4 MUTATE | 5 PREV  6 NEXT  7 LOCK  8 LIVE |
 *               9..12 section A B C D (the target) | 13 SONG (OCT+ confirms)  14 FILL (held)  15 ALL (held: the
 *               next action on every unlocked track)  16 TEXTURE on / off for the section
 *   black keys  1..8 on a DRUMS track: lock lane KICK SNARE CLAP HATCL HATOP TOM RIM BELL (the EDIT layer's order)
 *               9 BREED: crossover of this roll's seeds and the previous one's; 10 LEARN: the MARKOV methods learn
 *               the transitions of the track as the layer opened (what you played in)
 *   KNOB 1..4   the selected track's role macros; PRESETS: the STYLE; ALGORITHM: the track
 *   OCT+        COMMIT the selected track's render into its steps (and SONG's confirm); OCT-: the track as the
 *               layer found it (tl_open, as SEQ TOOLS); SAVE held: undo (one per action)
 * Every ROLL / MUTATE / PREV / NEXT renders the track at once so you hear it; what you hear is what COMMIT keeps.
 * The pages (tap ARP past ARP 2): GEN ROLE: ROLE STYLE TO MORPH; GEN SHAPE: VARY DRIFT ENERGY FILL;
 * HARMONY ROOT SCALE PROG RATE (the project's; the pitched tracks' P_ROOT / P_SCALE follow on the next render).
 * Storage: gen_state_t per project slot in its own flash record (STORE_GEN), beside the project, so a project saved
 * here still opens on stock Felucca; the slot's GEN state loads with it and is saved with it. */

/* ------------------------------------------------------------- the pages --- */
/* SC_GEN pages: the knob ids index GEN_DESC, read and written by gen_val / gen_set */
enum { GD_ROLE, GD_STYLE, GD_MORPHTO, GD_MORPH, GD_VAR, GD_DRIFT, GD_ENERGY, GD_FILL, GD_ROOT, GD_SCALE, GD_PROG, GD_RATE,
       GD_M1, GD_M2, GD_M3, GD_M4, GD_METHOD, GD_P1, GD_P2, GD_P3, GD_FORM, GD_SLEN, GD_GROW, GD_TAIL, GD_N };
typedef struct { const char *label; int16_t min, max; uint8_t pct; } gen_desc_t;
static const gen_desc_t GEN_DESC[GD_N] = {
    {"ROLE", 0, GR_N - 1, 0}, {"STYLE", 0, GEN_NSTYLE - 1, 0}, {"TO", 0, GEN_NSTYLE - 1, 0}, {"MORPH", 0, 100, 1},
    {"VARY", 0, 100, 1}, {"DRIFT", 0, 100, 1}, {"ENERGY", 0, 100, 1}, {"FILL", 0, 100, 1},
    {"ROOT", 0, 11, 0}, {"SCL", 0, 15, 0}, {"PROG", 0, GEN_NPROG - 1, 0}, {"RATE", 0, 2, 0},
    {"M1", 0, 100, 1}, {"M2", 0, 100, 1}, {"M3", 0, 100, 1}, {"M4", 0, 100, 1},
    {"METHOD", 0, GEN_METHODS_MAX - 1, 0}, {"P1", 0, 100, 1}, {"P2", 0, 100, 1}, {"P3", 0, 100, 1},
    {"FORM", 0, GFM_N - 1, 0}, {"LEN", 0, 2, 0}, {"GROW", 0, 3, 0}, {"TAIL", 0, 2, 0},
};
/* the methods a role offers and their parameters' names (gen.c: the GM* enums; "" = a method the role lacks) */
static const char *const GEN_METHOD_NAME[GR_N][GEN_METHODS_MAX] = {
    {"-", "", "", "", "", "", "", "", ""},
    {"STYLE", "EUCLID", "CA", "POLY", "TURING", "CALL", "", "", ""},
    {"FOLLOW", "WALK", "ARP", "DRONE", "MARKOV", "CHAOS", "CALL", "", ""},
    {"VOICE", "PULSE", "ARP", "TURING", "", "", "", "", ""},
    {"CELL", "WALK", "EUCLID", "LSYS", "TURING", "MARKOV", "CHAOS", "PINK", "CALL"}};
#define GMP_NONE {"-", "-", "-"}
#define GMP_TURING {"FLIP", "LEN", "FILL"}
#define GMP_CALL {"REF", "NOT", "DENS"}
#define GMP_MARKOV {"LEARN", "DENS", "REST"}
#define GMP_CHAOS {"R", "DENS", "RANGE"}
static const char *const GEN_MP_NAME[GR_N][GEN_METHODS_MAX][3] = {
    {GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE},
    {GMP_NONE, {"HITS", "CYCLE", "SPREAD"}, {"RULE", "SPAN", "THIN"}, {"HITS", "EVEN", "SPREAD"}, GMP_TURING, GMP_CALL, GMP_NONE, GMP_NONE, GMP_NONE},
    {GMP_NONE, {"STEP", "DENS", "HOME"}, {"RATE", "SHAPE", "REACH"}, {"RATE", "OCT", "FIFTH"}, GMP_MARKOV, GMP_CHAOS, GMP_CALL, GMP_NONE, GMP_NONE},
    {GMP_NONE, {"HITS", "TURN", "-"}, {"RATE", "DIR", "GAPS"}, GMP_TURING, GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE, GMP_NONE},
    {GMP_NONE, {"STEP", "DENS", "PULL"}, {"HITS", "TURN", "SHAPE"}, {"GENS", "RULES", "HOLD"}, GMP_TURING, GMP_MARKOV, GMP_CHAOS, {"DICE", "DENS", "RANGE"}, GMP_CALL}};
/* ---------------------------------------------------------------- icons --- */
/* the roles, the methods and the cards as pictures (Fukiai glyphs, tools/gen_aa_icons.py EXTRA): a drum, a bass
 * clef, a pad, a lead; a grid for a style, a circle for EUCLID, cells for CA, a loop for TURING, nodes for MARKOV.. */
static uint32_t gen_role_icon(uint32_t role)
{
    static const uint8_t R[GR_N] = {ICON_MUTE, ICON_DRUM, ICON_SUB, ICON_X_PAD, ICON_X_LEAD};
    return R[role % GR_N];
}
static uint32_t gen_method_icon(uint32_t role, uint32_t m)
{
    static const uint8_t D[GMD_N] = {ICON_X_GRID, ICON_X_CIRCLE, ICON_X_CELLS, ICON_X_GRID9, ICON_X_LOOP, ICON_X_LINK};
    static const uint8_t B[GMB_N] = {ICON_X_KICK, ICON_X_RANDOM, ICON_X_LOOP, ICON_X_WEIGHT, ICON_X_NODE, ICON_X_WARP, ICON_X_LINK};
    static const uint8_t C[GMC_N] = {ICON_X_PIANO, ICON_X_IMPULSE, ICON_X_LOOP, ICON_X_LOOP};
    static const uint8_t L[GML_N] = {ICON_VOICE, ICON_X_RANDOM, ICON_X_CIRCLE, ICON_X_LADDER, ICON_X_LOOP, ICON_X_NODE, ICON_X_WARP, ICON_X_PINK, ICON_X_LINK};
    switch (role) {
    case GR_DRUMS: return D[m % GMD_N]; case GR_BASS: return B[m % GMB_N]; case GR_CHORDS: return C[m % GMC_N]; case GR_LEAD: return L[m % GML_N];
    default: return ICON_NONE;
    }
}
static uint32_t gen_card_icon(uint32_t id)
{
    static const uint8_t MACRO[GR_N][4] = {
        {ICON_NONE, ICON_NONE, ICON_NONE, ICON_NONE}, {ICON_X_GRID9, ICON_X_SHUFFLE, ICON_X_CLOUD, ICON_X_HIT},
        {ICON_X_KICK, ICON_X_RISE, ICON_GLIDE, ICON_LENGTH}, {ICON_SIZE, ICON_TONE, ICON_X_IMPULSE, ICON_X_RISE},
        {ICON_VOICE, ICON_X_DICE, ICON_GATE, ICON_TRANSPOSE}};
    const gen_track_t *g = &gen.t[song.sel % NTRK];
    switch (id) {
    case GD_ROLE: return gen_role_icon(g->role); case GD_STYLE: case GD_MORPHTO: return ICON_X_GRID; case GD_MORPH: return ICON_GLIDE;
    case GD_VAR: return ICON_X_DICE; case GD_DRIFT: return ICON_X_MOTION; case GD_ENERGY: return ICON_ACCENT; case GD_FILL: return ICON_X_HIT;
    case GD_ROOT: return ICON_PITCH; case GD_SCALE: return ICON_SCALE; case GD_PROG: return ICON_X_NODE; case GD_RATE: return ICON_DIVISION;
    case GD_METHOD: return gen_method_icon(g->role, g->method); case GD_P1: case GD_P2: case GD_P3: return ICON_X_KNOB;
    case GD_FORM: return ICON_X_BOOKS; case GD_SLEN: return ICON_LENGTH; case GD_GROW: return ICON_X_LADDER; case GD_TAIL: return ICON_X_END;
    default: return MACRO[g->role % GR_N][(id - GD_M1) & 3u];
    }
}
static const uint8_t GEN_KEY_ICON[16] = {ICON_X_DICE, ICON_X_HIT, ICON_PITCH, ICON_X_SHUFFLE, ICON_X_PREV, ICON_X_NEXT, ICON_X_LOCK, ICON_X_MOTION,
                                         ICON_NONE, ICON_NONE, ICON_NONE, ICON_NONE, ICON_X_SONG, ICON_ACCENT, ICON_X_LINK, ICON_X_CLOUD};
static uint32_t gen_methods_of(uint32_t role) { return role == GR_DRUMS ? GMD_N : role == GR_BASS ? GMB_N : role == GR_CHORDS ? GMC_N : role == GR_LEAD ? GML_N : 1u; }
static const char *const GEN_ROLE_NAME[GR_N] = {"OFF", "DRUMS", "BASS", "CHORDS", "LEAD"};
static const char *const GEN_RATE_NAME[3] = {"1 BAR", "2 BAR", "4 BAR"};
static const char *const GEN_SLEN_NAME[3] = {"HALF", "NORM", "LONG"};
static const char *const GEN_TAIL_NAME[3] = {"NONE", "INTRO", "PEEL"};
static const char *const GEN_MACRO_NAME[GR_N][4] = {
    {"-", "-", "-", "-"}, {"DENS", "SKIP", "GHOST", "ROLL"}, {"FOLLOW", "MOVE", "GLIDE", "LEN"},
    {"SPREAD", "COLOR", "RHYTHM", "STRUM"}, {"CELL", "VARY", "REST", "RANGE"}};
static const char *const GEN_COLOR_NAME[GC_N] = {"TRIAD", "SUS", "ADD9", "7TH"};
static const char *const GEN_RHYTHM_NAME[GRH_N] = {"PAD", "HALF", "STAB", "ARP"};
static uint8_t gen_target;                             /* the section the layer works on, 0..3 (A..D) */
static uint8_t gen_all;                                /* ALL held (key 15): actions on every unlocked track */
static uint8_t gen_dirty;                              /* the state changed since its flash copy (saved with the project) */

static int32_t gen_val(uint32_t id)
{
    gen_track_t *g = &gen.t[song.sel % NTRK];
    switch (id) {
    case GD_ROLE: return g->role; case GD_STYLE: return g->style; case GD_MORPHTO: return g->morph_to; case GD_MORPH: return g->morph;
    case GD_VAR: return g->variation; case GD_DRIFT: return g->drift; case GD_ENERGY: return gen.p.energy[gen_target]; case GD_FILL: return g->fill;
    case GD_ROOT: return gen.p.root; case GD_SCALE: return gen.p.scale; case GD_PROG: return gen.p.prog; case GD_RATE: return gen.p.rate;
    case GD_METHOD: return g->method; case GD_P1: case GD_P2: case GD_P3: return g->mp[id - GD_P1];
    case GD_FORM: return gen.p.form; case GD_SLEN: return gen.p.song_len; case GD_GROW: return gen.p.grow; case GD_TAIL: return gen.p.tail;
    default: return g->macro[(id - GD_M1) & 3u];
    }
}
static void gen_set(uint32_t id, int32_t v)
{
    gen_track_t *g = &gen.t[song.sel % NTRK];
    const gen_desc_t *d = &GEN_DESC[id % GD_N];
    v = clamp(v, d->min, d->max);
    switch (id) {
    case GD_ROLE:                                      /* only a role the engine offers (gen_role_ok) */
        while (!gen_role_ok(TSEL, (uint32_t)v)) v = v + (v >= g->role ? 1 : -1), v = v < 0 ? GR_N - 1 : v % GR_N;
        if ((uint32_t)v != g->role) gen_init_track(g, (uint32_t)v);
        break;
    case GD_STYLE: g->style = (uint8_t)v; break; case GD_MORPHTO: g->morph_to = (uint8_t)v; break; case GD_MORPH: g->morph = (uint8_t)v; break;
    case GD_VAR: g->variation = (uint8_t)v; break; case GD_DRIFT: g->drift = (uint8_t)v; break;
    case GD_ENERGY: gen.p.energy[gen_target] = (uint8_t)v; break; case GD_FILL: g->fill = (uint8_t)v; break;
    case GD_ROOT: gen.p.root = (uint8_t)v; break; case GD_SCALE: gen.p.scale = (uint8_t)v; break;
    case GD_PROG: gen.p.prog = (uint8_t)v; break; case GD_RATE: gen.p.rate = (uint8_t)v; break;
    case GD_METHOD: g->method = (uint8_t)clamp(v, 0, (int32_t)gen_methods_of(g->role) - 1); break;
    case GD_P1: case GD_P2: case GD_P3: g->mp[id - GD_P1] = (uint8_t)v; break;
    case GD_FORM: gen.p.form = (uint8_t)v; break; case GD_SLEN: gen.p.song_len = (uint8_t)v; break;
    case GD_GROW: gen.p.grow = (uint8_t)v; break; case GD_TAIL: gen.p.tail = (uint8_t)v; break;
    default: g->macro[(id - GD_M1) & 3u] = (uint8_t)v; break;
    }
    gen_dirty = 1;
}
/* the value as shown */
static void gen_fmt(uint32_t id, char *val, const char **unit)
{
    const gen_desc_t *d = &GEN_DESC[id % GD_N];
    int32_t v = gen_val(id);
    uint32_t role = gen.t[song.sel % NTRK].role;
    *unit = "";
    if (id == GD_ROLE) { str_cpy(val, GEN_ROLE_NAME[v % GR_N], 12); return; }
    if (id == GD_STYLE || id == GD_MORPHTO) { str_cpy(val, gen_style((uint32_t)v)->name, 12); return; }
    if (id == GD_ROOT) { str_cpy(val, N_NOTE[v % 12], 12); return; }
    if (id == GD_SCALE) { str_cpy(val, N_SCALE[v % 16], 12); return; }
    if (id == GD_PROG) { str_cpy(val, gen_prog((uint32_t)v)->name, 12); return; }
    if (id == GD_RATE) { str_cpy(val, GEN_RATE_NAME[v % 3], 12); return; }
    if (id == GD_FORM) { str_cpy(val, gen_form_name((uint32_t)v), 12); return; }
    if (id == GD_SLEN) { str_cpy(val, GEN_SLEN_NAME[v % 3], 12); return; }
    if (id == GD_TAIL) { str_cpy(val, GEN_TAIL_NAME[v % 3], 12); return; }
    if (id == GD_GROW) { fmt_int(val, v + 1); return; }
    if (role == GR_CHORDS && id == GD_M2) { str_cpy(val, GEN_COLOR_NAME[(uint32_t)v * (GC_N - 1u) / 100u], 12); return; }
    if (role == GR_CHORDS && id == GD_M3) { str_cpy(val, GEN_RHYTHM_NAME[(uint32_t)v * (GRH_N - 1u) / 100u], 12); return; }
    if (id == GD_METHOD) { str_cpy(val, GEN_METHOD_NAME[role % GR_N][(uint32_t)v % GEN_METHODS_MAX], 12); return; }
    if (id == GD_P1 && role == GR_DRUMS && gen.t[song.sel % NTRK].method == GMD_CA) { fmt_int(val, GEN_CA_RULES[(uint32_t)v * 7u / 100u]); return; }
    fmt_int(val, v);
    if (d->pct) *unit = "%";
}
static const char *gen_label(uint32_t id)
{
    if (id >= GD_M1 && id < GD_M1 + 4u) return GEN_MACRO_NAME[gen.t[song.sel % NTRK].role % GR_N][id - GD_M1];
    if (id >= GD_P1 && id <= GD_P3) { const gen_track_t *g = &gen.t[song.sel % NTRK]; return GEN_MP_NAME[g->role % GR_N][g->method % GEN_METHODS_MAX][id - GD_P1]; }
    return GEN_DESC[id % GD_N].label;
}
/* the four cards of an SC_GEN page or the layer (ui_draw.c draws them through this) */
/* a card that does not apply right now (a style on a pitched track, P1..P3 of a role's own method, GROW off GROWN,
 * anything on an OFF track) shows "--" and no bar, so the knobs that matter stand out */
static int gen_applies(uint32_t id)
{
    const gen_track_t *g = &gen.t[song.sel % NTRK];
    if (id == GD_ROLE || id == GD_FORM || id == GD_SLEN || id == GD_TAIL || (id >= GD_ROOT && id <= GD_RATE)) return 1;
    if (id == GD_GROW) return gen.p.form % GFM_N == GFM_GRAMMAR;
    if (id == GD_ENERGY) return 1;
    if (g->role == GR_OFF) return 0;
    if (id == GD_STYLE || id == GD_MORPHTO || id == GD_MORPH) return g->role == GR_DRUMS;
    if (id >= GD_P1 && id <= GD_P3) return g->method != 0u && GEN_MP_NAME[g->role % GR_N][g->method % GEN_METHODS_MAX][id - GD_P1][0] != '-';
    if (id >= GD_M1 && id < GD_M1 + 4u) return GEN_MACRO_NAME[g->role % GR_N][id - GD_M1][0] != '-';
    return 1;
}
static void gen_cards(const page_t *pg)
{
    char val[12], lab[12];
    const char *unit, *label;
    uint32_t c;
    for (c = 0; c < 4u; c++) {
        uint32_t id = pg ? pg->id[c] : GD_M1 + c;
        const gen_desc_t *d = &GEN_DESC[id % GD_N];
        int32_t v = gen_val(id), span = d->max - d->min;
        label = gen_label(id);
        if (id == GD_ENERGY) {                         /* ENERGY of which section: "ENRGY B" */
            str_cpy(lab, "ENRGY ", sizeof lab); lab[6] = (char)('A' + gen_target % GEN_NSEC); lab[7] = 0; label = lab;
        }
        if (!gen_applies(id)) { draw_column(c, label, "--", "", T_DIM, -1, gen_card_icon(id)); continue; }
        gen_fmt(id, val, &unit);
        draw_column(c, label, val, unit, VAL(c), span ? (v - d->min) * 1000 / span : -1, gen_card_icon(id));
    }
}
/* ------------------------------------------------------- the pages' panel --- */
/* under the cards of every GEN page (GR_GEN): what the four tracks are (role, method; the selected one framed; a dot
 * for LIVE, a block for LOCK), the four sections' ENERGY (the target section in the theme ink, its letter too) and
 * the selected track's phrase as it stands: 64 steps in four bars, a hit's height its lanes (drums) or a note, ties
 * low. What ROLL just did is seen here without leaving the page */
static uint32_t graph_gen_sig(void)
{
    uint32_t h = gen_render_count * 2654435761u, i;
    const uint8_t *p = (const uint8_t *)&gen.p;
    for (i = 0; i < sizeof gen.p; i++) h = (h ^ p[i]) * 16777619u;
    for (i = 0; i < NTRK; i++) h = (h ^ (gen.t[i].role + gen.t[i].method * 16u + gen.t[i].flags * 256u + gen.t[i].locks_used * 4096u)) * 16777619u;
    for (i = 0; i < sizeof chain_config; i++) h = (h ^ ((const uint8_t *)&chain_config)[i]) * 16777619u;
    return h ^ gen_target * 7919u;
}
/* GEN METHOD: the generator as a picture in the sections' row (x 8..232, y 40..70): EUCLID a ring of n steps with
 * k lit, CA a few rows of its rule, TURING the register as a ring of bits, MARKOV seven degree nodes joined by
 * their likeliest moves, CHAOS the logistic map's trace, PINK three dice rows, LSYS the string's letters as steps,
 * WALK a random path, POLY the lanes' cycle lengths, STYLE the grid's weights, the rest a glyph and a word */
static void gen_dot(int32_t x, int32_t y, int32_t d, uint16_t col) { if (d <= 3) cv_rect(x, y, d, d, col); else cv_rrect(x, y, d, d, d / 2, col, T_SURF); }
static void gen_method_picture(const gen_track_t *g, uint16_t c)
{
    uint32_t role = g->role % GR_N, m = g->method, i, k, n;
    uint32_t euclid = (role == GR_DRUMS && m == GMD_EUCLID) || (role == GR_LEAD && m == GML_EUCLID) || (role == GR_CHORDS && m == GMC_PULSE);
    uint32_t turing = (role == GR_DRUMS && m == GMD_TURING) || (role == GR_LEAD && m == GML_TURING) || (role == GR_CHORDS && m == GMC_TURING);
    uint32_t markov = (role == GR_BASS && m == GMB_MARKOV) || (role == GR_LEAD && m == GML_MARKOV);
    uint32_t chaos = (role == GR_BASS && m == GMB_CHAOS) || (role == GR_LEAD && m == GML_CHAOS);
    uint32_t walk = (role == GR_BASS && m == GMB_WALK) || (role == GR_LEAD && m == GML_WALK);
    uint32_t call = (role == GR_DRUMS && m == GMD_CALL) || (role == GR_BASS && m == GMB_CALL) || (role == GR_LEAD && m == GML_CALL);
    char b[16];
    if (euclid) {                                      /* the ring: n steps, k of them lit, from the top; E(k,n) beside */
        static const int8_t CX[16] = {0, 5, 9, 12, 13, 12, 9, 5, 0, -5, -9, -12, -13, -12, -9, -5};
        n = role == GR_DRUMS ? (g->mp[1] < 25u ? 16u : g->mp[1] < 50u ? 12u : g->mp[1] < 75u ? 8u : 16u) : 16u;
        k = 1u + (role == GR_DRUMS ? 4u : 3u) * (g->mp[0] * 16u / 100u) / 4u; if (k > n) k = n;
        for (i = 0; i < n; i++) {
            uint32_t a = i * 16u / n;
            gen_dot(26 + CX[a] - 2, 55 - CX[(a + 4u) % 16u] - 2, gen_euclid(k, n, 0, i) ? 5 : 3, gen_euclid(k, n, 0, i) ? c : T_MID);
        }
        str_cpy(b, "E(", sizeof b); fmt_int(b + 2, (int32_t)k); str_cpy(b + str_len(b), ",", sizeof b - str_len(b)); fmt_int(b + str_len(b), (int32_t)n); str_cpy(b + str_len(b), ")", sizeof b - str_len(b));
        cv_text_on(50, 40 + CAP_IN(S, 30), &AF_S, b, T_TEXT, T_SURF);
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, "EVEN SPREAD", T_MID, T_SURF);
    } else if (role == GR_DRUMS && m == GMD_CA) {      /* four rows of the rule from one lit cell */
        uint32_t rule = GEN_CA_RULES[g->mp[0] * 7u / 100u], row = 1u << 16, r;
        for (r = 0; r < 5u; r++) {
            uint32_t nx = 0;
            for (i = 0; i < 33u; i++) { uint32_t l = (row >> ((i + 32u) % 33u)) & 1u, cc = (row >> i) & 1u, rr = (row >> ((i + 1u) % 33u)) & 1u;
                if (cc) cv_rect(8 + 6 * (int32_t)i, 41 + 6 * (int32_t)r, 5, 5, c);
                nx |= ((rule >> (l * 4u + cc * 2u + rr)) & 1u) << i; }
            row = nx;
        }
        str_cpy(b, "RULE ", sizeof b); fmt_int(b + 5, (int32_t)rule); cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, b, T_MID, T_SURF);
    } else if (turing) {                               /* the register as a ring of bits, FLIP beside */
        static const int8_t CX[16] = {0, 5, 9, 12, 13, 12, 9, 5, 0, -5, -9, -12, -13, -12, -9, -5};
        uint32_t reg = gen_turing(g->seed_rhythm + song.sel % NTRK, 0, 16u, g->mp[0] * 255u / 100u, 0);
        for (i = 0; i < 16u; i++) gen_dot(26 + CX[i] - 2, 55 - CX[(i + 4u) % 16u] - 2, (reg >> i) & 1u ? 5 : 3, (reg >> i) & 1u ? c : T_MID);
        cv_text_on(50, 40 + CAP_IN(S, 30), &AF_S, g->mp[0] == 0u ? "LOCKED LOOP" : g->mp[0] >= 45u && g->mp[0] <= 55u ? "RANDOM" : "DRIFTING LOOP", T_TEXT, T_SURF);
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, "SHIFT REGISTER", T_MID, T_SURF);
    } else if (markov) {                               /* seven nodes, the likeliest step from each as a line */
        for (i = 0; i < 7u; i++) {
            uint32_t best = 0, bw = 0, j;
            for (j = 0; j < 7u; j++) { uint32_t d = j > i ? j - i : i - j, w = (d == 0u ? 60u : d == 1u ? 100u : d == 2u ? 70u : d == 3u ? 35u : 12u) * (100u - g->mp[0]) / 100u + (uint32_t)gen_learned[song.sel % NTRK].n[i][j] * 40u * g->mp[0] / 100u; if (w > bw && j != i) { bw = w; best = j; } }
            cv_line(14 + 26 * (int32_t)i, 55, 14 + 26 * (int32_t)best, 55 + (best > i ? -10 : 10), T_MID);
        }
        for (i = 0; i < 7u; i++) { gen_dot(14 + 26 * (int32_t)i - 3, 52, 7, c); }
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, g->mp[0] > 50u ? "LEARNED" : "SEEDED", T_MID, T_SURF);
    } else if (chaos) {                                /* the logistic map's trace over 56 steps */
        uint32_t x = 16384u + (g->seed_pitch & 8191u);
        int32_t px = 8, py = 68;
        for (i = 0; i < 44u; i++) { int32_t xx = 8 + 4 * (int32_t)i, yy = 68 - (int32_t)(x * 26u / 65536u); x = gen_logistic(x, 350u + g->mp[0] / 2u); if (x < 300u) x = 300u; if (i) cv_line(px, py, xx, yy, c); px = xx; py = yy; }
        str_cpy(b, "R 3.", sizeof b); fmt_int(b + 4, (int32_t)(g->mp[0] / 2u + 50u) % 100); if (g->mp[0] == 100u) str_cpy(b, "R 4.00", sizeof b);
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, b, T_MID, T_SURF);
    } else if (role == GR_LEAD && m == GML_PINK) {     /* the dice rows: die d re-rolls every 2^d steps */
        n = 2u + g->mp[0] * 3u / 100u;
        for (k = 0; k < n && k < 4u; k++) for (i = 0; i < 32u; i++)
            cv_rect(8 + 7 * (int32_t)i, 41 + 7 * (int32_t)k, 6, 6, (i >> k) & 1u ? c : T_RAISE);
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, "1/F DICE", T_MID, T_SURF);
    } else if (role == GR_LEAD && m == GML_LSYS) {     /* the string, each letter a step up or down */
        static const char *const RULES[4][2] = {{"AB", "A"}, {"AB", "BA"}, {"ABB", "A"}, {"BA", "AAB"}};
        uint8_t ls[64] = "A", nx[64]; uint32_t gens = 2u + g->mp[0] * 5u / 100u, rs = g->mp[1] * 3u / 100u, gg, nn, j; int32_t y = 55;
        n = 1;
        for (gg = 0; gg < gens && n < 32u; gg++) { for (i = 0, nn = 0; i < n && nn < 60u; i++) { const char *r = RULES[rs][ls[i] == 'B']; for (j = 0; r[j] && nn < 60u; j++) nx[nn++] = (uint8_t)r[j]; } memcpy(ls, nx, nn); n = nn; }
        for (i = 0; i < n && i < 36u; i++) { y += ls[i] == 'A' ? -3 : 3; if (y < 42) y = 42; if (y > 66) y = 66; cv_rect(8 + 6 * (int32_t)i, y, 5, 3, ls[i] == 'A' ? c : T_MID); }
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, "L-SYSTEM", T_MID, T_SURF);
    } else if (walk) {                                 /* a random path, seeded as the track's */
        int32_t y = 55, py = 55;
        for (i = 0; i < 36u; i++) { int32_t d = (int32_t)(gen_rand(g->seed_pitch, song.sel, 1u, i, 71u) % 3u) - 1; y += d * (1 + (int32_t)g->mp[0] / 34); if (y < 42) y = 42; if (y > 66) y = 66; cv_line(8 + 6 * (int32_t)i, py, 14 + 6 * (int32_t)i, y, c); py = y; }
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, "RANDOM WALK", T_MID, T_SURF);
    } else if (role == GR_DRUMS && m == GMD_POLY) {    /* each lane's cycle as a bar of its length */
        static const uint8_t POLY_N[8] = {16, 16, 12, 6, 10, 7, 5, 9};
        for (i = 0; i < 8u; i++) { uint32_t len = POLY_N[i] + (16u - POLY_N[i]) * g->mp[1] / 100u; cv_rect(8, 41 + 4 * (int32_t)i, (int32_t)len * 8, 3, i < 2u ? c : T_MID); cv_text_on(140, 41 + 4 * (int32_t)i - 3, &AF_S, "", T_MID, T_SURF); }
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, "LANE CYCLES", T_MID, T_SURF);
    } else if (role == GR_DRUMS && m == GMD_STYLE) {   /* the style's grid: 8 lanes x 32 steps of weight */
        const gen_style_t *st = gen_style(g->style);
        for (k = 0; k < NLANE; k++) for (i = 0; i < 32u; i++) if (st->w[k][i] > 40u) cv_rect(8 + 6 * (int32_t)i, 41 + 4 * (int32_t)k, 4, 3, st->w[k][i] > 180u ? c : T_MID);
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, st->name, T_MID, T_SURF);
    } else if (call) {                                 /* two lanes: the reference's hits, this track answering in its gaps (or on them) */
        for (i = 0; i < 16u; i++) { uint32_t ref = (i % 4u) == 0u; cv_rect(8 + 14 * (int32_t)i, 43, 10, 6, ref ? T_MID : T_RAISE); cv_rect(8 + 14 * (int32_t)i, 59, 10, 6, (g->mp[1] < 50u ? ref : !ref) ? c : T_RAISE); }
        cv_text_r(232, 40 + CAP_IN(S, 30), &AF_S, g->mp[1] < 50u ? "WITH" : "BETWEEN", T_MID, T_SURF);
    } else {                                           /* a glyph and a word */
        static const char *const WORD[GR_N] = {"", "", "FOLLOWS THE KICK", "VOICINGS", "A CELL, TRANSFORMED"};
        const char *w = role == GR_BASS ? (m == GMB_ARP ? "CHORD TONES" : m == GMB_DRONE ? "ROOT, HELD" : WORD[2]) : role == GR_CHORDS ? (m == GMC_ARP ? "BROKEN CHORD" : WORD[3]) : WORD[role];
        cv_icon_on(8, 46, 16, gen_method_icon(role, m), c, T_SURF);
        cv_text_on(30, 40 + CAP_IN(S, 30), &AF_S, w, T_MID, T_SURF);
    }
}
/* HARMONY: the scale on two octaves of keys (the root in the theme ink, the scale's notes lit), and the target
 * section's four bars of chords as roman degrees with their root note */
static void gen_harmony_picture(uint16_t c)
{
    static const uint8_t BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    static const char *const ROMAN[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    uint8_t sc[7]; uint32_t i, j, bar;
    char b[12];
    gen_scale_notes(gen.p.scale, sc);
    for (i = 0; i < 24u; i++) {                        /* 24 keys of 9 px from C */
        uint32_t pc = i % 12u, in = 0, root = pc == gen.p.root % 12u;
        int32_t x = 8 + 9 * (int32_t)i;
        for (j = 0; j < 7u; j++) in |= (gen.p.root + sc[j]) % 12u == pc;
        cv_rrect(x, BLACK[pc] ? 40 : 52, 8, BLACK[pc] ? 14 : 22, 2, root ? c : in ? T_MID : T_RAISE, T_SURF);
    }
    for (bar = 0; bar < 4u; bar++) {                   /* the chords of the section, a bar each */
        uint32_t d = gen_degree_at(gen_target, bar);
        int32_t x = 8 + 56 * (int32_t)bar;
        cv_rrect(x, 82, 52, 28, 3, T_RAISE, T_SURF);
        str_cpy(b, ROMAN[d % 7u], sizeof b);
        cv_text_on(x + 5, 82 + CAP_IN(S, 28), &AF_S, b, c, T_RAISE);
        str_cpy(b, N_NOTE[(gen.p.root + sc[d % 7u]) % 12u], sizeof b);
        cv_text_r(x + 47, 82 + CAP_IN(S, 28), &AF_S, b, T_TEXT, T_RAISE);
    }
}
static void graph_gen(uint16_t c)
{
    const gen_track_t *g = &gen.t[song.sel % NTRK];
    uint32_t i, s, drum = g->role == GR_DRUMS;
    char b[24];
    for (i = 0; i < NTRK; i++) {                       /* the tracks: T1 DRUMS / EUCLID */
        const gen_track_t *gi = &gen.t[i];
        int32_t x = 8 + 56 * (int32_t)i;
        uint16_t fg = gi->role == GR_OFF ? T_DIM : T_TEXT, bg = T_SURF;
        if (ux.style == ST_RISO) { ux_pen = UI_RISO_INK[i % 4u]; ux_pen2 = UI_RISO_INK[(i + 1u) % 4u]; }
        if (i == song.sel % NTRK) { cv_rrect(x - 2, 4, 56, 30, 3, T_RAISE, T_SURF); bg = T_RAISE; }
        ux_pen = ux_pen2 = 0;
        {                                              /* the role's glyph and its name (the columns are T1..T4, left to right) */
            static const char *const SHORT[GR_N] = {"OFF", "DRUM", "BASS", "CHORD", "LEAD"};
            cv_icon_on(x, 5, 12, gen_role_icon(gi->role), fg, bg);
            cv_text_on(x + 14, 7, &AF_S, SHORT[gi->role % GR_N], fg, bg);
        }
        cv_text_on(x, 20, &AF_S, gi->role == GR_OFF ? "" : GEN_METHOD_NAME[gi->role % GR_N][gi->method % GEN_METHODS_MAX], T_MID, bg);
        if (gi->flags & GF_LOCK) cv_rect(x + 49, 20, 5, 4, fg);                     /* LOCK: a block, LIVE: a dot, after the method */
        if (gi->flags & GF_LIVE) cv_rect(x + 49, 26, 5, 4, T_ACCENT);
    }
    if (cur_page()->id[0] == GD_FORM) {                /* GEN FORM: the form itself, a block per row as wide as its bars,
                                                        * as tall as its section's ENERGY, PEEL rows (lanes silent) hollow */
        chain_config_t cc;
        uint32_t r, bars = 0, pos = 0;
        if (gen.p.form % GFM_N == GFM_CUSTOM) cc = chain_config; else gen_chain_write(&cc);
        for (r = 0; r < cc.count; r++) bars += cc.row[r].repeat;
        if (!bars) bars = 1;
        for (r = 0; r < cc.count; r++) {
            uint32_t sec = cc.row[r].slot[0] < GEN_NSEC ? cc.row[r].slot[0] : (cc.row[r].slot[1] < GEN_NSEC ? cc.row[r].slot[1] : cc.row[r].slot[2] % GEN_NSEC);
            int32_t x0 = 8 + (int32_t)(pos * 224u / bars), x1 = 8 + (int32_t)((pos + cc.row[r].repeat) * 224u / bars) - 1, w = x1 - x0;
            int32_t h = 14 + (int32_t)gen.p.energy[sec] * 40 / 100, silent = 0;
            for (i = 0; i < NTRK; i++) silent |= cc.row[r].slot[i] == CHAIN_SILENT;
            if (w < 2) w = 2;
            cv_rrect(x0, 100 - h, w, h, 2, silent ? T_RAISE : sec == GS_DROP ? c : T_MID, T_SURF);
            if (w >= 12) { b[0] = (char)('A' + sec); b[1] = 0; cv_text_in(x0, 100 - h + 2, w, &AF_S, b, silent ? T_MID : T_SURF, silent ? T_RAISE : sec == GS_DROP ? c : T_MID); }
            pos += cc.row[r].repeat;
        }
        fmt_int(b, (int32_t)cc.count); str_cpy(b + str_len(b), " ROWS  ", sizeof b - str_len(b)); fmt_int(b + str_len(b), (int32_t)bars); str_cpy(b + str_len(b), " BARS", sizeof b - str_len(b));
        cv_text_on(8, 102 + CAP_IN(S, 14), &AF_S, b, T_MID, T_SURF);
        cv_text_r(232, 102 + CAP_IN(S, 14), &AF_S, gen_form_name(gen.p.form), T_MID, T_SURF);
        return;
    }
    if (cur_page()->id[0] == GD_METHOD) { gen_method_picture(g, c); goto strip; }   /* GEN METHOD: the method drawn as itself */
    if (cur_page()->id[0] == GD_ROOT) { gen_harmony_picture(c); return; }         /* HARMONY: the scale on keys, the chords */
    for (s = 0; s < GEN_NSEC; s++) {                   /* the sections' ENERGY, the target in the theme ink */
        int32_t x = 8 + 56 * (int32_t)s, h = (int32_t)gen.p.energy[s] * 24 / 100;
        uint16_t ink = s == gen_target ? c : T_MID;
        b[0] = (char)('A' + s); b[1] = 0;
        cv_rrect(x, 42, 36, 26, 2, T_RAISE, T_SURF);
        if (h > 1) cv_rrect(x, 68 - h, 36, h, 2, ink, T_RAISE);
        cv_text_on(x + 42, 42 + CAP_IN(S, 26), &AF_S, b, ink, T_SURF);
    }
strip:
    for (s = 0; s < NSTEP; s++) {                      /* the phrase: 64 steps, four bars */
        const step_t *st = &TSEL->step[s];
        int32_t x = 8 + 3 * (int32_t)s + 5 * (int32_t)(s / 16u), h = 0;
        if (st->time == ST_NOTE) h = drum ? 6 + 6 * (int32_t)__builtin_popcount(st->hit) : 24;
        else if (st->time == ST_TIE) h = 5;
        if (drum && h > 24) h = 24;
        cv_rect(x, 80, 2, 30, T_RAISE);
        if (h) cv_rect(x, 110 - h, 2, h, st->time == ST_TIE ? T_MID : c);
    }
    fmt_int(b, g->locks_used); str_cpy(b + str_len(b), "/", sizeof b - str_len(b)); fmt_int(b + str_len(b), (int32_t)gen_lock_budget(g->role));
    cv_text_r(232, 80 + CAP_IN(S, 30), &AF_S, b, T_MID, T_SURF);
}
/* KNOB k on an SC_GEN page (ui_input.c) or in the layer: the value, then a render so it is heard */
static void gen_render_live(void);
static void gen_edit(const page_t *pg, uint32_t k, int32_t s)
{
    uint32_t id = pg ? pg->id[k & 3u] : GD_M1 + (k & 3u);
    const gen_desc_t *d = &GEN_DESC[id % GD_N];
    gen_set(id, gen_val(id) + accel(EN_K1 + (k & 3u), s, d->max - d->min));
    gen_render_live();
    ui.force = 1;
}

/* ----------------------------------------------------------- render, undo --- */
static void gen_undo_take(track_t *t) { step_undo_take(t, 0x6E4u); }   /* one undo per action (ui.c, SAVE held) */
/* the selected track (ALL held: every unlocked one) rendered into its steps for the target section: what is heard */
static void gen_render_live(void)
{
    uint32_t i;
    if (gen_all) { gen_render_section(gen_target); return; }
    i = song.sel % NTRK;
    gen_render_track(i, gen_target);
}
static void gen_act(void (*fn)(uint32_t, uint32_t, uint32_t), uint32_t a, uint32_t b)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        if (gen_all ? !(gen.t[i].flags & GF_LOCK) : i == song.sel % NTRK) { gen_undo_take(&trk[i]); fn(i, a, b); }
    gen_render_live();
    gen_dirty = 1;
}
static void gen_a_roll(uint32_t i, uint32_t r, uint32_t p) { gen_roll(i, r, p); }
static void gen_a_mutate(uint32_t i, uint32_t a, uint32_t b) { (void)a; (void)b; gen_mutate(i); }
static void gen_a_breed(uint32_t i, uint32_t a, uint32_t b) { (void)a; (void)b; gen_breed(i); }
static void gen_a_learn(uint32_t i, uint32_t a, uint32_t b) { (void)a; (void)b; gen_markov_learn(i, tl_open.step); ui_message("LEARNED"); }
static void gen_a_hist(uint32_t i, uint32_t dir, uint32_t b) { (void)b; gen_hist_step(i, dir ? 1 : -1); }

/* COMMIT: the render stands as the steps already; this writes GEN's locks within the budget and says so */
static void gen_commit(void)
{
    uint32_t i = song.sel % NTRK, r;
    char m[24];
    track_t *t = &trk[i];
    if (chain_busy()) { ui_message("STOP TO EDIT"); return; }
    motion_undo_take(t, 0x6E5u);
    gen_locks_clear(t, i);
    r = gen_locks_write(t, &gen.t[i], i);
    str_cpy(m, r & 0x100u ? "LOCKS FULL " : "LOCKS ", sizeof m);
    fmt_int(m + str_len(m), (int32_t)(r & 0xFFu));
    str_cpy(m + str_len(m), "/", sizeof m - str_len(m));
    fmt_int(m + str_len(m), (int32_t)gen_lock_budget(gen.t[i].role));
    ui_message(m);
    gen.t[i].flags &= (uint8_t)~GF_LIVE;              /* committed: the hand editor's now */
    gen_dirty = 1;
}

/* SONG: the four sections from the master seed into slots A..D, and the chain. Each section is rendered into the
 * live tracks and saved as that slot's project (project.c project_save: the sounds as they are now); the slot that
 * was selected is loaded back. Slots holding something ask first (CF_GEN_SONG, OCT+) */
static void gen_song_do(void)
{
    uint32_t s, i, sel = (uint32_t)song.g[G_SLOT] % GEN_NSEC;
    if (chain_busy()) { ui_message("STOP TO EDIT"); return; }
    for (i = 0; i < NTRK; i++) gen_undo_take(&trk[i]);
    gen_song_seed(0);
    for (s = 0; s < GEN_NSEC; s++) {
        gen_render_section(s);
        for (i = 0; i < NTRK; i++) if (gen.t[i].role != GR_OFF && !(gen.t[i].flags & GF_LOCK)) gen_locks_clear(&trk[i], i);
        if (project_save(s)) { ui_message("SONG: SAVE FAILED"); return; }
    }
    if (gen_chain_write(&chain_config)) ui.song_row = 0;   /* (CUSTOM: the chain stays as built) */
    gen_render_section(sel);
    gen_target = (uint8_t)sel;
    ui_message(gen.p.form % GFM_N == GFM_CUSTOM ? "SONG A-D RE-RENDERED" : "SONG A-D WRITTEN");
    gen_dirty = 1;
}
static int gen_song_needs_ask(void)
{
    uint32_t s;
    for (s = 0; s < GEN_NSEC; s++) if (proj_import(&proj_scratch, &proj_slot[s], sizeof(project_store_t))) return 1;
    return 0;
}

/* ------------------------------------------------------------- the layer --- */
static const char *const GEN_KEY_NAME[16] = {"ROLL", "RHYT", "PTCH", "MUTE", "PREV", "NEXT", "LOCK", "LIVE",   /* (4 letters, as the FX map's) */
                                            "SEC A", "SEC B", "SEC C", "SEC D", "SONG", "FILL", "ALL", "TEX"};
/* a word in the header for what a key did ("ROLLED T2", "ROLLED ALL", "HISTORY 3/8", "T1 LOCKED"): the result is
 * in the steps and the panel, the word says which key landed */
static void gen_said(const char *what)
{
    char b[24];
    str_cpy(b, what, sizeof b);
    if (gen_all) str_cpy(b + str_len(b), " ALL", sizeof b - str_len(b));
    else { str_cpy(b + str_len(b), " T", sizeof b - str_len(b)); b[str_len(b) + 1] = 0; b[str_len(b)] = (char)('1' + song.sel % NTRK); }
    ui_message(b);
}
static void gen_layer_key(uint32_t k, uint32_t p)
{
    gen_track_t *g = &gen.t[song.sel % NTRK];
    char b[24];
    if (key_black(k)) {                                /* lane locks on a DRUMS track; 9 BREED, 10 LEARN (any role) */
        if (p < NLANE && g->role == GR_DRUMS) {
            g->lane_lock ^= (uint8_t)(1u << p); gen_dirty = 1;
            str_cpy(b, drum_lane_abbr(TSEL, p), sizeof b); str_cpy(b + str_len(b), (g->lane_lock >> p) & 1u ? " LANE LOCKED" : " LANE FREE", sizeof b - str_len(b)); ui_message(b);
        }
        else if (p == 8u) { if (!g->hist_len && !gen_all) { ui_message("BREED NEEDS A ROLL"); return; } gen_act(gen_a_breed, 0, 0); gen_said("BRED"); }   /* (black key 9) */
        else if (p == 9u) gen_act(gen_a_learn, 0, 0);  /* (black key 10: MARKOV learns the track as the layer opened) */
        return;
    }
    if ((p < 6u || p == 12u) && (g->flags & GF_LOCK) && !gen_all) { ui_message("LOCKED: UNLOCK FIRST"); return; }
    switch (p) {
    case 0: gen_act(gen_a_roll, 1, 1); gen_said("ROLLED"); break;
    case 1: gen_act(gen_a_roll, 1, 0); gen_said("NEW RHYTHM"); break;
    case 2: gen_act(gen_a_roll, 0, 1); gen_said("NEW PITCHES"); break;
    case 3: gen_act(gen_a_mutate, 0, 0); gen_said("MUTATED"); break;
    case 4: case 5:
        if (!g->hist_len) { ui_message("NO HISTORY YET"); return; }
        gen_act(gen_a_hist, p == 5u, 0); gen_said(p == 5u ? "NEXT ROLL" : "PREV ROLL");
        break;
    case 6: g->flags ^= GF_LOCK; gen_dirty = 1; gen_said(g->flags & GF_LOCK ? "LOCKED" : "UNLOCKED"); break;
    case 7: g->flags ^= GF_LIVE; if (g->flags & GF_LIVE) gen_render_live(); gen_dirty = 1; gen_said(g->flags & GF_LIVE ? "LIVE ON" : "LIVE OFF"); break;
    case 8: case 9: case 10: case 11:
        gen_target = (uint8_t)(p - 8u); gen_render_live();
        str_cpy(b, "SECTION A: ENERGY ", sizeof b); b[8] = (char)('A' + gen_target); fmt_int(b + str_len(b), gen.p.energy[gen_target]); ui_message(b);
        break;
    case 12:
        if (gen_song_needs_ask()) confirm_open(CF_GEN_SONG, song.sel);
        else gen_song_do();
        break;
    case 13: gen_fill_held = 1; break;                 /* (layer_keys clears it when the key is let go) */
    case 14: gen_all = 1; break;
    case 15: gen.p.mask[gen_target] ^= GM_TEXTURE; gen_dirty = 1; ui_message(gen.p.mask[gen_target] & GM_TEXTURE ? "TEXTURE ON" : "TEXTURE OFF"); break;
    default: break;
    }
    ui.force = 1;
}
/* each pass: the keys of FILL and ALL still held (kb_layer: the layer's keys down) */
static void gen_layer_held(uint32_t kb)
{
    uint32_t k, fill = 0, all = 0;
    for (k = 0; k < 27u; k++)
        if ((kb >> k) & 1u && !key_black(k)) { uint32_t p = key_place(k); if (p == 13u) fill = 1; if (p == 14u) all = 1; }
    gen_fill_held = (uint8_t)fill;
    gen_all = (uint8_t)all;
    if (seq_fill != fill) { seq_fill = (uint8_t)fill; ui.force = 1; }   /* FILL: the steps' FILL conditions too (seq.c) */
}
/* the map: 4 x 4 cells of the white keys, the state of each; a DRUMS track: the 8 lane cells below */
static void gen_layer_map(void)
{
    gen_track_t *g = &gen.t[song.sel % NTRK];
    uint32_t p, drum = g->role == GR_DRUMS;
    char n[3] = {0, 0, 0};
    for (p = 0; p < 16u; p++) {
        uint32_t st = LS_OFF;
        if (p == 6u && (g->flags & GF_LOCK)) st = LS_SEL;
        if (p == 7u && (g->flags & GF_LIVE)) st = LS_SEL;
        if (p >= 8u && p < 12u && p - 8u == gen_target) st = LS_SEL;
        if (p == 13u && gen_fill_held) st = LS_HELD;
        if (p == 14u && gen_all) st = LS_HELD;
        if (p == 15u && (gen.p.mask[gen_target] & GM_TEXTURE)) st = LS_SEL;
        if ((p < 6u || p == 12u) && (g->flags & GF_LOCK)) st = LS_DIM;
        if (p == 12u && chain_busy()) st = LS_DIM;
        if ((p == 4u || p == 5u) && !g->hist_len) st = LS_DIM;
        int32_t cx = LC_X(p % 4u), cy = 4 + (LF_H + 4) * (int32_t)(p / 4u);
        uint32_t icon = GEN_KEY_ICON[p];
        n[0] = W_NOTE[p];
        if (p == 6u && (g->flags & GF_LOCK)) icon = ICON_X_UNLOCK;   /* (the key would unlock) */
        if (ux.style == ST_RISO) {                     /* RISO: the row's ink: orange rolls, blue state, teal sections, violet song */
            static const uint8_t ROW_INK[4] = {0, 1, 4, 3};
            ux_pen = UI_RISO_INK[ROW_INK[p / 4u]]; ux_pen2 = UI_RISO_INK[(ROW_INK[p / 4u] + 1u) % 4u];
        }
        /* the cell: its icon and name (the EDIT layer's form; the place is the key's); SEC A..D: the key letter and
         * the section's ENERGY as a bar along the cell's foot */
        lcell(cx, cy, LF_H, icon == ICON_NONE ? n : 0, icon, 0, GEN_KEY_NAME[p], st | (p & 1u ? LS_ALT : 0u), 0);   /* the FX map's rows */
        if (p >= 8u && p < 12u) {
            uint16_t ink; (void)lc_fill(st, &ink);
            cv_rect(cx + 4, cy + LF_H - 4, (int32_t)(LC_W - 8) * (int32_t)gen.p.energy[p - 8u] / 100, 2, st == LS_SEL ? (ux.style == ST_RISO ? T_BG : T_SURF) : (ux.style == ST_RISO && ux_pen ? ux_pen : ink));
        }
        ux_pen = ux_pen2 = 0;
    }
    if (drum && ux.style == ST_RISO)                   /* RISO: the lanes sit on a yellow block, as a second pass of the drum */
        cv_rect(4, LM_Y - 3, 232, LM_H + 6, UI_RISO_INK[6]);
    for (p = 0; drum && p < NLANE; p++) {              /* the lane locks (black keys 1..8) */
        int32_t x = 6 + 29 * (int32_t)p;               /* the black keys' row (the FX map's LM_Y): 8 cells of 25 px */
        uint16_t ink, fill;
        if (ux.style == ST_RISO) { ux_pen = UI_RISO_INK[2]; ux_pen2 = UI_RISO_INK[6]; }   /* RISO: green on the yellow block */
        fill = lc_box(x, LM_Y, 25, LM_H, lc_fill((g->lane_lock >> p) & 1u ? LS_SEL : LS_OFF, &ink));
        ux_pen = ux_pen2 = 0;
        cv_text_in(x, LM_Y + CAP_IN(S, LM_H), 25, &AF_S, drum_lane_abbr(TSEL, p), ink, fill);
    }
    if (!drum) {                                       /* a pitched track: the key, the role, the locks used */
        char b[24];
        str_cpy(b, GEN_ROLE_NAME[g->role % GR_N], sizeof b);
        str_cpy(b + str_len(b), "  ", sizeof b - str_len(b));
        str_cpy(b + str_len(b), N_NOTE[gen.p.root % 12u], sizeof b - str_len(b));
        str_cpy(b + str_len(b), " ", sizeof b - str_len(b));
        str_cpy(b + str_len(b), N_SCALE[gen.p.scale % 16u], sizeof b - str_len(b));
        if (g->flags & GF_DIRTY) str_cpy(b + str_len(b), " *", sizeof b - str_len(b));
        cv_text_on(6, LM_Y + CAP_IN(S, LM_H), &AF_S, b, T_MID, T_SURF);
        str_cpy(b, "LOCKS ", sizeof b);
        fmt_int(b + str_len(b), g->locks_used);
        str_cpy(b + str_len(b), "/", sizeof b - str_len(b));
        fmt_int(b + str_len(b), (int32_t)gen_lock_budget(g->role));
        cv_text_r(234, LM_Y + CAP_IN(S, LM_H), &AF_S, b, T_MID, T_SURF);
    }
}
/* the signature of what the map shows (draw_layer redraws on a change) */
static uint32_t gen_layer_sig(void)
{
    const gen_track_t *g = &gen.t[song.sel % NTRK];
    return (uint32_t)g->flags * 31u + g->lane_lock * 131u + gen_target * 977u + gen_all * 7u + gen_fill_held * 3u +
           g->hist_len * 13u + g->role * 1009u + gen.p.root * 17u + gen.p.scale * 19u + g->locks_used * 23u +
           (uint32_t)gen.p.mask[gen_target] * 29u + song.sel * 65537u + (uint32_t)chain_busy() * 5u;
}

/* --------------------------------------------------------------- LIVE --- */
/* called by the UI loop each pass (not the ISR): at a bar line of the selected target, the LIVE tracks re-render */
static uint32_t gen_last_bar = 0xFFFFu;
static void gen_tick(void)
{
    uint32_t i, bar;
    const track_t *t = 0;
    if (!song.playing) { gen_last_bar = 0xFFFFu; return; }
    for (i = 0; i < NTRK; i++) if (gen.t[i].flags & GF_LIVE) { t = &trk[i]; break; }
    if (!t) return;
    bar = t->seq_idx / GEN_BAR;                        /* the bar, from the first LIVE track's step */
    if (bar == gen_last_bar) return;
    gen_last_bar = bar;
    if (bar % 4u != 0u) return;                        /* phrases change at their start: a whole render is 4 bars */
    for (i = 0; i < NTRK; i++)
        if (gen.t[i].flags & GF_LIVE) gen_live_bar(i, (uint32_t)song.tick >> 8);
}

/* ------------------------------------------------------------ storage --- */
/* the flash copy: OBJ_GEN, an A/B pair at 0xE8000 / 0xE9000 (storage.c st_sector: the sectors above the FM6 voice
 * bank, unused by Felucca), one payload holding the four slots' gen_state_t. Slot s is saved into it when project s
 * is saved and read back when it is loaded; a slot with no state starts from gen_init, so projects saved by stock
 * Felucca open as before. A backup of the projects alone leaves GEN's seeds behind: the web editor's backup carries
 * this object too (editor_backup.c lists OBJ_GEN) */
#define GEN_STORE_MAGIC 0x47454E32u                   /* 'GEN2' (GEN1: the 92-byte track, before methods) */
typedef struct { uint32_t magic; uint8_t has[GEN_NSEC]; gen_state_t s[GEN_NSEC]; } gen_store_t;
static gen_store_t gen_store __attribute__((section(".pool")));   /* (the main loop only; the pool: .bss is tight) */
static uint8_t gen_store_ram;                          /* 1: gen_store holds the slots (no flash: the RAM copy is all there is) */
static int gen_store_read(void)
{
#if FELUCCA_FLASH
    int n = st_load(OBJ_GEN, &gen_store, sizeof gen_store);
    if (n != (int)sizeof gen_store || gen_store.magic != GEN_STORE_MAGIC) { memset(&gen_store, 0, sizeof gen_store); gen_store.magic = GEN_STORE_MAGIC; return 0; }
    return 1;
#else
    if (!gen_store_ram) { memset(&gen_store, 0, sizeof gen_store); gen_store.magic = GEN_STORE_MAGIC; gen_store_ram = 1; return 0; }
    return 1;
#endif
}
static void gen_store_save(uint32_t slot)
{
    gen_store_read();
    gen_store.has[slot % GEN_NSEC] = 1;
    gen_store.s[slot % GEN_NSEC] = gen;
#if FELUCCA_FLASH
    if (st_save(OBJ_GEN, &gen_store, sizeof gen_store)) ui_message("GEN SAVE FAILED");
#endif
    gen_dirty = 0;
}
static void gen_store_load(uint32_t slot)
{
    if (gen_store_read() && gen_store.has[slot % GEN_NSEC] && gen_store.s[slot % GEN_NSEC].p.version == GEN_VERSION) gen = gen_store.s[slot % GEN_NSEC];
    else gen_init();
    memset(gen_wrote, 0, sizeof gen_wrote);
    gen_target = (uint8_t)(slot % GEN_NSEC);
    gen_dirty = 0;
}
