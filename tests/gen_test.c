/* SPDX-License-Identifier: GPL-3.0-only
 * GEN (src/gen.c) on the host, through the same sources as hostsim: the generator against its budgets.
 *   build/host/gen_test [MIDIDIR]
 * 1. determinism: the same seeds render the same steps, twice; a different seed renders different steps.
 * 2. stability: raising DRUMS DENSITY only adds hits, never moves one (monotone in the knob).
 * 3. budgets (fixes 1 and 4): no step holds more than 4 notes; CHORDS never more than its voice share; DRUMS never
 *    more than 3 lanes a step; a COMMIT's motion records stay within gen_lock_budget and only on steps GEN marked.
 * 4. harmony (fix 3): every BASS / CHORDS / pitched LEAD note is in the project's scale; P_ROOT / P_SCALE pushed.
 * 5. roles: BASS lands on the DRUMS track's kicks at FOLLOW 100; CHOP LEAD notes are slices 60..75, no locks per step.
 * 6. sections: INTRO leaves the masked tracks silent; DROP has more hits than INTRO; sections share seeds.
 * 7. history: ROLL then PREV brings the previous render back exactly; NEXT re-does it.
 * 8. MIDI: with MIDIDIR, each section as a 4-track .mid (120 BPM) to listen to. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include "../firmware/src/gen.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t hits_of(const track_t *t) { uint32_t s, n = 0; for (s = 0; s < NSTEP; s++) if (t->step[s].time == ST_NOTE) n += t->step[s].hit ? __builtin_popcount(t->step[s].hit) : t->step[s].n; return n; }
static uint32_t hash_steps(const track_t *t) { uint32_t s, h = 2166136261u; const uint8_t *b = (const uint8_t *)t->step; for (s = 0; s < sizeof t->step; s++) { h ^= b[s]; h *= 16777619u; } return h; }
static int in_scale(uint32_t note) { return (SCALE_MASK[gen.p.scale] >> ((note + 120u - gen.p.root) % 12u)) & 1u; }

static void setup(void)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    for (i = 0; i < NTRK; i++) trk[i].eng_req = trk[i].engine = (uint8_t)(i == 0u ? ENGI_DRUM : 0u);
    memset(&motion, 0, sizeof motion);
    gen_init();
    gen_song_seed(0x1234u);
}

static void write_var(FILE *f, uint32_t v) { uint8_t b[4]; int n = 0; b[n++] = v & 0x7F; while (v >>= 7) b[n++] = 0x80 | (v & 0x7F); while (n--) fputc(b[n], f); }
static void midi_section(const char *dir, uint32_t sec)
{
    char path[256]; FILE *f; uint32_t ti, s, i;
    snprintf(path, sizeof path, "%s/gen_section_%c.mid", dir, 'A' + sec);
    f = fopen(path, "wb"); if (!f) return;
    fwrite("MThd\0\0\0\6\0\1\0\5\0\x60", 1, 14, f);  /* format 1, 5 tracks, 96 ppq */
    { uint8_t tempo[] = {'M','T','r','k',0,0,0,11, 0,0xFF,0x51,3,0x07,0xA1,0x20, 0,0xFF,0x2F,0}; fwrite(tempo, 1, sizeof tempo, f); }
    for (ti = 0; ti < NTRK; ti++) {
        long at = ftell(f), end; uint32_t len; uint8_t ch = ti == 0u ? 9u : (uint8_t)ti;
        fwrite("MTrk\0\0\0\0", 1, 8, f);
        for (s = 0; s < NSTEP; s++) {
            const step_t *st = &trk[ti].step[s]; uint32_t dur = 1;
            if (st->time != ST_NOTE) { continue; }
            while (s + dur < NSTEP && trk[ti].step[s + dur].time == ST_TIE) dur++;
            write_var(f, s == 0u ? 0u : 0u);
            if (st->hit) {
                for (i = 0; i < NLANE; i++) if ((st->hit >> i) & 1u) { fputc(0x90 | ch, f); fputc(DRUM_LANE_NOTE[i], f); fputc((st->acc >> i) & 1u ? 120 : st->vel, f); write_var(f, 0); }
                write_var(f, 24u * dur); for (i = 0; i < NLANE; i++) if ((st->hit >> i) & 1u) { fputc(0x80 | ch, f); fputc(DRUM_LANE_NOTE[i], f); fputc(0, f); write_var(f, 0); }
            } else {
                for (i = 0; i < st->n; i++) { fputc(0x90 | ch, f); fputc(st->note[i], f); fputc(st->vel, f); write_var(f, 0); }
                write_var(f, 24u * dur); for (i = 0; i < st->n; i++) { fputc(0x80 | ch, f); fputc(st->note[i], f); fputc(0, f); write_var(f, 0); }
            }
            /* account the time: pad rests between events */
            s += dur - 1u;
            { uint32_t gap = 0, k = s + 1u; while (k < NSTEP && trk[ti].step[k].time != ST_NOTE) { gap++; k++; } if (gap) { write_var(f, 24u * gap); fputc(0xFF, f); fputc(0x01, f); write_var(f, 0); } }
        }
        fputc(0, f); fputc(0xFF, f); fputc(0x2F, f); fputc(0, f);
        end = ftell(f); len = (uint32_t)(end - at - 8); fseek(f, at + 4, SEEK_SET);
        fputc(len >> 24, f); fputc(len >> 16, f); fputc(len >> 8, f); fputc(len, f); fseek(f, end, SEEK_SET);
    }
    fclose(f);
    printf("  wrote %s\n", path);
}

