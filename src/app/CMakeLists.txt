juce_add_gui_app(SoundeaselDAW
    PRODUCT_NAME "Soundeasel DAW"
    COMPANY_NAME "Soundeasel"
    BUNDLE_ID com.soundeasel.daw
    ICON_BIG ${CMAKE_SOURCE_DIR}/resources/icon.png
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Soundeasel DAW records audio from your microphone or audio interface on audio tracks.")

juce_add_binary_data(SoundeaselDAWData SOURCES ${CMAKE_SOURCE_DIR}/resources/logo.svg)

target_sources(SoundeaselDAW PRIVATE
    gui/Theme.cpp
    Main.cpp
    AppContext.cpp
    settings/Settings.cpp
    audio/AudioEngine.cpp
    audio/AudioFileCache.cpp
    audio/AudioExport.cpp
    midi/MidiDeviceManager.cpp
    recording/Recorder.cpp
    plugins/BasicSynth.cpp
    plugins/SoundFontPlayer.cpp
    plugins/JucePluginHost.cpp
    plugins/ClapPluginHost.cpp
    plugins/PluginManager.cpp
    gui/MainComponent.cpp
    gui/TransportBar.cpp
    gui/TrackListComponent.cpp
    gui/PianoRollComponent.cpp
    gui/StepSequencerComponent.cpp
    gui/ChordPanel.cpp
    gui/PluginBrowser.cpp
    gui/SettingsComponent.cpp
    gui/LevelMeter.cpp
    gui/AudioTracksComponent.cpp
    gui/MixerComponent.cpp
    gui/MidiOverviewComponent.cpp
    gui/EffectEditors.cpp
    gui/EqEditor.cpp
    gui/InsertsComponent.cpp
    gui/ExportDialog.cpp
    gui/HumanTouchDialog.cpp
    gui/AutomationLineEditor.cpp
)

# Song export: M4A (AAC) is encoded by macOS itself (Core Audio); elsewhere FFmpeg is used.
if(APPLE)
    target_sources(SoundeaselDAW PRIVATE audio/MacAacEncoder.cpp)
    target_link_libraries(SoundeaselDAW PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()

target_include_directories(SoundeaselDAW PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set(MC_HOST_AU 0)
set(MC_HOST_LV2 0)
if(APPLE)
    set(MC_HOST_AU 1)
endif()
if(UNIX AND NOT APPLE)
    set(MC_HOST_LV2 1)
endif()

# VST2 (legacy) hosting needs Steinberg's VST2 SDK headers, which are no longer
# distributed. Point MC_VST2_SDK_DIR at a folder containing pluginterfaces/vst2.x/aeffect.h
# to enable it.
set(MC_VST2_SDK_DIR "" CACHE PATH "Folder with the VST2 SDK (pluginterfaces/vst2.x) to enable VST2 hosting")
set(MC_HOST_VST2 0)
if(MC_VST2_SDK_DIR AND EXISTS "${MC_VST2_SDK_DIR}/pluginterfaces/vst2.x/aeffect.h")
    set(MC_HOST_VST2 1)
    target_include_directories(SoundeaselDAW PRIVATE "${MC_VST2_SDK_DIR}")
    message(STATUS "VST2 hosting enabled (${MC_VST2_SDK_DIR})")
endif()

# CLAP hosting: header-only CLAP SDK (MIT), vendored in third_party/clap.
target_include_directories(SoundeaselDAW PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include)

target_compile_definitions(SoundeaselDAW PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SoundeaselDAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SoundeaselDAW,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=${MC_HOST_VST2}
    JUCE_PLUGINHOST_AU=${MC_HOST_AU}
    JUCE_PLUGINHOST_LV2=${MC_HOST_LV2}
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SoundeaselDAW PRIVATE
    mc_core
    SoundeaselDAWData
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_devices
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Built-in instruments: GeneralUser GS SoundFont by S. Christian Collins
# (https://www.schristiancollins.com/generaluser). Downloaded from its GitHub
# repository at a pinned commit, verified, and placed next to the executable
# (inside the bundle on macOS). Its license must ship with the app.
option(MC_BUNDLE_SOUNDFONT "Download and bundle the GeneralUser GS SoundFont" ON)
if(MC_BUNDLE_SOUNDFONT)
    set(MC_GU_COMMIT 684543d5e5efaef08d02be50dcda8d552478fa60)
    set(MC_GU_BASE https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/${MC_GU_COMMIT})
    set(MC_SF2 ${CMAKE_BINARY_DIR}/sounds/GeneralUser-GS.sf2)
    set(MC_SF2_LICENSE ${CMAKE_BINARY_DIR}/sounds/GeneralUser-GS-LICENSE.txt)
    if(NOT EXISTS ${MC_SF2})
        message(STATUS "Downloading GeneralUser GS SoundFont (~31 MB)...")
        file(DOWNLOAD ${MC_GU_BASE}/GeneralUser-GS.sf2 ${MC_SF2}
             EXPECTED_HASH SHA256=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe
             TLS_VERIFY ON STATUS mc_dl_status)
        list(GET mc_dl_status 0 mc_dl_code)
        if(NOT mc_dl_code EQUAL 0)
            file(REMOVE ${MC_SF2})
            message(FATAL_ERROR "Could not download GeneralUser GS: ${mc_dl_status}. Configure with -DMC_BUNDLE_SOUNDFONT=OFF to build without built-in sounds.")
        endif()
    endif()
    if(NOT EXISTS ${MC_SF2_LICENSE})
        file(DOWNLOAD ${MC_GU_BASE}/documentation/LICENSE.txt ${MC_SF2_LICENSE}
             EXPECTED_HASH SHA256=7b32efefdf95ce38a043799f0659853ddc00fbaa14d8c50f0aca16b9b8b405be TLS_VERIFY ON)
    endif()

    set(MC_BUNDLED_FILES ${MC_SF2} ${MC_SF2_LICENSE})
endif()

# Built-in violin: Valiant Violin v2 GM by Giovanni Valentino with Mike77154 (WTFPL), vendored in resources/sounds.
list(APPEND MC_BUNDLED_FILES ${CMAKE_SOURCE_DIR}/resources/sounds/Valiant_Violin_V2.sf2
                             ${CMAKE_SOURCE_DIR}/resources/sounds/Valiant-Violin-LICENSE.txt)

# English user manual (Options > User manual). Generated by docs/manual/make_manual.py.
list(APPEND MC_BUNDLED_FILES ${CMAKE_SOURCE_DIR}/docs/Soundeasel-DAW-Manual.pdf)

# Bundled files go next to the executable (inside the bundle on macOS).
if(APPLE)
    set(mc_res_dest "$<TARGET_BUNDLE_CONTENT_DIR:SoundeaselDAW>/Resources")
else()
    set(mc_res_dest "$<TARGET_FILE_DIR:SoundeaselDAW>")
endif()
add_custom_command(TARGET SoundeaselDAW POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${mc_res_dest}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MC_BUNDLED_FILES} "${mc_res_dest}"
    VERBATIM)
