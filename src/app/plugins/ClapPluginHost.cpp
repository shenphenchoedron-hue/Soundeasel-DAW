#include "plugins/ClapPluginHost.h"
#include <clap/clap.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#if JUCE_LINUX || JUCE_BSD
 #include <dlfcn.h>
#endif

namespace mc::plugins {

namespace {

// ---------------------------------------------------------------- bundles
// A CLAP bundle is loaded once and its entry initialised once; it is never unloaded
// (plugins often leave threads behind, unloading would crash the app later).
struct Bundle {
    const clap_plugin_entry_t* entry = nullptr;
    juce::String error;
};

juce::File binaryOf(const juce::File& bundle)
{
   #if JUCE_MAC
    if (bundle.isDirectory()) {
        const auto macos = bundle.getChildFile("Contents/MacOS");
        for (auto& f : macos.findChildFiles(juce::File::findFiles, false)) return f;
    }
   #endif
    return bundle;
}

const Bundle& loadBundle(const juce::String& path)
{
    static std::mutex m;
    static std::map<juce::String, Bundle> bundles;
    std::lock_guard l(m);
    if (auto it = bundles.find(path); it != bundles.end()) return it->second;
    auto& b = bundles[path];
    const auto bin = binaryOf(juce::File(path));
    void* sym = nullptr;
   #if JUCE_LINUX || JUCE_BSD
    // RTLD_NODELETE: the library is never unmapped, even if something closes it.
    void* handle = dlopen(bin.getFullPathName().toRawUTF8(), RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
    if (!handle) { b.error = "Cannot load: " + juce::String(dlerror()); return b; }
    sym = dlsym(handle, "clap_entry");
   #else
    auto* lib = new juce::DynamicLibrary(); // intentionally leaked: stays loaded
    if (!lib->open(bin.getFullPathName())) { b.error = "Cannot load " + bin.getFullPathName(); return b; }
    sym = lib->getFunction("clap_entry");
   #endif
    auto* entry = static_cast<const clap_plugin_entry_t*>(sym);
    if (!entry) { b.error = "Not a CLAP plugin (no clap_entry)"; return b; }
    if (!clap_version_is_compatible(entry->clap_version)) { b.error = "Incompatible CLAP version"; return b; }
    if (!entry->init(path.toRawUTF8())) { b.error = "The plugin failed to initialise"; return b; }
    b.entry = entry;
    return b;
}

const clap_plugin_factory_t* factoryOf(const Bundle& b)
{
    return b.entry ? static_cast<const clap_plugin_factory_t*>(b.entry->get_factory(CLAP_PLUGIN_FACTORY_ID)) : nullptr;
}

bool hasFeature(const clap_plugin_descriptor_t* d, const char* feature)
{
    if (!d->features) return false;
    for (auto f = d->features; *f; ++f) if (std::strcmp(*f, feature) == 0) return true;
    return false;
}

template <typename F>
void onMessageThread(F&& f)
{
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm != nullptr && !mm->isThisTheMessageThread())
        mm->callFunctionOnMessageThread([](void* p) -> void* { (*static_cast<F*>(p))(); return nullptr; }, &f);
    else
        f();
}

// ---------------------------------------------------------------- instance
class ClapInstance;

class ClapEditor final : public juce::Component, private juce::Timer {
public:
    ClapEditor(ClapInstance& o, const char* api, bool floating, int w, int h);
    ~ClapEditor() override;
    void resized() override;
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff20252c));
        if (floating) {
            g.setColour(juce::Colours::white);
            g.drawFittedText("The plugin opened its own window.", getLocalBounds().reduced(10), juce::Justification::centred, 2);
        }
    }
    void pluginRequestedSize(int w, int h) { setSize(w, h); }

private:
    void timerCallback() override; // attaches to the native window once it exists
    ClapInstance& owner;
    const char* api;
    bool floating, attached = false;
};

