// Host-side harness for the chord mode voicer.
//
// Builds and runs on a normal Linux/macOS box - no Teensy, no Arduino. It
// exercises NuEVI/chordvoicer.cpp directly and reproduces the measurement
// tables in the "EWI Chord Mode" spec (sections 6-8), so the filters and the
// selector can be checked against their stated behaviour before any of this
// reaches hardware.
//
//   make -C tools && ./tools/chordtest

#include "../NuEVI/chordvoicer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failures = 0;

static void fail(const char *what, const char *detail) {
    printf("  FAIL  %s: %s\n", what, detail);
    g_failures++;
}

static const char *QNAME[CQ_COUNT] = {"dim7", "m7b5", "min7", "dom7", "maj7", "sus7"};
static const char *PC[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

static void formatVoicing(const ChordVoicing *v, char *out, size_t n) {
    size_t used = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < v->count; i++) {
        int note = v->notes[i];
        used += snprintf(out + used, n - used, "%s%s%d(%d)",
                         i ? " " : "",
                         (i < v->bassCount) ? "*" : "",
                         note, note);
        if (used >= n)
            return;
    }
}

static void nameVoicing(const ChordVoicing *v, char *out, size_t n) {
    size_t used = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < v->count; i++) {
        int note = v->notes[i];
        used += snprintf(out + used, n - used, "%s%s%s%d",
                         i ? " " : "", (i < v->bassCount) ? "*" : "",
                         PC[note % 12], note / 12 - 1);
        if (used >= n)
            return;
    }
}

// ------------------------------------------------------------ invariants

// The fifth of each quality, semitones above the root. Used by the doubling
// check: only the root and the fifth may be doubled (spec section 4).
static int fifthOf(ChordQuality q) {
    return (q == CQ_DIM7 || q == CQ_M7B5) ? 6 : 7;
}

static void checkVoicing(const ChordVoicing *v, uint8_t rootPc, ChordQuality q,
                         const ChordVoicerConfig *cfg, const char *ctx) {
    char buf[128];
    formatVoicing(v, buf, sizeof buf);
    char msg[256];

    if (v->count < 3 || v->count > CHORD_MAX_VOICES) {
        snprintf(msg, sizeof msg, "%s voice count %u [%s]", ctx, v->count, buf);
        fail("voice count", msg);
        return;
    }

    // Ascending order.
    for (uint8_t i = 1; i < v->count; i++) {
        if (v->notes[i] <= v->notes[i - 1]) {
            snprintf(msg, sizeof msg, "%s not ascending [%s]", ctx, buf);
            fail("ordering", msg);
            return;
        }
    }

    const uint8_t *up = v->notes + v->bassCount;
    uint8_t un = (uint8_t)(v->count - v->bassCount);

    if (up[0] < cfg->upperLo || up[un - 1] > cfg->upperHi) {
        snprintf(msg, sizeof msg, "%s upper structure outside [%u,%u] [%s]", ctx,
                 cfg->upperLo, cfg->upperHi, buf);
        fail("upper window", msg);
    }
    if (up[un - 1] < cfg->topLo || up[un - 1] > cfg->topHi) {
        snprintf(msg, sizeof msg, "%s top voice %u outside [%u,%u] [%s]", ctx,
                 up[un - 1], cfg->topLo, cfg->topHi, buf);
        fail("top voice window", msg);
    }
    if ((up[un - 1] - up[0]) > cfg->maxSpan) {
        snprintf(msg, sizeof msg, "%s span %d > %u [%s]", ctx, up[un - 1] - up[0],
                 cfg->maxSpan, buf);
        fail("span", msg);
    }
    if (v->bassCount) {
        if (v->notes[0] < cfg->bassLo || v->notes[0] > cfg->bassHi) {
            snprintf(msg, sizeof msg, "%s bass %u outside [%u,%u] [%s]", ctx,
                     v->notes[0], cfg->bassLo, cfg->bassHi, buf);
            fail("bass window", msg);
        }
        if ((v->notes[0] % 12) != rootPc) {
            snprintf(msg, sizeof msg, "%s bass is not the root [%s]", ctx, buf);
            fail("bass pitch", msg);
        }
    }

    // Low interval limits on adjacent pairs of the upper structure.
    for (uint8_t i = 0; i + 1 < un; i++) {
        uint8_t iv = (uint8_t)(up[i + 1] - up[i]);
        uint8_t limit = chordLowIntervalLimit(iv, cfg->lilOffset);
        if (limit && up[i] < limit) {
            snprintf(msg, sizeof msg, "%s interval %u at bottom note %u, limit %u [%s]",
                     ctx, iv, up[i], limit, buf);
            fail("low interval limit", msg);
        }
    }

    // Minor 9th rule across the whole upper structure.
    for (uint8_t i = 0; i < un; i++) {
        for (uint8_t j = i + 1; j < un; j++) {
            if ((up[j] - up[i]) != 13)
                continue;
            if (q == CQ_DOM7 && (up[i] % 12) == rootPc)
                continue;
            snprintf(msg, sizeof msg, "%s minor 9th %u->%u [%s]", ctx, up[i], up[j], buf);
            fail("minor 9th", msg);
        }
    }

    // Doubling: only the root and the fifth may appear twice. Never the 3rd,
    // especially the major 3rd of a dominant. Never the 7th.
    int seen[12] = {0};
    for (uint8_t i = 0; i < v->count; i++)
        seen[v->notes[i] % 12]++;
    for (int pc = 0; pc < 12; pc++) {
        if (seen[pc] < 2)
            continue;
        int degree = ((pc - rootPc) + 12) % 12;
        if (degree == 0 || degree == fifthOf(q))
            continue;
        snprintf(msg, sizeof msg, "%s doubled degree %d [%s]", ctx, degree, buf);
        fail("doubling", msg);
    }
}

