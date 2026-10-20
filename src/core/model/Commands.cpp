#include "model/Commands.h"
#include <algorithm>
#include <map>

namespace mc::model {

void UndoStack::perform(std::unique_ptr<Command> c)
{
    c->apply(project_);
    done_.push_back(std::move(c));
    if (done_.size() > kMaxDepth) done_.erase(done_.begin());
    undone_.clear();
    project_.notifyChanged();
}

bool UndoStack::undo()
{
    if (done_.empty()) return false;
    auto c = std::move(done_.back());
    done_.pop_back();
    c->revert(project_);
    undone_.push_back(std::move(c));
    project_.notifyChanged();
    return true;
}

bool UndoStack::redo()
{
    if (undone_.empty()) return false;
    auto c = std::move(undone_.back());
    undone_.pop_back();
    c->apply(project_);
    done_.push_back(std::move(c));
    project_.notifyChanged();
    return true;
}

// ---- notes
void AddNotesCommand::apply(Project& p)
{
    auto* t = p.midiTrack(track_);
    if (!t) return;
    const bool first = ids_.empty();
    for (size_t i = 0; i < notes_.size(); ++i) {
        Note n = notes_[i];
        n.id = first ? 0 : ids_[i]; // reuse ids on redo
        const NoteId id = t->addNote(n);
        if (first) ids_.push_back(id);
    }
}

void AddNotesCommand::revert(Project& p)
{
    if (auto* t = p.midiTrack(track_))
        for (auto id : ids_) t->removeNote(id);
}

void RemoveNotesCommand::apply(Project& p)
{
    removed_.clear();
    auto* t = p.midiTrack(track_);
    if (!t) return;
    for (auto id : ids_)
        if (auto* n = t->findNote(id)) { removed_.push_back(*n); t->removeNote(id); }
}

void RemoveNotesCommand::revert(Project& p)
{
    if (auto* t = p.midiTrack(track_))
        for (auto& n : removed_) t->addNote(n);
}

void ModifyNotesCommand::apply(Project& p)
{
    if (auto* t = p.midiTrack(track_)) t->updateNotes(after_);
}

void ModifyNotesCommand::revert(Project& p)
{
    if (auto* t = p.midiTrack(track_)) t->updateNotes(before_);
}

// ---- tracks
void AddTrackCommand::apply(Project& p)
{
    if (stash_) { id_ = stash_->id(); p.insertTrack((int)p.tracks().size(), std::move(stash_)); }
    else id_ = kind_ == TrackKind::Audio ? p.addAudioTrack(name_).id() : p.addMidiTrack(name_).id();
    if (kind_ == TrackKind::Midi) p.activeTrack = id_; // the piano roll edits MIDI tracks only
}

void AddTrackCommand::revert(Project& p) { stash_ = p.removeTrack(id_); }

void RemoveTrackCommand::apply(Project& p)
{
    prevActive_ = p.activeTrack;
    index_ = p.indexOf(id_);
    stash_ = p.removeTrack(id_);
}

void RemoveTrackCommand::revert(Project& p)
{
    if (stash_) p.insertTrack(index_, std::move(stash_));
    p.activeTrack = prevActive_;
}

void PasteTracksCommand::apply(Project& p)
{
    prevActive_ = p.activeTrack;
    std::vector<std::unique_ptr<Track>> copies;
    if (ids_.empty()) {
        std::map<std::uint64_t, std::uint64_t> groups; // source step sequencer -> new one
        std::uint64_t nextGroup = p.unusedStepGroup();
        for (auto& src : sources_) {
            if (!src) continue;
            auto c = src->cloneAs(p.allocateTrackId());
            if (auto* m = dynamic_cast<MidiTrack*>(c.get()); m && m->isStepLine()) {
                auto [it, fresh] = groups.try_emplace(m->stepGroup, nextGroup);
                if (fresh) ++nextGroup;
                m->stepGroup = it->second;
                m->regenerateStepNotes();
            }
            if (auto* a = dynamic_cast<AudioTrack*>(c.get())) { a->armed = false; a->monitor = false; }
            ids_.push_back(c->id());
            copies.push_back(std::move(c));
        }
        sources_.clear();
    } else {
        copies = std::move(stash_);
        stash_.clear();
    }
    int at = index_ < 0 ? (int)p.tracks().size() : std::min(index_, (int)p.tracks().size());
    for (auto& c : copies) p.insertTrack(at++, std::move(c));
    // The piano roll shows the first pasted instrument track.
    for (auto id : ids_)
        if (auto* m = p.midiTrack(id); m && !m->isStepLine()) { p.activeTrack = id; break; }
}

void PasteTracksCommand::revert(Project& p)
{
    stash_.clear();
    for (auto id : ids_)
        if (auto t = p.removeTrack(id)) stash_.push_back(std::move(t));
    if (p.track(prevActive_)) p.activeTrack = prevActive_;
}

TrackProperties TrackProperties::from(const Track& t)
{
    TrackProperties tp;
    tp.name = t.name; tp.mute = t.mute; tp.solo = t.solo; tp.volume = t.volume; tp.pan = t.pan; tp.colour = t.colour;
    tp.eq = t.eq; tp.compressor = t.compressor; tp.reverb = t.reverb;
    tp.inserts = t.inserts; tp.insertsAfterBuiltins = t.insertsAfterBuiltins;
    if (auto* m = dynamic_cast<const MidiTrack*>(&t)) { tp.channel = m->channel; tp.plugin = m->plugin; }
    if (auto* a = dynamic_cast<const AudioTrack*>(&t)) { tp.input = a->input; tp.stereoInput = a->stereoInput; }
    return tp;
}

void TrackProperties::applyTo(Track& t) const
{
    t.name = name; t.mute = mute; t.solo = solo; t.volume = volume; t.pan = pan; t.colour = colour;
    t.eq = eq; t.compressor = compressor; t.reverb = reverb;
    t.inserts = inserts; t.insertsAfterBuiltins = insertsAfterBuiltins;
    if (auto* m = dynamic_cast<MidiTrack*>(&t)) { m->channel = channel; m->plugin = plugin; }
    if (auto* a = dynamic_cast<AudioTrack*>(&t)) { a->input = input; a->stereoInput = stereoInput; }
}

void SetTrackPropertiesCommand::apply(Project& p)  { if (auto* t = p.track(id_)) after_.applyTo(*t); }
void SetTrackPropertiesCommand::revert(Project& p) { if (auto* t = p.track(id_)) before_.applyTo(*t); }

void SetStepPatternsCommand::set(Project& p, const std::vector<Entry>& v)
{
    for (auto& [id, pat] : v)
        if (auto* t = p.midiTrack(id)) { t->step = pat; t->regenerateStepNotes(); }
}
void SetStepPatternsCommand::apply(Project& p)  { set(p, after_); }
void SetStepPatternsCommand::revert(Project& p) { set(p, before_); }

} // namespace mc::model
