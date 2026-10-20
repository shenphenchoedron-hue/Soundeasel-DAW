# Soundeasel DAW

**Soundeasel DAW** is an easy, cross-platform digital audio workstation for Linux, macOS and Windows,
built with C++20, JUCE 8 and CMake. It grew out of *Easy MIDI Composer*.

The goal is a DAW where you never have to hunt for why a track makes no sound:

- Every instrument track gets a built-in instrument automatically, and the instrument is shown on the track
  and in the mixer (red when missing).
- Every track has a peak meter (L and R bars, a dB scale and the peak value), so you can see which track is playing
  and how loud it is.
- New audio tracks listen to the first input of your sound device; press the red record button on the
  track and then *Rec*. The input meter on the track shows that your microphone/instrument arrives.
- The sound status (device, sample rate, inputs) is always shown at the top right; click it to change the
  device or play a test sound. *Options > Play test sound* checks your speakers/headphones.

## Track types
- **Instrument (piano roll)** - MIDI notes with scale-aware chord input.
- **Step sequencer** - drum and pattern lines, each with its own instrument.
- **Audio track** - record from an input, or import WAV, AIFF, FLAC, Ogg Vorbis and MP3 files.

Every track can be copied and pasted (or duplicated) with everything on it: settings, instrument and effects,
notes, step patterns, audio clips, volume and sustain lines. Instrument tracks have a **sustain pedal line** in the
piano roll and **Human Touch**, which gives programmed notes a natural, played feel. See *Usage* below.

## Mixer
*Mixer* (top bar, F3 or *View > Mixer*) shows one channel strip for every track - instrument tracks, every
step sequencer line and audio tracks - plus **Mains**, where all tracks are mixed before going to your speakers.
Each strip shows where its sound comes from (instrument or audio input), pan, mute/solo (and record enable on
audio tracks), two built-in effects, a fader in dB and a level meter.

Built-in channel strip effects (before the fader, in this order; each can be switched off, e.g. when you use a plugin instead):
- **EQ** - parametric EQ with up to 8 bands (bell, low/high shelf, low/high cut). In its settings (**...**)
  double-click the graph to add a band, drag a point for frequency and gain, use the mouse wheel on a point for
  the width (Q), right-click a point to change its type or delete it.
- **COMP** - feed-forward compressor with peak or RMS detector, soft knee, threshold, ratio, attack, release
  and makeup gain; the orange bar under the button shows the gain reduction.
- **REVERB** - algorithmic stereo reverb after Schroeder/Moorer (pre-delay, early reflections, 8 damped comb
  filters and 4 all-pass filters per side) with mix, size, damping, width and pre-delay.

Click **...** next to an effect to open its settings.

Effect plugins (inserts): every strip has a list of effect plugins. **+ FX** lists the scanned effects per format
(VST3, AU on macOS, LV2 on Linux, CLAP); click a plugin's name to open its window, the power button bypasses it,
right-click for move up/down/to top/to bottom, replace, remove and "Plugins after EQ / COMP / REVERB" (default: plugins first).
Plugins are processed from top to bottom: **drag a plugin's name up or down** to change the order (a line shows
where it lands; the list scrolls when you drag to its edge). Reordering can be undone.
Every strip shows all of its plugins (the mixer grows with the longest chain). **FX plugins ▾** in the mixer header
(or **▴** next to **+ FX**) folds the lists on all strips to one line each, e.g. "▸ 3 FX (1 off)" - hover it to see
the chain, click it to unfold again. The choice is remembered.
Plugin lists from older versions only contain instruments; the first time you add an effect you are offered a
rescan. VST2 hosting needs Steinberg's VST2 SDK, which is no longer distributed: configure with
`-DMC_VST2_SDK_DIR=<folder containing pluginterfaces/vst2.x>` to enable it. CLAP plugins (instruments and
effects) are hosted with the CLAP SDK headers in `src/app/third_party/clap` (MIT).

