#include "io/ProjectSerializer.h"
#include "io/Json.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace mc::io {

using namespace mc::model;

namespace {
namespace fs = std::filesystem;

// UTF-8 <-> path (std::string paths are UTF-8 everywhere in the app, also on Windows).
fs::path pathOf(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const fs::path& p) { const auto s = p.generic_u8string(); return std::string(s.begin(), s.end()); }

std::string toStoredPath(const std::string& file, const std::string& baseDir)
{
    if (baseDir.empty() || file.empty()) return file;
    const auto rel = pathOf(file).lexically_relative(pathOf(baseDir));
    if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") return file; // outside the project folder: keep absolute
    return utf8(rel);
}

std::string fromStoredPath(const std::string& file, const std::string& baseDir)
{
    if (baseDir.empty() || file.empty()) return file;
    const auto f = pathOf(file);
    if (f.is_absolute() || f.has_root_name() || f.has_root_directory()) return file;
    return utf8((pathOf(baseDir) / f).lexically_normal());
}
} // namespace

std::string ProjectSerializer::toString(const Project& p, const std::string& baseDir)
{
    Json root;
    root.set("format", kProjectMagic);
    root.set("version", kProjectFormatVersion);
    root.set("tempoBpm", p.tempoBpm);
    root.set("timeSignature", Json::Object{{"numerator", p.timeSig.numerator}, {"denominator", p.timeSig.denominator}});
    root.set("ppq", kPPQ);
    root.set("playhead", (long long)p.playhead);
    root.set("loop", Json::Object{{"enabled", p.loopEnabled}, {"start", (long long)p.loopStart}, {"end", (long long)p.loopEnd}});
    root.set("activeTrack", (long long)p.activeTrack);
    root.set("master", Json::Object{{"volume", p.master.volume}, {"balance", p.master.balance}, {"mute", p.master.mute}});
    root.set("harmony", Json::Object{{"root", p.harmony.root}, {"scale", p.harmony.scaleId}, {"chordMode", p.harmony.chordMode}});
    if (!p.instrumentMenu.empty()) {
        Json::Array menu;
        for (auto& k : p.instrumentMenu) menu.push_back(k);
        root.set("instrumentMenu", menu); // plugins shown in the instrument menus (empty/absent = all)
    }
    Json::Array mapping;
    for (int n : p.controllerMapping.notes) mapping.push_back(n);
    root.set("controllerMapping", Json::Object{{"inversion1", mapping[0]}, {"inversion2", mapping[1]},
                                               {"seventh", mapping[2]}, {"ninth", mapping[3]}});

    Json::Array tracks;
    for (auto& t : p.tracks()) {
        Json jt;
        jt.set("id", (long long)t->id());
        jt.set("name", t->name);
        jt.set("mute", t->mute);
        jt.set("solo", t->solo);
        jt.set("volume", t->volume);
        jt.set("pan", t->pan);
        if (t->colour) jt.set("colour", (long long)t->colour);
        {
            Json::Array bands;
            for (auto& b : t->eq.bands)
                bands.push_back(Json::Object{{"type", (int)b.type}, {"freq", b.freq}, {"gain", b.gainDb}, {"q", b.q}, {"enabled", b.enabled}});
            jt.set("eq", Json::Object{{"enabled", t->eq.enabled}, {"bands", bands}});
            Json::Array ins;
            for (auto& i : t->inserts)
                ins.push_back(Json::Object{{"format", i.format}, {"identifier", i.identifier}, {"name", i.name},
                                           {"manufacturer", i.manufacturer}, {"state", i.stateBase64}, {"bypassed", i.bypassed},
                                           {"uid", (long long)i.uid}});
            jt.set("inserts", ins);
            jt.set("insertsAfterBuiltins", t->insertsAfterBuiltins);
            const auto& c = t->compressor;
            jt.set("compressor", Json::Object{{"enabled", c.enabled}, {"rms", c.rms}, {"threshold", c.thresholdDb}, {"ratio", c.ratio},
                                              {"knee", c.kneeDb}, {"attack", c.attackMs}, {"release", c.releaseMs}, {"makeup", c.makeupDb}});
            const auto& r = t->reverb;
            jt.set("reverb", Json::Object{{"enabled", r.enabled}, {"mix", r.mix}, {"size", r.size}, {"damping", r.damping},
                                          {"width", r.width}, {"preDelay", r.preDelayMs}});
        }
        if (auto* m = dynamic_cast<const MidiTrack*>(t.get())) {
            jt.set("type", "midi");
            jt.set("channel", m->channel);
            jt.set("plugin", Json::Object{{"format", m->plugin.format}, {"identifier", m->plugin.identifier},
                                          {"name", m->plugin.name}, {"manufacturer", m->plugin.manufacturer},
                                          {"state", m->plugin.stateBase64}, {"bypassed", m->plugin.bypassed}});
            Json::Array notes;
            for (auto& n : m->notes()) {
                Json::Array jn{(long long)n.start, (long long)n.length, n.pitch, n.velocity, n.channel};
                if (n.humanized) jn.push_back(1); // optional 6th value: already had Human Touch
                notes.push_back(jn);
            }
            jt.set("notes", notes);
            if (!m->sustain.empty()) {
                Json::Array sus;
                for (auto& v : m->sustain) sus.push_back(Json::Array{(long long)v.tick, v.gain});
                jt.set("sustain", sus);
            }
            if (m->isStepLine()) {
                Json::Array steps;
                for (auto v : m->step.steps) steps.push_back((int)v);
                jt.set("stepGroup", (long long)m->stepGroup);
                jt.set("step", Json::Object{{"pitch", m->step.pitch}, {"stepTicks", (long long)m->step.stepTicks},
                                            {"barTicks", (long long)m->step.barTicks}, {"bars", m->step.bars},
                                            {"start", (long long)m->step.startTick}, {"steps", steps},
                                            {"title", m->step.title}});
            }
        } else if (auto* a = dynamic_cast<const AudioTrack*>(t.get())) {
            jt.set("type", "audio");
            jt.set("input", a->input);
            jt.set("stereoInput", a->stereoInput);
            jt.set("armed", a->armed);
            jt.set("monitor", a->monitor);
            Json::Array clips;
            for (auto& c : a->clips())
                clips.push_back(Json::Object{{"file", toStoredPath(c.file, baseDir)}, {"name", c.name}, {"start", (long long)c.start},
                                             {"offset", c.offset}, {"length", c.length}, {"gain", c.gain}});
            jt.set("clips", clips);
            jt.set("showVolumeLine", a->showVolumeLine);
            Json::Array vol;
            for (auto& v : a->volumeAutomation) vol.push_back(Json::Array{(long long)v.tick, v.gain});
            jt.set("volumeAutomation", vol);
        }
        tracks.push_back(jt);
    }
    root.set("tracks", tracks);
    return root.dump(1);
}

