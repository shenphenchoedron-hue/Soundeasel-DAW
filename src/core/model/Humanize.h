#pragma once
// Human Touch: gives placed notes small random variations in placement (start),
// length and velocity, so programmed parts sound less mechanical. Pure function:
// the caller turns the result into one undoable ModifyNotesCommand.
#include "model/Note.h"
#include <cstdint>
#include <vector>

namespace mc::model {

struct HumanizeSettings {
    double notesPercent = 100.0;    // share of the notes that are changed, picked at random (0..100)
    double timingPercent = 10.0;    // largest shift of a note start, in % of timingRange (0..100)
    Tick timingRange = kPPQ / 4;    // 100 % placement = up to one 1/16 note earlier or later
    double lengthPercent = 10.0;    // largest change of a note's length, in % of that length (0..100)
    double velocityPercent = 0.0;   // largest change of a note's velocity, in % of that velocity (0..100)
    bool onlyNew = false;           // skip notes Human Touch already changed (e.g. after adding notes)
};

// Notes Human Touch would pick from (all, or only the ones it has not changed yet).
std::vector<Note> humanizeCandidates(const std::vector<Note>& notes, const HumanizeSettings&);

// Returns the changed notes (same ids, marked `humanized`); notes that are not picked
// are not returned. Starts never go below 0, lengths keep at least 10 % of their
// original length (and 1 tick), velocities stay 1..127. Same seed = same result.
// Notes on the same key never get a new overlap and keep their order on that line
// (a note that would run into the next one is shortened).
std::vector<Note> humanizeNotes(const std::vector<Note>& notes, const HumanizeSettings&, std::uint32_t seed);

} // namespace mc::model
