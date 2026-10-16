# GEN: a generative sequencer for the M-VAVE FM-1

GEN is a generative sequencer built into a fork of [Felucca](https://github.com/hugelton/Felucca), the multi-engine
firmware for the M-VAVE FM-1. Hold **ARP**, press a key, and a track gets a part: a 4-bar phrase of drums, bass,
chords or a lead, in one key, with the micro-timing, ghost notes, ratchets and gaps of 2-step, trap, witch house and
ambient. Roll again until you like it, lock what you keep, commit it to the steps and edit by hand from there, or
leave it LIVE and let it drift. One key writes a whole song's worth of sections into the four project slots.

It is aimed at the sound of Burial, Purity Ring, witch house and their neighbours, but every table it draws from is
data, so it can be pointed anywhere.

Everything GEN writes is an ordinary Felucca pattern: steps with notes, hits, chance, ratchets, nudge, accents, slides,
velocity and ties. The sequencer, the FX, the song mode and the web editor see nothing new. Projects saved here still
open on stock Felucca; GEN's own state (seeds, roles, knobs) rides beside them in its own flash object.

## Status

- **Source complete and host-tested.** `gen.c` (the generator) and `gen_ui.c` (the panel, pages, COMMIT, SONG,
  storage) compile into Felucca's real sources and pass Felucca's full UI test suite (`tests/ui_test.c`, 100+
  checks) with 16 new GEN checks driven through the actual panel code, plus `tests/gen_test.c` (determinism,
  monotone knobs, every budget, harmony, roles, sections, history).
- **Not yet flashed.** The JieLi pi32v2 toolchain is only available from JieLi's package server, which this build
  environment could not reach. Build it with `./build.sh` as [BUILDING.md](BUILDING.md) describes; nothing in GEN
  touches the HAL, the audio ISR or the flash map beyond one new A/B object. If you flash it first, read
  "If the FM-1 does not start" in the Felucca README first: recovery needs an RP2040.
- Not in this version: the web editor pages for styles and progressions (the tables are in `gen.c`, three user
  styles and two user progressions are reserved in RAM for the editor to fill); the TEXTURE role (it needs the Sloop
  texture bed ported to this fork; its mask bit and controls are in place and inert); the VOX engine (CHOP mode works
  on the stock SAMPLE, GRAIN and SLICE engines instead).

## The layer: hold ARP

Hold **ARP** for the GEN layer (double-tap to keep it open, as every Felucca layer). **ALGORITHM** picks the track,
as on every page. The 16 white keys, in four groups of four (keys 1, 5, 9 and 13 are the landmarks):

| Key | Name | What it does |
| --- | --- | --- |
| 1 | ROLL | New rhythm and pitch seeds for the selected track, rendered at once |
| 2 | RHYT | New rhythm seed only: the notes stay, where they fall changes |
| 3 | PTCH | New pitch seed only: the groove stays, the notes change |
| 4 | MUTE | Re-decides a fraction of the steps (GEN SHAPE > VARY); most of the part survives |
| 5 | PREV | Back through the last 8 rolls of the track |
| 6 | NEXT | Forward again |
| 7 | LOCK | The track is frozen: ROLL, MUTATE, ALL and SONG leave it alone |
| 8 | LIVE | The track re-renders itself at every phrase line, with DRIFT (GEN SHAPE) moving a seed now and then |
| 9 – 12 | SEC A B C D | The section being made: the project slot it belongs to, with its ENERGY and role mask |
| 13 | SONG | The four sections A–D from one master seed, into slots A–D, plus the song chain in the FORM set on the GEN FORM page (OCT+ confirms over used slots) |
| 14 | FILL | Held: the phrase-end fill plays now (and the steps' FILL conditions) |
| 15 | ALL | Held: the next key acts on every unlocked track |
| 16 | TEX | The section's texture-bed bit (inert until a texture bed exists) |

- **Black keys 1–8**, on a DRUMS track: lock a lane (KICK SNARE CLAP HATCL HATOP TOM RIM BELL, the EDIT layer's
  order). A locked lane keeps its hits while the others re-roll: keep the kick, re-roll the hats.
- **Black key 9**: BREED, a child of this roll and the last one (see Methods). **Black key 10**: LEARN, the MARKOV
  methods learn the transitions of whatever was in the track as the layer opened: play a line in, open GEN, LEARN,
  pick MARKOV, ROLL for variations of your own line.
- **KNOB 1–4**: the selected track's four role macros (below). **PRESETS**: the drum style.
- **OCT+**: COMMIT. The render already stands in the steps; COMMIT writes GEN's parameter locks (within its budget)
  and hands the track to the hand editor (LIVE goes off). The header says `LOCKS 3/16`.
- **OCT−**: the track as the layer found it. **SAVE held**: undo, one step per action, as everywhere in Felucca.
- Every ROLL, MUTATE, PREV, NEXT and knob turn renders at once: what you hear is what COMMIT keeps.

## The pages: tap ARP past ARP 2

| Page | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
| --- | --- | --- | --- | --- |
| GEN ROLE | ROLE (OFF DRUMS BASS CHORDS LEAD) | STYLE | TO (the morph target style) | MORPH 0–100 % |
| GEN SHAPE | VARY: how far bar 4 strays, and MUTATE's share | DRIFT: LIVE's chance per phrase of moving a seed | ENRGY A–D: the current section's ENERGY | FILL: how much of the phrase end fills |
| HARMONY | ROOT | SCL (Felucca's 16 scales) | PROG (the progression) | RATE: a chord every 1, 2 or 4 bars |
| GEN METHOD | METHOD | P1 | P2 | P3 (see Methods) |
| GEN FORM | FORM (see Forms) | LEN HALF / NORM / LONG | GROW 1–4 | TAIL NONE / INTRO / PEEL |

A card that does not apply right now shows `--` and no bar: STYLE / TO / MORPH on a pitched track (styles are the
drums'), P1–P3 under a role's own method (STYLE, FOLLOW, VOICE, CELL take the macros instead), GROW unless the
form is GROWN, everything on an OFF track. The knobs that matter are the ones with a value.

**Pictures.** Every card and every layer key carries a glyph (Fukiai's icon font, which Felucca already uses):
dice for ROLL and VARY, a hit for RHYT and FILL, arrows for PREV / NEXT, a padlock for LOCK (open when the key would
unlock), motion for LIVE and DRIFT, a disc for SONG; the roles are a drum, a bass clef, a pad and a lead; the methods
a grid (STYLE), a circle (EUCLID), cells (CA), a loop (TURING), nodes (MARKOV), a warp (CHAOS), pink noise (PINK), a
link (CALL), a ladder (LSYS). The SEC A–D keys carry their section's ENERGY as a bar along the foot of the cell.

**The panel** under the cards is the same on every GEN page (so what ROLL did is seen without leaving the page):

- the four tracks, left to right T1–T4, each its role and method; the selected one framed; a dot for LIVE, a block
  for LOCK;
- the four sections' ENERGY as bars, the target section (the SEC key) in the theme ink;
- the selected track's phrase as it stands: 64 steps in four bars; on drums a hit's height is how many lanes it
  strikes, on a pitched track a note is tall and a tie low; the locks used at the right (`3/16`).

On GEN METHOD the sections' row gives way to the method drawn as itself: EUCLID a ring of n steps with k lit and
`E(k,n)`; CA a few generations of its rule from one cell; TURING the register as a ring of bits and whether FLIP
makes it a locked, drifting or random loop; MARKOV seven degree nodes joined by their likeliest moves (SEEDED or
LEARNED); CHAOS the logistic map's trace at the current R; PINK the dice rows; LSYS the string as steps up and down;
WALK a seeded path; POLY the lanes' cycle lengths; STYLE the style's weight grid. On HARMONY the panel is the scale
on two octaves of keys (the root in the theme ink) and the target section's four chords as roman degrees with their
root note. On GEN FORM the lower two rows give way to the form itself: a block per row, as wide as its bars, as tall as its
section's ENERGY, the DROP in the theme ink, PEEL rows hollow; under it the row and bar count.

Every key in the layer answers in the header (`ROLLED T2`, `NEW RHYTHM ALL`, `LOCKED T1`, `SECTION C: ENERGY 75`,
`KICK LANE LOCKED`), and a key that cannot act says why (`LOCKED: UNLOCK FIRST`, `NO HISTORY YET`, `BREED NEEDS A
ROLL`). The SONG key's dialog names the form and its length (`CLASSIC, 20 BARS`).

ROLE offers only what the engine can do: a DRUM-engine track is DRUMS or OFF, any other track BASS, CHORDS, LEAD or
OFF. Changing a role resets that track's macros to the role's defaults.

## Roles and their macros

Four roles, one per track. The defaults are T1 DRUMS, T2 BASS, T3 CHORDS, T4 LEAD.

**DRUMS** — DENS, SKIP, GHOST, ROLL. A style is a grid of 8 lanes by 32 steps of hit weights, with a timing bias per
lane; MORPH blends two styles. DENS scales every weight (and only ever adds or removes hits, never moves one, so
you can sweep it live). SKIP moves kick weight off the beat onto the off-beats (the 2-step skip). GHOST adds soft,
shifted copies of each lane. ROLL puts ratchets on the hats toward the fill, and triplet bursts on open hats and
rims. A style's anchors (weight 255: the one, the backbeat) always land. At most 3 lanes sound on one step.

Styles: **2-STEP** (skipped second kick, late snare, shuffled hats, ghost rims), **HALF-T** (snare on 3, sparse
syncopated kick, straight hats with rolls), **WITCH** (very sparse, long gaps, triplet bursts, a bell), **DNB**
(a broken 2-bar break), **DRONE** (a kick a bar and a bell), and three user slots.

**BASS** — FOLLOW, MOVE, GLIDE, LEN. FOLLOW is how strongly the bass lands on the DRUMS track's kicks (at 100 it
is the kick's shadow; at 0 it pulses on its own). MOVE adds fifths and octaves, and as the phrase's tension rises,
scale-step passing tones under the next chord's root. GLIDE is the chance of a slide into the note (an 808's).
LEN ties steps after each note. The note is always the harmony bus's chord root, in key.

**CHORDS** — SPREAD, COLOR, RHYTHM, STRUM. The harmony bus's chord, voiced from the previous voicing by moving each
note the least distance. COLOR: TRIAD, SUS, ADD9, 7TH. RHYTHM: PAD (held through the chord), HALF (every half bar),
STAB (off-beat stabs), ARP (one note a step, up). STRUM spreads the notes over the following steps. Never more than
4 notes on a step, and never more than the voice budget (below).

**LEAD** — CELL, VARY, REST, RANGE. A cell of 2–4 scale notes with its own durations; each bar plays it through a
transform (repeat, transpose to the chord, invert, truncate, reverse), VARY picking how far. REST thins it into
call-and-response. RANGE lets it climb an octave as tension rises, with a leap into the last bar. On a SAMPLE,
GRAIN or SLICE track the LEAD can be a **chop**: the notes are slices (one key per slice from middle C) and the
pitch is the track's, so a vocal-chop line spends no parameter lock per step. Chops get stutters (x2 ratchets).

## The harmony bus

One key for the whole project, on the HARMONY page. GEN pushes ROOT and SCALE into every pitched track's SCL page
on each render, so the parts agree; the SCL page still works by hand (the map marks the track `*` until the next
render). CHOP tracks and DRUM tracks are never pushed, and a CHOP track's QNT is set to OFF so slices are not snapped.

A progression is a weighted table of transitions between the scale's seven degrees, walked from the start of the
song, so bar 1 of VERSE follows the chord INTRO ended on. Chords are the scale's own triads, so every chord is
diatonic in any of the 16 scales. Factory tables: **i-VI-III** (VII), **i-iv-VI**, **i-VII-VI**, **SUSPEND** (stays
on i), **WANDER** (a free walk), **PHRYGIA** (i–bII), and two user slots.

**Tension** rises over the four bars of a phrase. The roles read it: fills lean in, the bass walks toward the next
chord, the lead leaps, the brightness lock opens.

## Phrases, sections, songs

A render is a 4-bar phrase (64 steps) in an A A A B shape: bar 4 re-decides VARY percent of its steps. FILL
decides how much of the phrase end fills (ratchets, accents, the dropped kick).

**SONG** renders four sections from one master seed into slots A–D: each track's seeds are derived from the master,
so the sections are recognisably the same piece under a different ENERGY and role mask:

| Slot | Section | ENERGY | Roles |
| --- | --- | --- | --- |
| A | INTRO | 25 % | CHORDS only |
| B | VERSE | 55 % | all |
| C | BUILD | 75 % | no BASS, DRUMS without its KICK |
| D | DROP | 100 % | all |

ENERGY scales density, rests and chord activity; it never changes seeds. The masks and ENERGY are per section and
yours to change (GEN SHAPE's ENRGY card follows the SEC key; the masks via the section keys and TEXTUR). The chain written
to the song mode is the **FORM** on the GEN FORM page (below); the default, CLASSIC, is A×2 B×4 C×2 D×4 B×2 D×4 A×2.

### Forms (the GEN FORM page)

| FORM | Sections (× bars) |
| --- | --- |
| CLASSIC | A×2 B×4 C×2 D×4 B×2 D×4 A×2 — in, verse, build, drop, again, out |
| POP | A×2 B×4 D×4 B×4 D×4 C×2 D×4 D×4 A×2 — verse / chorus / verse / chorus / bridge / double chorus |
| AABA | A×1 B×4 B×4 D×4 B×4 — the 32-bar song |
| RISE | A×4 C×4 D×8 B×4 C×4 D×8 — two builds into two drops, the second longer |
| ARCH | A×2 B×2 C×2 D×4 C×2 B×2 A×2 — up and back down |
| THROUGH | A×2 B×2 C×2 D×2 — each once |
| LOOP | D×16 — the drop only, for jamming |
| LONG | A×4 B×4 A×2 B×4 C×4 D×4 B×4 A×4 — slow, a Burial side: verses, one drop |
| GROWN | a form grown by a grammar (S → I M O; M → V C D, V D V C D or M M; V → V V; O → I, V I or D) rewritten GROW times from the master seed, so SONG with a new seed gives a new shape as well as new parts |
| CUSTOM | the chain stays as you built it in song mode; SONG only re-renders the four slots into it |

- **LEN** halves or doubles every row's repeats (HALF / NORM / LONG, clamped to 1–16 bars a row).
- **GROW** 1–4: the grammar's depth (GROWN only): 1 is three or four sections, 4 a long one.
- **TAIL**: NONE; INTRO adds a two-bar reprise of A at the end; PEEL adds the last section twice more, first without
  the LEAD, then without the LEAD and CHORDS (the song-mode "-" lanes), so the song falls apart rather than stops.
- A form longer than the chain's 16 rows loses rows from its middle, never its ends.

## Budgets: what keeps the FM-1 happy

- **Voices.** The FM-1's 8 voices are shared by the four tracks. GEN sizes every part to a share: BASS 1, LEAD 1,
  CHORDS 3, DRUMS 3. A role that is OFF or masked out of a section hands its voices to CHORDS (up to 4). The
  allocator's steal order is Felucca's; CHORDS' pad tails are the voices most likely to be stolen, by design.
- **Parameter locks.** Felucca has 128 automation / lock records shared by all four tracks. Per-step chance, ratchet,
  nudge, velocity, accent and slide are fields of the step itself and cost nothing; GEN leans on those. On COMMIT a
  pitched track writes at most its budget of lock records (LEAD 16, DRUMS 8, BASS 4, CHORDS 4; 32 of 128 at NORM),
  replaces only the ones it wrote itself, and drops its own lowest-priority locks first if the pool is short
  (`LOCKS FULL`). LIVE mode writes none.
- **Chords** are capped at 4 notes and 3 lanes may sound on one drum step.
- **CPU.** Generation runs on the UI side once per action or once per phrase, never in the audio ISR. The
  styles and progressions live in flash (1.7 KB); the working state is 388 bytes plus the user tables in the pool.

## Reproducibility

Every decision is a hash of (seed, track, lane, step) against a threshold. A part is its seeds and a few knobs,
so it is reproducible from 92 bytes, the knobs sweep smoothly, and sections share material by sharing seeds.
Three seeds per track: rhythm, pitch and variation. ROLL RHYTHM and ROLL PITCH each change one. The last 8 rolls
of each track are kept for PREV and NEXT.

## Storage

GEN's state per project slot lives in one new flash object (`OBJ_GEN`, A/B sectors 0xE8000 / 0xE9000, above
Felucca's FM6 voice bank, unused by stock Felucca). It is saved when a project is saved and loaded when one is
loaded; a slot with no state starts from the defaults. Projects themselves are unchanged. The web editor's full
backup does not yet include `OBJ_GEN`. The record is versioned (`gen_project_t.version`, now 3: the FORM fields were
added); a slot saved by an earlier GEN build starts from the defaults rather than being misread.

## Methods: more generators per role (GEN METHOD)

Every role has a METHOD (GEN METHOD, tap ARP past HARMONY): the generator it uses, with three parameters P1 P2 P3 that
change meaning with the method. Method 0 is always the one described under the role above. All of them keep the
budgets, the key and the lane locks; ROLL, MUTATE, PREV / NEXT, LIVE and SONG work the same whatever the method.

| Role | Method | What it does | P1 · P2 · P3 |
| --- | --- | --- | --- |
| DRUMS | STYLE | the style grid (above) | the style's; MORPH on GEN ROLE |
| DRUMS | EUCLID | Bjorklund rhythms per lane, k hits in n | HITS scales the counts · CYCLE 16 / 12 / 8 / 32 against the bar · SPREAD of the lanes' rotations (low: kick on the one, snare on the backbeat) |
| DRUMS | CA | an elementary cellular automaton row per lane per bar, hits where cells live | RULE 30 90 110 150 73 54 126 182 · SPAN bars one row lasts · THIN |
| DRUMS | POLY | each lane its own cycle (KICK 16 SNARE 16 CLAP 12 HATCL 6 HATOP 10 TOM 7 RIM 5 BELL 9) with Euclidean hits in it | HITS · EVEN pulls the odd cycles toward 16 · SPREAD |
| BASS | FOLLOW | lands on the DRUMS track's kicks (above) | FOLLOW MOVE GLIDE LEN on the macros |
| BASS | WALK | a random walk over the scale from the chord's degree, on a Euclidean rhythm | STEP size · DENS of the rhythm · HOME, the pull back to the root |
| BASS | ARP | the chord's tones in the bass octave, one a step | RATE 1 / 2 / 4 steps · SHAPE up, down, up-down, root-fifth · REACH an octave |
| BASS | DRONE | the root held and retriggered, octave jumps, a fifth at tension | RATE · OCT · FIFTH |
| CHORDS | VOICE | least-movement voicings, RHYTHM on the macros (above) | — |
| CHORDS | PULSE | the voicing as stabs on a Euclidean rhythm | HITS · TURN · — |
| CHORDS | ARP | the voicing broken one note a step | RATE · DIR up, down, up-down · GAPS |
| LEAD | CELL | a cell transformed across the phrase (above) | — |
| LEAD | WALK | a random walk, pulled toward chord tones | STEP · DENS · PULL |
| LEAD | EUCLID | a Euclidean rhythm with a melodic contour over the chord | HITS · TURN · SHAPE rise, fall, arch |
| LEAD | LSYS | an L-system string (A → AB, B → A and three other rule sets) rewritten P1 times, each letter a step up or down | GENS · RULES · HOLD |
| DRUMS, CHORDS, LEAD | TURING | a Turing-machine shift register per lane (DRUMS: 8 of them), a bit a step. FLIP is the chance the recirculating bit flips: 0 is a locked loop, 50 is noise, 100 flips every bit. LIVE runs the registers on, a cycle a bar. CHORDS: stabs on the register; LEAD: the top bits are the pitch, a bit the gate | FLIP · LEN 8 / 16 / 32 · FILL (DRUMS) / RANGE (LEAD) |
| BASS, LEAD | MARKOV | a Markov chain over the scale degrees. Its table is a seeded one that prefers small intervals, blended by LEARN with one learned from what you played into the track (black key 10 LEARN reads the track as the layer opened) | LEARN · DENS · REST |
| BASS, LEAD | CHAOS | the logistic map x' = r·x·(1 − x): R at the bottom (3.5) gives a cycle of 4, at the top (4.0) chaos; the value is the degree above the chord | R · DENS · RANGE |
| LEAD | PINK | 1/f pink dice (Voss–McCartney): DICE dice, die d re-rolled every 2^d steps, their sum the degree, so the line drifts over bars the way pink noise does | DICE 2–5 · DENS · RANGE |
| DRUMS, BASS, LEAD | CALL | cross-track logic: this track fires only where the reference track did (NOT low) or did not (NOT high). REF: the kick, the snare, any drum, the BASS, the LEAD. Call-and-response, a bass in the kick's gaps, a lead between the snares | REF · NOT · DENS |

**And on any method**, three more things from the same bag of tricks:

- **BREED** (black key 9): a child of this roll and the previous one, each bit of the seeds from one parent or the
  other, so the part keeps traits of both. It goes into the history like a ROLL, so PREV brings either parent back.
- **Trig conditions**: at VARY above 0 a varied quiet drum step (ghosts, the quiet lanes) takes one of Felucca's
  1.5.5 conditions (2:2, 1:3, 2:3, 3:3, 1:4, 2:4, 3:4) instead of a chance, so each pass of a committed pattern
  differs by design, not by dice; the pattern you see in EDIT shows them as Felucca does.
- **Render order**: DRUMS render first, then BASS, CHORDS, LEAD, so CALL always reads a reference that is already
  this phrase's.

The Euclidean form is the closed one, (i × k) mod n < k, which gives Bjorklund's pattern for every k and n with the
first hit on step 0 (E(3,8) = x..x..x.); `tests/gen_test.c` checks every (k, n) up to 32. CHOP tracks use the
methods with slices in place of pitches.

## The look: full-drum Risograph

The firmware starts in **RISO**, a palette and a style of its own, the first time it boots (MENU > DISPLAY > COLOR
and STYLE change it; Felucca's ten palettes and FLAT / LINE are all still there).

- **The palette** is a risograph print: paper ground, black text, a dense orange for values, pink for the one hot
  thing. It passes Felucca's 4.5:1 contrast gate on every pairing, which is why the two palette inks are denser than
  fluoro.
- **The style** draws every box as a printed one: a 2 px ink outline with a 1 px shadow in the next ink, a pixel off,
  the way a second drum misregisters. The four knob cards on every page print in their own inks (orange, blue, green,
  violet); the GEN layer's key rows do the same (orange rolls, blue state, teal sections, violet song); the drum lanes
  sit on a yellow block with green knockouts; lit cells are solid ink with paper text. Eight inks in all
  (`gfx.c UI_RISO_INK`).
- `docs/riso/` has the firmware's screens rendered in it. Every screen in every palette and style still passes the
  layout lint and the alignment check (`tests/ui_render.c`).

## Source

| File | What |
| --- | --- |
| `firmware/src/gen.c` | the generator: hashing, styles, progressions, the harmony bus, the four roles, locks, sections, seeds |
| `firmware/src/gen_ui.c` | the layer, the pages, LIVE, COMMIT, SONG, the flash object |
| `tests/gen_test.c` | the generator against its budgets; `build/host/gen_test DIR` also writes the four sections as MIDI files |
| `tests/ui_test.c` | `test_gen`: the layer, keys, COMMIT, SONG, pages and storage through the real panel code |

Hooks in Felucca's own files are each a few lines, marked `GEN`: `storage.c` (the object), `params.c` (the `SC_GEN`
page scope and the three pages), `ui.c` (the layer id, the dialog, the prototypes), `ui_layer.c` (the layer entry,
keys, knobs, map, OCT±), `ui_draw.c` and `ui_input.c` (the `SC_GEN` cards and knobs, the dialog, the ARP button),
`ui_help.c` (hints), `project.c` (save and load), `main.c` (init and the LIVE tick), `felucca.c` (the includes).

## Testing on the host

```
python3 tools/gen_tables.py build/gen/felucca_tables.h      # (and the other generators: see build.py generate())
cc -O1 -w -Ibuild/gen -Ifirmware/src -o build/host/gen_test tests/gen_test.c -lm
build/host/gen_test build/gen_demo        # the checks, and gen_section_A..D.mid to listen to
cc -O1 -w -Ibuild/gen -Ifirmware/src -o build/host/ui_test tests/ui_test.c -lm
build/host/ui_test                        # Felucca's UI suite with the GEN checks
```

`tests/run_tests.sh` runs both as part of the suite.

## Licence

GPL-3.0-only, as Felucca. GEN's own files: Copyright (C) 2026 GEN contributors. Felucca: Copyright (C) 2026 Leo
Kuroshita (@kurogedelic), Hügelton Instruments. M-VAVE and FM-1 are trademarks of their owners; this is not
affiliated with them.