LoadResult ProjectSerializer::fromString(const std::string& text, Project& out, const std::string& baseDir)
{
    LoadResult r;
    Json root;
    try { root = Json::parse(text); } catch (const std::exception& e) { r.error = e.what(); return r; }
    if (root["format"].str() != kProjectMagic) { r.error = "Not a project file"; return r; }
    r.fileVersion = (int)root["version"].num(0);
    r.newerThanApp = r.fileVersion > kProjectFormatVersion;

    Project p;
    p.tempoBpm = root["tempoBpm"].num(120.0);
    if (p.tempoBpm < 10 || p.tempoBpm > 999) p.tempoBpm = 120.0;
    p.timeSig.numerator = (int)root["timeSignature"]["numerator"].num(4);
    p.timeSig.denominator = (int)root["timeSignature"]["denominator"].num(4);
    if (p.timeSig.numerator < 1) p.timeSig.numerator = 4;
    if (p.timeSig.denominator < 1) p.timeSig.denominator = 4;
    const double ppq = root["ppq"].num(kPPQ);
    auto tick = [&](const Json& j, double def) { return (Tick)(j.num(def) * kPPQ / (ppq > 0 ? ppq : kPPQ)); };
    p.playhead = tick(root["playhead"], 0);
    p.loopEnabled = root["loop"]["enabled"].boolean(false);
    p.loopStart = tick(root["loop"]["start"], 0);
    p.loopEnd = tick(root["loop"]["end"], 16 * ppq);
    p.master.volume = std::clamp((float)root["master"]["volume"].num(1.0), 0.0f, 2.0f);
    p.master.mute = root["master"]["mute"].boolean(false);
    p.master.balance = std::clamp((float)root["master"]["balance"].num(0.0), -1.0f, 1.0f);
    p.harmony.root = ((int)root["harmony"]["root"].num(9) % 12 + 12) % 12;
    p.harmony.scaleId = root["harmony"]["scale"].str("natural_minor");
    p.harmony.chordMode = root["harmony"]["chordMode"].boolean(false);
    p.instrumentMenu.clear();
    for (auto& k : root["instrumentMenu"].arr())
        if (!k.str("").empty()) p.instrumentMenu.insert(k.str(""));
    auto& cm = root["controllerMapping"];
    const char* keys[] = {"inversion1", "inversion2", "seventh", "ninth"};
    for (int i = 0; i < 4; ++i) p.controllerMapping.notes[i] = (int)cm[keys[i]].num(p.controllerMapping.notes[i]);

    for (auto& jt : root["tracks"].arr()) {
        const auto id = (TrackId)jt["id"].num((double)p.nextTrackId());
        std::unique_ptr<Track> t;
        if (jt["type"].str("midi") == "audio") {
            auto a = std::make_unique<AudioTrack>(id, jt["name"].str("Audio"));
            a->input = std::clamp((int)jt["input"].num(0), -1, 63);
            a->stereoInput = jt["stereoInput"].boolean(false);
            a->armed = jt["armed"].boolean(false);
            a->monitor = jt["monitor"].boolean(false);
            std::vector<AudioClip> clips;
            for (auto& jc : jt["clips"].arr()) {
                AudioClip c;
                c.file = fromStoredPath(jc["file"].str(), baseDir);
                c.name = jc["name"].str();
                c.start = tick(jc["start"], 0);
                c.offset = jc["offset"].num(0);
                c.length = jc["length"].num(0);
                c.gain = (float)jc["gain"].num(1.0);
                if (!c.file.empty() && c.length > 0) clips.push_back(std::move(c));
            }
            a->setClips(std::move(clips));
            a->showVolumeLine = jt["showVolumeLine"].boolean(false);
            for (auto& jv : jt["volumeAutomation"].arr()) {
                auto& v = jv.arr();
                if (v.size() >= 2) a->volumeAutomation.push_back({tick(v[0], 0), (float)v[1].num(1.0)});
            }
            normalizeAutomation(a->volumeAutomation);
            t = std::move(a);
        } else {
            auto m = std::make_unique<MidiTrack>(id, jt["name"].str("Track"));
            m->channel = (int)jt["channel"].num(0) & 15;
            auto& jp = jt["plugin"];
            m->plugin.format = jp["format"].str();
            m->plugin.identifier = jp["identifier"].str();
            m->plugin.name = jp["name"].str();
            m->plugin.manufacturer = jp["manufacturer"].str();
            m->plugin.stateBase64 = jp["state"].str();
            m->plugin.bypassed = jp["bypassed"].boolean(false);
            for (auto& jn : jt["notes"].arr()) {
                auto& a = jn.arr();
                if (a.size() < 4) continue;
                Note n;
                n.start = tick(a[0], 0);
                n.length = tick(a[1], ppq);
                n.pitch = (int)a[2].num(60);
                n.velocity = (int)a[3].num(100);
                n.channel = a.size() > 4 ? (int)a[4].num(-1) : -1;
                n.humanized = a.size() > 5 && a[5].num(0) != 0;
                m->addNote(n);
            }
            for (auto& jv : jt["sustain"].arr()) {
                auto& v = jv.arr();
                if (v.size() >= 2) m->sustain.push_back({tick(v[0], 0), (float)v[1].num(0.0)});
            }
            normalizeAutomation(m->sustain, 1.0f);
            if (const auto g = (std::uint64_t)jt["stepGroup"].num(0); g > 0) {
                auto& js = jt["step"];
                m->stepGroup = g;
                m->step.pitch = (int)js["pitch"].num(36);
                m->step.stepTicks = std::max<Tick>(1, tick(js["stepTicks"], ppq / 4));
                m->step.startTick = tick(js["start"], 0);
                m->step.title = js["title"].str();
                std::vector<std::uint8_t> steps;
                for (auto& v : js["steps"].arr()) steps.push_back((std::uint8_t)std::clamp((int)v.num(0), 0, 127));
                const Tick bar = (Tick)p.timeSig.numerator * kPPQ * 4 / std::max(1, p.timeSig.denominator);
                if (js["bars"].isNull()) {
                    // 0.3.0 test format: one pattern of numSteps repeated `repeats` times -> independent bars
                    const int numSteps = std::max(1, (int)js["numSteps"].num((double)steps.size()));
                    const int repeats = std::max(1, (int)js["repeats"].num(1));
                    steps.resize((size_t)numSteps, 0);
                    std::vector<std::uint8_t> all;
                    for (int r = 0; r < repeats; ++r) all.insert(all.end(), steps.begin(), steps.end());
                    m->step.barTicks = bar;
                    const Tick len = (Tick)all.size() * m->step.stepTicks;
                    m->step.bars = (int)std::max<Tick>(1, (len + bar - 1) / bar);
                    m->step.steps = std::move(all);
                } else {
                    m->step.barTicks = std::max<Tick>(1, tick(js["barTicks"], (double)bar * ppq / kPPQ));
                    m->step.bars = (int)js["bars"].num(1);
                    m->step.steps = std::move(steps);
                }
                m->regenerateStepNotes();
            }
            t = std::move(m);
        }
        t->mute = jt["mute"].boolean(false);
        t->solo = jt["solo"].boolean(false);
        t->volume = (float)jt["volume"].num(0.8);
        t->pan = (float)jt["pan"].num(0.0);
        t->colour = (std::uint32_t)jt["colour"].num(0);
        {
            for (auto& ji : jt["inserts"].arr()) {
                PluginReference r;
                r.format = ji["format"].str();
                r.identifier = ji["identifier"].str();
                r.name = ji["name"].str();
                r.manufacturer = ji["manufacturer"].str();
                r.stateBase64 = ji["state"].str();
                r.bypassed = ji["bypassed"].boolean(false);
                r.uid = (std::uint64_t)std::clamp(ji["uid"].num(0), 0.0, 9.0e15);
                if (!r.empty()) t->inserts.push_back(std::move(r));
            }
            // ids must be unique within the track (old files have none)
            std::set<std::uint64_t> seen;
            for (auto& r : t->inserts) if (r.uid != 0 && !seen.insert(r.uid).second) r.uid = 0;
            assignInsertIds(t->inserts);
            t->insertsAfterBuiltins = jt["insertsAfterBuiltins"].boolean(false);
            const auto& je = jt["eq"];
            t->eq.enabled = je["enabled"].boolean(false);
            for (auto& jb : je["bands"].arr()) {
                if ((int)t->eq.bands.size() >= EqSettings::kMaxBands) break;
                EqBand b;
                b.type = (EqBand::Type)std::clamp((int)jb["type"].num(0), 0, 4);
                b.freq = std::clamp((float)jb["freq"].num(1000), 20.0f, 20000.0f);
                b.gainDb = std::clamp((float)jb["gain"].num(0), -24.0f, 24.0f);
                b.q = std::clamp((float)jb["q"].num(1), 0.1f, 18.0f);
                b.enabled = jb["enabled"].boolean(true);
                t->eq.bands.push_back(b);
            }
            const auto& jc = jt["compressor"];
            auto& c = t->compressor;
            const CompressorSettings d;
            c.enabled = jc["enabled"].boolean(false);
            c.rms = jc["rms"].boolean(d.rms);
            c.thresholdDb = std::clamp((float)jc["threshold"].num(d.thresholdDb), -60.0f, 0.0f);
            c.ratio = std::clamp((float)jc["ratio"].num(d.ratio), 1.0f, 20.0f);
            c.kneeDb = std::clamp((float)jc["knee"].num(d.kneeDb), 0.0f, 24.0f);
            c.attackMs = std::clamp((float)jc["attack"].num(d.attackMs), 0.1f, 200.0f);
            c.releaseMs = std::clamp((float)jc["release"].num(d.releaseMs), 5.0f, 2000.0f);
            c.makeupDb = std::clamp((float)jc["makeup"].num(d.makeupDb), 0.0f, 24.0f);
            const auto& jr = jt["reverb"];
            auto& r = t->reverb;
            const ReverbSettings dr;
            r.enabled = jr["enabled"].boolean(false);
            r.mix = std::clamp((float)jr["mix"].num(dr.mix), 0.0f, 1.0f);
            r.size = std::clamp((float)jr["size"].num(dr.size), 0.0f, 1.0f);
            r.damping = std::clamp((float)jr["damping"].num(dr.damping), 0.0f, 1.0f);
            r.width = std::clamp((float)jr["width"].num(dr.width), 0.0f, 1.0f);
            r.preDelayMs = std::clamp((float)jr["preDelay"].num(dr.preDelayMs), 0.0f, 100.0f);
        }
        if (p.track(id)) continue; // duplicate id: skip rather than corrupt
        p.insertTrack((int)p.tracks().size(), std::move(t));
    }
    p.activeTrack = (TrackId)root["activeTrack"].num(0);
    if (!p.track(p.activeTrack)) p.activeTrack = p.tracks().empty() ? 0 : p.tracks().front()->id();

    out = p;
    r.ok = true;
    return r;
}

bool ProjectSerializer::saveFile(const Project& p, const std::string& path, std::string* error)
{
    namespace fs = std::filesystem;
    const fs::path target = pathOf(path);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (error) *error = "Cannot write " + utf8(tmp); return false; }
        f << toString(p, utf8(target.parent_path()));
        if (!f) { if (error) *error = "Write failed"; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) { // e.g. Windows when target exists
        fs::remove(target, ec);
        fs::rename(tmp, target, ec);
        if (ec) { if (error) *error = ec.message(); return false; }
    }
    return true;
}

LoadResult ProjectSerializer::loadFile(const std::string& path, Project& out)
{
    std::ifstream f(pathOf(path), std::ios::binary);
    if (!f) return {false, "Cannot open " + path};
    std::stringstream ss;
    ss << f.rdbuf();
    return fromString(ss.str(), out, utf8(pathOf(path).parent_path()));
}

} // namespace mc::io
