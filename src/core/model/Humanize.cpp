#include "model/Humanize.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <random>

namespace mc::model {

std::vector<Note> humanizeCandidates(const std::vector<Note>& notes, const HumanizeSettings& s)
{
    std::vector<Note> v;
    for (auto& n : notes)
        if (!s.onlyNew || !n.humanized) v.push_back(n);
    return v;
}

namespace {
// Notes on the same key (same piano roll line) must not run into each other: an overlap
// retriggers the note (a click) and two voices on one key can hang in some synths.
// Only the changed notes are adjusted, and only overlaps Human Touch would create:
// - a note never moves past its neighbours on the line (the order stays),
// - a note that now runs into the next one is shortened, a note moved into the tail of an
//   unchanged note starts where that note ends.
// Overlaps that were already there before are left alone.
void keepLinesApart(const std::vector<Note>& original, std::vector<Note>& changed)
{
    std::map<NoteId, Note*> byId;
    for (auto& n : changed) byId[n.id] = &n;
    std::map<std::pair<int, int>, std::vector<const Note*>> lines; // (pitch, channel) -> notes in original order
    for (auto& n : original) lines[{n.pitch, n.channel}].push_back(&n);

    for (auto& [key, line] : lines) {
        if (line.size() < 2) continue;
        std::sort(line.begin(), line.end(), [](const Note* a, const Note* b) { return a->start != b->start ? a->start < b->start : a->id < b->id; });
        auto cur = [&](size_t i) -> Note& {
            auto it = byId.find(line[i]->id);
            return it != byId.end() ? *it->second : const_cast<Note&>(*line[i]); // unchanged notes are only read
        };
        auto isChanged = [&](size_t i) { return byId.count(line[i]->id) > 0; };
        // 1. starts keep their order on the line
        for (size_t i = 0; i < line.size(); ++i) {
            if (!isChanged(i)) continue;
            Note& n = cur(i);
            const Tick lo = i > 0 ? line[i - 1]->start + 1 : 0;
            const Tick hi = i + 1 < line.size() ? line[i + 1]->start - 1 : std::numeric_limits<Tick>::max();
            if (lo > hi || line[i]->start < lo || line[i]->start > hi) { n.start = line[i]->start; continue; } // notes at the same time: stay
            n.start = std::clamp(n.start, lo, hi);
            if (i > 0) n.start = std::max(n.start, cur(i - 1).start + 1); // the previous note moved later (stays <= hi)
        }
        // 2. no new overlaps
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            if (line[i]->end() > line[i + 1]->start) continue; // overlapped before: not caused by Human Touch
            Note& a = cur(i);
            Note& b = cur(i + 1);
            if (a.end() <= b.start) continue;
            if (isChanged(i)) a.length = std::max<Tick>(1, b.start - a.start);
            else if (isChanged(i + 1)) b.start = a.end(); // <= its original start, so the order stays
        }
    }
}
} // namespace

std::vector<Note> humanizeNotes(const std::vector<Note>& notes, const HumanizeSettings& s, std::uint32_t seed)
{
    auto picked = humanizeCandidates(notes, s);
    std::mt19937 rng(seed);
    // "Random notes": an exact share of the candidates, chosen at random.
    const double share = std::clamp(s.notesPercent, 0.0, 100.0) / 100.0;
    const auto count = (size_t)std::llround(share * (double)picked.size());
    std::shuffle(picked.begin(), picked.end(), rng);
    picked.resize(count);

    std::uniform_real_distribution<double> var(-1.0, 1.0);
    const double maxShift = std::clamp(s.timingPercent, 0.0, 100.0) / 100.0 * (double)std::max<Tick>(0, s.timingRange);
    const double lengthAmount = std::clamp(s.lengthPercent, 0.0, 100.0) / 100.0;
    const double velocityAmount = std::clamp(s.velocityPercent, 0.0, 100.0) / 100.0;
    for (auto& n : picked) {
        // Always draw all three values so each setting changes independently of the others.
        const double dt = var(rng), dl = var(rng), dv = var(rng);
        n.start = std::max<Tick>(0, n.start + (Tick)std::llround(dt * maxShift));
        const Tick minLength = std::max<Tick>(1, (Tick)std::llround((double)n.length * 0.1));
        n.length = std::max(minLength, (Tick)std::llround((double)n.length * (1.0 + dl * lengthAmount)));
        n.velocity = std::clamp((int)std::lround((double)n.velocity * (1.0 + dv * velocityAmount)), 1, 127);
        n.humanized = true;
    }
    keepLinesApart(notes, picked);
    std::sort(picked.begin(), picked.end(), [](const Note& a, const Note& b) { return a.id < b.id; });
    return picked;
}

} // namespace mc::model