// ------------------------------------------------------------------- tests

static void testInvariantsAndCoverage() {
    printf("\n== coverage and hard filters (spec sections 6-7) ==\n");

    for (int m = 0; m < 2; m++) {
        ChordVoicerConfig cfg;
        chordVoicerDefaults(&cfg);
        cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;
        const char *mode = m ? "without bass" : "with bass";

        int combos = 0, covered = 0, totalCands = 0;
        for (uint8_t root = 0; root < 12; root++) {
            for (int q = 0; q < CQ_COUNT; q++) {
                combos++;
                ChordVoicing cand[24];
                uint8_t n = chordCandidates(root, (ChordQuality)q, &cfg, cand, 24);
                if (n > 0)
                    covered++;
                totalCands += n;

                char ctx[64];
                snprintf(ctx, sizeof ctx, "%s %s/%s", mode, PC[root], QNAME[q]);
                for (uint8_t i = 0; i < n; i++)
                    checkVoicing(&cand[i], root, (ChordQuality)q, &cfg, ctx);

                if (n == 0) {
                    char msg[96];
                    snprintf(msg, sizeof msg, "%s produced no candidate", ctx);
                    fail("coverage", msg);
                }
            }
        }
        printf("  %-13s %d/%d combinations covered, %d candidates checked\n",
               mode, covered, combos, totalCands);
    }
}

static void testCandidateCounts() {
    printf("\n== candidate counts per root, after filtering (spec section 7) ==\n");
    printf("  %-6s  %-18s  %-18s\n", "", "with bass", "without bass");

    // Spec's measured table, for direct comparison.
    static const struct {
        const char *name;
        double withAvg;
        int withLo, withHi;
        double woAvg;
        int woLo, woHi;
    } SPEC[CQ_COUNT] = {
        {"dim7", 2.3, 2, 3, 2.3, 2, 3},
        {"m7b5", 2.8, 1, 6, 4.0, 2, 7},
        {"min7", 4.0, 3, 6, 5.1, 4, 8},
        {"dom7", 4.3, 3, 6, 5.5, 4, 8},
        {"maj7", 4.8, 4, 6, 6.0, 5, 8},
        {"sus7", 4.0, 2, 6, 5.2, 3, 7},
    };

    // Iterate qualities in the spec table's order, which is by density, not by
    // roller slot.
    static const ChordQuality ORDER[CQ_COUNT] = {CQ_DIM7, CQ_M7B5, CQ_MIN7,
                                                 CQ_DOM7, CQ_MAJ7, CQ_SUS7};

    for (int oi = 0; oi < CQ_COUNT; oi++) {
        ChordQuality q = ORDER[oi];
        double avg[2];
        int lo[2], hi[2];

        for (int m = 0; m < 2; m++) {
            ChordVoicerConfig cfg;
            chordVoicerDefaults(&cfg);
            cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;

            int total = 0;
            lo[m] = 999;
            hi[m] = 0;
            for (uint8_t root = 0; root < 12; root++) {
                ChordVoicing cand[24];
                int n = chordCandidates(root, q, &cfg, cand, 24);
                total += n;
                if (n < lo[m])
                    lo[m] = n;
                if (n > hi[m])
                    hi[m] = n;
            }
            avg[m] = total / 12.0;
        }

        printf("  %-6s  %.1f avg (%d-%-3d)     %.1f avg (%d-%-3d)   spec: %.1f (%d-%d) / %.1f (%d-%d)\n",
               QNAME[q], avg[0], lo[0], hi[0], avg[1], lo[1], hi[1],
               SPEC[oi].withAvg, SPEC[oi].withLo, SPEC[oi].withHi,
               SPEC[oi].woAvg, SPEC[oi].woLo, SPEC[oi].woHi);
    }
}

