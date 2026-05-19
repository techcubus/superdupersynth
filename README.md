# Super-Synth — C / Linux port
A faithful Linux port of the Commodore 64 BASIC "SUPER-SYNTH" program,
using **ncurses** (VIC-II approximation) and **reSID** (6581 SID emulation)
with **ALSA** for audio output.

---

## Files

| File | Purpose |
|------|---------|
| `supersynth.c` | Main program (C99) |
| `resid_wrap.cpp` | Thin C-callable wrapper around the reSID C++ API |
| `Makefile` | Build rules |

---

## Dependencies

```bash
# Debian / Ubuntu / Mint
sudo apt install libncurses-dev libasound2-dev libresid-dev

# Fedora / RHEL
sudo dnf install ncurses-devel alsa-lib-devel resid-devel

# Arch
sudo pacman -S ncurses alsa-lib resid
```

> **If `libresid-dev` is not in your repos**, build reSID from source:
> ```bash
> # reSID is bundled with the VICE emulator source
> # https://sourceforge.net/projects/vice-emu/
> # Or the standalone mirror: https://github.com/libsidplayfp/resid
> git clone https://github.com/libsidplayfp/resid.git
> cd resid && ./configure && make && sudo make install
> ```

---

## Build

```bash
make
```

If reSID headers/libs are in a non-standard path, edit `Makefile` and
uncomment `RESID_INC` / `RESID_LIB` lines.

---

## Run

```bash
./supersynth
```

Your terminal should be at least **80×24** characters.

---

## Key Map

### Upper keyboard row  (maps to C64 top two rows)

```
Black keys:  2  3     5  6  7     9  0     -  =
White keys:  Q  W  E  R  T  Y  U  I  O  P  [  \  ]
```

### Middle row

```
Black keys:  D  F     H  J  K     :  '
White keys:  A  S  D  F  G  H  J  K  L  ;  '
```

### Bottom row

```
             Z  X  C  V  B  N  M  ,  .  /
```

### Function keys

| Key    | Action |
|--------|--------|
| F1     | Reset to default patch |
| F3     | Randomise sound (new random patch) |
| F5     | Save patch to file |
| F7     | Load patch from file |
| Enter  | Toggle values / keyboard screen |
| ESC    | Quit |

---

## How it works

### Frequency tables  (`build_freq_tables`)
The original DATA statements embed raw 16-bit SID frequency register values
for ~40 notes.  The BASIC pre-computed three octave bands (normal, ×2 up,
÷2 down) plus a de-tuned second voice (×1.005) — all replicated here.

### Voice modes (`patch.z`, 1–6)
| z | Behaviour |
|---|-----------|
| 1 | Octave up (×2 freq) |
| 2 | Normal frequency |
| 3 | Normal + filter cutoff = F_hi |
| 4 | Normal + filter cutoff = F_hi / 0.7 |
| 5 | Normal + filter cutoff = F_hi / 2 |
| 6 | Octave down (÷2 freq) |

### Effect flag (`patch.fl`)
| fl | Behaviour |
|----|-----------|
| 0  | Plain sustain |
| 1  | Vibrato (sweep V1 frequency) |
| 2  | Pulse-width sweep |

### Patch parameters (saved / loaded as plain text)
`z fl w1 w2 at de su re po xt vi vs db dc dd vo sl`

All parameters are integers; meanings match the original BASIC variable names.

---

## Architecture

```
main()
  └─ build_freq_tables()     pre-compute note registers (once)
  └─ build_key_map()         map ncurses key codes → note indices
  └─ init_alsa()             open PCM device
  └─ init_sid()              create reSID instance, set PAL clock
  └─ default_patch()         sane initial settings
  └─ apply_patch()           write ADSR + filter regs to SID
  └─ run_main_loop()
        getch() (non-blocking, 1 ms timeout)
        │
        ├─ F1/F3/F5/F7/Enter/ESC → patch ops / screen switch
        ├─ note key → play_note(t) → write freq + ctrl regs to SID
        │                release_note() on key-up / key change
        └─ audio_tick() → resid_clock_delta() → snd_pcm_writei()
```
