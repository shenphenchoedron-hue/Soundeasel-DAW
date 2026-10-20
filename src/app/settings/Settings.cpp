#include "settings/Settings.h"

namespace mc::settings {

Settings::Settings()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "SoundeaselDAW";
    o.filenameSuffix = ".settings";
    o.folderName = "SoundeaselDAW";
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;

    // First start of Soundeasel DAW: take over the settings, plugin list and sounds of MIDI Composer.
    const auto newFile = o.getDefaultFile();
    if (!newFile.existsAsFile()) {
        auto old = o;
        old.applicationName = "MidiComposer";
        old.folderName = "MidiComposer";
        const auto oldFile = old.getDefaultFile();
        if (oldFile.existsAsFile()) {
            const auto dir = newFile.getParentDirectory(), oldDir = oldFile.getParentDirectory();
            dir.createDirectory();
            oldFile.copyFileTo(newFile);
            if (auto cache = oldDir.getChildFile("plugin-cache.json"); cache.existsAsFile()) cache.copyFileTo(dir.getChildFile(cache.getFileName()));
            if (auto sounds = oldDir.getChildFile("Sounds"); sounds.isDirectory()) sounds.copyDirectoryTo(dir.getChildFile("Sounds"));
        }
    }
    props.setStorageParameters(o);
}

juce::File Settings::dataDirectory() const { return file().getFile().getParentDirectory(); }

input::ControllerMapping Settings::controllerMapping() const
{
    input::ControllerMapping m;
    const char* keys[] = {"modInversion1", "modInversion2", "modSeventh", "modNinth"};
    for (int i = 0; i < (int)input::Modifier::Count; ++i) m.notes[i] = file().getIntValue(keys[i], m.notes[i]);
    return m;
}

void Settings::setControllerMapping(const input::ControllerMapping& m)
{
    const char* keys[] = {"modInversion1", "modInversion2", "modSeventh", "modNinth"};
    for (int i = 0; i < (int)input::Modifier::Count; ++i) file().setValue(keys[i], m.notes[i]);
    file().saveIfNeeded();
}

juce::String Settings::midiInputIdentifier() const { return file().getValue("midiInput"); }
void Settings::setMidiInputIdentifier(const juce::String& s) { file().setValue("midiInput", s); file().saveIfNeeded(); }

std::unique_ptr<juce::XmlElement> Settings::audioDeviceState() const { return file().getXmlValue("audioDevice"); }
void Settings::setAudioDeviceState(const juce::XmlElement* x)
{
    if (x) file().setValue("audioDevice", x);
    file().saveIfNeeded();
}

bool Settings::metronome() const { return file().getBoolValue("metronome", false); }
void Settings::setMetronome(bool b) { file().setValue("metronome", b); file().saveIfNeeded(); }
float Settings::metronomeLevel() const { return juce::jlimit(0.0f, 1.0f, (float)file().getDoubleValue("metronomeLevel", 0.6)); }
void Settings::setMetronomeLevel(float v) { file().setValue("metronomeLevel", (double)v); file().saveIfNeeded(); }
int Settings::countInBars() const { return juce::jlimit(0, 8, file().getIntValue("countInBars", 1)); }
void Settings::setCountInBars(int n) { file().setValue("countInBars", n); file().saveIfNeeded(); }

bool Settings::pianoRollVisible() const { return file().getBoolValue("pianoRollVisible", true); }
void Settings::setPianoRollVisible(bool b) { file().setValue("pianoRollVisible", b); file().saveIfNeeded(); }
bool Settings::mixerVisible() const { return file().getBoolValue("mixerVisible", false); }
void Settings::setMixerVisible(bool b) { file().setValue("mixerVisible", b); file().saveIfNeeded(); }
bool Settings::mixerInsertsCollapsed() const { return file().getBoolValue("mixerInsertsCollapsed", false); }
void Settings::setMixerInsertsCollapsed(bool b) { file().setValue("mixerInsertsCollapsed", b); file().saveIfNeeded(); }

juce::File Settings::lastDirectory() const
{
    juce::File f(file().getValue("lastDir"));
    return f.isDirectory() ? f : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

void Settings::setLastDirectory(const juce::File& f) { file().setValue("lastDir", f.getFullPathName()); }

juce::String Settings::exportSetting(const juce::String& key, const juce::String& fallback) const
{
    return file().getValue("export." + key, fallback);
}

void Settings::setExportSetting(const juce::String& key, const juce::String& value)
{
    file().setValue("export." + key, value);
    file().saveIfNeeded();
}

double Settings::humanizeSetting(const juce::String& key, double fallback) const
{
    return file().getDoubleValue("humanize." + key, fallback);
}

void Settings::setHumanizeSetting(const juce::String& key, double value)
{
    file().setValue("humanize." + key, value);
    file().saveIfNeeded();
}

juce::File Settings::encoderPath() const
{
    const auto p = file().getValue("encoderPath");
    return juce::File::isAbsolutePath(p) ? juce::File(p) : juce::File();
}

void Settings::setEncoderPath(const juce::File& f)
{
    file().setValue("encoderPath", f.getFullPathName());
    file().saveIfNeeded();
}

std::vector<std::uint32_t> Settings::colourSwatches() const
{
    std::vector<std::uint32_t> out;
    for (auto& s : juce::StringArray::fromTokens(file().getValue("colourSwatches"), ",", ""))
        if (s.trim().isNotEmpty()) out.push_back((std::uint32_t)s.trim().getHexValue64());
    return out;
}

void Settings::setColourSwatches(const std::vector<std::uint32_t>& v)
{
    juce::StringArray a;
    for (auto c : v) a.add(juce::String::toHexString((juce::int64)c));
    file().setValue("colourSwatches", a.joinIntoString(","));
}

} // namespace mc::settings