static void testTemperature() {
    printf("\n== top-voice motion vs temperature (spec section 8) ==\n");
    printf("  %-8s %-22s %s\n", "temp", "mean top-voice leap", "max leap");

    static const int TEMPS[] = {5, 20, 60, 200}; // tenths: 0.5, 2.0, 6.0, 20.0
    static const double SPEC_MEAN[] = {2.22, 2.27, 2.38, 2.84};
    static const int SPEC_MAX[] = {12, 12, 11, 12};

    const int STEPS = 4000;

    for (unsigned t = 0; t < sizeof TEMPS / sizeof TEMPS[0]; t++) {
        ChordVoicerConfig cfg;
        chordVoicerDefaults(&cfg);
        cfg.tempTenths = (uint8_t)TEMPS[t];

        // Same pseudo-random progression for every temperature so the only
        // variable is the selector.
        chordVoicerSeed(0xC0FFEEu);
        uint32_t walk = 0x9E3779B9u;

        ChordVoicing prev, cur;
        prev.count = 0;
        prev.bassCount = 0;
        long sum = 0;
        int maxLeap = 0, n = 0;

        for (int s = 0; s < STEPS; s++) {
            walk = walk * 1664525u + 1013904223u;
            uint8_t root = (uint8_t)((walk >> 16) % 12);
            ChordQuality q = (ChordQuality)((walk >> 24) % CQ_COUNT);

            chordVoice(root, q, &cfg, prev.count ? &prev : nullptr, &cur);

            if (prev.count) {
                int pTop = prev.notes[prev.count - 1];
                int cTop = cur.notes[cur.count - 1];
                int leap = pTop > cTop ? pTop - cTop : cTop - pTop;
                sum += leap;
                if (leap > maxLeap)
                    maxLeap = leap;
                n++;
            }
            prev = cur;
        }

        printf("  %-8.1f %-22.2f %-8d   spec: %.2f st, max %d\n",
               TEMPS[t] / 10.0, n ? (double)sum / n : 0.0, maxLeap,
               SPEC_MEAN[t], SPEC_MAX[t]);
    }
}

static void testProgression() {
    printf("\n== ii-V-I, voice led (roller slots 3 -> 4 -> 5) ==\n");

    struct Step {
        uint8_t root;
        ChordQuality q;
    };
    // Dm7 - G7 - Cmaj7, then Em7b5 - A7 - Dm7 (minor ii-V-i).
    static const Step PROG[] = {
        {2, CQ_MIN7}, {7, CQ_DOM7}, {0, CQ_MAJ7},
        {4, CQ_M7B5}, {9, CQ_DOM7}, {2, CQ_MIN7},
    };

    for (int m = 0; m < 2; m++) {
        ChordVoicerConfig cfg;
        chordVoicerDefaults(&cfg);
        cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;
        cfg.tempTenths = 0; // deterministic, so the printout is stable
        printf("  %s (* = bass)\n", m ? "without bass" : "with bass");

        ChordVoicing prev, cur;
        prev.count = 0;
        prev.bassCount = 0;

        for (unsigned i = 0; i < sizeof PROG / sizeof PROG[0]; i++) {
            bool ok = chordVoice(PROG[i].root, PROG[i].q, &cfg,
                                 prev.count ? &prev : nullptr, &cur);
            char names[128];
            nameVoicing(&cur, names, sizeof names);
            float cost = prev.count ? chordVoiceLeadingCost(&prev, &cur) : 0.0f;
            printf("    %-2s%-5s  %-28s  cost %7.1f%s\n", PC[PROG[i].root],
                   QNAME[PROG[i].q], names, cost, ok ? "" : "  (FALLBACK)");
            prev = cur;
        }
    }
}

