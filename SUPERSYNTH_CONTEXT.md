# SuperSynth — Claude Code Context Document

> Hand this file to Claude Code at the start of any session on this project.
> It covers origin, architecture, current state, known issues, and conventions.

---

## 1. What This Project Is

A **Linux/C port of a Commodore 64 BASIC synthesizer program** called "SUPER-SYNTH."
The original used the C64's **6581 SID chip** for audio and **VIC-II** text mode for UI.
The Linux version replaces those with:

| C64 original | Linux equivalent |
|---|---|
| VIC-II text screen (PETSCII box art) | **ncurses** (80×24 terminal) |
| 6581 SID chip (hardware) | **reSID** (emulation via `libresid-builder`) |
| C64 hardware audio out | **ALSA** (`libasound`) |
| BASIC `PEEK`/`POKE` | Direct register writes via C wrapper |

The goal is **faithful behavior**, not a rewrite. Variable names, data tables, patch
parameters, and screen layout all trace directly back to the BASIC source.

---

## 2. File Map

```
supersynth.c       Main program. C99. All UI, audio loop, patch logic, key handling.
resid_wrap.cpp     C++ file. Thin shim exposing reSID as plain C functions.
Makefile           Build rules. See Section 5 for dep notes.
supersynth.bas     Original C64 BASIC source (reference only, do not modify).
```

---

## 3. Build

### Dependencies (Debian/Ubuntu)

```bash
sudo apt install libncurses-dev libasound2-dev \
                 libresid-builder-dev libsidplay2-dev
```

**Critical package distinction:**
- `libresid-builder-dev` → provides the shared library (`-lresid-builder`)
- `libsidplay2-dev` → provides the C++ header `<resid/sid.h>` (the raw SID class)
- Both come from the **sidplay-libs 2.1.1** source package — versions must match
- The older standalone `libresid-dev` package does **not** exist on modern Ubuntu/Debian
- Do **not** use `libsidplayfp-dev` — that is a different, incompatible fork

### Compile

```bash
make
# Which runs:
#   gcc -O2 -c supersynth.c -o supersynth.o
#   g++ -O2 -c resid_wrap.cpp -o resid_wrap.o
#   g++ supersynth.o resid_wrap.o -lncurses -lasound -lresid-builder -lm -lstdc++ -o supersynth
```

### Run

```bash
./supersynth
# Terminal must be >= 80x24
```

---

## 4. Architecture

### 4.1 Startup sequence

```
main()
  build_freq_tables()   ← pre-compute SID register values for all ~40 notes
  build_key_map()       ← map ncurses keycodes to note indices
  init_alsa()           ← open default PCM, 44100 Hz, mono, S16_LE
  init_sid()            ← create reSID instance, set PAL clock (985248 Hz)
  default_patch()       ← sane initial patch values
  apply_patch()         ← write ADSR + filter registers to SID
  [ncurses init]
  run_main_loop()
```

### 4.2 Main loop (run_main_loop)

Non-blocking `getch()` polled continuously. Every iteration:
1. Handle any keypress (function keys, note keys, ESC)
2. Call `audio_tick()` unconditionally — clocks reSID and writes to ALSA

This means the SID envelope runs even with no key held, which is correct behavior
(release phase continues after key-up).

### 4.3 Audio pipeline

```
audio_tick()
  resid_clock_delta(sid, cycles_per_buffer, buf, &count)
    → internally: SID::clock(delta_t, short *buf, int n)  [reSID C++ API]
  snd_pcm_writei(pcm, buf, count)
```

- Buffer size: `AUDIO_FRAMES 2048` samples
- SID clock: `SID_CLOCK 985248` Hz (PAL)
- Sample rate: `SAMPLE_RATE 44100` Hz
- Mono, signed 16-bit little-endian

### 4.4 reSID wrapper (resid_wrap.cpp)

Exposes 5 plain C functions (declared `extern "C"`):

```c
void *resid_new(void);                                    // allocate + init SID
void  resid_free(void *sid);                              // destroy
void  resid_reset(void *sid);                             // hardware reset
int   resid_write(void *sid, int reg, int val);           // poke a register
void  resid_clock_delta(void *sid, int cycles,            // advance + produce audio
                        short *buf, int *count);
```

