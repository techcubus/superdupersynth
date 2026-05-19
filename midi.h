/*
 * midi.h — ALSA sequencer MIDI input for supersynth.
 *
 * Opens a named ALSA sequencer port and spawns a background listener
 * thread.  All SID interaction is delegated to caller-supplied
 * callbacks; this module has no knowledge of patch or SID state.
 */

#ifndef MIDI_H
#define MIDI_H

/*
 * Open the ALSA sequencer, register an input port named "input", and
 * start the listener thread.
 *   on_note_on(midi_note) — called on note-on (velocity > 0)
 *   on_note_off()         — called on note-off or velocity-0 note-on
 * Both callbacks run in the MIDI thread; callers must hold sid_lock
 * before touching shared SID state.
 * Prints "aconnect" instructions to stderr on success.
 * Silently does nothing if the ALSA sequencer is unavailable.
 */
void midi_init(void (*on_note_on)(int midi_note), void (*on_note_off)(void));

/*
 * Close the sequencer port.  The blocking snd_seq_event_input() in
 * the listener thread returns an error and the thread exits cleanly.
 * Safe to call even if midi_init() was never called or failed.
 */
void midi_cleanup(void);

#endif /* MIDI_H */
