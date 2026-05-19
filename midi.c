/*
 * midi.c — ALSA sequencer MIDI input for supersynth.
 *
 * Spawns a detached listener thread that blocks on snd_seq_event_input().
 * note-on and note-off events are forwarded to caller-supplied callbacks.
 * snd_seq_close() in midi_cleanup() unblocks the call, causing the
 * thread to exit cleanly.
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

/* callbacks supplied by the caller */
static void (*cb_note_on)(int) = NULL;
static void (*cb_note_off)(void) = NULL;

static void *midi_thread_fn(void *arg)
{
    (void)arg;
    snd_seq_event_t *ev;

    /* blocks until an event arrives; returns < 0 when seq is closed */
    while (snd_seq_event_input(seq, &ev) >= 0) {
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

    /* detached so we don't need to join on exit */
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, midi_thread_fn, NULL);
    pthread_attr_destroy(&attr);
}

void midi_cleanup(void)
{
    if (seq) {
        snd_seq_close(seq);   /* unblocks snd_seq_event_input → thread exits */
        seq = NULL;
    }
}