static void testFallback() {
    printf("\n== shell fallback reachability (spec section 7) ==\n");

    // With the shipping windows the fallback should never be reached.
    ChordVoicerConfig cfg;
    chordVoicerDefaults(&cfg);
    int fellBack = 0;
    for (int m = 0; m < 2; m++) {
        cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;
        for (uint8_t root = 0; root < 12; root++)
            for (int q = 0; q < CQ_COUNT; q++) {
                ChordVoicing v;
                if (!chordVoice(root, (ChordQuality)q, &cfg, nullptr, &v))
                    fellBack++;
            }
    }
    printf("  shipping windows: %d/144 fell back (expected 0)\n", fellBack);
    if (fellBack)
        fail("fallback", "shipping windows should never reach the shell fallback");

    // Squeeze the top-voice window shut and the fallback must still produce a
    // sane shell rather than failing.
    cfg.topLo = 70;
    cfg.topHi = 70;
    cfg.upperLo = 70;
    cfg.upperHi = 70;
    int forced = 0;
    for (uint8_t root = 0; root < 12; root++)
        for (int q = 0; q < CQ_COUNT; q++) {
            ChordVoicing v;
            if (!chordVoice(root, (ChordQuality)q, &cfg, nullptr, &v))
                forced++;
            if (v.count < 2)
                fail("fallback", "shell voicing had fewer than 2 notes");
        }
    printf("  degenerate windows: %d/72 fell back, all produced a shell\n", forced);
}


// The firmware exposes CHORD LIL (-12..+12) and CHORD TOP (0..12 semitones of
// downward shift) in the menu. Every reachable setting must still voice all 144
// root x quality x mode combinations without dropping to the shell fallback,
// otherwise the menu range is advertising settings that silently degrade.
// Squeezing the windows is *meant* to make the shell fallback reachable (spec
// section 7); it stays musical, just plain. What the menu must not expose is a
// setting where most chords degrade.
#define FALLBACK_BUDGET 8

static void testSettingRanges() {
    printf("\n== menu setting ranges keep full coverage ==\n");

    printf("  CHORD LIL   ");
    int worstLil = 0, worstLilAt = 0;
    for (int off = -12; off <= 6; off++) { // CHORD LIL menu range
        int fell = 0;
        for (int m = 0; m < 2; m++) {
            ChordVoicerConfig cfg;
            chordVoicerDefaults(&cfg);
            cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;
            cfg.lilOffset = (int8_t)off;
            for (uint8_t root = 0; root < 12; root++)
                for (int q = 0; q < CQ_COUNT; q++) {
                    ChordVoicing v;
                    if (!chordVoice(root, (ChordQuality)q, &cfg, nullptr, &v))
                        fell++;
                }
        }
        if (fell > worstLil) {
            worstLil = fell;
            worstLilAt = off;
        }
    }
    printf("worst %d/144 fell back to a shell (at offset %+d)\n", worstLil, worstLilAt);
    if (worstLil > FALLBACK_BUDGET)
        fail("setting range", "CHORD LIL range reaches settings that mostly fall back");

    // Structural invariants must hold at every exposed shift, not just the
    // default - lowering the top band is what previously let the bass window
    // collide with the upper structure.
    for (int shift = 0; shift <= 5; shift++) {
        for (int m = 0; m < 2; m++) {
            ChordVoicerConfig cfg;
            chordVoicerDefaults(&cfg);
            cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;
            cfg.topLo = (uint8_t)(58 - shift);
            cfg.topHi = (uint8_t)(71 - shift);
            for (uint8_t root = 0; root < 12; root++)
                for (int q = 0; q < CQ_COUNT; q++) {
                    ChordVoicing cand[24];
                    uint8_t n = chordCandidates(root, (ChordQuality)q, &cfg, cand, 24);
                    char ctx[64];
                    snprintf(ctx, sizeof ctx, "shift -%d %s/%s", shift, PC[root], QNAME[q]);
                    for (uint8_t i = 0; i < n; i++)
                        checkVoicing(&cand[i], root, (ChordQuality)q, &cfg, ctx);
                }
        }
    }

    printf("  CHORD TOP   ");
    int worstTop = 0, worstTopAt = 0;
    for (int shift = 0; shift <= 5; shift++) { // CHORD TOP menu range
        int fell = 0;
        for (int m = 0; m < 2; m++) {
            ChordVoicerConfig cfg;
            chordVoicerDefaults(&cfg);
            cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;
            // Mirrors chordBuildConfig() in NuEVI.ino.
            cfg.topLo = (uint8_t)(58 - shift);
            cfg.topHi = (uint8_t)(71 - shift);
            for (uint8_t root = 0; root < 12; root++)
                for (int q = 0; q < CQ_COUNT; q++) {
                    ChordVoicing v;
                    if (!chordVoice(root, (ChordQuality)q, &cfg, nullptr, &v))
                        fell++;
                }
        }
        if (fell > worstTop) {
            worstTop = fell;
            worstTopAt = shift;
        }
    }
    printf("worst %d/144 fell back to a shell (at shift -%d)\n", worstTop, worstTopAt);
    if (worstTop > FALLBACK_BUDGET)
        fail("setting range", "CHORD TOP range reaches settings that mostly fall back");
}

