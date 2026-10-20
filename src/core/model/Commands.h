#pragma once
// Every model mutation from the UI goes through a Command so it can be undone.
#include "model/Project.h"
#include <memory>
#include <string>
#include <vector>

namespace mc::model {

class Command {
public:
    virtual ~Command() = default;
    virtual void apply(Project&) = 0;
    virtual void revert(Project&) = 0;
    virtual std::string name() const = 0;
};

class UndoStack {
public:
    explicit UndoStack(Project& p) : project_(p) {}
    void perform(std::unique_ptr<Command> c);   // applies + records + notifies
    bool undo();
    bool redo();
    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    void clear() { done_.clear(); undone_.clear(); }
    std::string undoName() const { return done_.empty() ? "" : done_.back()->name(); }
private:
    Project& project_;
    std::vector<std::unique_ptr<Command>> done_, undone_;
    static constexpr size_t kMaxDepth = 500;
};

// Adds notes (single note, pasted notes, a generated chord, a recorded take).
class AddNotesCommand : public Command {
public:
    AddNotesCommand(TrackId t, std::vector<Note> notes, std::string name = "Add notes")
        : track_(t), notes_(std::move(notes)), name_(std::move(name)) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return name_; }
    const std::vector<NoteId>& addedIds() const { return ids_; }
private:
    TrackId track_;
    std::vector<Note> notes_;
    std::vector<NoteId> ids_;
    std::string name_;
};

class RemoveNotesCommand : public Command {
public:
    RemoveNotesCommand(TrackId t, std::vector<NoteId> ids) : track_(t), ids_(std::move(ids)) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return "Delete notes"; }
private:
    TrackId track_;
    std::vector<NoteId> ids_;
    std::vector<Note> removed_;
};

// Move / resize / velocity change: stores before and after states (same ids).
class ModifyNotesCommand : public Command {
public:
    ModifyNotesCommand(TrackId t, std::vector<Note> before, std::vector<Note> after, std::string name = "Edit notes")
        : track_(t), before_(std::move(before)), after_(std::move(after)), name_(std::move(name)) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return name_; }
private:
    TrackId track_;
    std::vector<Note> before_, after_;
    std::string name_;
};

class AddTrackCommand : public Command {
public:
    explicit AddTrackCommand(std::string name = {}, TrackKind kind = TrackKind::Midi) : name_(std::move(name)), kind_(kind) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return kind_ == TrackKind::Audio ? "Add audio track" : "Add track"; }
    TrackId trackId() const { return id_; }
private:
    std::string name_;
    TrackKind kind_;
    TrackId id_ = 0;
    std::unique_ptr<Track> stash_;
};

class RemoveTrackCommand : public Command {
public:
    explicit RemoveTrackCommand(TrackId id) : id_(id) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return "Delete track"; }
private:
    TrackId id_;
    int index_ = -1;
    TrackId prevActive_ = 0;
    std::unique_ptr<Track> stash_;
};

// Inserts copies of tracks (copy & paste, duplicate). Everything is copied: settings,
// instrument and effect plugins (with the state stored in their references), notes,
// step patterns, audio clips and the volume line. The copies get new track ids, are
// inserted at `index` (in order) and copied step sequencer lines form new step
// sequencers (lines that shared a step sequencer still share one). Copied audio
// tracks are not record-enabled or monitoring, so pasting never starts feedback.
class PasteTracksCommand : public Command {
public:
    PasteTracksCommand(std::vector<std::unique_ptr<Track>> sources, int index, std::string name = "Paste track")
        : sources_(std::move(sources)), index_(index), name_(std::move(name)) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return name_; }
    const std::vector<TrackId>& trackIds() const { return ids_; }
private:
    std::vector<std::unique_ptr<Track>> sources_; // first apply only
    int index_;
    std::string name_;
    std::vector<TrackId> ids_;
    std::vector<std::unique_ptr<Track>> stash_;   // the copies while undone
    TrackId prevActive_ = 0;
};

struct TrackProperties {
    std::string name;
    bool mute = false, solo = false;
    float volume = 0.8f, pan = 0.0f;
    int channel = 0;
    std::uint32_t colour = 0;
    PluginReference plugin;
    int input = 0;              // audio tracks
    bool stereoInput = false;   // audio tracks
    EqSettings eq;
    CompressorSettings compressor;
    ReverbSettings reverb;
    std::vector<PluginReference> inserts;
    bool insertsAfterBuiltins = false;
    static TrackProperties from(const Track&);
    void applyTo(Track&) const;
};

