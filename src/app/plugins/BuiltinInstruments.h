#pragma once
// The built-in band instruments, played from the bundled GeneralUser GS
// SoundFont (and a few better single-instrument SoundFonts bundled next to it,
// see `file`). Deliberately short: the most common instruments for rock, pop and
// jazz. Users can add more sounds by dropping .sf2 files into the Sounds folder.
//
// Ids are stable (stored in projects) - never change or reuse them.
#include <array>

namespace mc::plugins {

struct BuiltinInstrument {
    const char* id;        // stable identifier suffix
    const char* group;     // "Rock", "Pop", "Jazz" (shown as sub-menus under Internal)
    const char* name;      // display name
    int bank, preset;      // SoundFont bank/preset (bank 128 = drum kits)
    const char* file = nullptr; // bundled .sf2 file; nullptr = GeneralUser GS
};

inline constexpr const char* kBuiltinSoundFontFile = "GeneralUser-GS.sf2";
inline constexpr const char* kBuiltinSoundFontLicense = "GeneralUser-GS-LICENSE.txt";
// Valiant Violin v2 GM by Giovanni Valentino with Mike77154 (WTFPL): a much better violin than GeneralUser's.
inline constexpr const char* kValiantViolinFile = "Valiant_Violin_V2.sf2";
inline constexpr const char* kValiantViolinLicense = "Valiant-Violin-LICENSE.txt";

inline constexpr std::array<BuiltinInstrument, 22> kBuiltinInstruments{{
    // Rock
    {"rock-drums",        "Rock", "Rock Drums",              128, 16},
    {"rock-dist-guitar",  "Rock", "Distortion Guitar",         0, 30},
    {"rock-od-guitar",    "Rock", "Overdrive Guitar",          0, 29},
    {"rock-clean-guitar", "Rock", "Clean Electric Guitar",     0, 27},
    {"rock-pick-bass",    "Rock", "Electric Bass (Pick)",      0, 34},
    {"rock-organ",        "Rock", "Rock Organ",                0, 18},
    // Pop
    {"pop-drums",         "Pop",  "Pop Drums",               128,  0},
    {"pop-piano",         "Pop",  "Grand Piano",               0,  0},
    {"pop-epiano",        "Pop",  "Electric Piano",            0,  4},
    {"pop-steel-guitar",  "Pop",  "Acoustic Guitar",           0, 25},
    {"pop-ukulele",       "Pop",  "Ukulele",                   8, 24},
    {"pop-finger-bass",   "Pop",  "Electric Bass (Finger)",    0, 33},
    {"pop-strings",       "Pop",  "Strings",                   0, 48},
    // Jazz
    {"jazz-drums",        "Jazz", "Jazz Drums",              128, 32},
    {"jazz-brushes",      "Jazz", "Brush Drums",             128, 40},
    {"jazz-piano",        "Jazz", "Jazz Piano",                0,  0},
    {"jazz-upright-bass", "Jazz", "Upright Bass",              0, 32},
    {"jazz-guitar",       "Jazz", "Jazz Guitar",               0, 26},
    {"jazz-tenor-sax",    "Jazz", "Tenor Sax",                 0, 66},
    {"jazz-trumpet",      "Jazz", "Trumpet",                   0, 56},
    {"jazz-vibraphone",   "Jazz", "Vibraphone",                0, 11},
    {"jazz-violin",       "Jazz", "Violin",                    0, 40, kValiantViolinFile},
}};

} // namespace mc::plugins