// A roller move with the root unchanged should revoice, not retrigger: the
// firmware only sends note-offs and note-ons for voices that actually change.
// If most voices moved every time, "revoice in place" would buy nothing.
static void testRevoiceHoldsVoices() {
    printf("\n== revoicing on a roller move holds voices (spec section 9) ==\n");

    ChordVoicerConfig cfg;
    chordVoicerDefaults(&cfg);
    cfg.tempTenths = 0;

    int totalHeld = 0, totalVoices = 0, worstCase = 99;
    long topMotion = 0;
    int moves = 0, maxTopMotion = 0;
    for (uint8_t root = 0; root < 12; root++) {
        // Walk the roller across adjacent slots, which is what the slot ordering
        // in spec section 3 is optimised for.
        for (int q = 0; q + 1 < CQ_COUNT; q++) {
            ChordVoicing a, b;
            chordVoice(root, (ChordQuality)q, &cfg, nullptr, &a);
            chordVoice(root, (ChordQuality)(q + 1), &cfg, &a, &b);

            int held = 0;
            for (uint8_t i = 0; i < b.count; i++)
                for (uint8_t j = 0; j < a.count; j++)
                    if (b.notes[i] == a.notes[j]) {
                        held++;
                        break;
                    }
            totalHeld += held;
            totalVoices += b.count;
            if (held < worstCase)
                worstCase = held;

            int d = a.notes[a.count - 1] - b.notes[b.count - 1];
            if (d < 0)
                d = -d;
            topMotion += d;
            moves++;
            if (d > maxTopMotion)
                maxTopMotion = d;
        }
    }
    printf("  adjacent slot moves: %d/%d voices held (%.0f%%), worst case %d held\n",
           totalHeld, totalVoices, 100.0 * totalHeld / totalVoices, worstCase);
    printf("  top voice motion:    %.2f st mean, %d max\n",
           (double)topMotion / moves, maxTopMotion);

    // Held-voice count is an outcome of the weights, not a target - the top
    // voice is weighted 6x precisely because it is the audible line, and it
    // will outrank holding a third inner voice. That line is what must stay
    // still on a roller move.
    if ((double)topMotion / moves > 3.0)
        fail("revoicing", "top voice wanders on an adjacent roller move");
    if (totalHeld == 0)
        fail("revoicing", "no voices at all survive a roller move");
}


// ---------------------------------------------------------- player tests
//
// Every bug the review found in chord mode was a sequencing bug in the held-
// voice bookkeeping, not a voicing bug. These drive ChordPlayer through the
// exact sequences that broke, and assert the one invariant that matters on a
// MIDI wire: every note-on is eventually matched by a note-off, and nothing is
// turned off that was not on.

struct NoteLog {
    int held[128];   // net on-count per note number
    int spuriousOff; // note-off for a note that was not sounding
    int doubleOn;    // note-on for a note already sounding
    int ons, offs;
};

static void logInit(NoteLog *l) {
    for (int i = 0; i < 128; i++)
        l->held[i] = 0;
    l->spuriousOff = l->doubleOn = l->ons = l->offs = 0;
}

static void logOn(uint8_t note, uint8_t vel, void *user) {
    NoteLog *l = (NoteLog *)user;
    (void)vel;
    if (l->held[note])
        l->doubleOn++;
    l->held[note]++;
    l->ons++;
}

static void logOff(uint8_t note, void *user) {
    NoteLog *l = (NoteLog *)user;
    if (!l->held[note])
        l->spuriousOff++;
    else
        l->held[note]--;
    l->offs++;
}

static int stillHeld(const NoteLog *l) {
    int n = 0;
    for (int i = 0; i < 128; i++)
        n += l->held[i];
    return n;
}

static void checkLog(const NoteLog *l, const char *what, int expectHeld) {
    char msg[192];
    if (l->spuriousOff) {
        snprintf(msg, sizeof msg, "%s: %d note-off(s) for notes that were not sounding", what, l->spuriousOff);
        fail("note balance", msg);
    }
    if (l->doubleOn) {
        snprintf(msg, sizeof msg, "%s: %d note-on(s) for notes already sounding", what, l->doubleOn);
        fail("note balance", msg);
    }
    if (stillHeld(l) != expectHeld) {
        snprintf(msg, sizeof msg, "%s: %d notes still held, expected %d", what, stillHeld(l), expectHeld);
        fail("note balance", msg);
    }
}

