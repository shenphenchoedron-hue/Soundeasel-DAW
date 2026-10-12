#include "Test.h"
#include "io/ProjectSerializer.h"
#include "model/Commands.h"
#include "sequencer/Sequence.h"

using namespace mc;
using namespace mc::model;

static AudioClip clip(const std::string& file, Tick start, double length)
{
    AudioClip c;
    c.file = file;
    c.name = "take";
    c.start = start;
    c.length = length;
    return c;
}

TEST(audio_track_clips_sorted_with_ids)
{
    Project p;
    auto& a = p.addAudioTrack();
    CHECK_EQ(a.name, std::string("Audio 1"));
    const auto id2 = a.addClip(clip("/b.wav", 1920, 1.0));
    const auto id1 = a.addClip(clip("/a.wav", 0, 2.0));
    CHECK(id1 != id2);
    CHECK_EQ(a.clips().size(), (size_t)2);
    CHECK_EQ(a.clips()[0].file, std::string("/a.wav"));
    CHECK(a.removeClip(id2));
    CHECK(!a.findClip(id2));
    CHECK_EQ(p.addAudioTrack().name, std::string("Audio 2"));
}

TEST(audio_clip_end_uses_tempo)
{
    auto c = clip("/a.wav", 960, 1.0); // 1 s at 120 bpm = 2 beats
    CHECK_EQ(c.end(120.0), 960.0 + 2 * kPPQ);
}

TEST(add_audio_track_command_keeps_piano_roll_track_active)
{
    Project p;
    UndoStack u(p);
    auto& m = p.addMidiTrack("Piano");
    p.activeTrack = m.id();
    auto cmd = std::make_unique<AddTrackCommand>("Vocals", TrackKind::Audio);
    auto* raw = cmd.get();
    u.perform(std::move(cmd));
    CHECK(p.audioTrack(raw->trackId()) != nullptr);
    CHECK_EQ(p.activeTrack, m.id());
    u.undo();
    CHECK(p.track(raw->trackId()) == nullptr);
    u.redo();
    CHECK_EQ(p.track(raw->trackId())->name, std::string("Vocals"));
}

TEST(set_audio_clips_command_undo)
{
    Project p;
    UndoStack u(p);
    auto& a = p.addAudioTrack();
    a.addClip(clip("/a.wav", 0, 1.0));
    auto before = a.clips();
    auto after = before;
    after[0].start = 3840;
    after.push_back(clip("/b.wav", 0, 0.5));
    u.perform(std::make_unique<SetAudioClipsCommand>(a.id(), before, after, "Move"));
    CHECK_EQ(a.clips().size(), (size_t)2);
    CHECK_EQ(a.clips()[1].start, (Tick)3840);
    u.undo();
    CHECK_EQ(a.clips().size(), (size_t)1);
    CHECK_EQ(a.clips()[0].start, (Tick)0);
}

TEST(track_properties_audio_input)
{
    Project p;
    UndoStack u(p);
    auto& a = p.addAudioTrack();
    auto before = TrackProperties::from(a), after = before;
    after.input = 2;
    after.stereoInput = true;
    u.perform(std::make_unique<SetTrackPropertiesCommand>(a.id(), before, after));
    CHECK_EQ(a.input, 2);
    CHECK(a.stereoInput);
    u.undo();
    CHECK_EQ(a.input, 0);
}

TEST(master_command_undo)
{
    Project p;
    UndoStack u(p);
    auto after = p.master;
    after.volume = 0.5f;
    after.mute = true;
    u.perform(std::make_unique<SetMasterCommand>(p.master, after));
    CHECK_EQ(p.master.volume, 0.5f);
    u.undo();
    CHECK_EQ(p.master.volume, 1.0f);
    CHECK(!p.master.mute);
}

TEST(snapshot_includes_audio_tracks_and_master)
{
    Project p;
    p.tempoBpm = 120;
    p.addMidiTrack("Piano").addNote({0, 60, 100, 0, 960, -1});
    auto& a = p.addAudioTrack();
    a.addClip(clip("/a.wav", 3840, 2.0)); // ends at 3840 + 4 beats
    a.armed = true;
    a.solo = true;
    p.master.volume = 0.7f;
    p.master.balance = 0.5f;
    auto s = seq::SequenceSnapshot::build(p);
    CHECK_EQ(s.tracks.size(), (size_t)2);
    CHECK(!s.tracks[0].isAudio);
    CHECK(!s.tracks[0].audible); // the audio track is soloed
    CHECK(s.tracks[1].isAudio);
    CHECK(s.tracks[1].armed);
    CHECK_EQ(s.tracks[1].clips.size(), (size_t)1);
    CHECK_EQ(s.lengthTicks, (Tick)(3840 + 4 * kPPQ));
    CHECK_EQ(s.masterVolume, 0.7f);
    CHECK_EQ(s.masterBalance, 0.5f);
    p.master.mute = true;
    CHECK_EQ(seq::SequenceSnapshot::build(p).masterVolume, 0.0f);
}