class ClapInstance final : public InstrumentPlugin, private juce::Timer {
public:
    ClapInstance(const juce::String& path, const juce::String& id, bool effect, juce::String& error) : effect_(effect)
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "Soundeasel DAW";
        host.vendor = "Soundeasel";
        host.url = "https://github.com/shenphenchoedron-hue/Soundeasel-DAW";
        host.version = "0.4";
        host.get_extension = [](const clap_host_t* h, const char* ext) -> const void* { return self(h)->hostExtension(ext); };
        // These may be called from the audio thread: only set flags, a message-thread timer acts on them.
        host.request_restart = [](const clap_host_t* h) { self(h)->restartRequested = true; };
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t* h) { self(h)->callbackRequested = true; };
        setupHostExtensions();

        const auto& bundle = loadBundle(path);
        auto* factory = factoryOf(bundle);
        if (!factory) { error = bundle.error.isNotEmpty() ? bundle.error : juce::String("No plugin factory"); return; }
        plugin = factory->create_plugin(factory, &host, id.toRawUTF8());
        if (!plugin) { error = "The plugin could not be created"; return; }
        if (!plugin->init(plugin)) { error = "The plugin failed to initialise"; plugin->destroy(plugin); plugin = nullptr; return; }
        name_ = plugin->desc && plugin->desc->name ? plugin->desc->name : "CLAP plugin";
        gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        timerExt = static_cast<const clap_plugin_timer_support_t*>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
        fdExt = static_cast<const clap_plugin_posix_fd_support_t*>(plugin->get_extension(plugin, CLAP_EXT_POSIX_FD_SUPPORT));
        hasEditor_ = gui && (gui->is_api_supported(plugin, windowApi(), false) || gui->is_api_supported(plugin, windowApi(), true));
        startTimerHz(30);
    }

    ~ClapInstance() override
    {
        stopTimer();
        timers.clear();
       #if JUCE_LINUX || JUCE_BSD
        for (int fd : fds) juce::LinuxEventLoop::unregisterFdCallback(fd);
       #endif
        if (!plugin) return;
        deactivate();
        plugin->destroy(plugin);
    }

    bool ok() const { return plugin != nullptr; }

    std::string name() const override { return name_; }
    std::string format() const override { return "CLAP"; }
    bool hasEditor() const override { return hasEditor_; }

    // Called when the audio device (re)starts: the audio thread is not processing.
    void prepare(double sr, int bs) override
    {
        const juce::SpinLock::ScopedLockType l(processLock);
        deactivateLocked();
        sampleRate = sr;
        maxBlock = std::max(1, bs);
        if (!plugin->activate(plugin, sr, 1, (uint32_t)maxBlock)) return;
        active = true;
        setupPorts();
    }

    void prepareToRemove() override { stopOnAudioThread(); }

    void release() override { deactivate(); }

    juce::Component* createEditor() override
    {
        if (!hasEditor_ || editor) return nullptr;
        bool floating = !gui->is_api_supported(plugin, windowApi(), false);
        if (!gui->create(plugin, windowApi(), floating)) return nullptr;
        uint32_t w = 640, h = 400;
        if (!floating) gui->get_size(plugin, &w, &h);
        else { gui->suggest_title(plugin, name_.c_str()); gui->show(plugin); w = 360; h = 80; }
        editor = new ClapEditor(*this, windowApi(), floating, (int)w, (int)h);
        return editor;
    }

    juce::MemoryBlock getState() override
    {
        juce::MemoryBlock m;
        if (!state) return m;
        juce::MemoryOutputStream out(m, false);
        clap_ostream_t s{&out, [](const clap_ostream_t* st, const void* buf, uint64_t size) -> int64_t {
            return static_cast<juce::MemoryOutputStream*>(st->ctx)->write(buf, (size_t)size) ? (int64_t)size : -1;
        }};
        state->save(plugin, &s);
        out.flush();
        return m;
    }

    void setState(const juce::MemoryBlock& m) override
    {
        if (!state || m.getSize() == 0) return;
        juce::MemoryInputStream in(m, false);
        clap_istream_t s{&in, [](const clap_istream_t* st, void* buf, uint64_t size) -> int64_t {
            return (int64_t)static_cast<juce::MemoryInputStream*>(st->ctx)->read(buf, (int)size);
        }};
        state->load(plugin, &s);
    }

    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        const juce::SpinLock::ScopedTryLockType l(processLock);
        if (!l.isLocked() || !active) return;
        const int n = audio.getNumSamples();
        if (n > maxBlock) return;
        audioThread = juce::Thread::getCurrentThreadId();
        lastProcessMs = juce::Time::getMillisecondCounter();
        if (stopRequested.load()) { // the message thread wants to deactivate / remove the plugin
            if (processing) { plugin->stop_processing(plugin); processing = false; }
            return;
        }
        if (!processing) {
            if (!plugin->start_processing(plugin)) return;
            processing = true;
        }
        // inputs: port 0 gets the track audio, other ports are silent
        for (size_t p = 0; p < inPorts.size(); ++p) {
            auto& port = inPorts[p];
            for (uint32_t c = 0; c < port.buf.channel_count; ++c) {
                float* d = port.ptrs[c];
                if (p == 0 && effect_) {
                    if (port.buf.channel_count == 1) {
                        juce::FloatVectorOperations::copyWithMultiply(d, audio.getReadPointer(0), 0.5f, n);
                        juce::FloatVectorOperations::addWithMultiply(d, audio.getReadPointer(1), 0.5f, n);
                    } else {
                        juce::FloatVectorOperations::copy(d, audio.getReadPointer((int)std::min<uint32_t>(c, 1)), n);
                    }
                } else {
                    juce::FloatVectorOperations::clear(d, n);
                }
            }
        }
        for (auto& port : outPorts)
            for (uint32_t c = 0; c < port.buf.channel_count; ++c) juce::FloatVectorOperations::clear(port.ptrs[c], n);

        // events
        numEvents = 0;
        if (noteDialect != 0)
            for (const auto meta : midi) {
                if (numEvents >= events.size()) break;
                const auto m = meta.getMessage();
                auto& e = events[numEvents];
                const auto time = (uint32_t)juce::jlimit(0, n - 1, meta.samplePosition);
                if (noteDialect == CLAP_NOTE_DIALECT_CLAP && (m.isNoteOn() || m.isNoteOff())) {
                    e.note.header = {sizeof(clap_event_note_t), time, CLAP_CORE_EVENT_SPACE_ID,
                                     (uint16_t)(m.isNoteOn() ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
                    e.note.note_id = -1;
                    e.note.port_index = 0;
                    e.note.channel = (int16_t)(m.getChannel() - 1);
                    e.note.key = (int16_t)m.getNoteNumber();
                    e.note.velocity = m.getFloatVelocity();
                } else if (m.getRawDataSize() <= 3) {
                    e.midi.header = {sizeof(clap_event_midi_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                    e.midi.port_index = 0;
                    std::memset(e.midi.data, 0, 3);
                    std::memcpy(e.midi.data, m.getRawData(), (size_t)m.getRawDataSize());
                } else {
                    continue;
                }
                ++numEvents;
            }
        clap_input_events_t in{this, [](const clap_input_events_t* l) -> uint32_t { return (uint32_t)static_cast<ClapInstance*>(l->ctx)->numEvents; },
                               [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
                                   return &static_cast<ClapInstance*>(l->ctx)->events[i].note.header; }};
        clap_output_events_t out{this, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};

        clap_process_t pr{};
        pr.steady_time = steadyTime;
        pr.frames_count = (uint32_t)n;
        pr.transport = nullptr;
        pr.audio_inputs = inBufs.data();
        pr.audio_inputs_count = (uint32_t)inBufs.size();
        pr.audio_outputs = outBufs.data();
        pr.audio_outputs_count = (uint32_t)outBufs.size();
        pr.in_events = &in;
        pr.out_events = &out;
        const auto status = plugin->process(plugin, &pr);
        steadyTime += n;
        if (status == CLAP_PROCESS_ERROR) return;

        // output port 0 replaces the track audio (mono output goes to both sides)
        if (outPorts.empty() || outPorts[0].buf.channel_count == 0) {
            if (!effect_) audio.clear();
            return;
        }
        const auto& o = outPorts[0];
        for (int c = 0; c < audio.getNumChannels(); ++c)
            juce::FloatVectorOperations::copy(audio.getWritePointer(c), o.ptrs[std::min<uint32_t>((uint32_t)c, o.buf.channel_count - 1)], n);
    }

    // ---- used by the editor
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    ClapEditor* editor = nullptr;

    static const char* windowApi()
    {
       #if JUCE_MAC
        return CLAP_WINDOW_API_COCOA;
       #elif JUCE_WINDOWS
        return CLAP_WINDOW_API_WIN32;
       #else
        return CLAP_WINDOW_API_X11;
       #endif
    }

private:
    static ClapInstance* self(const clap_host_t* h) { return static_cast<ClapInstance*>(h->host_data); }

    struct Port {
        clap_audio_buffer_t buf{};
        std::vector<float*> ptrs;
    };
    union Event {
        clap_event_note_t note;
        clap_event_midi_t midi;
    };

    // Asks the audio thread to call stop_processing (CLAP: [audio-thread]) and waits for it,
    // as long as the audio thread is actually calling process().
    void stopOnAudioThread()
    {
        if (!processing.load()) return;
        stopRequested = true;
        const auto start = juce::Time::getMillisecondCounter();
        while (processing.load()) {
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastProcessMs.load() > 100 || now - start > 500) break; // audio thread idle or stuck
            juce::Thread::sleep(1);
        }
    }

    void deactivate()
    {
        stopOnAudioThread();
        const juce::SpinLock::ScopedLockType l(processLock);
        deactivateLocked();
    }
    void deactivateLocked()
    {
        if (!plugin) return;
        // Fallback when no audio thread is running (device stopped): the plugin is not processing.
        if (processing) { plugin->stop_processing(plugin); processing = false; }
        if (active) { plugin->deactivate(plugin); active = false; }
        stopRequested = false;
    }

    // Called on the message thread with the process lock held.
    void setupPorts()
    {
        inPorts.clear();
        outPorts.clear();
        noteDialect = 0;
        auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        auto addPorts = [&](bool input, std::vector<Port>& list) {
            const uint32_t count = ports ? ports->count(plugin, input) : (input ? (effect_ ? 1u : 0u) : 1u);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t channels = 2;
                clap_audio_port_info_t info{};
                if (ports && ports->get(plugin, i, input, &info)) channels = std::max<uint32_t>(info.channel_count, 1);
                Port p;
                p.buf.channel_count = channels;
                list.push_back(std::move(p));
            }
        };
        addPorts(true, inPorts);
        addPorts(false, outPorts);
        size_t totalChannels = 0;
        for (auto* list : {&inPorts, &outPorts}) for (auto& p : *list) totalChannels += p.buf.channel_count;
        pool.assign(totalChannels * (size_t)maxBlock, 0.0f);
        size_t offset = 0;
        inBufs.clear();
        outBufs.clear();
        for (auto* list : {&inPorts, &outPorts})
            for (auto& p : *list) {
                p.ptrs.resize(p.buf.channel_count);
                for (auto& ptr : p.ptrs) { ptr = pool.data() + offset; offset += (size_t)maxBlock; }
                p.buf.data32 = p.ptrs.data();
                p.buf.data64 = nullptr;
                p.buf.latency = 0;
                p.buf.constant_mask = 0;
            }
        for (auto& p : inPorts) inBufs.push_back(p.buf);
        for (auto& p : outPorts) outBufs.push_back(p.buf);
        // notes: prefer CLAP note events, else raw MIDI
        if (auto* np = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS)); np && np->count(plugin, true) > 0) {
            clap_note_port_info_t info{};
            if (np->get(plugin, 0, true, &info))
                noteDialect = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) ? CLAP_NOTE_DIALECT_CLAP
                            : (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) ? CLAP_NOTE_DIALECT_MIDI : 0;
        }
    }

    void timerCallback() override
    {
        if (callbackRequested.exchange(false) && plugin) plugin->on_main_thread(plugin);
        if (restartRequested.exchange(false) && plugin && active) {
            stopOnAudioThread(); // processing must stop on the audio thread before deactivating
            prepare(sampleRate, maxBlock);
        }
    }

    // ---- host extensions
    void setupHostExtensions()
    {
        hostLog.log = [](const clap_host_t*, clap_log_severity sev, const char* msg) {
            if (sev >= CLAP_LOG_WARNING) { DBG("CLAP: " << msg); }
            juce::ignoreUnused(sev, msg);
        };
        hostThreadCheck.is_main_thread = [](const clap_host_t*) { return juce::MessageManager::existsAndIsCurrentThread(); };
        hostThreadCheck.is_audio_thread = [](const clap_host_t* h) { return juce::Thread::getCurrentThreadId() == self(h)->audioThread.load(); };
        hostParams.rescan = [](const clap_host_t*, clap_param_rescan_flags) {};
        hostParams.clear = [](const clap_host_t*, clap_id, clap_param_clear_flags) {};
        hostParams.request_flush = [](const clap_host_t*) {}; // process() runs all the time
        hostState.mark_dirty = [](const clap_host_t*) {};
        hostLatency.changed = [](const clap_host_t*) {};
        hostAudioPorts.is_rescan_flag_supported = [](const clap_host_t*, uint32_t) { return false; };
        hostAudioPorts.rescan = [](const clap_host_t* h, uint32_t) { self(h)->restartRequested = true; };
        hostNotePorts.supported_dialects = [](const clap_host_t*) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; };
        hostNotePorts.rescan = [](const clap_host_t*, uint32_t) {};
        hostGui.resize_hints_changed = [](const clap_host_t*) {};
        hostGui.request_resize = [](const clap_host_t* h, uint32_t w, uint32_t hh) {
            auto* s = self(h);
            juce::Component::SafePointer<juce::Component> ed(s->editor);
            juce::MessageManager::callAsync([ed, w, hh] { if (auto* e = dynamic_cast<ClapEditor*>(ed.getComponent())) e->pluginRequestedSize((int)w, (int)hh); });
            return true;
        };
        hostGui.request_show = [](const clap_host_t*) { return true; };
        hostGui.request_hide = [](const clap_host_t*) { return true; };
        hostGui.closed = [](const clap_host_t*, bool) {};
        hostTimer.register_timer = [](const clap_host_t* h, uint32_t periodMs, clap_id* id) { return self(h)->registerTimer(periodMs, id); };
        hostTimer.unregister_timer = [](const clap_host_t* h, clap_id id) { return self(h)->timers.erase(id) > 0; };
        hostFd.register_fd = [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) { return self(h)->registerFd(fd, flags); };
        hostFd.modify_fd = [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) { self(h)->unregisterFd(fd); return self(h)->registerFd(fd, flags); };
        hostFd.unregister_fd = [](const clap_host_t* h, int fd) { return self(h)->unregisterFd(fd); };
    }

    const void* hostExtension(const char* ext) const
    {
        if (!std::strcmp(ext, CLAP_EXT_LOG)) return &hostLog;
        if (!std::strcmp(ext, CLAP_EXT_THREAD_CHECK)) return &hostThreadCheck;
        if (!std::strcmp(ext, CLAP_EXT_PARAMS)) return &hostParams;
        if (!std::strcmp(ext, CLAP_EXT_STATE)) return &hostState;
        if (!std::strcmp(ext, CLAP_EXT_LATENCY)) return &hostLatency;
        if (!std::strcmp(ext, CLAP_EXT_AUDIO_PORTS)) return &hostAudioPorts;
        if (!std::strcmp(ext, CLAP_EXT_NOTE_PORTS)) return &hostNotePorts;
        if (!std::strcmp(ext, CLAP_EXT_GUI)) return &hostGui;
        if (!std::strcmp(ext, CLAP_EXT_TIMER_SUPPORT)) return &hostTimer;
       #if JUCE_LINUX || JUCE_BSD
        if (!std::strcmp(ext, CLAP_EXT_POSIX_FD_SUPPORT)) return &hostFd;
       #endif
        return nullptr;
    }

    struct PluginTimer final : juce::Timer {
        PluginTimer(ClapInstance& o, clap_id i) : owner(o), id(i) {}
        void timerCallback() override { if (owner.timerExt) owner.timerExt->on_timer(owner.plugin, id); }
        ClapInstance& owner;
        clap_id id;
    };

    bool registerTimer(uint32_t periodMs, clap_id* id)
    {
        if (!timerExt) timerExt = static_cast<const clap_plugin_timer_support_t*>(plugin ? plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT) : nullptr);
        const clap_id newId = nextTimerId++;
        auto t = std::make_unique<PluginTimer>(*this, newId);
        t->startTimer((int)std::max<uint32_t>(periodMs, 5));
        timers[newId] = std::move(t);
        *id = newId;
        return true;
    }

    bool registerFd(int fd, clap_posix_fd_flags_t flags)
    {
       #if JUCE_LINUX || JUCE_BSD
        if (!fdExt) fdExt = static_cast<const clap_plugin_posix_fd_support_t*>(plugin ? plugin->get_extension(plugin, CLAP_EXT_POSIX_FD_SUPPORT) : nullptr);
        short events = 0;
        if (flags & CLAP_POSIX_FD_READ) events |= 0x001;  // POLLIN
        if (flags & CLAP_POSIX_FD_WRITE) events |= 0x004; // POLLOUT
        if (flags & CLAP_POSIX_FD_ERROR) events |= 0x008; // POLLERR
        juce::LinuxEventLoop::registerFdCallback(fd, [this, flags](int f) { if (fdExt) fdExt->on_fd(plugin, f, flags); }, events);
        fds.insert(fd);
        return true;
       #else
        juce::ignoreUnused(fd, flags);
        return false;
       #endif
    }

    bool unregisterFd(int fd)
    {
       #if JUCE_LINUX || JUCE_BSD
        if (!fds.erase(fd)) return false;
        juce::LinuxEventLoop::unregisterFdCallback(fd);
        return true;
       #else
        juce::ignoreUnused(fd);
        return false;
       #endif
    }

    clap_host_t host{};
    clap_host_log_t hostLog{};
    clap_host_thread_check_t hostThreadCheck{};
    clap_host_params_t hostParams{};
    clap_host_state_t hostState{};
    clap_host_latency_t hostLatency{};
    clap_host_audio_ports_t hostAudioPorts{};
    clap_host_note_ports_t hostNotePorts{};
    clap_host_gui_t hostGui{};
    clap_host_timer_support_t hostTimer{};
    clap_host_posix_fd_support_t hostFd{};

    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_timer_support_t* timerExt = nullptr;
    const clap_plugin_posix_fd_support_t* fdExt = nullptr;
    std::map<clap_id, std::unique_ptr<PluginTimer>> timers;
    clap_id nextTimerId = 1;
    std::set<int> fds;

    std::string name_;
    bool effect_ = false, hasEditor_ = false;
    juce::SpinLock processLock;
    bool active = false;
    std::atomic<bool> processing{false}, stopRequested{false};
    std::atomic<juce::uint32> lastProcessMs{0};
    double sampleRate = 44100.0;
    int maxBlock = 512;
    int64_t steadyTime = 0;
    std::atomic<juce::Thread::ThreadID> audioThread{nullptr};
    std::atomic<bool> callbackRequested{false}, restartRequested{false};
    std::vector<Port> inPorts, outPorts;
    std::vector<clap_audio_buffer_t> inBufs, outBufs;
    std::vector<float> pool;
    uint32_t noteDialect = 0;
    std::array<Event, 1024> events{};
    size_t numEvents = 0;
};

