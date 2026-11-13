#include "chordvoicer.h"

#include <math.h>

// ---------------------------------------------------------------- templates
//
// Semitones above the chord ROOT (spec section 7). Every template is rootless -
// none contains 0 - so CHORD_BASS_EXTERNAL uses them as-is and CHORD_BASS_SELF
// just adds a bass note underneath.
//
// A and B forms alternate by inversion so the voicer can stay inside the window
// as roots move. Unused slots are -1.

#define TPL_MAX 4

static const int8_t TPL_A[CQ_COUNT][TPL_MAX] = {
    {3, 6, 9, -1},   // dim7  b3 b5 bb7
    {3, 6, 10, 17},  // m7b5  b3 b5 b7 11
    {3, 7, 10, 14},  // min7  b3 5 b7 9
    {4, 7, 10, 14},  // dom7  3 5 b7 9
    {4, 7, 11, 14},  // maj7  3 5 7 9
    {5, 10, 14, 19}, // sus7  4 b7 9 13
};

static const int8_t TPL_B[CQ_COUNT][TPL_MAX] = {
    {6, 9, 15, -1},   // dim7
    {6, 10, 15, 17},  // m7b5
    {10, 14, 15, 19}, // min7  b7 9 b3 5
    {10, 14, 16, 21}, // dom7  b7 9 3 13
    {11, 14, 16, 19}, // maj7  7 9 3 5
    {7, 10, 14, 17},  // sus7
};

// Three-note reduced forms. Only offered in CHORD_BASS_SELF, where the root is
// already sounding underneath.
static const int8_t TPL_R[CQ_COUNT][3] = {
    {3, 6, 9},   // dim7
    {3, 6, 10},  // m7b5
    {3, 10, 14}, // min7
    {4, 10, 14}, // dom7
    {4, 11, 14}, // maj7
    {5, 10, 14}, // sus7
};

// Guide tones for the fallback shell voicing (spec section 7).
static const int8_t GUIDE[CQ_COUNT][2] = {
    {3, 9},  // dim7  b3 bb7
    {3, 10}, // m7b5  b3 b7
    {3, 10}, // min7  b3 b7
    {4, 10}, // dom7  3 b7
    {4, 11}, // maj7  3 7
    {5, 10}, // sus7  4 b7
};

// ------------------------------------------------------- low interval limits
//
// Lowest permissible bottom note per interval, MIDI numbers, from acoustic
// orchestration practice (spec section 6). Index is the interval in semitones.
// Index 0 is unused; intervals above an octave are unconstrained.
static const uint8_t LIL[13] = {
    0,  // 0  unison - not generated
    52, // 1  m2  E3
    51, // 2  M2  Eb3
    48, // 3  m3  C3
    46, // 4  M3  Bb2
    41, // 5  P4  F2
    41, // 6  TT  F2
    34, // 7  P5  Bb1
    43, // 8  m6  G2
    41, // 9  M6  F2
    41, // 10 m7  F2
    41, // 11 M7  F2
    28, // 12 P8  E1
};

uint8_t chordLowIntervalLimit(uint8_t interval, int8_t lilOffset) {
    if (interval == 0 || interval > 12)
        return 0;
    int v = (int)LIL[interval] + lilOffset;
    if (v < 0)
        v = 0;
    if (v > 127)
        v = 127;
    return (uint8_t)v;
}

// ------------------------------------------------------------------ scoring
//
// Spec section 8. Top voice dominates because with no melody line, whatever
// ends up on top becomes the perceived tune.
#define W_TOP_VOICE 6.0f
#define W_DENSITY 4.0f
#define W_COMMON_TONE 3.0f
#define W_REGISTER 0.5f
#define REGISTER_CENTER 63.0f

static inline int iabs(int v) { return v < 0 ? -v : v; }

// --------------------------------------------------------------------- rng
// xorshift32. Seeded from micros() on the firmware; fixed in the harness so
// the measurement tables are reproducible.
static uint32_t s_rng = 0x2545F491u;

void chordVoicerSeed(uint32_t seed) {
    s_rng = seed ? seed : 0x2545F491u;
}