TEST(serializer_audio_roundtrip_relative_paths)
{
    Project p;
    p.master = {0.5f, -0.25f, true};
    auto& a = p.addAudioTrack("Vocals");
    a.input = 1;
    a.stereoInput = true;
    a.armed = true;
    a.volume = 0.6f;
    auto c = clip("/music/song/song Audio/take1.wav", 1920, 3.5);
    c.offset = 0.25;
    c.gain = 0.5f;
    a.addClip(c);
    a.addClip(clip("/elsewhere/loop.wav", 0, 1.0));

    const auto text = io::ProjectSerializer::toString(p, "/music/song");
    CHECK(text.find("\"song Audio/take1.wav\"") != std::string::npos);
    CHECK(text.find("/elsewhere/loop.wav") != std::string::npos);

    Project q;
    auto r = io::ProjectSerializer::fromString(text, q, "/other/place");
    CHECK(r.ok);
    CHECK_EQ(q.master.volume, 0.5f);
    CHECK_EQ(q.master.balance, -0.25f);
    CHECK(q.master.mute);
    auto* b = q.audioTrack(a.id());
    CHECK(b != nullptr);
    if (!b) return;
    CHECK_EQ(b->name, std::string("Vocals"));
    CHECK_EQ(b->input, 1);
    CHECK(b->stereoInput);
    CHECK(b->armed);
    CHECK_EQ(b->volume, 0.6f);
    CHECK_EQ(b->clips().size(), (size_t)2);
    CHECK_EQ(b->clips()[0].file, std::string("/elsewhere/loop.wav"));
    CHECK_EQ(b->clips()[1].file, std::string("/other/place/song Audio/take1.wav"));
    CHECK_EQ(b->clips()[1].start, (Tick)1920);
    CHECK_EQ(b->clips()[1].offset, 0.25);
    CHECK_EQ(b->clips()[1].length, 3.5);
    CHECK_EQ(b->clips()[1].gain, 0.5f);
}

TEST(channel_effects_roundtrip_undo_and_snapshot)
{
    Project p;
    UndoStack u(p);
    auto& m = p.addMidiTrack("Keys");
    auto& a = p.addAudioTrack("Vox");
    auto before = TrackProperties::from(a), after = before;
    after.compressor.enabled = true;
    after.compressor.rms = true;
    after.compressor.thresholdDb = -24;
    after.compressor.attackMs = 3;
    after.reverb.enabled = true;
    after.reverb.size = 0.9f;
    u.perform(std::make_unique<SetTrackPropertiesCommand>(a.id(), before, after));
    CHECK(a.compressor.enabled);
    CHECK_EQ(a.reverb.size, 0.9f);
    m.reverb.enabled = true;
    m.reverb.mix = 0.4f;

    auto s = seq::SequenceSnapshot::build(p);
    CHECK(s.tracks[0].reverb.enabled);
    CHECK(s.tracks[1].compressor.rms);

    Project q;
    CHECK(io::ProjectSerializer::fromString(io::ProjectSerializer::toString(p), q).ok);
    CHECK(q.track(a.id())->compressor == a.compressor);
    CHECK(q.track(a.id())->reverb == a.reverb);
    CHECK(q.track(m.id())->reverb == m.reverb);
    CHECK(!q.track(m.id())->compressor.enabled);

    u.undo();
    CHECK(!a.compressor.enabled);
    CHECK(!a.reverb.enabled);
}

TEST(volume_automation_interpolates)
{
    std::vector<AutomationPoint> pts;
    CHECK_EQ(automationAt(pts, 100), 1.0f);              // no line: no change
    pts = {{1920, 1.0f}, {960, 0.5f}, {3840, 0.0f}};
    normalizeAutomation(pts);                            // sorted by time
    CHECK_EQ(pts[0].tick, (Tick)960);
    CHECK_EQ(automationAt(pts, 0), 0.5f);                // flat before the first point
    CHECK_EQ(automationAt(pts, 960), 0.5f);
    CHECK_EQ(automationAt(pts, 1440), 0.75f);            // halfway between 0.5 and 1.0
    CHECK_EQ(automationAt(pts, 2880), 0.5f);             // halfway between 1.0 and 0.0
    CHECK_EQ(automationAt(pts, 99999), 0.0f);            // flat after the last point
    pts.push_back({100, 5.0f});
    normalizeAutomation(pts);
    CHECK_EQ(pts[0].gain, 2.0f);                         // clamped to +6 dB
}

