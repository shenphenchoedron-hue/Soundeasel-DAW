#!/usr/bin/env bash
# Packages the Release build as a .deb (Debian/Ubuntu).
# Usage: scripts/make-deb.sh <version> <output-dir>
set -euo pipefail
VERSION="${1#v}"
OUT="${2:-dist}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/src/app/SoundeaselDAW_artefacts/Release/Soundeasel DAW"
ARCH="$(dpkg --print-architecture)"
PKG="$(mktemp -d)/soundeasel-daw"

install -Dm755 "$BIN" "$PKG/usr/lib/soundeasel-daw/soundeasel-daw"
mkdir -p "$PKG/usr/bin"
ln -s ../lib/soundeasel-daw/soundeasel-daw "$PKG/usr/bin/soundeasel-daw"
install -Dm644 "$ROOT/resources/icon.png" "$PKG/usr/share/icons/hicolor/512x512/apps/soundeasel-daw.png"
install -Dm644 "$ROOT/LICENSE" "$PKG/usr/share/doc/soundeasel-daw/copyright"
# Built-in instruments (GeneralUser GS SoundFont) + its license
ART="$(dirname "$BIN")"
install -Dm644 "$ART/GeneralUser-GS.sf2" "$PKG/usr/share/soundeasel-daw/GeneralUser-GS.sf2"
install -Dm644 "$ART/GeneralUser-GS-LICENSE.txt" "$PKG/usr/share/soundeasel-daw/GeneralUser-GS-LICENSE.txt"
install -Dm644 "$ART/GeneralUser-GS-LICENSE.txt" "$PKG/usr/share/doc/soundeasel-daw/GeneralUser-GS-LICENSE.txt"
install -Dm644 "$ART/Valiant_Violin_V2.sf2" "$PKG/usr/share/soundeasel-daw/Valiant_Violin_V2.sf2"
install -Dm644 "$ART/Valiant-Violin-LICENSE.txt" "$PKG/usr/share/doc/soundeasel-daw/Valiant-Violin-LICENSE.txt"
install -Dm644 "$ART/Soundeasel-DAW-Manual.pdf" "$PKG/usr/share/doc/soundeasel-daw/Soundeasel-DAW-Manual.pdf"

mkdir -p "$PKG/usr/share/applications"
cat > "$PKG/usr/share/applications/soundeasel-daw.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Soundeasel DAW
Comment=Easy digital audio workstation: instruments, step sequencer, audio tracks and mixer
Exec=soundeasel-daw %f
Icon=soundeasel-daw
Terminal=false
Categories=AudioVideo;Audio;Midi;Music;
MimeType=audio/midi;audio/x-midi;audio/x-wav;audio/flac;
DESK

mkdir -p "$PKG/DEBIAN"
cat > "$PKG/DEBIAN/control" <<CTRL
Package: soundeasel-daw
Version: $VERSION
Section: sound
Priority: optional
Architecture: $ARCH
Depends: libasound2 | libasound2t64, libfreetype6, libfontconfig1, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1, libgtk-3-0 | libgtk-3-0t64
Maintainer: shenphenchoedron-hue <shenphenchoedron-hue@users.noreply.github.com>
Homepage: https://github.com/shenphenchoedron-hue/Soundeasel-DAW
Description: Easy digital audio workstation
 Instrument tracks with piano roll and step sequencer, audio tracks
 (record and import audio), a mixer with Mains, MIDI keyboard input,
 built-in band instruments (GeneralUser GS), SoundFont (.sf2) and
 instrument plugins (VST3/LV2), and diatonic chord input with inversions
 and 7th/9th chords.
CTRL

mkdir -p "$OUT"
dpkg-deb --build --root-owner-group "$PKG" "$OUT/soundeasel-daw_${VERSION}_${ARCH}.deb"