**API note for sidplay-libs 2.x:** The `SID` class, `MOS6581`, `SAMPLE_FAST`,
`reg_t`, `reg8`, `cycle_count` are all in the **global namespace** (no `reSID::`
prefix). This differs from the standalone reSID library. Do not add namespace
qualifiers.

---

## 5. SID Register Map

All writes go through `sid_write(int reg, int val)` which clamps to 0–255.
Register offsets are `#define`d constants (base address = 0, implied):

```c
V1_FREQ_LO  0    V2_FREQ_LO  7    V3_FREQ_LO  14
V1_FREQ_HI  1    V2_FREQ_HI  8    V3_FREQ_HI  15
V1_PW_LO    2    V2_PW_LO    9    V3_PW_LO    16
V1_PW_HI    3    V2_PW_HI   10    V3_PW_HI    17
V1_CTRL     4    V2_CTRL    11    V3_CTRL     18
V1_AD       5    V2_AD      12    V3_AD       19
V1_SR       6    V2_SR      13    V3_SR       20
FC_LO      21    FC_HI      22    RES_FILT    23    MODE_VOL  24
```

---

## 6. Patch System

All sound state lives in one `Patch` struct (global `patch`):

```c
typedef struct {
    int z;          // voice mode, 1–6 (see table below)
    int fl;         // effect flag, 0–2 (see table below)
    int w1, w2;     // waveform control byte, voice 1 and 2
                    //   valid values: 17=triangle 33=sawtooth 65=pulse
                    //                129=noise 21=tri+pulse 23=saw+pulse 85=all
    int at, de;     // attack (0–15), decay (0–15)
    int su, re;     // sustain (0–15), release (0–15)
    int po;         // filter resonance+routing byte (240–243)
    int xt;         // sync/sweep step size (1–40)
    int vi;         // vibrato speed — written to V3_FREQ_LO (55–254)
    int vs;         // vibrato waveform — written to V3_FREQ_HI
                    //   valid values: 17 33 65 129
    int db, dc, dd; // pulse-width for voices 1, 2, 3 (1–8)
    int vo;         // volume+filter mode byte: 31=low 45=bandpass 79=high
    int sl;         // sweep step limit (1–255)
} Patch;
```

### Voice modes (patch.z)

| z | Behaviour |
|---|---|
| 1 | Octave up — uses G (×2 freq) table |
| 2 | Normal — uses F table |
| 3 | Normal freq, no filter mod |
| 4 | Normal freq + FC_HI = F1[note] / 0.7 |
| 5 | Normal freq + FC_HI = F1[note] / 2 |
| 6 | Octave down — uses H (÷2 freq) table |

### Effect flags (patch.fl)

| fl | Behaviour |
|----|---|
| 0  | Plain sustain |
| 1  | Vibrato — sweep V1_FREQ_LO during hold |
| 2  | Pulse-width sweep — sweep V2_PW during hold |

### ADSR encoding

```c
AD byte = (at & 0xF) << 4 | (de & 0xF)    // written to V1_AD, V2_AD
SR byte = (su & 0xF) << 4 | (re & 0xF)    // written to V1_SR, V2_SR
```

### Save/load format

Plain text, one integer per line, 17 values in this order:
`z fl w1 w2 at de su re po xt vi vs db dc dd vo sl`

---

## 7. Frequency Tables

Built once at startup in `build_freq_tables()`. Source data is 41 pairs
`{note_index, raw_16bit_SID_freq}` from the original BASIC DATA statements.

For each note index `t` with raw frequency `N`:

```
M  = 1.005        (de-tune factor for voice 2)
K  = 256          (hi/lo split)

N1 = N * M        voice 2 de-tuned normal
N2 = N * 2        octave up
N3 = N * 2 * M    octave up de-tuned
N4 = N / 2        octave down
N5 = N / 2 * M    octave down de-tuned

F1[t]=N/K   F2[t]=N-(F1*K)     // normal, voice 1 hi/lo
F3[t]=N1/K  F4[t]=N1-(F3*K)    // normal, voice 2 hi/lo  (de-tuned)
G1[t]=N2/K  G2[t]=N2-(G1*K)    // octave up, voice 1
G3[t]=N3/K  G4[t]=N3-(G3*K)    // octave up, voice 2
H1[t]=N4/K  H2[t]=N4-(H1*K)    // octave down, voice 1
H3[t]=N5/K  H4[t]=N5-(H3*K)    // octave down, voice 2
```

