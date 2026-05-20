/*
 * sequencer.c — step sequencer editor for superdupersynth.
 *
 * Standalone binary.  4 tracks × 64 steps (4 pages of 16).
 * Tracker-style note entry on the QWERTY piano row; groovebox-style
 * grid navigation.  Unicode output via raw ANSI — no ncurses needed.
 *
 * Clock / transport:
 *   Receives 24 PPQN MIDI clock and START/STOP/CONTINUE on "clock in".
 *   Every 6 ticks advances one step (16th note).  Space bar sends ALSA
 *   START or STOP to "transport out" and controls local playback directly.
 *
 * Note output:
 *   Note-on/off sent on "notes out" port.  Each track uses its own MIDI
 *   channel (track 1 → ch 1, etc.).  Accent → velocity 127; normal → 80.
 *   Slide or tie on a step suppresses the note-off before the next step.
 *
 * Note entry (standard tracker piano, one octave):
 *   q  2  w  3  e  r  5  t  6  y  7  u  i
 *   C  C# D  D# E  F  F# G  G# A  A# B  C(+1 oct)
 *
 * Navigation:
 *   ← →         step left / right (auto-advances page)
 *   ↑ ↓         track up / down
 *   PgUp / PgDn page left / right
 *   [ / ]       entry octave down / up
 *   - / =       transpose current step ±1 octave
 *
 * Step editing:
 *   a / s / f   toggle accent / slide / tie
 *   d / DEL/BS  clear step (rest + all flags)
 *   l           set pattern length to current step
 *
 * Transport:
 *   SPC         start / stop
 *
 * ESC to quit.
 */

#define _GNU_SOURCE
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <alsa/asoundlib.h>

#include "termw.h"

/* ── grid dimensions ────────────────────────────────────────────── */
#define NUM_TRACKS    4
#define NUM_STEPS    64
#define PAGE_STEPS   16
#define NUM_PAGES    (NUM_STEPS / PAGE_STEPS)

/* ── display geometry (all 0-based rows/cols) ───────────────────── */
#define ROW_TITLE     0
#define ROW_SEP1      1
#define ROW_STEPNUM   2
#define ROW_GRID      3    /* first track note row; each track = 2 rows */
#define COL_MARGIN    1
#define COL_LABEL     4    /* "T1 │" width */
#define CELL_W        4    /* chars per step cell: "C-4 " or "--- " */

/* ── playback constants ─────────────────────────────────────────── */
#define TICKS_PER_STEP   6    /* 24 PPQN / 4 subdivisions = 16th note */
#define VEL_NORMAL      80
#define VEL_ACCENT     127

/* ── data model ─────────────────────────────────────────────────── */
typedef struct {
    int note;     /* MIDI note 0-127; -1 = rest */
    int accent;
    int slide;
    int tie;
} Step;

typedef struct {
    Step steps[NUM_TRACKS][NUM_STEPS];
    int  length;    /* active step count (1-NUM_STEPS) */
    char name[32];
} Pattern;

/* ── editor state ───────────────────────────────────────────────── */
static Pattern pat;
static int cursor_track = 0;
static int cursor_step  = 0;
static int current_page = 0;
static int entry_octave = 4;

/* ── playback state ─────────────────────────────────────────────── */
static volatile int playing    = 0;  /* 1 = clock running and we respond */
static volatile int play_step  = 0;  /* current playback position (0-based) */
static volatile int tick_count = 0;  /* MIDI clocks received since last step */
static int held_note[NUM_TRACKS];    /* note currently held per track, -1=none */

static pthread_mutex_t play_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       clock_tid;
static volatile int    clock_stop = 0;  /* set 1 to signal thread exit */

/* ── ALSA seq ───────────────────────────────────────────────────── */
static snd_seq_t *seq        = NULL;
static int notes_port        = -1;  /* output: note-on/off, one channel per track */
static int clock_port        = -1;  /* input:  CLOCK / START / STOP / CONTINUE   */
static int trans_port        = -1;  /* output: START / STOP transport messages    */

