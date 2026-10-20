#include "Test.h"
#include "model/Commands.h"
#include "model/Humanize.h"
#include "sequencer/Sequence.h"
#include "sequencer/Timing.h"
#include <algorithm>
#include <cstdlib>

using namespace mc::model;
using namespace mc::seq;

TEST(tracks_are_independent)
{
    Project p;
    auto& a = p.addMidiTrack("A");
    auto& b = p.addMidiTrack("B");
    a.addNote({0, 60, 100, 0, 480, -1});
    CHECK_EQ(a.notes().size(), (size_t)1);
    CHECK_EQ(b.notes().size(), (size_t)0);
}

TEST(undo_redo_notes_and_tracks)
{
    Project p;
    UndoStack u(p);
    u.perform(std::make_unique<AddTrackCommand>("Piano"));
    const auto tid = p.activeTrack;
    u.perform(std::make_unique<AddNotesCommand>(tid, std::vector<Note>{{0, 57, 100, 0, 960, -1}, {0, 60, 100, 0, 960, -1}}));
    CHECK_EQ(p.midiTrack(tid)->notes().size(), (size_t)2);

    auto before = p.midiTrack(tid)->notes()[0];
    auto after = before; after.start = 960; after.length = 480;
    u.perform(std::make_unique<ModifyNotesCommand>(tid, std::vector<Note>{before}, std::vector<Note>{after}));
    CHECK_EQ(p.midiTrack(tid)->findNote(before.id)->start, (Tick)960);
    u.undo();
    CHECK_EQ(p.midiTrack(tid)->findNote(before.id)->start, (Tick)0);
    u.undo();
    CHECK_EQ(p.midiTrack(tid)->notes().size(), (size_t)0);
    u.redo();
    CHECK_EQ(p.midiTrack(tid)->notes().size(), (size_t)2);

    u.perform(std::make_unique<RemoveTrackCommand>(tid));
    CHECK(p.tracks().empty());
    u.undo();
    CHECK_EQ(p.midiTrack(tid)->notes().size(), (size_t)2);
}

TEST(timing)
{
    TimeSignature ts{4, 4};
    CHECK_EQ(ticksPerBar(ts), (Tick)3840);
    CHECK_EQ(ticksToSeconds(960, 120), 0.5);
    auto bb = toBarBeat(3840 + 960 * 2 + 10, ts);
    CHECK_EQ(bb.bar, 2); CHECK_EQ(bb.beat, 3); CHECK_EQ(bb.tick, (Tick)10);
    CHECK_EQ((GridValue{16}.ticks()), (Tick)240);
    CHECK_EQ((GridValue{8, true}.ticks()), (Tick)320);
    CHECK_EQ(snapNearest(250, 240), (Tick)240);
    CHECK_EQ(snapFloor(479, 240), (Tick)240);
    CHECK_EQ(ticksPerBeat({6, 8}), (Tick)480);
}

TEST(sequence_snapshot_mute_solo_and_order)
{
    Project p;
    auto& a = p.addMidiTrack("A");
    auto& b = p.addMidiTrack("B");
    a.addNote({0, 60, 100, 0, 480, -1});
    a.addNote({0, 60, 100, 480, 480, -1});
    b.addNote({0, 64, 90, 0, 960, -1});
    b.solo = true;
    auto s = SequenceSnapshot::build(p);
    CHECK(!s.tracks[0].audible);
    CHECK(s.tracks[1].audible);
    // note-off at 480 precedes the next note-on at 480
    CHECK(!s.tracks[0].events[1].noteOn);
    CHECK(s.tracks[0].events[2].noteOn);
    int n = 0;
    s.tracks[0].forEachInRange(480, 960, [&](const SeqEvent&) { ++n; });
    CHECK_EQ(n, 2);
    CHECK_EQ(s.lengthTicks, (Tick)960);
}