static uint32_t rngNext() {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float rngUnit() {
    return (float)(rngNext() >> 8) / (float)(1u << 24);
}

// ------------------------------------------------------------------ helpers

void chordVoicerDefaults(ChordVoicerConfig *cfg) {
    cfg->bassMode = CHORD_BASS_EXTERNAL;
    cfg->lilOffset = 0;
    cfg->upperLo = 48; // C3
    cfg->upperHi = 72; // C5
    cfg->topLo = 58;   // Bb3
    cfg->topHi = 71;   // B4
    cfg->bassLo = 36;  // C2
    cfg->bassHi = 47;  // B2
    cfg->maxSpan = 24;
    cfg->tempTenths = 20; // 2.0
    cfg->simple = 0;
}

// Simple chord mode: plain root-position seventh chords, R 3 5 7 stacked from
// the root. Intervals in semitones above the root, one row per ChordQuality.
static const uint8_t SIMPLE_CHORD[CQ_COUNT][4] = {
    {0, 3, 6, 9},  // dim7
    {0, 3, 6, 10}, // m7b5
    {0, 3, 7, 10}, // min7
    {0, 4, 7, 10}, // dom7
    {0, 4, 7, 11}, // maj7
    {0, 5, 7, 10}, // sus7 (4 replaces 3)
};
#define SIMPLE_ROOT_LO 48   // first chord: root position, root in C3-B3
#define SIMPLE_BOTTOM_LO 46 // later chords: lowest note Bb2..C#4, so the whole
#define SIMPLE_BOTTOM_HI 61 // chord stays within Bb2..C5 and can never drift away

// Simple chord mode. Always exactly the four chord tones in close position
// (all within an octave), but the inversion and octave are chosen to move the
// voices as little as possible from the previous chord - total semitones moved,
// voice by voice from the bottom - the way a comping keyboard player connects
// chords. Ties go to the shape nearest the middle of the range, then to the one
// with the lower bottom note, so the choice is always deterministic.
static void simpleVoicing(uint8_t rootPc, ChordQuality quality, const ChordVoicing *prev,
                          ChordVoicing *out) {
    rootPc %= 12;
    out->count = 4;
    out->bassCount = 0;
    if (!prev || prev->count == 0) {
        const uint8_t root = (uint8_t)(SIMPLE_ROOT_LO + rootPc);
        for (uint8_t i = 0; i < 4; i++)
            out->notes[i] = (uint8_t)(root + SIMPLE_CHORD[quality][i]);
        return;
    }
    int best = -1;
    int bestCost = 0, bestReg = 0;
    uint8_t cand[4]; // seeded with root position; always replaced (every chord has a shape in range)
    for (uint8_t i = 0; i < 4; i++)
        cand[i] = (uint8_t)(SIMPLE_ROOT_LO + rootPc + SIMPLE_CHORD[quality][i]);
    for (uint8_t inv = 0; inv < 4; inv++) {
        // Close position starting on chord tone `inv`: each tone above the last.
        int pcs[4];
        for (uint8_t i = 0; i < 4; i++)
            pcs[i] = (rootPc + SIMPLE_CHORD[quality][(inv + i) % 4]) % 12;
        for (int bottom = SIMPLE_BOTTOM_LO; bottom <= SIMPLE_BOTTOM_HI; bottom++) {
            if (bottom % 12 != pcs[0])
                continue;
            int n[4];
            n[0] = bottom;
            for (uint8_t i = 1; i < 4; i++) {
                n[i] = n[i - 1] + (pcs[i] - n[i - 1] % 12 + 12) % 12;
                if (n[i] == n[i - 1])
                    n[i] += 12;
            }
            int cost = 0;
            for (uint8_t i = 0; i < 4; i++) {
                // Compare voice i with the previous chord's voice i, mapping when
                // the previous chord had a different voice count.
                const uint8_t pi = (uint8_t)((i * prev->count) / 4);
                cost += iabs(n[i] - (int)prev->notes[pi]);
            }
            const int reg = iabs((n[0] + n[3]) - 2 * (int)REGISTER_CENTER);
            if (best < 0 || cost < bestCost || (cost == bestCost && reg < bestReg)) {
                best = inv;
                bestCost = cost;
                bestReg = reg;
                for (uint8_t i = 0; i < 4; i++)
                    cand[i] = (uint8_t)n[i];
            }
        }
    }
    for (uint8_t i = 0; i < 4; i++)
        out->notes[i] = cand[i];
}

static void sortAscending(uint8_t *a, uint8_t n) {
    for (uint8_t i = 1; i < n; i++) {
        uint8_t v = a[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

// Section 6 hard filters, applied to the upper structure only.
static bool upperPasses(const uint8_t *notes, uint8_t n, const ChordVoicerConfig *cfg,
                        ChordQuality quality, uint8_t rootPc) {
    if (n < 3)
        return false;

    if (notes[0] < cfg->upperLo || notes[n - 1] > cfg->upperHi)
        return false;
    if (notes[n - 1] < cfg->topLo || notes[n - 1] > cfg->topHi)
        return false;
    if ((notes[n - 1] - notes[0]) > cfg->maxSpan)
        return false;

    // Low interval limits: each adjacent pair's bottom note must sit at or above
    // the limit for that interval.
    for (uint8_t i = 0; i + 1 < n; i++) {
        uint8_t iv = notes[i + 1] - notes[i];
        if (iv == 0)
            return false; // no unison doubling inside the upper structure
        uint8_t limit = chordLowIntervalLimit(iv, cfg->lilOffset);
        if (limit && notes[i] < limit)
            return false;
    }

    // Minor 9th rule: no two voices 13 semitones apart, with one exception -
    // a b9 above the root of a dominant chord. Our dominant templates carry no
    // b9 (spec section 5 excludes alterations), so the exception is currently
    // unreachable; it is implemented so the rule stays correct if altered
    // dominants are ever added.
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if ((notes[j] - notes[i]) != 13)
                continue;
            bool lowerIsRoot = ((notes[i] % 12) == rootPc);
            if (quality == CQ_DOM7 && lowerIsRoot)
                continue; // permitted b9 over a dominant root
            return false;
        }
    }
    return true;
}

// Expand one template into close / drop 2 / drop 3 at one octave placement.
// Writes up to 3 candidate note-sets. `offs` must be ascending.
static uint8_t expandForm(const int8_t *offs, uint8_t n, int rootMidi,
                          uint8_t out[3][CHORD_MAX_VOICES], uint8_t *outN) {
    uint8_t made = 0;

    for (uint8_t variant = 0; variant < 3; variant++) {
        if (variant == 1 && n < 4)
            continue; // drop 2 is defined on four-part close harmony
        if (variant == 2 && n < 4)
            continue; // drop 3 needs four voices

        int tmp[CHORD_MAX_VOICES];
        for (uint8_t i = 0; i < n; i++)
            tmp[i] = rootMidi + offs[i];

        // tmp is ascending, so index n-1 is the top. Drop 2 lowers the second
        // voice from the top; drop 3 lowers the third from the top.
        if (variant == 1)
            tmp[n - 2] -= 12;
        else if (variant == 2)
            tmp[n - 3] -= 12;

        bool inRange = true;
        for (uint8_t i = 0; i < n; i++) {
            if (tmp[i] < 0 || tmp[i] > 127) {
                inRange = false;
                break;
            }
        }
        if (!inRange)
            continue;

        for (uint8_t i = 0; i < n; i++)
            out[made][i] = (uint8_t)tmp[i];
        sortAscending(out[made], n);
        outN[made] = n;
        made++;
    }
    return made;
}

static bool sameNotes(const uint8_t *a, uint8_t an, const uint8_t *b, uint8_t bn) {
    if (an != bn)
        return false;
    for (uint8_t i = 0; i < an; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

uint8_t chordCandidates(uint8_t rootPc, ChordQuality quality, const ChordVoicerConfig *cfg,
                        ChordVoicing *out, uint8_t maxOut) {
    if (quality >= CQ_COUNT || maxOut == 0)
        return 0;
    rootPc %= 12;

    const bool self = (cfg->bassMode == CHORD_BASS_SELF);
    uint8_t count = 0;

    // Form 0 = A, 1 = B, 2 = reduced (CHORD_BASS_SELF only).
    for (uint8_t form = 0; form < (self ? 3 : 2); form++) {
        int8_t offs[TPL_MAX];
        uint8_t n = 0;

        if (form == 2) {
            for (uint8_t i = 0; i < 3; i++)
                offs[n++] = TPL_R[quality][i];
        } else {
            const int8_t *src = (form == 0) ? TPL_A[quality] : TPL_B[quality];
            for (uint8_t i = 0; i < TPL_MAX; i++)
                if (src[i] >= 0)
                    offs[n++] = src[i];
        }

        // Every octave placement of the root that could put the structure in
        // the window. The upper window is at most two octaves, so scanning
        // rootPc + 12k over the full MIDI range is cheap and exhaustive.
        for (int rootMidi = rootPc; rootMidi <= 108; rootMidi += 12) {
            uint8_t sets[3][CHORD_MAX_VOICES];
            uint8_t setN[3];
            uint8_t made = expandForm(offs, n, rootMidi, sets, setN);

            for (uint8_t s = 0; s < made; s++) {
                if (!upperPasses(sets[s], setN[s], cfg, quality, rootPc))
                    continue;

                bool dup = false;
                for (uint8_t c = 0; c < count && !dup; c++) {
                    const uint8_t *ex = out[c].notes + out[c].bassCount;
                    dup = sameNotes(ex, (uint8_t)(out[c].count - out[c].bassCount),
                                    sets[s], setN[s]);
                }
                if (dup)
                    continue;

                ChordVoicing *v = &out[count];
                v->count = 0;
                v->bassCount = 0;
                if (self) {
                    // Deterministic given the root, so it contributes a constant
                    // to voice-leading cost and is excluded from scoring.
                    int bass = cfg->bassLo + ((rootPc - (cfg->bassLo % 12) + 12) % 12);
                    if (bass > cfg->bassHi)
                        bass -= 12;

                    // Belt and braces: the bass must actually be the bottom of
                    // the chord. Callers are expected to keep the bass and
                    // upper windows disjoint, but the ascending contract in
                    // chordvoicer.h must hold even if one does not.
                    while (bass >= sets[s][0])
                        bass -= 12;
                    if (bass < 12)
                        continue; // no room left underneath

                    v->notes[v->count++] = (uint8_t)bass;
                    v->bassCount = 1;
                }
                for (uint8_t i = 0; i < setN[s]; i++)
                    v->notes[v->count++] = sets[s][i];

                if (++count >= maxOut)
                    return count;
            }
        }
    }
    return count;
}

float chordVoiceLeadingCost(const ChordVoicing *prev, const ChordVoicing *cur) {
    if (!prev || prev->count == 0)
        return 0.0f;

    // Upper structures only - the bass is deterministic given the root.
    const uint8_t *pu = prev->notes + prev->bassCount;
    uint8_t pn = (uint8_t)(prev->count - prev->bassCount);
    const uint8_t *cu = cur->notes + cur->bassCount;
    uint8_t cn = (uint8_t)(cur->count - cur->bassCount);
    if (pn == 0 || cn == 0)
        return 0.0f;

    // Descending, so index 0 is the top voice.
    int p[CHORD_MAX_VOICES], c[CHORD_MAX_VOICES];
    for (uint8_t i = 0; i < pn; i++)
        p[i] = pu[pn - 1 - i];
    for (uint8_t i = 0; i < cn; i++)
        c[i] = cu[cn - 1 - i];

    uint8_t n = (pn < cn) ? pn : cn;

    float cost = 0.0f;
    for (uint8_t i = 1; i < n; i++)
        cost += (float)iabs(p[i] - c[i]);

    cost += W_TOP_VOICE * (float)iabs(p[0] - c[0]);
    cost += W_DENSITY * (float)iabs((int)pn - (int)cn);

    // Holding a voice completely still is qualitatively better than moving it a
    // semitone, which a linear motion penalty does not capture.
    uint8_t common = 0;
    for (uint8_t i = 0; i < pn; i++)
        for (uint8_t j = 0; j < cn; j++)
            if (pu[i] == cu[j]) {
                common++;
                break;
            }
    cost -= W_COMMON_TONE * (float)common;

    return cost;
}

// Register pull toward REGISTER_CENTER. Declared alongside the voice-leading
// weights in the spec but absent from its cost snippet, so it is applied here
// as a separate additive term rather than folded into the function above.
static float registerCost(const ChordVoicing *v) {
    if (v->count == v->bassCount)
        return 0.0f;
    int sum = 0;
    for (uint8_t i = v->bassCount; i < v->count; i++)
        sum += v->notes[i];
    float mean = (float)sum / (float)(v->count - v->bassCount);
    float d = mean - REGISTER_CENTER;
    if (d < 0.0f)
        d = -d;
    return W_REGISTER * d;
}

// Root plus guide tones, or guide tones alone with a bassist. Never fails,
// never muddy. Currently unreachable, but tightening the windows could make it
// reachable (spec section 7).
static void shellFallback(uint8_t rootPc, ChordQuality quality, const ChordVoicerConfig *cfg,
                          ChordVoicing *out) {
    out->count = 0;
    out->bassCount = 0;

    if (cfg->bassMode == CHORD_BASS_SELF) {
        int bass = cfg->bassLo + ((rootPc - (cfg->bassLo % 12) + 12) % 12);
        if (bass > cfg->bassHi)
            bass -= 12;
        out->notes[out->count++] = (uint8_t)bass;
        out->bassCount = 1;
    }

    // Place the lower guide tone in the bottom half of the upper window, then
    // stack the second above it.
    int base = cfg->upperLo + ((rootPc - (cfg->upperLo % 12) + 12) % 12);
    int g0 = base + GUIDE[quality][0];
    int g1 = base + GUIDE[quality][1];
    while (g1 > cfg->upperHi && g0 - 12 >= cfg->upperLo) {
        g0 -= 12;
        g1 -= 12;
    }
    if (g0 < 0)
        g0 = cfg->upperLo;
    if (g1 > 127)
        g1 = cfg->upperHi;

    out->notes[out->count++] = (uint8_t)g0;
    out->notes[out->count++] = (uint8_t)g1;
    sortAscending(out->notes + out->bassCount, (uint8_t)(out->count - out->bassCount));
}

#define MAX_CANDIDATES 24

bool chordVoice(uint8_t rootPc, ChordQuality quality, const ChordVoicerConfig *cfg,
                const ChordVoicing *prev, ChordVoicing *out) {
    if (cfg->simple) {
        simpleVoicing(rootPc, quality < CQ_COUNT ? quality : CQ_DOM7, prev, out);
        return true;
    }
    if (quality >= CQ_COUNT) {
        shellFallback(rootPc % 12, CQ_DOM7, cfg, out);
        return false;
    }
    rootPc %= 12;

    ChordVoicing cand[MAX_CANDIDATES];
    uint8_t n = chordCandidates(rootPc, quality, cfg, cand, MAX_CANDIDATES);

    if (n == 0) {
        shellFallback(rootPc, quality, cfg, out);
        return false;
    }

    float cost[MAX_CANDIDATES];
    float best = 0.0f;
    uint8_t bestIdx = 0;
    for (uint8_t i = 0; i < n; i++) {
        cost[i] = chordVoiceLeadingCost(prev, &cand[i]) + registerCost(&cand[i]);
        if (i == 0 || cost[i] < best) {
            best = cost[i];
            bestIdx = i;
        }
    }

    if (cfg->tempTenths == 0 || n == 1) {
        *out = cand[bestIdx];
        return true;
    }

    // Softmax over negated cost. Measured, temperature barely moves the result
    // because candidate sets are only 2-8 members after filtering; that is a
    // legitimate outcome for a sustaining comping instrument, not a bug.
    float temp = (float)cfg->tempTenths / 10.0f;
    float w[MAX_CANDIDATES];
    float total = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        w[i] = expf(-(cost[i] - best) / temp);
        total += w[i];
    }

    float r = rngUnit() * total;
    for (uint8_t i = 0; i < n; i++) {
        r -= w[i];
        if (r <= 0.0f) {
            *out = cand[i];
            return true;
        }
    }
    *out = cand[bestIdx];
    return true;
}

// ------------------------------------------------------------------ player

// 256 / sqrt(n) for n = 0..CHORD_MAX_VOICES. An n-note voicing at a given breath
// level is roughly n times the acoustic energy of a single line (spec section 9).
// Integer so this stays cheap on a Teensy 3.2, which has no FPU.
static const uint16_t INV_SQRT[CHORD_MAX_VOICES + 1] = {256, 256, 181, 148, 128, 114};

static uint8_t chordScaledVelocity(uint8_t velocity, uint8_t voices) {
    if (voices < 2)
        return velocity;
    if (voices > CHORD_MAX_VOICES)
        voices = CHORD_MAX_VOICES;
    int v = ((int)velocity * INV_SQRT[voices]) >> 8;
    if (v < 1)
        v = 1;
    if (v > 127)
        v = 127;
    return (uint8_t)v;
}

// Release bookkeeping without touching p->last (see ChordPlayer::last).
static void playerRelease(ChordPlayer *p) {
    if (p->sounding.count)
        p->last = p->sounding;
    p->sounding.count = 0;
    p->sounding.bassCount = 0;
    p->activeRootPc = CHORD_NONE;
    p->activeQuality = CHORD_NONE;
}

void chordPlayerInit(ChordPlayer *p) {
    p->sounding.count = 0;
    p->sounding.bassCount = 0;
    p->last.count = 0;
    p->last.bassCount = 0;
    p->activeRootPc = CHORD_NONE;
    p->activeQuality = CHORD_NONE;
}

// What the next chord should voice-lead from: the chord still sounding, else
// the last one played. Without the fallback every fresh attack (tongued or
// re-breathed) voiced from nothing and each root landed on the same fixed shape.
static const ChordVoicing *leadFrom(const ChordPlayer *p) {
    if (p->sounding.count)
        return &p->sounding;
    if (p->last.count)
        return &p->last;
    return 0;
}

bool chordPlayerWouldChange(const ChordPlayer *p, uint8_t rootPc, ChordQuality quality) {
    return (p->activeRootPc != rootPc) || (p->activeQuality != (uint8_t)quality);
}

static bool voicingContains(const ChordVoicing *v, uint8_t note) {
    for (uint8_t i = 0; i < v->count; i++)
        if (v->notes[i] == note)
            return true;
    return false;
}

void chordPlayerStart(ChordPlayer *p, uint8_t rootPc, ChordQuality quality,
                      uint8_t velocity, const ChordVoicerConfig *cfg,
                      ChordNoteOnFn noteOn, ChordNoteOffFn noteOff, void *user) {
    ChordVoicing prev = p->sounding;

    ChordVoicing next;
    chordVoice(rootPc, quality, cfg, leadFrom(p), &next);

    // A new root retriggers every voice, so release the old chord first even
    // where the two share notes.
    for (uint8_t i = 0; i < prev.count; i++)
        noteOff(prev.notes[i], user);

    uint8_t vel = chordScaledVelocity(velocity, next.count);
    for (uint8_t i = 0; i < next.count; i++)
        noteOn(next.notes[i], vel, user);

    p->sounding = next;
    p->last = next;
    p->activeRootPc = rootPc;
    p->activeQuality = (uint8_t)quality;
}

void chordPlayerRevoice(ChordPlayer *p, uint8_t rootPc, ChordQuality quality,
                        uint8_t velocity, const ChordVoicerConfig *cfg,
                        ChordNoteOnFn noteOn, ChordNoteOffFn noteOff, void *user) {
    ChordVoicing prev = p->sounding;

    ChordVoicing next;
    chordVoice(rootPc, quality, cfg, leadFrom(p), &next);

    uint8_t vel = chordScaledVelocity(velocity, next.count);

    // Release only what is leaving...
    for (uint8_t i = 0; i < prev.count; i++)
        if (!voicingContains(&next, prev.notes[i]))
            noteOff(prev.notes[i], user);

    // ...and attack only what is new. Everything else keeps its envelope.
    for (uint8_t i = 0; i < next.count; i++)
        if (!voicingContains(&prev, next.notes[i]))
            noteOn(next.notes[i], vel, user);

    p->sounding = next;
    p->last = next;
    p->activeRootPc = rootPc;
    p->activeQuality = (uint8_t)quality;
}

void chordPlayerStop(ChordPlayer *p, ChordNoteOffFn noteOff, void *user) {
    for (uint8_t i = 0; i < p->sounding.count; i++)
        noteOff(p->sounding.notes[i], user);
    playerRelease(p);
}

void chordPlayerForget(ChordPlayer *p) {
    playerRelease(p);
}