/* ── note helpers ───────────────────────────────────────────────── */
static const char * const note_names[12] = {
    "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
};

/*
 * MIDI note → 3-char tracker string ("C-4", "F#3", "---").
 * MIDI 60 = C4: 60/12=5, 5-1=4 → "C-4".
 */
static void note_to_str(int note, char buf[4])
{
    if (note < 0) { memcpy(buf, "---", 4); return; }
    int oct = note / 12 - 1;
    int sem = note % 12;
    buf[0] = note_names[sem][0];
    buf[1] = note_names[sem][1];
    buf[2] = (char)('0' + (oct < 0 ? 0 : oct > 9 ? 9 : oct));
    buf[3] = '\0';
}

/*
 * QWERTY piano row → semitone offset within the current octave (0-12).
 * Returns -1 for non-note keys.
 */
static int key_to_semitone(int ch)
{
    switch (ch) {
    case 'q': return  0;   /* C  */
    case '2': return  1;   /* C# */
    case 'w': return  2;   /* D  */
    case '3': return  3;   /* D# */
    case 'e': return  4;   /* E  */
    case 'r': return  5;   /* F  */
    case '5': return  6;   /* F# */
    case 't': return  7;   /* G  */
    case '6': return  8;   /* G# */
    case 'y': return  9;   /* A  */
    case '7': return 10;   /* A# */
    case 'u': return 11;   /* B  */
    case 'i': return 12;   /* C (octave + 1) */
    default:  return -1;
    }
}

/* ── ALSA output helpers ─────────────────────────────────────────── */

static void seq_dispatch(snd_seq_event_t *ev, int port)
{
    if (!seq || port < 0) return;
    snd_seq_ev_set_source(ev, port);
    snd_seq_ev_set_subs(ev);
    snd_seq_ev_set_direct(ev);
    snd_seq_event_output_direct(seq, ev);
}

/* Note-on for track (track index = MIDI channel). */
static void alsa_note_on(int track, int note, int vel)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteon(&ev, track, note, vel);
    seq_dispatch(&ev, notes_port);
}

/* Note-off for track. */
static void alsa_note_off(int track, int note)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteoff(&ev, track, note, 0);
    seq_dispatch(&ev, notes_port);
}

/* Send a transport event (SND_SEQ_EVENT_START / _STOP / _CONTINUE). */
static void alsa_transport(int type)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    ev.type = type;
    seq_dispatch(&ev, trans_port);
}

/* Release all held notes.  Call with play_lock held. */
static void release_all(void)
{
    for (int tr = 0; tr < NUM_TRACKS; tr++) {
        if (held_note[tr] >= 0) {
            alsa_note_off(tr, held_note[tr]);
            held_note[tr] = -1;
        }
    }
}

/*
 * Fire notes for play_step on all tracks.
 * Previous step's tie/slide flags suppress the intervening note-off.
 * Call with play_lock held.
 */
static void trigger_step(void)
{
    int prev_idx = (play_step == 0) ? pat.length - 1 : play_step - 1;

    for (int tr = 0; tr < NUM_TRACKS; tr++) {
        Step *prev = &pat.steps[tr][prev_idx];
        Step *cur  = &pat.steps[tr][play_step];

        /* release previous note unless it slides or ties into this step */
        if (held_note[tr] >= 0 && !prev->tie && !prev->slide) {
            alsa_note_off(tr, held_note[tr]);
            held_note[tr] = -1;
        }

        if (cur->note >= 0) {
            int vel = cur->accent ? VEL_ACCENT : VEL_NORMAL;
            alsa_note_on(tr, cur->note, vel);
            held_note[tr] = cur->note;
        }
    }
}