TEST(volume_automation_undo_roundtrip_snapshot)
{
    Project p;
    UndoStack u(p);
    auto& a = p.addAudioTrack();
    a.showVolumeLine = true;
    std::vector<AutomationPoint> after{{0, 1.0f}, {1920, 0.25f}};
    u.perform(std::make_unique<SetVolumeAutomationCommand>(a.id(), a.volumeAutomation, after));
    CHECK_EQ(a.volumeAutomation.size(), (size_t)2);
    CHECK_EQ(seq::SequenceSnapshot::build(p).tracks[0].volumeAutomation.size(), (size_t)2);
    Project q;
    CHECK(io::ProjectSerializer::fromString(io::ProjectSerializer::toString(p), q).ok);
    CHECK(q.audioTrack(a.id())->volumeAutomation == after);
    CHECK(q.audioTrack(a.id())->showVolumeLine);
    u.undo();
    CHECK(a.volumeAutomation.empty());
}

TEST(channel_eq_roundtrip_undo_snapshot)
{
    Project p;
    UndoStack u(p);
    auto& m = p.addMidiTrack("Keys");
    auto before = TrackProperties::from(m), after = before;
    after.eq.enabled = true;
    after.eq.bands = {{EqBand::Type::LowCut, 80, 0, 0.707f, true}, {EqBand::Type::Bell, 2500, -4.5f, 2.0f, false}};
    u.perform(std::make_unique<SetTrackPropertiesCommand>(m.id(), before, after));
    CHECK(m.eq == after.eq);
    CHECK(seq::SequenceSnapshot::build(p).tracks[0].eq == after.eq);
    Project q;
    CHECK(io::ProjectSerializer::fromString(io::ProjectSerializer::toString(p), q).ok);
    CHECK(q.track(m.id())->eq == after.eq);
    u.undo();
    CHECK(!m.eq.enabled);
    CHECK(m.eq.bands.empty());
}

TEST(insert_plugins_roundtrip_undo_snapshot)
{
    Project p;
    UndoStack u(p);
    auto& a = p.addAudioTrack("Vox");
    auto before = TrackProperties::from(a), after = before;
    after.inserts = {{"VST3", "/x/Comp.vst3|1", "Comp", "ACME", "QUJD", false}, {"CLAP", "/y/verb.clap|com.y.verb", "Verb", "Y", "", true}};
    after.insertsAfterBuiltins = true;
    u.perform(std::make_unique<SetTrackPropertiesCommand>(a.id(), before, after));
    CHECK_EQ(a.inserts.size(), (size_t)2);
    auto s = seq::SequenceSnapshot::build(p);
    CHECK_EQ(s.tracks[0].insertBypassed.size(), (size_t)2);
    CHECK(s.tracks[0].insertBypassed[0] == 0 && s.tracks[0].insertBypassed[1] == 1);
    CHECK(s.tracks[0].insertsAfterBuiltins);
    Project q;
    CHECK(io::ProjectSerializer::fromString(io::ProjectSerializer::toString(p), q).ok);
    auto* b = q.track(a.id());
    CHECK_EQ(b->inserts.size(), (size_t)2);
    CHECK_EQ(b->inserts[0].stateBase64, std::string("QUJD"));
    CHECK(b->inserts[1].bypassed);
    CHECK(b->inserts[1].sameType(a.inserts[1]));
    CHECK(b->insertsAfterBuiltins);
    u.undo();
    CHECK(a.inserts.empty());
}

TEST(insert_ids_tell_copies_of_one_plugin_apart)
{
    std::vector<PluginReference> v{{"LV2", "comp", "Comp", "", "s1", false}, {"LV2", "comp", "Comp", "", "s2", false}};
    assignInsertIds(v);
    CHECK(v[0].uid != 0 && v[1].uid != 0 && v[0].uid != v[1].uid);
    const auto second = v[1].uid;
    v.erase(v.begin());                    // remove the first copy
    v.push_back({"LV2", "comp", "Comp", "", "s3", false});
    assignInsertIds(v);
    CHECK_EQ(v[0].uid, second);            // the remaining copy keeps its id
    CHECK(v[1].uid != second);

    // ids survive save/load; duplicates or missing ids in a file are repaired
    Project p;
    auto& t = p.addMidiTrack();
    t.inserts = v;
    t.inserts.push_back({"LV2", "comp", "Comp", "", "", false});
    t.inserts.back().uid = v[0].uid;       // duplicate id
    Project q;
    CHECK(io::ProjectSerializer::fromString(io::ProjectSerializer::toString(p), q).ok);
    auto& qi = q.track(t.id())->inserts;
    CHECK_EQ(qi.size(), (size_t)3);
    CHECK_EQ(qi[0].uid, v[0].uid);
    CHECK_EQ(qi[1].uid, v[1].uid);
    CHECK(qi[2].uid != qi[0].uid && qi[2].uid != qi[1].uid && qi[2].uid != 0);
}