// ---------------------------------------------------------------- editor
ClapEditor::ClapEditor(ClapInstance& o, const char* a, bool f, int w, int h) : owner(o), api(a), floating(f)
{
    setOpaque(true);
    setSize(std::max(w, 50), std::max(h, 30));
    if (!floating) startTimer(30);
}

ClapEditor::~ClapEditor()
{
    stopTimer();
    if (owner.gui && owner.plugin) {
        if (!floating && attached) owner.gui->hide(owner.plugin);
        owner.gui->destroy(owner.plugin);
    }
    owner.editor = nullptr;
}

void ClapEditor::timerCallback()
{
    // The plugin view is placed into the native window that shows this component
    // (the editor window's content area, which starts at 0,0).
    auto* peer = getPeer();
    if (!peer || !isShowing()) return;
    stopTimer();
    clap_window_t w{};
    w.api = api;
   #if JUCE_MAC
    w.cocoa = peer->getNativeHandle();
   #elif JUCE_WINDOWS
    w.win32 = peer->getNativeHandle();
   #else
    w.x11 = (clap_xwnd)(juce::pointer_sized_uint)peer->getNativeHandle();
   #endif
    if (owner.gui->set_parent(owner.plugin, &w)) {
        attached = true;
        owner.gui->show(owner.plugin);
    }
}