/* ── ALSA init ──────────────────────────────────────────────────── */
static void alsa_init(void)
{
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        seq = NULL;
        return;
    }
    snd_seq_set_client_name(seq, "sequencer");

    notes_port = snd_seq_create_simple_port(seq, "notes out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    clock_port = snd_seq_create_simple_port(seq, "clock in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    trans_port = snd_seq_create_simple_port(seq, "transport out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
}

/* ── clock listener thread ──────────────────────────────────────── */

/*
 * Blocks on snd_seq_event_input().  snd_seq_close() in cleanup
 * unblocks the call so the thread can see clock_stop and exit.
 */
static void *clock_thread_fn(void *arg)
{
    (void)arg;
    snd_seq_event_t *ev;
    while (!clock_stop && snd_seq_event_input(seq, &ev) >= 0) {
        pthread_mutex_lock(&play_lock);
        switch (ev->type) {
        case SND_SEQ_EVENT_START:
            release_all();
            play_step  = 0;
            tick_count = 0;
            playing    = 1;
            trigger_step();   /* fire step 0 immediately on start */
            break;
        case SND_SEQ_EVENT_CONTINUE:
            playing = 1;
            break;
        case SND_SEQ_EVENT_STOP:
            playing = 0;
            release_all();
            break;
        case SND_SEQ_EVENT_CLOCK:
            if (playing && ++tick_count >= TICKS_PER_STEP) {
                tick_count = 0;
                play_step  = (play_step + 1) % pat.length;
                trigger_step();
            }
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&play_lock);
    }
    return NULL;
}

/* ── drawing ────────────────────────────────────────────────────── */

static void draw_title(void)
{
    termw_move(ROW_TITLE, COL_MARGIN);
    printf(TW_BOLD TW_CYAN " superdupersynth sequencer" TW_RESET
           TW_CYAN "   %.20s" TW_RESET,
           pat.name[0] ? pat.name : "untitled");

    termw_move(ROW_TITLE, 58);
    printf(TW_CYAN " PG ");
    for (int p = 0; p < NUM_PAGES; p++) {
        if (p == current_page)
            printf(TW_BOLD "[%d]" TW_RESET TW_CYAN, p + 1);
        else
            printf(TW_DIM " %d " TW_RESET TW_CYAN, p + 1);
    }
    printf(TW_RESET);
}

static void draw_sep(int row)
{
    termw_move(row, COL_MARGIN);
    printf(TW_DIM);
    for (int i = 0; i < 76; i++) putchar('-');
    printf(TW_RESET);
}

static void draw_step_numbers(void)
{
    int page_start = current_page * PAGE_STEPS;
    termw_move(ROW_STEPNUM, COL_MARGIN);
    printf(TW_DIM "    │" TW_RESET);
    for (int s = 0; s < PAGE_STEPS; s++) {
        int abs_step = page_start + s;
        int is_last  = (abs_step == pat.length - 1);
        if (is_last)
            printf(TW_BOLD TW_BRED "%2d] " TW_RESET, abs_step + 1);
        else if (abs_step % 4 == 0)
            printf(TW_BOLD TW_YELLOW "%2d  " TW_RESET, abs_step + 1);
        else
            printf(TW_DIM "%2d  " TW_RESET, abs_step + 1);
    }
}

static void draw_step_cell(int track, int abs_step)
{
    Step *st = &pat.steps[track][abs_step];
    int is_cursor = (track == cursor_track && abs_step == cursor_step);
    int is_play   = (abs_step == (int)play_step);
    int page_step = abs_step % PAGE_STEPS;
    int note_row  = ROW_GRID + track * 2;
    int attr_row  = note_row + 1;
    int col       = COL_MARGIN + COL_LABEL + page_step * CELL_W;

    char nbuf[4];
    note_to_str(st->note, nbuf);

    /* ── note cell ── */
    termw_move(note_row, col);
    if (is_cursor)
        printf(TW_REVERSE TW_BOLD "%-3s " TW_RESET, nbuf);
    else if (is_play)
        printf(TW_BG_CYAN TW_BOLD "%-3s " TW_RESET, nbuf);
    else if (st->note < 0)
        printf(TW_DIM "%-3s " TW_RESET, nbuf);
    else
        printf(TW_BGREEN "%-3s " TW_RESET, nbuf);

    /* ── attribute cell (accent / slide / tie) ── */
    termw_move(attr_row, col);
    if (st->accent) printf(TW_YELLOW  "a" TW_RESET);
    else             printf(TW_DIM    "·" TW_RESET);
    if (st->slide)  printf(TW_CYAN   "s" TW_RESET);
    else             printf(TW_DIM   "·" TW_RESET);
    if (st->tie)    printf(TW_MAGENTA"t" TW_RESET);
    else             printf(TW_DIM   "·" TW_RESET);
    printf(" ");
}

static void draw_track_label(int track)
{
    int note_row = ROW_GRID + track * 2;
    int attr_row = note_row + 1;
    termw_move(note_row, COL_MARGIN);
    printf(TW_CYAN " T%d │" TW_RESET, track + 1);
    termw_move(attr_row, COL_MARGIN);
    printf(TW_DIM "    │" TW_RESET);
}

static void draw_status(void)
{
    int row = ROW_GRID + NUM_TRACKS * 2 + 1;
    termw_move(row, COL_MARGIN);
    const char *play_str = playing
        ? TW_BOLD TW_BGREEN "PLAY" TW_RESET TW_DIM
        : TW_DIM            "STOP";
    printf("%s Oct:%d  T%d:Step%02d  Len:%02d  "
           "[SPC=start/stop  ←→↑↓=nav  PgUp/Dn=page"
           "  [/]=oct  -/+=step oct  a/s/f=flags  d=rest  l=last  ESC=quit]"
           TW_RESET,
           play_str,
           entry_octave, cursor_track + 1, cursor_step + 1, pat.length);
}

static void draw_grid(void)
{
    draw_step_numbers();
    for (int tr = 0; tr < NUM_TRACKS; tr++) {
        draw_track_label(tr);
        for (int s = 0; s < PAGE_STEPS; s++)
            draw_step_cell(tr, current_page * PAGE_STEPS + s);
    }
}

static void redraw_all(void)
{
    termw_clear();
    draw_title();
    draw_sep(ROW_SEP1);
    draw_grid();
    draw_sep(ROW_GRID + NUM_TRACKS * 2);
    draw_status();
    termw_flush();
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void)
{
    setlocale(LC_ALL, "");

    alsa_init();

    for (int tr = 0; tr < NUM_TRACKS; tr++)
        held_note[tr] = -1;

    if (seq)
        pthread_create(&clock_tid, NULL, clock_thread_fn, NULL);

    /* initialise pattern — all rests */
    memset(&pat, 0, sizeof(pat));
    pat.length = PAGE_STEPS;
    for (int tr = 0; tr < NUM_TRACKS; tr++)
        for (int st = 0; st < NUM_STEPS; st++)
            pat.steps[tr][st].note = -1;
    strncpy(pat.name, "untitled", sizeof(pat.name) - 1);

    termw_init();
    redraw_all();

    int ch;
    int prev_play_step = -1;

    while ((ch = termw_read_key_timeout(20)) != TK_ESC) {
        int prev_track = cursor_track;
        int prev_step  = cursor_step;
        int dirty_all  = 0;

        /* timeout: repaint only if the play cursor moved */
        if (ch == -1) {
            int ps = (int)play_step;
            if (ps != prev_play_step) {
                prev_play_step = ps;
                redraw_all();
            }
            continue;
        }

        /* ── transport ── */
        if (ch == ' ') {
            pthread_mutex_lock(&play_lock);
            if (playing) {
                playing = 0;
                release_all();
                alsa_transport(SND_SEQ_EVENT_STOP);
            } else {
                play_step  = 0;
                tick_count = 0;
                playing    = 1;
                trigger_step();
                alsa_transport(SND_SEQ_EVENT_START);
            }
            dirty_all = 1;
            pthread_mutex_unlock(&play_lock);

        /* ── navigation ── */
        } else if (ch == TK_LEFT) {
            if (cursor_step > 0) {
                cursor_step--;
                current_page = cursor_step / PAGE_STEPS;
                dirty_all = 1;
            }
        } else if (ch == TK_RIGHT) {
            if (cursor_step < NUM_STEPS - 1) {
                cursor_step++;
                current_page = cursor_step / PAGE_STEPS;
                dirty_all = 1;
            }
        } else if (ch == TK_UP) {
            if (cursor_track > 0) cursor_track--;
        } else if (ch == TK_DOWN) {
            if (cursor_track < NUM_TRACKS - 1) cursor_track++;

        /* ── page navigation ── */
        } else if (ch == TK_PGUP) {
            if (current_page > 0) {
                current_page--;
                cursor_step = current_page * PAGE_STEPS + (cursor_step % PAGE_STEPS);
                dirty_all = 1;
            }
        } else if (ch == TK_PGDN) {
            if (current_page < NUM_PAGES - 1) {
                current_page++;
                cursor_step = current_page * PAGE_STEPS + (cursor_step % PAGE_STEPS);
                dirty_all = 1;
            }

        /* ── entry octave ── */
        } else if (ch == '[') {
            if (entry_octave > 0) entry_octave--;
            dirty_all = 1;
        } else if (ch == ']') {
            if (entry_octave < 9) entry_octave++;
            dirty_all = 1;

        /* ── note entry and step editing ── */
        } else {
            int semi = key_to_semitone(ch);
            if (semi >= 0) {
                int oct  = entry_octave + (semi == 12 ? 1 : 0);
                int note = 12 * (oct + 1) + (semi % 12);
                if (note >= 0 && note <= 127)
                    pat.steps[cursor_track][cursor_step].note = note;
                /* advance cursor after entry */
                if (cursor_step < NUM_STEPS - 1) {
                    cursor_step++;
                    if (cursor_step / PAGE_STEPS != current_page) {
                        current_page = cursor_step / PAGE_STEPS;
                        dirty_all = 1;
                    }
                }

            } else if (ch == 'a') {
                pat.steps[cursor_track][cursor_step].accent ^= 1;
            } else if (ch == 's') {
                pat.steps[cursor_track][cursor_step].slide ^= 1;
            } else if (ch == 'd' || ch == TK_DEL || ch == TK_BACKSPACE) {
                pat.steps[cursor_track][cursor_step].note   = -1;
                pat.steps[cursor_track][cursor_step].accent = 0;
                pat.steps[cursor_track][cursor_step].slide  = 0;
                pat.steps[cursor_track][cursor_step].tie    = 0;
            } else if (ch == 'f') {
                pat.steps[cursor_track][cursor_step].tie ^= 1;
            } else if (ch == 'l') {
                pthread_mutex_lock(&play_lock);
                pat.length = cursor_step + 1;
                pthread_mutex_unlock(&play_lock);
                dirty_all = 1;
            } else if (ch == '-') {
                int *n = &pat.steps[cursor_track][cursor_step].note;
                if (*n >= 12) *n -= 12;
            } else if (ch == '=') {
                int *n = &pat.steps[cursor_track][cursor_step].note;
                if (*n >= 0 && *n <= 115) *n += 12;
            }
        }

        /* ── targeted redraw — avoid full clear to prevent flicker ── */
        if (dirty_all) {
            redraw_all();
        } else {
            if (prev_step != cursor_step || prev_track != cursor_track)
                draw_step_cell(prev_track, prev_step);
            draw_step_cell(cursor_track, cursor_step);
            draw_status();
            termw_flush();
        }
    }

    termw_cleanup();

    /* stop playback and release notes before tearing down ALSA */
    pthread_mutex_lock(&play_lock);
    playing    = 0;
    clock_stop = 1;
    release_all();
    pthread_mutex_unlock(&play_lock);

    if (seq) {
        snd_seq_close(seq);   /* unblocks snd_seq_event_input in clock thread */
        pthread_join(clock_tid, NULL);
        seq = NULL;
    }

    return 0;
}