TEST(paste_tracks_copies_everything_with_new_ids)
{
    Project p;
    UndoStack u(p);
    auto& piano = p.addMidiTrack("Piano");
    piano.volume = 0.5f; piano.pan = -0.3f; piano.colour = 0xff112233u; piano.channel = 3;
    piano.plugin.format = "Internal"; piano.plugin.identifier = "x"; piano.plugin.stateBase64 = "c3RhdGU=";
    piano.eq.enabled = true; piano.reverb.enabled = true;
    PluginReference fx; fx.format = "VST3"; fx.identifier = "fx"; fx.uid = 1; fx.stateBase64 = "abc";
    piano.inserts.push_back(fx);
    piano.addNote({0, 60, 100, 0, 480, -1});
    piano.addNote({0, 64, 90, 480, 480, -1});
    auto& audio = p.addAudioTrack("Vox");
    AudioClip clip; clip.file = "/tmp/a.wav"; clip.length = 2.0; clip.start = 960;
    audio.addClip(clip);
    audio.volumeAutomation = {{0, 1.0f}, {960, 0.5f}};
    audio.armed = true; audio.monitor = true;
    const auto pianoId = piano.id(), audioId = audio.id();

    std::vector<std::unique_ptr<Track>> src;
    src.push_back(p.track(pianoId)->clone());
    auto cmd = std::make_unique<PasteTracksCommand>(std::move(src), 1);
    auto* raw = cmd.get();
    u.perform(std::move(cmd));
    CHECK_EQ(p.tracks().size(), (size_t)3);
    const auto copyId = raw->trackIds().at(0);
    CHECK(copyId != pianoId && copyId != audioId);
    CHECK_EQ(p.indexOf(copyId), 1);
    CHECK_EQ(p.activeTrack, copyId);
    auto* c = p.midiTrack(copyId);
    CHECK(c != nullptr);
    CHECK_EQ(c->notes().size(), (size_t)2);
    CHECK_EQ(c->volume, 0.5f);
    CHECK_EQ(c->pan, -0.3f);
    CHECK_EQ(c->colour, 0xff112233u);
    CHECK_EQ(c->channel, 3);
    CHECK_EQ(c->plugin.stateBase64, std::string("c3RhdGU="));
    CHECK(c->eq.enabled && c->reverb.enabled);
    CHECK_EQ(c->inserts.size(), (size_t)1);
    CHECK_EQ(c->inserts[0].stateBase64, std::string("abc"));
    // the copy is independent of the original
    c->addNote({0, 67, 100, 960, 480, -1});
    CHECK_EQ(p.midiTrack(pianoId)->notes().size(), (size_t)2);

    // audio track: clips + volume line copied, never armed or monitoring
    std::vector<std::unique_ptr<Track>> src2;
    src2.push_back(p.track(audioId)->clone());
    auto cmd2 = std::make_unique<PasteTracksCommand>(std::move(src2), -1);
    auto* raw2 = cmd2.get();
    u.perform(std::move(cmd2));
    auto* a2 = p.audioTrack(raw2->trackIds().at(0));
    CHECK(a2 != nullptr);
    CHECK_EQ(p.indexOf(a2->id()), 3);
    CHECK_EQ(a2->clips().size(), (size_t)1);
    CHECK_EQ(a2->clips()[0].file, std::string("/tmp/a.wav"));
    CHECK_EQ(a2->volumeAutomation.size(), (size_t)2);
    CHECK(!a2->armed && !a2->monitor);
    CHECK_EQ(p.activeTrack, copyId); // pasting an audio track keeps the piano roll track

    // undo / redo keep ids
    const auto a2Id = a2->id();
    u.undo();
    CHECK(p.track(a2Id) == nullptr);
    u.undo();
    CHECK(p.track(copyId) == nullptr);
    CHECK_EQ(p.activeTrack, pianoId);
    u.redo();
    CHECK(p.midiTrack(copyId) != nullptr);
    CHECK_EQ(p.indexOf(copyId), 1);
    u.redo();
    CHECK(p.audioTrack(a2Id) != nullptr);
    CHECK(p.nextTrackId() > a2Id);
}

TEST(paste_step_sequencer_gets_a_new_group)
{
    Project p;
    UndoStack u(p);
    auto& kick = p.addMidiTrack("Kick");
    kick.stepGroup = 1; kick.step.pitch = 36; kick.step.normalise(); kick.step.steps[0] = 100; kick.regenerateStepNotes();
    auto& snare = p.addMidiTrack("Snare");
    snare.stepGroup = 1; snare.step.pitch = 38; snare.step.normalise(); snare.step.steps[4] = 90; snare.regenerateStepNotes();
    std::vector<std::unique_ptr<Track>> src;
    src.push_back(kick.clone());
    src.push_back(snare.clone());
    auto cmd = std::make_unique<PasteTracksCommand>(std::move(src), 2);
    auto* raw = cmd.get();
    u.perform(std::move(cmd));
    CHECK_EQ(raw->trackIds().size(), (size_t)2);
    auto* k2 = p.midiTrack(raw->trackIds()[0]);
    auto* s2 = p.midiTrack(raw->trackIds()[1]);
    CHECK(k2->stepGroup == s2->stepGroup);
    CHECK(k2->stepGroup != 1);
    CHECK(k2->step == p.midiTrack(kick.id())->step);
    CHECK_EQ(s2->notes().size(), (size_t)1);
}