class SetTrackPropertiesCommand : public Command {
public:
    SetTrackPropertiesCommand(TrackId id, TrackProperties before, TrackProperties after)
        : id_(id), before_(std::move(before)), after_(std::move(after)) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return "Change track"; }
private:
    TrackId id_;
    TrackProperties before_, after_;
};

// Replaces all clips of an audio track (add, move, trim, split, delete clips).
class SetAudioClipsCommand : public Command {
public:
    SetAudioClipsCommand(TrackId t, std::vector<AudioClip> before, std::vector<AudioClip> after, std::string name = "Edit audio")
        : track_(t), before_(std::move(before)), after_(std::move(after)), name_(std::move(name)) {}
    void apply(Project& p) override { if (auto* t = p.audioTrack(track_)) t->setClips(after_); }
    void revert(Project& p) override { if (auto* t = p.audioTrack(track_)) t->setClips(before_); }
    std::string name() const override { return name_; }
private:
    TrackId track_;
    std::vector<AudioClip> before_, after_;
    std::string name_;
};

// Replaces the volume line (automation points) of an audio track.
class SetVolumeAutomationCommand : public Command {
public:
    SetVolumeAutomationCommand(TrackId t, std::vector<AutomationPoint> before, std::vector<AutomationPoint> after)
        : track_(t), before_(std::move(before)), after_(std::move(after)) {}
    void apply(Project& p) override { if (auto* t = p.audioTrack(track_)) t->volumeAutomation = after_; }
    void revert(Project& p) override { if (auto* t = p.audioTrack(track_)) t->volumeAutomation = before_; }
    std::string name() const override { return "Edit volume line"; }
private:
    TrackId track_;
    std::vector<AutomationPoint> before_, after_;
};

// Replaces the sustain pedal line of a MIDI track.
class SetSustainCommand : public Command {
public:
    SetSustainCommand(TrackId t, std::vector<AutomationPoint> before, std::vector<AutomationPoint> after)
        : track_(t), before_(std::move(before)), after_(std::move(after)) {}
    void apply(Project& p) override { if (auto* t = p.midiTrack(track_)) t->sustain = after_; }
    void revert(Project& p) override { if (auto* t = p.midiTrack(track_)) t->sustain = before_; }
    std::string name() const override { return "Edit sustain pedal"; }
private:
    TrackId track_;
    std::vector<AutomationPoint> before_, after_;
};

class SetMasterCommand : public Command {
public:
    SetMasterCommand(MasterBus before, MasterBus after) : before_(before), after_(after) {}
    void apply(Project& p) override { p.master = after_; }
    void revert(Project& p) override { p.master = before_; }
    std::string name() const override { return "Change Mains"; }
private:
    MasterBus before_, after_;
};

// Several commands performed and undone as one step.
class CompoundCommand : public Command {
public:
    CompoundCommand(std::vector<std::unique_ptr<Command>> c, std::string name) : cmds_(std::move(c)), name_(std::move(name)) {}
    void apply(Project& p) override { for (auto& c : cmds_) c->apply(p); }
    void revert(Project& p) override { for (auto it = cmds_.rbegin(); it != cmds_.rend(); ++it) (*it)->revert(p); }
    std::string name() const override { return name_; }
private:
    std::vector<std::unique_ptr<Command>> cmds_;
    std::string name_;
};

// Edits one or more step sequencer lines; their notes are regenerated.
class SetStepPatternsCommand : public Command {
public:
    using Entry = std::pair<TrackId, StepPattern>;
    SetStepPatternsCommand(std::vector<Entry> before, std::vector<Entry> after, std::string name = "Edit steps")
        : before_(std::move(before)), after_(std::move(after)), name_(std::move(name)) {}
    void apply(Project&) override;
    void revert(Project&) override;
    std::string name() const override { return name_; }
private:
    static void set(Project&, const std::vector<Entry>&);
    std::vector<Entry> before_, after_;
    std::string name_;
};

} // namespace mc::model
