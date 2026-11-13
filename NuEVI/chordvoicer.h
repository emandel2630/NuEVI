#ifndef __CHORDVOICER_H
#define __CHORDVOICER_H

#include <stdint.h>

// Chord mode voicer.
//
// Deliberately free of Arduino/Teensy dependencies so it can be built and
// exercised by the host-side harness in simulation/chordtest. Nothing here
// allocates; all working storage is on the stack or in file statics.
//
// The fingered note supplies the chord root (pitch class only - register is
// discarded and supplied by the voicer). The octave rollers supply the chord
// quality. See "EWI Chord Mode" spec sections 5-8.

// Roller slots, ordered so adjacent slots are adjacent in real progressions.
// Major ii-V-I is CQ_MIN7 -> CQ_DOM7 -> CQ_MAJ7, monotonic in one direction.
enum ChordQuality : uint8_t {
    CQ_DIM7 = 0,
    CQ_M7B5 = 1,
    CQ_MIN7 = 2,
    CQ_DOM7 = 3,
    CQ_MAJ7 = 4,
    CQ_SUS7 = 5,
    CQ_COUNT = 6
};

// Upper structure is at most 4 notes, plus one bass note when we cover root duty.
#define CHORD_MAX_VOICES 5

struct ChordVoicing {
    uint8_t notes[CHORD_MAX_VOICES]; // MIDI note numbers, ascending
    uint8_t count;                   // 0 means "no chord sounding"
    uint8_t bassCount;               // leading notes[] entries that are bass (0 or 1)
};

// Who is covering the root.
enum ChordBassMode : uint8_t {
    CHORD_BASS_EXTERNAL = 0, // someone else plays bass -> rootless upper structures
    CHORD_BASS_SELF = 1      // we cover root duty -> add a bass note, reduced forms allowed
};

struct ChordVoicerConfig {
    ChordBassMode bassMode;
    int8_t lilOffset;   // shifts the whole low-interval-limit table, semitones.
                        // Negative permits lower/muddier, positive is more conservative.
    uint8_t upperLo;    // upper structure window, MIDI
    uint8_t upperHi;
    uint8_t topLo;      // top voice window, MIDI. Anything above topHi belongs to the soloist.
    uint8_t topHi;
    uint8_t bassLo;     // bass note window, MIDI (CHORD_BASS_SELF only)
    uint8_t bassHi;
    uint8_t maxSpan;    // max upper-structure span, semitones
    uint8_t tempTenths; // softmax temperature x10. 20 == 2.0, the shipping value.
                        // 0 selects the lowest-cost candidate deterministically.
    uint8_t simple;     // 1 = simple chord mode (PolySelect::EChordSimple): always the
                        // plain root-position seventh chord, root in C3-B3, stacked
                        // R-3-5-7. No inversions, no extensions, no voice leading;
                        // bassMode, lilOffset, top window and temperature are ignored.
};

// Defaults from spec section 6.
void chordVoicerDefaults(ChordVoicerConfig *cfg);

// Seed the sampler. Any nonzero value; on the firmware, micros() at boot.
void chordVoicerSeed(uint32_t seed);

// Voice one chord.
//
// rootPc   0-11, C = 0. Take it from fingeredNote % 12: the roller contributes
//          only whole octaves to the fingered note, so the pitch class is already
//          independent of roller position.
// prev     previous voicing to voice-lead against, or null for the first chord.
// out      receives the chosen voicing.
//
// Returns true if a template-based voicing was found, false if it fell back to a
// shell voicing (never fails, never muddy - see spec section 7).
bool chordVoice(uint8_t rootPc, ChordQuality quality, const ChordVoicerConfig *cfg,
                const ChordVoicing *prev, ChordVoicing *out);

// Enumerate every candidate that survives the section 6 filters, without scoring
// or selecting. Used by the host harness to reproduce the spec's candidate-count
// and coverage tables. Returns the number written, capped at maxOut.
uint8_t chordCandidates(uint8_t rootPc, ChordQuality quality, const ChordVoicerConfig *cfg,
                        ChordVoicing *out, uint8_t maxOut);

// Voice-leading cost of moving from prev to cur, exactly as spec section 8.
// Bass notes are excluded: in CHORD_BASS_SELF the bass is deterministic given the
// root, so it contributes a constant and must not affect ranking.
float chordVoiceLeadingCost(const ChordVoicing *prev, const ChordVoicing *cur);

// Lowest permissible bottom note for an interval, after cfg->lilOffset.
// interval is in semitones; anything above an octave is unconstrained.
uint8_t chordLowIntervalLimit(uint8_t interval, int8_t lilOffset);

// ---------------------------------------------------------------- player
//
// Tracks which voices are actually sounding and turns chord changes into note
// on/off events. This lives here rather than in the firmware so the host
// harness can drive the exact same code: every bug found in this bookkeeping
// so far was a sequencing bug, not a voicing bug.
//
// The player never calls MIDI directly - it emits through callbacks, so the
// firmware passes midiSendNoteOn/midiSendNoteOff and the harness passes
// recorders.

typedef void (*ChordNoteOnFn)(uint8_t note, uint8_t velocity, void *user);
typedef void (*ChordNoteOffFn)(uint8_t note, void *user);

#define CHORD_NONE 0xFF

struct ChordPlayer {
    ChordVoicing sounding;  // exactly the notes currently held down
    ChordVoicing last;      // the most recent voicing, kept through releases and
                            // panics so the next attack voice-leads from it
                            // instead of restarting at a fixed shape per root
    uint8_t activeRootPc;   // root the sounding chord was built on, or CHORD_NONE
    uint8_t activeQuality;  // quality the sounding chord was built on, or CHORD_NONE
};

void chordPlayerInit(ChordPlayer *p);

// True if the given root and quality would change what is sounding.
bool chordPlayerWouldChange(const ChordPlayer *p, uint8_t rootPc, ChordQuality quality);

// Fresh attack, or a new root while blowing: every voice retriggers.
void chordPlayerStart(ChordPlayer *p, uint8_t rootPc, ChordQuality quality,
                      uint8_t velocity, const ChordVoicerConfig *cfg,
                      ChordNoteOnFn noteOn, ChordNoteOffFn noteOff, void *user);

// Roller moved while the note is held: revoice in place, touching only the
// voices that actually change so held voices keep their envelopes.
void chordPlayerRevoice(ChordPlayer *p, uint8_t rootPc, ChordQuality quality,
                        uint8_t velocity, const ChordVoicerConfig *cfg,
                        ChordNoteOnFn noteOn, ChordNoteOffFn noteOff, void *user);

// Release everything currently held.
void chordPlayerStop(ChordPlayer *p, ChordNoteOffFn noteOff, void *user);

// Drop the record of what is sounding WITHOUT emitting note-offs. For use after
// an all-notes-off, which silences the voices behind the player's back; leaving
// them recorded as held would suppress their note-ons forever.
void chordPlayerForget(ChordPlayer *p);

#endif