int main(int argc, char **argv)
{
    uint32_t h1, h2, s, i, sec, hits_intro, hits_drop;
    /* 1. determinism */
    setup(); gen_render_section(GS_DROP); h1 = hash_steps(&trk[0]) ^ hash_steps(&trk[1]) ^ hash_steps(&trk[3]);
    gen_render_section(GS_DROP); h2 = hash_steps(&trk[0]) ^ hash_steps(&trk[1]) ^ hash_steps(&trk[3]);
    CHECK(h1 == h2, "same seeds, different render");
    gen.t[0].seed_rhythm ^= 0xABCDu; gen_render_section(GS_DROP);
    CHECK(h1 != (hash_steps(&trk[0]) ^ hash_steps(&trk[1]) ^ hash_steps(&trk[3])), "a new seed rendered the same");
    /* 2. DENSITY is monotone */
    setup();
    { uint64_t prev = 0; uint32_t d;
      for (d = 0; d <= 100u; d += 10u) { uint64_t m = 0; gen.t[0].macro[0] = (uint8_t)d; gen_render_track(0, GS_DROP);
          for (s = 0; s < NSTEP; s++) if (trk[0].step[s].time == ST_NOTE && (trk[0].step[s].hit & 1u)) m |= 1ull << s;
          CHECK((m & prev) == prev, "DENSITY %u moved a kick", d); prev = m; } }
    /* 3. budgets */
    setup();
    for (sec = 0; sec < GEN_NSEC; sec++) {
        gen_render_section(sec);
        for (i = 0; i < NTRK; i++) for (s = 0; s < NSTEP; s++) {
            const step_t *st = &trk[i].step[s];
            CHECK(st->n <= 4u, "T%u step %u: %u notes", i + 1u, s, st->n);
            if (gen.t[i].role == GR_CHORDS) CHECK(st->n <= gen_voices_for(GR_CHORDS), "CHORDS %u notes over %u voices (sec %u)", st->n, gen_voices_for(GR_CHORDS), sec);
            if (gen.t[i].role == GR_DRUMS) CHECK(__builtin_popcount(st->hit) <= 3, "DRUMS step %u: %d lanes", s, __builtin_popcount(st->hit));
            if (gen.t[i].role == GR_BASS || gen.t[i].role == GR_LEAD) CHECK(st->n <= 1u, "mono role with %u notes", st->n);
        }
    }
    for (i = 1; i < NTRK; i++) { uint32_t r = gen_locks_write(&trk[i], &gen.t[i], i); CHECK((r & 0xFFu) <= gen_lock_budget(gen.t[i].role), "T%u locks %u over budget", i + 1u, r & 0xFFu); CHECK(!(r & 0x100u), "pool full"); }
    { uint32_t n = 0; for (i = 0; i < motion.count; i++) n++; CHECK(n <= 32u, "%u records after a full COMMIT (32 max at NORM)", n); }
    for (i = 1; i < NTRK; i++) gen_locks_clear(&trk[i], i);
    CHECK(motion.count == 0u, "locks not cleared: %u", motion.count);
    /* 4. harmony */
    setup(); gen_render_section(GS_DROP);
    for (i = 1; i < NTRK; i++) {
        CHECK(trk[i].p[P_ROOT] == gen.p.root && trk[i].p[P_SCALE] == gen.p.scale, "T%u key not pushed", i + 1u);
        for (s = 0; s < NSTEP; s++) if (trk[i].step[s].time == ST_NOTE) for (h1 = 0; h1 < trk[i].step[s].n; h1++)
            CHECK(in_scale(trk[i].step[s].note[h1]), "T%u step %u note %u out of key", i + 1u, s, trk[i].step[s].note[h1]);
    }
    /* 5. roles */
    setup(); gen.t[1].macro[0] = 100; gen_render_section(GS_DROP);
    { uint32_t on = 0, off = 0; for (s = 0; s < NSTEP; s++) if (trk[1].step[s].time == ST_NOTE) { if (trk[0].step[s].time == ST_NOTE && (trk[0].step[s].hit & 1u)) on++; else off++; }
      CHECK(on > off, "FOLLOW 100: %u on kicks, %u off", on, off); }
    setup(); trk[3].eng_req = trk[3].engine = ENGI_SAMPLE; gen.t[3].flags |= GF_CHOP; gen_render_section(GS_DROP);
    for (s = 0; s < NSTEP; s++) if (trk[3].step[s].time == ST_NOTE) CHECK(trk[3].step[s].note[0] >= 60u && trk[3].step[s].note[0] < 76u, "CHOP note %u", trk[3].step[s].note[0]);
    CHECK(trk[3].p[P_QUANT] == QN_OFF, "CHOP track still quantized");
    /* 6. sections */
    setup(); gen_render_section(GS_INTRO); hits_intro = hits_of(&trk[0]) + hits_of(&trk[1]) + hits_of(&trk[3]);
    CHECK(hits_of(&trk[0]) == 0u && hits_of(&trk[1]) == 0u, "INTRO: drums or bass sounding");
    gen_render_section(GS_DROP); hits_drop = hits_of(&trk[0]) + hits_of(&trk[1]) + hits_of(&trk[3]);
    CHECK(hits_drop > hits_intro, "DROP %u not busier than INTRO %u", hits_drop, hits_intro);
    /* 7. history */
    setup(); gen_render_track(0, GS_DROP); h1 = hash_steps(&trk[0]);
    gen_roll(0, 1, 1); gen_render_track(0, GS_DROP); h2 = hash_steps(&trk[0]); CHECK(h1 != h2, "ROLL changed nothing");
    gen_hist_step(0, -1); gen_render_track(0, GS_DROP); CHECK(hash_steps(&trk[0]) == h1, "PREV did not bring the roll back");
    gen_hist_step(0, 1); gen_render_track(0, GS_DROP); CHECK(hash_steps(&trk[0]) == h2, "NEXT did not redo");
    /* 9. the other generators: Euclid's counts, every method of every role within the budgets and in key */
    { uint32_t k, n2, c, tot = 0;
      for (n2 = 1; n2 <= 32u; n2++) for (k = 0; k <= n2; k++) { c = 0; for (s = 0; s < n2; s++) c += gen_euclid(k, n2, 3u, s) != 0; CHECK(c == k, "euclid(%u,%u): %u hits", k, n2, c); }
      CHECK(gen_euclid(3u, 8u, 0u, 0) && gen_euclid(3u, 8u, 0u, 3) && gen_euclid(3u, 8u, 0u, 6), "E(3,8) is not x..x..x.");
      for (i = 0; i < 4u; i++) { uint32_t m, nm = i == 0u ? GMD_N : i == 1u ? GMB_N : i == 2u ? GMC_N : GML_N;
        for (m = 1; m < nm; m++) { setup(); gen.t[i].method = (uint8_t)m; gen.t[i].mp[0] = 60; gen.t[i].mp[1] = 40; gen.t[i].mp[2] = 50; gen_render_section(GS_DROP);
          CHECK(hits_of(&trk[i]) > 0u, "role %u method %u rendered nothing", i, m); tot += hits_of(&trk[i]);
          for (s = 0; s < NSTEP; s++) { const step_t *st = &trk[i].step[s];
            CHECK(st->n <= 4u && (i == 0u ? __builtin_popcount(st->hit) <= 3 : i == 2u ? st->n <= gen_voices_for(GR_CHORDS) : st->n <= 1u), "role %u method %u step %u over budget", i, m, s);
            if (i && st->time == ST_NOTE) for (h1 = 0; h1 < st->n; h1++) CHECK(in_scale(st->note[h1]), "role %u method %u note %u out of key", i, m, st->note[h1]); } } }
      CHECK(tot > 60u, "the methods are thin: %u hits in all", tot);
      setup(); gen.t[0].method = GMD_CA; gen_render_track(0, GS_DROP); h1 = hash_steps(&trk[0]); gen_render_track(0, GS_DROP); CHECK(h1 == hash_steps(&trk[0]), "CA is not deterministic");
      setup(); gen.t[3].method = GML_LSYS; gen.t[3].mp[0] = 100; gen_render_track(3, GS_DROP); CHECK(hits_of(&trk[3]) >= 8u, "LSYS at full depth: %u notes", hits_of(&trk[3])); }
    /* 10. the chat's techniques: a Turing register at FLIP 0 is a locked loop and flips at 50; CALL answers the drums
     *     (fires only where the kick did not); MARKOV learns a played-in track; BREED keeps bits of both parents;
     *     varied quiet drum steps take trig conditions */
    { uint32_t r0 = gen_turing(77u, 0, 16u, 0u, 0), r1 = gen_turing(77u, 0, 16u, 0u, 5), r2 = gen_turing(77u, 0, 16u, 128u, 5), bits = 0;
      CHECK(r0 == r1, "TURING at FLIP 0 changed: %08x -> %08x", r0, r1);
      CHECK(r0 != r2, "TURING at FLIP 50 never flipped");
      for (i = 0; i < 4u; i++) { uint32_t m = i == 0u ? GMD_TURING : i == 2u ? GMC_TURING : i == 3u ? GML_TURING : 0u; if (!m) continue;
        setup(); gen.t[i].method = (uint8_t)m; gen.t[i].mp[0] = 0; gen.t[i].mp[1] = 0; gen.t[i].variation = 0; gen.t[i].macro[2] = 0; gen_render_track(i, GS_DROP);
        for (s = 0; s < 8u; s++) CHECK(trk[i].step[s].time == trk[i].step[s + 8u].time && trk[i].step[s].hit == trk[i].step[s + 8u].hit, "role %u TURING LEN 8 FLIP 0 is not an 8-step loop at step %u", i, s); }
      setup(); gen.t[1].method = GMB_CALL; gen.t[1].mp[0] = 0; gen.t[1].mp[1] = 100; gen.t[1].mp[2] = 100; gen_render_section(GS_DROP);
      for (s = 0; s < NSTEP; s++) if (trk[1].step[s].time == ST_NOTE) { bits++; CHECK(!(trk[0].step[s].time == ST_NOTE && (trk[0].step[s].hit & 1u)), "BASS CALL NOT KICK fired with the kick at step %u", s); }
      CHECK(bits > 4u, "BASS CALL rendered %u notes", bits);
      setup(); for (s = 0; s < NSTEP; s++) { gen_step_clear(&trk[3].step[s]); if (s % 4u == 0u) { trk[3].step[s].time = ST_NOTE; trk[3].step[s].n = 1; trk[3].step[s].note[0] = (uint8_t)(48u + gen.p.root + (s / 4u % 2u ? 7u : 0u)); } }
      gen_markov_learn(3, trk[3].step);
      CHECK(gen_learned[3].n[0][4] > 0u && gen_learned[3].n[4][0] > 0u && gen_learned[3].n[0][1] == 0u, "LEARN did not pick up the root <-> fifth alternation");
      gen.t[3].method = GML_MARKOV; gen.t[3].mp[0] = 100; gen.t[3].mp[1] = 60; gen.t[3].mp[2] = 0; gen_render_track(3, GS_DROP);
      for (s = 0, bits = 0; s < NSTEP; s++) if (trk[3].step[s].time == ST_NOTE) { uint32_t pc = (trk[3].step[s].note[0] + 120u - gen.p.root) % 12u; bits++; CHECK(pc == 0u || pc == 7u, "learned MARKOV at LEARN 100 left the learned degrees: pc %u", pc); }
      CHECK(bits > 4u, "learned MARKOV rendered %u notes", bits);
      setup(); { uint32_t a = gen.t[3].seed_rhythm; gen_roll(3, 1, 1); { uint32_t b = gen.t[3].seed_rhythm; gen_breed(3); CHECK(gen.t[3].seed_rhythm != a && gen.t[3].seed_rhythm != b && ((gen.t[3].seed_rhythm ^ a) & (gen.t[3].seed_rhythm ^ b)) == 0u, "BREED is not a crossover of the two parents"); } }
      setup(); gen.t[0].variation = 100; for (sec = 0, bits = 0; sec < GEN_NSEC; sec++) { gen_render_track(0, sec); for (s = 0; s < NSTEP; s++) if (step_cond(&trk[0].step[s])) bits++; }
      CHECK(bits > 0u, "no trig conditions at full VARIATION"); }
    /* 11. FORM: every form a valid chain (1..16 rows, sections A-D, repeats 1..16, every track on the row's slot),
     *     LEN halves / doubles it, TAIL REPRISE ends on A, PEEL silences the LEAD then CHORDS, GRAMMAR depends on
     *     the seed and GROW and is deterministic, CUSTOM leaves the chain as it was */
    { chain_config_t c, c2; uint32_t f, r, bars, bars2, bars3;
      setup();
      for (f = 0; f < GFM_N; f++) { gen.p.form = (uint8_t)f; gen.p.song_len = 1; gen.p.tail = 0; memset(&c, 0, sizeof c);
        if (f == GFM_CUSTOM) { c.count = 3; CHECK(gen_chain_write(&c) == 0u && c.count == 3u, "CUSTOM rewrote the chain"); continue; }
        CHECK(gen_chain_write(&c) >= 1u && c.count >= 1u && c.count <= CHAIN_ROWS, "form %s: %u rows", gen_form_name(f), c.count);
        for (r = 0; r < c.count; r++) { CHECK(c.row[r].repeat >= 1u && c.row[r].repeat <= 16u, "form %s row %u repeats %u", gen_form_name(f), r, c.row[r].repeat);
          for (i = 0; i < NTRK; i++) CHECK(c.row[r].slot[i] < GEN_NSEC && c.row[r].slot[i] == c.row[r].slot[0], "form %s row %u track %u slot %u", gen_form_name(f), r, i, c.row[r].slot[i]); } }
      gen.p.form = GFM_CLASSIC; gen.p.song_len = 1; gen_chain_write(&c); for (r = 0, bars = 0; r < c.count; r++) bars += c.row[r].repeat;
      gen.p.song_len = 0; gen_chain_write(&c); for (r = 0, bars2 = 0; r < c.count; r++) bars2 += c.row[r].repeat;
      gen.p.song_len = 2; gen_chain_write(&c); for (r = 0, bars3 = 0; r < c.count; r++) bars3 += c.row[r].repeat;
      CHECK(bars == 20u && bars2 == 10u && bars3 == 40u, "LEN: %u %u %u bars", bars2, bars, bars3);
      gen.p.song_len = 1; gen.p.tail = 1; gen_chain_write(&c); CHECK(c.row[c.count - 1u].slot[0] == GS_INTRO && c.count == 8u, "REPRISE did not add an INTRO row");
      gen.p.tail = 2; gen_chain_write(&c); CHECK(c.count == 9u && c.row[7].slot[3] == CHAIN_SILENT && c.row[7].slot[2] != CHAIN_SILENT && c.row[8].slot[3] == CHAIN_SILENT && c.row[8].slot[2] == CHAIN_SILENT && c.row[8].slot[0] == GS_INTRO, "PEEL: LEAD then CHORDS silent on the last section");
      gen.p.tail = 0; gen.p.form = GFM_GRAMMAR; gen.p.grow = 2; gen.p.master_seed = 11; gen_chain_write(&c); gen_chain_write(&c2);
      CHECK(memcmp(&c, &c2, sizeof c) == 0, "GRAMMAR is not deterministic");
      CHECK(c.row[0].slot[0] == GS_INTRO && c.count >= 3u, "GRAMMAR does not open with the INTRO (%u rows)", c.count);
      for (f = 12, bars = 0; f < 44u; f++) { gen.p.master_seed = f; gen_chain_write(&c2); if (memcmp(&c, &c2, sizeof c)) bars++; }
      CHECK(bars > 16u, "GRAMMAR: only %u of 32 seeds differ", bars);
      gen.p.master_seed = 11; gen.p.grow = 0; gen_chain_write(&c2); gen.p.grow = 3; gen_chain_write(&c); CHECK(c.count >= c2.count, "GROW 4 shorter than GROW 1");
      gen.p.form = GFM_LOOP; gen.p.song_len = 2; gen_chain_write(&c); CHECK(c.count == 1u && c.row[0].repeat == 16u, "LOOP LONG: repeats clamp to 16"); }
    /* 8. MIDI */
    if (argc > 1) { setup(); for (sec = 0; sec < GEN_NSEC; sec++) { gen_render_section(sec); midi_section(argv[1], sec); } }
    /* a picture of the DROP for the log */
    setup(); gen_render_section(GS_DROP);
    for (i = 0; i < NTRK; i++) {
        printf("T%u %-6s ", i + 1u, i == 0u ? "DRUMS" : i == 1u ? "BASS" : i == 2u ? "CHORDS" : "LEAD");
        for (s = 0; s < NSTEP; s++) { const step_t *st = &trk[i].step[s]; putchar(st->time == ST_TIE ? '-' : st->time != ST_NOTE ? '.' : st->hit ? ((st->hit & 1u) ? 'K' : (st->hit & 2u) ? 'S' : 'h') : st->n > 1u ? '#' : 'o'); if (s % 16u == 15u) putchar(' '); }
        putchar('\n');
    }
    printf(fails ? "gen_test: %d FAILED\n" : "gen_test: ok\n", fails);
    return fails != 0;
}