## Built-in instruments
Soundeasel DAW makes sound without any plugins: the most common band
instruments are built in, grouped as **Rock**, **Pop** and **Jazz** under
*Internal* in each track's instrument menu (drums, guitars, basses, piano,
electric piano, organ, ukulele, strings, sax, trumpet, vibraphone, violin).

Want more? Copy SoundFont files (`.sf2`) into the Sounds folder
(*Options → Open Sounds folder*), then *Options → Rescan plugins*. Every preset
appears under *SoundFonts*.

Credits: built-in sounds are **GeneralUser GS** by S. Christian Collins
(https://www.schristiancollins.com/generaluser, license shipped as
`GeneralUser-GS-LICENSE.txt`). The Jazz violin is **Valiant Violin v2 GM** by Giovanni Valentino with
Mike77154 (WTFPL, `resources/sounds`, license shipped as `Valiant-Violin-LICENSE.txt`). SoundFont playback uses **TinySoundFont** by
Bernhard Schelling (MIT). The SoundFont is downloaded at configure time
(`-DMC_BUNDLE_SOUNDFONT=OFF` to skip).

## Build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build            # core unit tests (no GUI/audio/plugins)
```
JUCE is used from `external/JUCE` if present, otherwise fetched (tag 8.0.4; newer JUCE 8 releases also work).
`-DMC_BUILD_APP=OFF` builds only the core library + tests (no JUCE needed).
Linux deps: libasound2-dev libfreetype-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libgtk-3-dev.

## Layout
- `src/core` – pure C++ (no JUCE): `dsp` (channel strip EQ, compressor and reverb), `theory` (Scale/Chord Engine), `model` (Track/MidiTrack/AudioTrack with clips, Project with Mains, undoable Commands),
  `sequencer` (timing, playback snapshots), `input` (ChordInputProcessor, modifier mapping, MIDI Learn), `io` (JSON project format v1, SMF import/export).
- `src/app` – JUCE: `audio` (realtime engine: sequencer, audio clips, input monitoring/recording, channel strips, Mains; audio file cache; offline song export), `midi` (device manager), `recording`, `plugins` (InstrumentPlugin interface; Internal, JUCE [VST3/AU/LV2], CLAP stub hosts), `settings`, `gui`.
- `tests` – unit tests.

## Usage
- Audio tracks: *+ Audio track* (in the AUDIO TRACKS bar below the piano roll, or *+ Track*). Choose the input,
  press ● (record enable), then *Rec*. *Mon* plays the input through the track (use headphones). Drag audio
  files from your file manager onto the panel (onto a track, or the bar for a new track), or *File > Import audio file*.
  Drag clips to move, drag their edges to trim, right-click to split at the playhead, duplicate, change clip gain
  or delete; Del deletes the selected clip.
- Volume line (automation): press **VOL** on an audio track. Click in the lane to split the line at that point,
  then drag a piece up/down - only that piece moves, so the volume changes in steps. Drag a split point up/down
  for a rising or falling volume (the pieces next to it become ramps), or sideways to move it in time (the first
  movement decides which); double-click or right-click it to remove it. The line changes the track's volume over time (before
  the fader, up to +6 dB). Press VOL again to edit clips. While audio is recorded the song plays straight through (no loop).
  Recordings are saved as 24-bit WAV; when you save the project every audio file it uses is copied into
  "<project name> Audio" next to the project file (unsaved projects record into the app's Recordings folder).
- Piano roll: click = add note (or chord in Chord Mode), drag = move, drag left or right edge = resize (the cursor
  changes to a resize arrow over an edge), right-click = delete, Ctrl/Shift-drag = rubber band, Del, Ctrl+A/C/X/V,
  arrows transpose/nudge. Zoom: toolbar buttons *Zoom out/in* (time) and *Lower/Taller* (rows + keyboard),
  `+`/`-` keys, Ctrl+wheel (time), Alt+wheel (height).
- Sustain pedal line: the lane above the velocity lane is the track's sustain pedal (MIDI CC 64): 100 % = pedal down,
  0 % = pedal up. It works like the volume line: click to add a point, drag a piece of the line up/down, drag a point
  up/down to make the line slope (sideways to move it in time), double-click a point to remove it, right-click for
  more. The pedal is released when you stop, jump or loop and put back where the line is when playback continues.
  It is included in song and MIDI export, and CC 64 in imported MIDI files becomes a sustain line.
- Copy and paste tracks: right-click a track card (instrument track, audio track, step sequencer card or mixer strip)
  and choose *Copy track*, *Paste track after this* or *Duplicate track*. The copy gets everything: name, colour,
  volume, pan, mute/solo, EQ/COMP/REVERB, effect plugins and the instrument with their current settings, notes,
  step patterns, audio clips, the volume line and the sustain line. Copying a step sequencer line copies the whole step sequencer.
  *Edit > Copy active track* (Ctrl/Cmd+Shift+C), *Paste track* (Ctrl/Cmd+Shift+V) and *Duplicate active track*
  (Ctrl/Cmd+D) work on the active piano roll track; *+ Track > Paste track* adds the copy at the end. Pasted
  audio tracks are never record-enabled or monitoring. A copied track can also be pasted into another project.
- Human Touch: press **Touch** on an instrument track (or *Edit > Human Touch*) to give its notes random variation.
  Set how many notes are changed (picked at random), *Placement* (100 % = up to one 1/16 note earlier or later),
  *Length* (in % of each note's length) and *Velocity*. Add more notes and use Human Touch again: with *Only notes
  that have not had Human Touch yet* the earlier notes keep their place. Notes on the same key never overlap. One undo step per run.
- Step sequencer: *+ Track* -> *Step sequencer* inserts a step sequencer track. Step sequencers are placed after
  the piano roll (scroll the window to reach them; the piano roll keeps its size), each can be collapsed (▾/▸) and
  coloured (colour dot), with their setup card on the left and the step rows aligned with the piano roll grid.
  *+ Line* adds a line; each line has its own instrument and note. Click steps to toggle, Shift-click = accent,
  right-click = clear, drag to paint. Step size, bars and start bar apply to the whole step sequencer; every bar has
  its own steps. Right-click a bar in the strip above the steps to copy, duplicate, clear or delete it.
- Metronome and count-in: *Click* (or K) turns the metronome on (right-click for the volume); *Count-in* sets
  0/1/2/4 bars of metronome before recording starts. Grid and Snap are in the bar above the piano roll.
- Timeline: click = set position, Shift/right-drag = loop range.
- Export the finished song: *File > Export song* (Ctrl/Cmd+E). Choose **WAV**, **MP3**, **OGG Vorbis**, **FLAC**,
  **MIDI** or **M4A (AAC)**, the sample rate, bit depth or bit rate, the whole song or only the loop region, whether
  reverb and note releases may ring out at the end, and optional normalising (loudest peak at -1 dB). The song is
  rendered offline (faster than real time, with a progress window and Cancel) through copies of all instruments,
  effect plugins and channel strips, so the file sounds exactly like playback through the Mains - mute, solo,
  volume, pan, EQ, compressor, reverb, volume lines and audio clips included. WAV, FLAC and OGG are written by the
  app itself; M4A uses the AAC encoder built into macOS. MP3 (and M4A on Windows/Linux) needs the free encoder
  **LAME** or **FFmpeg** (`brew install lame`, `sudo apt install lame`, or ffmpeg.org); it is found automatically,
  or choose it with *Locate encoder...* the first time. MIDI writes the notes and the sustain pedal (same as *Export MIDI file*).
- Keys: Space play/stop, R record, L loop, C chord mode, F3 mixer, Home return, Ctrl+Z/Y undo/redo, Ctrl+S/O/N, Ctrl+E export,
  Ctrl/Cmd+Shift+C/V copy/paste track, Ctrl/Cmd+D duplicate track.
- Default modifier keys: C1=1st inv, D1=2nd inv, C#1=7, D#1=9 (change/MIDI Learn in Options > Settings).
