# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

A Linux/C port of a Commodore 64 BASIC synthesizer ("SUPER-SYNTH"). The goal is **faithful behavior** to the original — variable names, data tables, patch parameters, and screen layout all trace directly back to the BASIC source. Do not rename patch fields or restructure logic for cleanliness; cross-referencing with the original BASIC source is intentional.

## Dependencies

```bash
# Debian/Ubuntu — critical package distinction:
sudo apt install libncurses-dev libasound2-dev libresid-builder-dev libsidplay2-dev
```

- `libresid-builder-dev` → shared library (`-lresid-builder`)
- `libsidplay2-dev` → C++ header `<resid/sid.h>`
- Both come from sidplay-libs 2.1.1 — versions must match
- Do **not** use `libresid-dev` (doesn't exist on modern systems) or `libsidplayfp-dev` (incompatible fork)

## Build and Run

```bash
make          # builds ./supersynth
make clean    # remove build artifacts
./supersynth  # terminal must be >= 80x24
```

The Makefile has `RESID_INC` / `RESID_LIB` lines to uncomment if headers/libs are in a non-standard path.

## Architecture

**Two source files only:**
- `supersynth.c` — C99, all UI (ncurses), audio loop, patch logic, key handling
- `resid_wrap.cpp` — thin C++ shim exposing reSID as 5 plain C functions (`extern "C"`)

**Startup sequence:** `build_freq_tables()` → `build_key_map()` → `init_alsa()` → `init_sid()` → `default_patch()` → `apply_patch()` → `run_main_loop()`

**Main loop:** Non-blocking `getch()` polled continuously. Every iteration handles keypresses then calls `audio_tick()` unconditionally — the SID envelope runs even with no key held (release phase continues after key-up).

**Audio pipeline:** `audio_tick()` → `resid_clock_delta()` (reSID C++ API) → `snd_pcm_writei()`. Buffer: 2048 frames, 44100 Hz mono S16_LE, SID clock 985248 Hz (PAL).

## Key Conventions

- All SID register writes go through `sid_write(reg, val)` — never call `resid_write()` directly from main logic.
- `apply_patch()` is the authoritative "flush patch to SID" function. Call it after any patch mutation.
- Patch fields use original BASIC variable names (`z`, `fl`, `po`, `vi`, `vs`, `db`, `dc`, `dd`, `vo`, `sl`, `xt`) intentionally.
- The `Patch` struct is a plain global (`patch`). No threading, no locking.
- Do **not** add `reSID::` namespace qualifiers in `resid_wrap.cpp` — sidplay-libs 2.x uses the global namespace.
- Terminal output outside ncurses must happen before `initscr()` or after `endwin()`.

## Patch System

All sound state lives in the global `Patch patch`. Save/load format is plain text, 17 integers one per line in this order: `z fl w1 w2 at de su re po xt vi vs db dc dd vo sl`.

ADSR encoding: `AD byte = (at & 0xF) << 4 | (de & 0xF)`, `SR byte = (su & 0xF) << 4 | (re & 0xF)`.

## Known Incomplete Features

- **Note release on key-up** — not implemented; ncurses has no key-up events. Options: timer thread or raw terminal + `select()`.
- **FL modes** (vibrato `fl=1`, pulse-width sweep `fl=2`) — `randomise_patch()` sets `fl` but `play_note()` / `run_main_loop()` don't act on it yet.
- **`xt` (sync speed)** — stored and saved/loaded but not applied to the SID.
- **CP_ACTIVE key highlight** — color pair defined but never used; playing key should visually highlight on the keyboard screen.
- **ALSA non-blocking** — under-runs handled with `snd_pcm_prepare()` but may glitch; a dedicated audio thread would help.