static void testPlayerBalance() {
    printf("\n== player note balance (every on matched by an off) ==\n");

    ChordVoicerConfig cfg;
    chordVoicerDefaults(&cfg);

    // 1. A long pseudo-random performance: attacks, roller-only revoices, and
    //    releases, in both bass modes.
    for (int m = 0; m < 2; m++) {
        cfg.bassMode = m ? CHORD_BASS_SELF : CHORD_BASS_EXTERNAL;
        chordVoicerSeed(0x5EED0001u);

        NoteLog log;
        logInit(&log);
        ChordPlayer p;
        chordPlayerInit(&p);

        uint32_t w = 0x12345678u;
        uint8_t root = 0;
        ChordQuality q = CQ_DOM7;
        bool sounding = false;

        for (int step = 0; step < 6000; step++) {
            w = w * 1664525u + 1013904223u;
            int action = (w >> 16) % 10;

            if (!sounding) {
                root = (uint8_t)((w >> 8) % 12);
                q = (ChordQuality)((w >> 20) % CQ_COUNT);
                chordPlayerStart(&p, root, q, 100, &cfg, logOn, logOff, &log);
                sounding = true;
            } else if (action < 4) {
                // roller only - revoice in place
                q = (ChordQuality)((w >> 20) % CQ_COUNT);
                if (chordPlayerWouldChange(&p, root, q))
                    chordPlayerRevoice(&p, root, q, 100, &cfg, logOn, logOff, &log);
            } else if (action < 7) {
                // new root while blowing - full retrigger
                root = (uint8_t)((w >> 8) % 12);
                if (chordPlayerWouldChange(&p, root, q))
                    chordPlayerStart(&p, root, q, 100, &cfg, logOn, logOff, &log);
            } else {
                chordPlayerStop(&p, logOff, &log);
                sounding = false;
            }
        }
        chordPlayerStop(&p, logOff, &log);
        checkLog(&log, m ? "without bass performance" : "with bass performance", 0);
        printf("  %-13s %d note-ons, %d note-offs, 0 held at end\n",
               m ? "without bass" : "with bass", log.ons, log.offs);
    }

    // 2. Panic mid-chord. midiPanic silences everything behind the player's
    //    back; chordPlayerForget must drop the record so the next revoice does
    //    not suppress the note-ons for voices panic already killed.
    {
        cfg.bassMode = CHORD_BASS_EXTERNAL;
        NoteLog log;
        logInit(&log);
        ChordPlayer p;
        chordPlayerInit(&p);

        chordPlayerStart(&p, 2, CQ_MIN7, 100, &cfg, logOn, logOff, &log);
        // simulate an all-notes-off: everything is silent now
        for (int i = 0; i < 128; i++)
            log.held[i] = 0;
        chordPlayerForget(&p);

        chordPlayerStart(&p, 2, CQ_MIN7, 100, &cfg, logOn, logOff, &log);
        if (stillHeld(&log) == 0)
            fail("panic recovery", "no voices sounding after re-attack following a panic");
        printf("  panic recovery: re-attack sounded %d voices\n", stillHeld(&log));
        chordPlayerStop(&p, logOff, &log);
        checkLog(&log, "panic recovery", 0);
    }

    // 3. Stop then immediately replay the SAME root and quality. If stop left
    //    the edge-detector keys populated, this is silently treated as "no
    //    change" and nothing sounds.
    {
        NoteLog log;
        logInit(&log);
        ChordPlayer p;
        chordPlayerInit(&p);

        chordPlayerStart(&p, 7, CQ_DOM7, 100, &cfg, logOn, logOff, &log);
        chordPlayerStop(&p, logOff, &log);
        if (!chordPlayerWouldChange(&p, 7, CQ_DOM7))
            fail("stop invalidates", "replaying the same chord after a stop reads as no change");
        chordPlayerStart(&p, 7, CQ_DOM7, 100, &cfg, logOn, logOff, &log);
        printf("  replay after stop: %d voices sounded\n", stillHeld(&log));
        if (stillHeld(&log) == 0)
            fail("stop invalidates", "replaying the same chord after a stop sounded nothing");
        chordPlayerStop(&p, logOff, &log);
        checkLog(&log, "replay after stop", 0);
    }

    // 4. A fresh player must not think anything is already sounding.
    {
        ChordPlayer p;
        chordPlayerInit(&p);
        if (!chordPlayerWouldChange(&p, 0, CQ_DIM7))
            fail("init", "a freshly initialised player reports root 0 / dim7 as already active");
        printf("  fresh player: reports no active chord\n");
    }

    // 5. Revoicing across every quality at every root, checking balance.
    {
        NoteLog log;
        logInit(&log);
        ChordPlayer p;
        chordPlayerInit(&p);
        for (uint8_t root = 0; root < 12; root++) {
            chordPlayerStart(&p, root, CQ_DIM7, 100, &cfg, logOn, logOff, &log);
            for (int q = 1; q < CQ_COUNT; q++)
                chordPlayerRevoice(&p, root, (ChordQuality)q, 100, &cfg, logOn, logOff, &log);
            chordPlayerStop(&p, logOff, &log);
        }
        checkLog(&log, "exhaustive revoice sweep", 0);
        printf("  exhaustive revoice sweep: %d ons / %d offs, balanced\n", log.ons, log.offs);
    }
}

