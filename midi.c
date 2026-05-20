/*
 * midi.c — ALSA sequencer MIDI input for supersynth.
 *
 * Spawns a joinable listener thread that blocks on snd_seq_event_input().
 * note-on and note-off events are forwarded to caller-supplied callbacks.
 * midi_cleanup() closes the seq handle (unblocking the thread) then joins
 * to ensure the thread is fully done before the caller frees resources.
 *
 * This module has no knowledge of SID state, patches, or ncurses.
 * Thread safety for SID access is the caller's responsibility.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include "midi.h"

static snd_seq_t *seq      = NULL;   /* ALSA sequencer handle  */
static int        seq_port = -1;     /* our input port number  */
static pthread_t  midi_tid = 0;      /* joinable thread handle */

/* callbacks supplied by the caller */
static void (*cb_note_on)(int) = NULL;
static void (*cb_note_off)(void) = NULL;

static void *midi_thread_fn(void *arg)
{
    /* Use a local copy of the handle passed at thread creation.
     * The main thread sets the global seq=NULL during cleanup; using the
     * local copy means we never call snd_seq_event_input(NULL,...), which
     * would trigger ALSA's internal assert and abort. */
    snd_seq_t      *s  = (snd_seq_t *)arg;
    snd_seq_event_t *ev;

    /* snd_seq_close() in midi_cleanup() closes the underlying fd, causing
     * this blocking call to return < 0 (EBADF) and exit the loop */
    while (snd_seq_event_input(s, &ev) >= 0) {
        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            if (ev->data.note.velocity > 0) {
                /* genuine note-on */
                if (cb_note_on) cb_note_on(ev->data.note.note);
            } else {
                /* velocity-0 note-on is note-off per MIDI spec */
                if (cb_note_off) cb_note_off();
            }
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            if (cb_note_off) cb_note_off();
            break;
        default:
            break;
        }
    }
    return NULL;
}

void midi_init(void (*on_note_on)(int), void (*on_note_off)(void))
{
    cb_note_on  = on_note_on;
    cb_note_off = on_note_off;

    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        seq = NULL;
        return;   /* no ALSA sequencer available — keyboard still works */
    }

    snd_seq_set_client_name(seq, "supersynth");

    seq_port = snd_seq_create_simple_port(seq, "input",
                   SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                   SND_SEQ_PORT_TYPE_APPLICATION);
    if (seq_port < 0) {
        snd_seq_close(seq);
        seq = NULL;
        return;
    }

    /* tell the user how to wire up a controller */
    fprintf(stderr, "MIDI ready — connect with:  aconnect <src_port> %d:%d\n",
            snd_seq_client_id(seq), seq_port);

    /* pass seq as arg so the thread has its own local copy of the handle */
    pthread_create(&midi_tid, NULL, midi_thread_fn, seq);
}

void midi_cleanup(void)
{
    if (seq) {
        snd_seq_close(seq);   /* closing the handle unblocks snd_seq_event_input */
        seq = NULL;
        /* wait for the thread to finish its last callback and fully exit
         * before the caller proceeds to free SID and ALSA resources */
        if (midi_tid) { pthread_join(midi_tid, NULL); midi_tid = 0; }
    }
}