void ClapEditor::resized()
{
    if (attached && !floating && owner.gui->can_resize(owner.plugin)) {
        uint32_t w = (uint32_t)getWidth(), h = (uint32_t)getHeight();
        if (owner.gui->adjust_size(owner.plugin, &w, &h)) owner.gui->set_size(owner.plugin, w, h);
    }
}

std::vector<std::string> crashedPaths(const juce::File& f)
{
    std::vector<std::string> v;
    juce::StringArray lines;
    if (f.existsAsFile()) lines.addLines(f.loadFileAsString());
    for (auto& l : lines) if (l.trim().isNotEmpty()) v.push_back(l.trim().toStdString());
    return v;
}
} // namespace

// ---------------------------------------------------------------- host
juce::FileSearchPath ClapPluginHost::defaultSearchPaths()
{
    juce::FileSearchPath p;
   #if JUCE_WINDOWS
    p.add(juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory).getChildFile("Common Files/CLAP"));
    p.add(juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Programs/Common/CLAP"));
   #elif JUCE_MAC
    p.add(juce::File("/Library/Audio/Plug-Ins/CLAP"));
    p.add(juce::File("~/Library/Audio/Plug-Ins/CLAP"));
   #else
    p.add(juce::File("~/.clap"));
    p.add(juce::File("/usr/lib/clap"));
    p.add(juce::File("/usr/local/lib/clap"));
   #endif
    // CLAP_PATH: extra folders, like other hosts
    const auto env = juce::SystemStats::getEnvironmentVariable("CLAP_PATH", {});
    for (auto& d : juce::StringArray::fromTokens(env, juce::File::getSeparatorChar() == '\\' ? ";" : ":", ""))
        if (d.isNotEmpty()) p.add(juce::File(d));
    return p;
}