// Simple chord mode (cfg.simple): the plain R-3-5-7 chord tones in close
// position. First chord root position; after that the inversion/octave that
// moves the voices least, within Bb2..C5.
static int simpleMove(const ChordVoicing *a, const ChordVoicing *b) {
    int cost = 0;
    for (int i = 0; i < 4; i++)
        cost += abs((int)b->notes[i] - (int)a->notes[(i * a->count) / 4]);
    return cost;
}

static void testSimpleMode() {
    printf("\nSimple chord mode\n");
    static const uint8_t IV[CQ_COUNT][4] = {
        {0, 3, 6, 9}, {0, 3, 6, 10}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 4, 7, 11}, {0, 5, 7, 10}};
    ChordVoicerConfig cfg;
    chordVoicerDefaults(&cfg);
    cfg.simple = 1;
    cfg.bassMode = CHORD_BASS_SELF; // must be ignored
    cfg.lilOffset = 6;              // must be ignored
    char got[64], what[64];

    // 1. First chord (nothing to lead from) is root position, root in C3-B3.
    for (uint8_t root = 0; root < 12; root++)
        for (int q = 0; q < CQ_COUNT; q++) {
            ChordVoicing v;
            chordVoice(root, (ChordQuality)q, &cfg, nullptr, &v);
            bool ok = v.count == 4 && v.bassCount == 0;
            for (int i = 0; ok && i < 4; i++)
                ok = v.notes[i] == 48 + root + IV[q][i];
            if (!ok) {
                formatVoicing(&v, got, sizeof got);
                snprintf(what, sizeof what, "simple first %s %s", PC[root], QNAME[q]);
                fail(what, got);
            }
        }

    // 2. Every (previous, next) pair: right tones, close position, in range,
    //    and no other legal shape of that chord moves less.
    int pairs = 0;
    for (uint8_t r0 = 0; r0 < 12; r0++)
        for (int q0 = 0; q0 < CQ_COUNT; q0++) {
            ChordVoicing a;
            chordVoice(r0, (ChordQuality)q0, &cfg, nullptr, &a);
            for (uint8_t r1 = 0; r1 < 12; r1++)
                for (int q1 = 0; q1 < CQ_COUNT; q1++) {
                    ChordVoicing b;
                    chordVoice(r1, (ChordQuality)q1, &cfg, &a, &b);
                    pairs++;
                    bool ok = b.count == 4 && b.notes[0] >= 46 && b.notes[0] <= 61 &&
                              b.notes[3] - b.notes[0] < 12;
                    for (int i = 1; ok && i < 4; i++)
                        ok = b.notes[i] > b.notes[i - 1];
                    for (int i = 0; ok && i < 4; i++) {
                        bool isTone = false;
                        for (int k = 0; k < 4; k++)
                            isTone |= (b.notes[i] % 12) == (r1 + IV[q1][k]) % 12;
                        ok = isTone;
                    }
                    // brute force every close-position shape with bottom in range
                    int bestCost = 1 << 30;
                    for (int bottom = 46; bottom <= 61; bottom++)
                        for (int inv = 0; inv < 4; inv++) {
                            if (bottom % 12 != (r1 + IV[q1][inv]) % 12)
                                continue;
                            ChordVoicing c;
                            c.count = 4;
                            c.bassCount = 0;
                            int n = bottom;
                            for (int i = 0; i < 4; i++) {
                                int pc = (r1 + IV[q1][(inv + i) % 4]) % 12;
                                while (n % 12 != pc)
                                    n++;
                                c.notes[i] = (uint8_t)n;
                                n++;
                            }
                            int cost = simpleMove(&a, &c);
                            if (cost < bestCost)
                                bestCost = cost;
                        }
                    if (!ok || simpleMove(&a, &b) != bestCost) {
                        formatVoicing(&b, got, sizeof got);
                        snprintf(what, sizeof what, "simple %s%s -> %s%s", PC[r0], QNAME[q0], PC[r1], QNAME[q1]);
                        fail(what, got);
                    }
                }
        }

    // 3. Major ii-V-I from a root-position Dm7 stays close.
    {
        ChordVoicing dm, g7, cm;
        chordVoice(2, CQ_MIN7, &cfg, nullptr, &dm);
        chordVoice(7, CQ_DOM7, &cfg, &dm, &g7);
        chordVoice(0, CQ_MAJ7, &cfg, &g7, &cm);
        int m1 = simpleMove(&dm, &g7), m2 = simpleMove(&g7, &cm);
        if (m1 > 4 || m2 > 4)
            fail("simple ii-V-I", "a step moved more than 4 semitones in total");
        printf("  ii-V-I moves %d then %d semitones in total\n", m1, m2);
    }

    // 4. The player voice-leads across a release (tongue / breath) too.
    {
        ChordPlayer p;
        chordPlayerInit(&p);
        NoteLog log;
        logInit(&log);
        chordPlayerStart(&p, 0, CQ_MIN7, 100, &cfg, logOn, logOff, &log); // C E- G Bb, root pos
        ChordVoicing cm = p.sounding;
        chordPlayerStop(&p, logOff, &log);
        chordPlayerStart(&p, 5, CQ_DOM7, 100, &cfg, logOn, logOff, &log);
        ChordVoicing want;
        chordVoice(5, CQ_DOM7, &cfg, &cm, &want);
        bool same = p.sounding.count == want.count;
        for (int i = 0; same && i < want.count; i++)
            same = p.sounding.notes[i] == want.notes[i];
        if (!same || p.sounding.notes[0] == 53)
            fail("simple across release", "F7 after Cm7 should be led from Cm7, not reset to root position");
        // roller move min7 -> dom7 on C moves exactly one voice
        chordPlayerStop(&p, logOff, &log);
        chordPlayerInit(&p);
        chordPlayerStart(&p, 0, CQ_MIN7, 100, &cfg, logOn, logOff, &log);
        int onsBefore = log.ons, offsBefore = log.offs;
        chordPlayerRevoice(&p, 0, CQ_DOM7, 100, &cfg, logOn, logOff, &log);
        if (log.ons - onsBefore != 1 || log.offs - offsBefore != 1)
            fail("simple revoice", "min7->dom7 should move exactly one voice");
        chordPlayerStop(&p, logOff, &log);
        checkLog(&log, "simple player", 0);
    }

    // 5. Full chord mode also leads across a release (deterministic at temp 0).
    {
        ChordVoicerConfig full;
        chordVoicerDefaults(&full);
        full.tempTenths = 0;
        ChordPlayer p;
        chordPlayerInit(&p);
        NoteLog log;
        logInit(&log);
        chordPlayerStart(&p, 2, CQ_MIN7, 100, &full, logOn, logOff, &log);
        ChordVoicing dm = p.sounding;
        chordPlayerStop(&p, logOff, &log);
        chordPlayerStart(&p, 7, CQ_DOM7, 100, &full, logOn, logOff, &log);
        ChordVoicing want;
        chordVoice(7, CQ_DOM7, &full, &dm, &want);
        bool same = p.sounding.count == want.count;
        for (int i = 0; same && i < want.count; i++)
            same = p.sounding.notes[i] == want.notes[i];
        if (!same)
            fail("full across release", "G7 after a released Dm7 was not voice-led from it");
        chordPlayerStop(&p, logOff, &log);
        checkLog(&log, "full player across release", 0);
    }
    printf("  %d chord-to-chord moves are the shortest legal shape\n", pairs);
}

int main() {
    printf("NuEVI chord mode voicer - host harness\n");
    printf("=======================================\n");

    testInvariantsAndCoverage();
    testCandidateCounts();
    testProgression();
    testTemperature();
    testFallback();
    testSettingRanges();
    testRevoiceHoldsVoices();
    testPlayerBalance();
    testSimpleMode();

    printf("\n");
    if (g_failures) {
        printf("FAILED: %d problem(s)\n", g_failures);
        return 1;
    }
    printf("All invariant checks passed.\n");
    return 0;
}