TEST(humanize_varies_within_limits_and_marks_notes)
{
    std::vector<Note> notes;
    for (int i = 0; i < 200; ++i) notes.push_back({(NoteId)(i + 1), 60, 100, (Tick)(i * 960 + 960), 480, -1});
    HumanizeSettings s;
    s.timingPercent = 50; s.timingRange = 240; // up to +-120 ticks
    s.lengthPercent = 20;                      // 384..576
    s.velocityPercent = 10;                    // 90..110
    auto out = humanizeNotes(notes, s, 42);
    CHECK_EQ(out.size(), notes.size());
    bool moved = false;
    for (auto& n : out) {
        const auto& o = notes[(size_t)n.id - 1];
        CHECK(std::llabs(n.start - o.start) <= 120);
        CHECK(n.length >= 384 && n.length <= 576);
        CHECK(n.velocity >= 90 && n.velocity <= 110);
        CHECK(n.humanized);
        CHECK_EQ(n.pitch, o.pitch);
        moved = moved || n.start != o.start;
    }
    CHECK(moved);
    // same seed, same result
    CHECK(humanizeNotes(notes, s, 42) == out);

    // share of notes
    s.notesPercent = 25;
    CHECK_EQ(humanizeNotes(notes, s, 7).size(), (size_t)50);

    // only new notes: humanised ones are left alone
    s.notesPercent = 100;
    s.onlyNew = true;
    auto mixed = notes;
    for (size_t i = 0; i < 150; ++i) mixed[i].humanized = true;
    auto fresh = humanizeNotes(mixed, s, 3);
    CHECK_EQ(fresh.size(), (size_t)50);
    for (auto& n : fresh) CHECK(n.id > 150);

    // never below 0, length keeps at least 10 %
    std::vector<Note> edge{{1, 60, 100, 0, 100, -1}};
    s.onlyNew = false; s.timingPercent = 100; s.lengthPercent = 100;
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
        auto e = humanizeNotes(edge, s, seed);
        CHECK(e[0].start >= 0);
        CHECK(e[0].length >= 10);
    }
}

TEST(update_notes_batch_matches_single_updates)
{
    MidiTrack t(1, "A");
    for (int i = 0; i < 5; ++i) t.addNote({0, 60 + i, 100, (Tick)(i * 480), 480, -1});
    auto a = t.notes()[0], b = t.notes()[4];
    a.start = 5000; a.velocity = 300;   // moves to the end, velocity clamped
    b.start = -10; b.length = 0;        // moves to the front, start/length clamped
    Note unknown{999, 60, 100, 0, 480, -1};
    t.updateNotes({a, b, unknown});
    CHECK_EQ(t.notes().size(), (size_t)5);
    CHECK_EQ(t.notes().front().id, b.id);
    CHECK_EQ(t.notes().front().start, (Tick)0);
    CHECK_EQ(t.notes().front().length, (Tick)1);
    CHECK_EQ(t.notes().back().id, a.id);
    CHECK_EQ(t.notes().back().velocity, 127);
}

TEST(humanize_never_overlaps_notes_on_the_same_line)
{
    // legato line: every note ends exactly where the next starts, plus a chord note on another key
    std::vector<Note> notes;
    for (int i = 0; i < 64; ++i) notes.push_back({(NoteId)(i + 1), 60, 100, (Tick)(i * 240), 240, -1});
    notes.push_back({100, 64, 100, 0, 64 * 240, -1});
    HumanizeSettings s;
    s.timingPercent = 100; s.timingRange = 240; s.lengthPercent = 100;
    for (std::uint32_t seed = 0; seed < 40; ++seed) {
        for (double share : {100.0, 50.0}) {
            s.notesPercent = share;
            auto result = notes;
            for (auto& c : humanizeNotes(notes, s, seed))
                for (auto& n : result) if (n.id == c.id) n = c;
            std::vector<Note> line;
            for (auto& n : result) if (n.pitch == 60) line.push_back(n);
            std::sort(line.begin(), line.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
            for (size_t i = 0; i + 1 < line.size(); ++i) {
                CHECK(line[i].end() <= line[i + 1].start);   // no overlap
                CHECK(line[i].id < line[i + 1].id);          // order kept
                CHECK(line[i].length >= 1);
            }
        }
    }
}

TEST(overlapping_notes_never_leave_a_voice_hanging)
{
    Project p;
    auto& t = p.addMidiTrack("A");
    t.addNote({0, 60, 100, 0, 500, -1});    // A: 0..500
    t.addNote({0, 60, 100, 480, 480, -1});  // B: 480..960, starts while A still sounds
    t.addNote({0, 60, 100, 600, 100, -1});  // C: 600..700, inside B
    const auto ev = noteEvents(t);
    // every note-on is preceded by silence on that key, and the key ends silent
    int sounding = 0;
    for (auto& e : ev) {
        if (e.noteOn) { CHECK_EQ(sounding, 0); ++sounding; }
        else { CHECK_EQ(sounding, 1); --sounding; }
    }
    CHECK_EQ(sounding, 0);
    CHECK_EQ(ev.back().tick, (Tick)960); // the last note-off of the overlapping notes is kept
    auto s = SequenceSnapshot::build(p);
    CHECK(s.tracks[0].noteSoundingAt(0, 60, 800));
    CHECK(!s.tracks[0].noteSoundingAt(0, 60, 961));
    CHECK(!s.tracks[0].noteSoundingAt(0, 61, 800));
}
