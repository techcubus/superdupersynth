# superdupersynth

A Linux port of **Super Synth**, a Commodore 64 BASIC synthesizer by
**Lawrence Cotton**, published in *Compute!'s Gazette*, May 1986 (Issue 35,
page 72).

Original article on the Internet Archive:
https://archive.org/details/computes.gazette/Compute_Gazette_Issue_35_1986_May/

The port aims to be faithful to the original: variable names, patch
parameters, frequency tables, and screen layout all trace directly back to
the BASIC source.  The SID chip is emulated by reSID; audio goes out via
ALSA.  No additional hardware is required — a computer keyboard is enough
to make sound.

---

## Dependencies

Only tested on Debian/Ubuntu.  The reSID package names are confusing:

```bash
sudo apt install libncurses-dev libasound2-dev libresid-builder-dev libsidplay2-dev
```

- `libresid-builder-dev` provides the shared library (`-lresid-builder`)
- `libsidplay2-dev` provides the C++ header `<resid/sid.h>`
- Both come from sidplay-libs 2.1.1 — **versions must match**
- Do **not** use `libresid-dev` (does not exist on modern systems) or
  `libsidplayfp-dev` (incompatible fork)

If headers or libs are in a non-standard path, edit `Makefile` and
uncomment the `RESID_INC` / `RESID_LIB` lines.

Fedora, Arch, and other distros are untested.  You will likely need to
build reSID from the sidplay-libs 2.1.1 source.

---

## Build

```bash
make
./supersynth
```

Terminal must be at least **80×24**.

---

## Command-line flags

| Flag | Effect |
|------|--------|
| `-s <file>` | Load a scale file on startup (see Microtonal below) |
| `-x` | Force sixel graphics on without capability detection |

---

## Playing

The computer keyboard maps to a two-octave piano layout derived from the
original C64 keyboard matrix:

```
Black keys:   2  3     5  6  7     9  0     -  `
White keys:   Q  W  E  R  T  Y  U  I  O  P  [  ]  \

Black keys:      d  f     h  j  k     ;  '
White keys:   Z  X  C  V  B  N  M  ,  .  /
```

The ZXCV row is one octave above the QWERTY row.  Hold a key to sustain;
the note releases ~120 ms after you lift it (ncurses has no key-up events,
so a timer is used).

### Function keys

| Key | Action |
|-----|--------|
| F1  | Reset to default patch |
| F3  | New random patch |
| F5  | Save patch to file |
| F7  | Load patch from file |
| F9  | Load scale file |
| F11 | Save scale file |
| Enter | Toggle keyboard / values screen |
| ESC | Quit |

---

## MIDI input

An ALSA sequencer port is opened automatically on startup.  The client and
port number are printed to stderr before ncurses starts:

```
MIDI ready — connect with:  aconnect <src_port> 128:0
```

Connect any MIDI controller:

```bash
# list available MIDI ports
aconnect -l

# wire a controller to supersynth
aconnect 24:0 128:0
```

MIDI note-off events are used directly, so the 120 ms timer hack does not
apply to MIDI-sourced notes — attack and release behave properly.

The computer keyboard continues to work alongside MIDI at all times.

---

## Sixel oscilloscope

If your terminal supports sixel graphics (e.g. Konsole, mlterm, xterm
with `-ti vt340`), a 128×128 green-phosphor oscilloscope is drawn to the
right of the keyboard display.

Sixel support is detected automatically via the Primary Device Attributes
query (`ESC[c`).  Use `-x` to force it on if detection fails.

**Known issue:** xterm does not clear the sixel area between frames, so
waveform frames accumulate (ghost).  Konsole redraws correctly.

---

## Patch parameters

Patches are plain text files, 17 integers one per line.  Parameter names
match the original BASIC variables:

| Param | Meaning |
|-------|---------|
| `z`   | Octave mode (1=up, 2–5=normal+filter variants, 6=down) |
| `fl`  | Effect (0=none, 1=vibrato, 2=pulse-width sweep) |
| `w1`/`w2` | Waveform + gate for voices 1 and 2 |
| `at de su re` | ADSR (4-bit each) |
| `po`  | Filter resonance and routing |
| `xt`  | Sync speed (stored, not yet applied) |
| `vi`/`vs` | Vibrato speed and waveform (voice 3 LFO) |
| `db dc dd` | Pulse width high bytes for voices 1–3 |
| `vo`  | Master volume and filter mode |
| `sl`  | Filter sweep limit |

---

## Microtonal / user scales

Frequency tables are built from the original BASIC DATA statements but can
be overridden with a scale file:

```
# one line per key: key_char Hz
q 261.63
w 277.18
...
```

Load with `-s <file>` on the command line, or F9/F11 in-app.  MIDI input
always uses standard equal temperament regardless of the loaded scale.

---

## Known issues and missing features

- **Note release on keyboard** — ncurses has no key-up events.  A 120 ms
  timer is used as a workaround; it works but limits playing style.  MIDI
  input does not have this limitation.
- **`xt` sync speed** — stored and saved/loaded but not applied to the SID.
- **Z=4 pitch sweep** — the original BASIC lines 380–390 sweep the voice 3
  frequency; not yet implemented.
- **Key highlight** — the currently playing key is not highlighted on the
  keyboard screen.
- **ALSA underruns** — rare glitches are possible under CPU load; a
  dedicated audio thread would fix this but is not implemented.
- **Single voice** — currently monophonic.  Multi-SID polyphony (one SID
  instance per MIDI channel) is planned.