std::vector<PluginInfo> ClapPluginHost::scan(const std::function<bool(float, const juce::String&)>& progress)
{
    std::vector<PluginInfo> out;
    juce::Array<juce::File> bundles;
    const auto paths = defaultSearchPaths();
    for (int i = 0; i < paths.getNumPaths(); ++i) {
        const juce::File dir(paths.getRawString(i));
        if (!dir.isDirectory()) continue;
       #if JUCE_MAC
        bundles.addArray(dir.findChildFiles(juce::File::findDirectories, true, "*.clap"));
       #else
        bundles.addArray(dir.findChildFiles(juce::File::findFiles, true, "*.clap"));
       #endif
    }
    // A plugin that crashes the app while being scanned is written here first and skipped from then on.
    auto pedal = crashedList.getSiblingFile("clap-scan-current.txt");
    if (pedal.existsAsFile()) { // the previous scan crashed on this one
        const auto path = pedal.loadFileAsString().trim();
        if (path.isNotEmpty() && crashedList != juce::File()) crashedList.appendText(path + "\n");
        pedal.deleteFile();
    }
    const auto skip = crashedPaths(crashedList);

    for (int i = 0; i < bundles.size(); ++i) {
        const auto path = bundles[i].getFullPathName();
        if (progress && !progress((float)i / (float)std::max(1, bundles.size()), "CLAP: " + path)) break;
        if (std::find(skip.begin(), skip.end(), path.toStdString()) != skip.end()) continue;
        if (crashedList != juce::File()) pedal.replaceWithText(path);
        onMessageThread([&] {
            try {
                auto* factory = factoryOf(loadBundle(path));
                if (!factory) return;
                const uint32_t count = factory->get_plugin_count(factory);
                for (uint32_t k = 0; k < count; ++k) {
                    const auto* d = factory->get_plugin_descriptor(factory, k);
                    if (!d || !d->id) continue;
                    const bool instrument = hasFeature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT);
                    const bool effect = hasFeature(d, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);
                    if (!instrument && !effect) continue; // note effects, analysers without audio, ...
                    PluginInfo info;
                    info.format = "CLAP";
                    info.identifier = (path + "|" + d->id).toStdString();
                    info.name = d->name ? d->name : d->id;
                    info.manufacturer = d->vendor ? d->vendor : "";
                    juce::StringArray features;
                    if (d->features) for (auto f = d->features; *f; ++f) features.add(*f);
                    info.category = features.joinIntoString(", ").toStdString();
                    info.hostData = path.toStdString();
                    info.isInstrument = instrument;
                    out.push_back(std::move(info));
                }
            } catch (...) {}
        });
        pedal.deleteFile();
    }
    return out;
}

bool ClapPluginHost::stillExists(const PluginInfo& i) const
{
    return juce::File(juce::String(i.identifier).upToLastOccurrenceOf("|", false, false)).exists();
}

std::unique_ptr<InstrumentPlugin> ClapPluginHost::create(const PluginInfo& info, double sr, int bs, juce::String& error)
{
    const juce::String ident(info.identifier);
    const auto path = ident.upToLastOccurrenceOf("|", false, false), id = ident.fromLastOccurrenceOf("|", false, false);
    if (!juce::File(path).exists()) { error = "Plugin file not found: " + path; return nullptr; }
    auto inst = std::make_unique<ClapInstance>(path, id, !info.isInstrument, error);
    if (!inst->ok()) return nullptr;
    inst->prepare(sr, bs);
    return inst;
}

} // namespace mc::plugins