Note indices are **not** sequential — they are the original C64 key-matrix
positions and go up to 62. Index 0 = unused.

---

## 8. Key Map

`key_to_note[256]` maps ncurses `ch` values (0–255) to note indices.
0 = not a note key. Both upper and lower case map to the same note.

```
Upper white:  Q=30  W=33  E=38  R=41  T=46  Y=49  U=54  I=12  O=23  P=20  [=31  ]=28
Upper black:  2=9   3=14  5=22  6=25  7=36  9=44  0=47  -=39  ==55
Lower white:  A=62  S=59  D=8   F=16  G=19  H=24  J=32  K=35  L=43  ;=48  '=51
Lower black:  Z=18  X=21  C=29  V=34  B=37  N=45  M=50  ,=17  .=62  /=55
```

---

## 9. UI / ncurses

### Screens

Two screens toggled with **Enter**:

**Keyboard screen** (`draw_keyboard_screen`)
- Cyan box-art piano keyboard (approximates PETSCII original)
- Function key hints at top
- One-line patch summary (z, fl, w1, w2, ADSR) at bottom of keyboard area
- Yellow status bar at row 22
- 80×24 minimum terminal size

**Values screen** (`draw_values_screen`)
- Yellow background
- All 17 patch parameters listed with labels
- Press Enter to return

### Color pairs

```c
CP_NORMAL  1   white on black
CP_CYAN    2   cyan on black      (keyboard borders, decorations)
CP_YELLOW  3   yellow on black    (function key hints, status bar)
CP_GREEN   4   green on black     (patch summary text)
CP_WHITE_K 5   black on white     (white piano keys)
CP_BLACK_K 6   white on black     (black piano keys, bold)
CP_TITLE   7   white on blue      (title bar)
CP_ACTIVE  8   black on cyan      (reserved: playing key highlight — not yet used)
```

---

## 10. Known Issues / TODO

- **Note release on key-up is not implemented.** Currently a note only stops when
  a different note is pressed (monophonic legato) or ESC is hit. True key-release
  (gate bit cleared when key is physically released) requires either a second
  `getch` pass or a tracking approach — ncurses doesn't give key-up events
  natively. Options: use a timer thread, or switch to raw terminal + `select()`.

- **FL modes (vibrato, pulse sweep) are not yet wired up** in the main loop.
  `randomise_patch()` sets `fl` but `play_note()` / `run_main_loop()` don't act
  on it yet. The original BASIC swept registers inside a `FOR` loop while checking
  `PEEK(KB)` — needs a hold-while-key-down idiom in C.

- **CP_ACTIVE (key highlight)** is defined but never used. Playing key should
  visually highlight on the ncurses keyboard — not implemented.

- **`xt` (sync speed) parameter** is stored and saved/loaded but not applied to
  the SID. Voice 3 sync sweep was the original's ring/sync effect driver.

- **ALSA is opened non-blocking** (`SND_PCM_NONBLOCK`). Under-runs are handled
  with `snd_pcm_prepare()` but may cause audio glitches. Could be improved with
  a dedicated audio thread.

- **`.` and `/` key collisions** in `build_key_map()` — both `.=62` and `A=62`
  map to the same note index. Not a bug per se (both are valid C64 layout keys
  for that note) but worth reviewing against the original DATA table.

---

## 11. Conventions / Style Notes

- All SID register writes go through `sid_write(reg, val)` — never call
  `resid_write()` directly from main logic.
- `apply_patch()` is the authoritative "flush patch to SID" function. Call it
  after any patch mutation (load, randomise, manual edit).
- Patch fields use the original BASIC variable names (`z`, `fl`, `po`, `vi`,
  `vs`, `db`, `dc`, `dd`, `vo`, `sl`, `xt`) intentionally — makes cross-
  referencing the BASIC source easier.
- The `Patch` struct is a plain global (`patch`). No threading, no locking.
- Don't add `reSID::` namespace qualifiers in `resid_wrap.cpp` — sidplay-libs
  2.x is global namespace.
- Terminal output outside ncurses (e.g. error messages) must happen before
  `initscr()` or after `endwin()`.
