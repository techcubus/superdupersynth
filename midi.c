/*
 * midi.c — ALSA sequencer MIDI input for supersynth.
 *
 * Spawns a joinable listener thread that polls the ALSA sequencer with a
 * short timeout and checks a stop flag, so it can exit cleanly on demand.
 * midi_cleanup() sets the stop flag, joins the thread, then closes the
 * seq handle — avoiding any race between the thread and snd_seq_close().
 *
 * This module has no knowledge of SID state, patches, or ncurses.
 * Thread safety for SID access is the caller's responsibility.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <poll.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include "midi.h"

static snd_seq_t       *seq      = NULL;  /* ALSA sequencer handle  */
static int              seq_port = -1;    /* our input port number  */
static pthread_t        midi_tid = 0;     /* joinable thread handle */
static volatile int     midi_stop = 0;   /* set to 1 to ask thread to exit */

/* callbacks supplied by the caller */
static void (*cb_note_on)(int) = NULL;
static void (*cb_note_off)(void) = NULL;

static void *midi_thread_fn(void *arg)
{
    snd_seq_t      *s = (snd_seq_t *)arg;  /* local copy — never becomes NULL */
    snd_seq_event_t *ev;
    struct pollfd    pfd;

    /* get the poll descriptor for reading; one fd is enough for our port */
    snd_seq_poll_descriptors(s, &pfd, 1, POLLIN);

    while (!midi_stop) {
        /* poll with a short timeout so we wake up regularly to check midi_stop */
        int r = poll(&pfd, 1, 50);
        if (r < 0) break;
        if (r == 0) continue;   /* timeout — go back and check midi_stop */
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        /* drain all pending events in one burst */
        while (snd_seq_event_input_pending(s, 0) > 0) {
            if (snd_seq_event_input(s, &ev) < 0) goto done;
            switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                if (ev->data.note.velocity > 0) {
                    if (cb_note_on) cb_note_on(ev->data.note.note);
                } else {
                    /* velocity-0 note-on = note-off per MIDI spec */
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
    }
done:
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
    if (seq && midi_tid) {
        /* signal the poll loop to exit, then wait for the thread to finish
         * before closing seq — avoids closing the handle under the thread */
        midi_stop = 1;
        pthread_join(midi_tid, NULL);
        midi_tid  = 0;
        midi_stop = 0;
        snd_seq_close(seq);
        seq = NULL;
    }
}
