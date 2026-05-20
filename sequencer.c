/*
 * sequencer.c — step sequencer editor for superdupersynth.
 *
 * Standalone binary.  4 tracks × 64 steps (4 pages of 16).
 * Tracker-style note entry on the QWERTY piano row; groovebox-style
 * grid navigation.  Unicode output via raw ANSI — no ncurses needed.
 *
 * Note entry (standard tracker piano, one octave):
 *   q  2  w  3  e  r  5  t  6  y  7  u  i
 *   C  C# D  D# E  F  F# G  G# A  A# B  C(+1 oct)
 *
 * Navigation:
 *   ← →         step left / right (auto-advances page)
 *   ↑ ↓         track up / down
 *   PgUp / PgDn page left / right
 *   [ / ]       octave down / up
 *
 * Step editing:
 *   DEL / BS    clear step (rest)
 *   F2          toggle accent
 *   F3          toggle slide
 *   F4          toggle tie
 *
 * ESC to quit.
 */

#define _GNU_SOURCE
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ── ALSA seq (stub — ports ready for clock/note integration) ───── */
static snd_seq_t *seq = NULL;

static void alsa_init(void)
{
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        seq = NULL;
        return;
    }
    snd_seq_set_client_name(seq, "sequencer");

    /* output: note-on/off events when playback is added */
    snd_seq_create_simple_port(seq, "notes out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    /* input: START/STOP/CLOCK from seqclock */
    snd_seq_create_simple_port(seq, "clock in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
}

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

/* ── drawing ────────────────────────────────────────────────────── */

static void draw_title(void)
{
    termw_move(ROW_TITLE, COL_MARGIN);
    printf(TW_BOLD TW_CYAN " superdupersynth sequencer" TW_RESET
           TW_CYAN "   %.20s" TW_RESET,
           pat.name[0] ? pat.name : "untitled");

    /* page indicator */
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
        /* bold on beat boundaries */
        if (abs_step % 4 == 0)
            printf(TW_BOLD TW_DIM "%2d  " TW_RESET, abs_step + 1);
        else
            printf(TW_DIM "%2d  " TW_RESET, abs_step + 1);
    }
}

static void draw_step_cell(int track, int abs_step)
{
    Step *st = &pat.steps[track][abs_step];
    int is_cursor = (track == cursor_track && abs_step == cursor_step);
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
    else if (st->note < 0)
        printf(TW_DIM "%-3s " TW_RESET, nbuf);
    else
        printf(TW_BGREEN "%-3s " TW_RESET, nbuf);

    /* ── attribute cell (accent / slide / tie) ── */
    termw_move(attr_row, col);
    /* accent */
    if (st->accent) printf(TW_YELLOW  "a" TW_RESET);
    else             printf(TW_DIM    "·" TW_RESET);
    /* slide */
    if (st->slide)  printf(TW_CYAN   "s" TW_RESET);
    else             printf(TW_DIM   "·" TW_RESET);
    /* tie */
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
    printf(TW_DIM " Oct:%d  T%d:Step%02d  "
           "[arrows=nav  PgUp/Dn=page  [/]=oct"
           "  F2=accent  F3=slide  F4=tie  DEL=rest  ESC=quit]"
           TW_RESET,
           entry_octave, cursor_track + 1, cursor_step + 1);
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
    while ((ch = termw_read_key()) != TK_ESC) {
        int prev_track = cursor_track;
        int prev_step  = cursor_step;
        int dirty_all  = 0;   /* 1 = full redraw needed */

        /* ── navigation ── */
        if (ch == TK_LEFT) {
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

        /* ── octave ── */
        } else if (ch == '[') {
            if (entry_octave > 0) entry_octave--;
            dirty_all = 1;
        } else if (ch == ']') {
            if (entry_octave < 9) entry_octave++;
            dirty_all = 1;

        /* ── note entry ── */
        } else {
            int semi = key_to_semitone(ch);
            if (semi >= 0) {
                int oct  = entry_octave + (semi == 12 ? 1 : 0);
                int note = 12 * (oct + 1) + (semi % 12);
                if (note >= 0 && note <= 127)
                    pat.steps[cursor_track][cursor_step].note = note;
                /* advance cursor */
                if (cursor_step < NUM_STEPS - 1) {
                    cursor_step++;
                    if (cursor_step / PAGE_STEPS != current_page) {
                        current_page = cursor_step / PAGE_STEPS;
                        dirty_all = 1;
                    }
                }

            /* ── clear step ── */
            } else if (ch == TK_DEL || ch == TK_BACKSPACE) {
                pat.steps[cursor_track][cursor_step].note = -1;

            /* ── attribute toggles ── */
            } else if (ch == TK_F2) {
                pat.steps[cursor_track][cursor_step].accent ^= 1;
            } else if (ch == TK_F3) {
                pat.steps[cursor_track][cursor_step].slide ^= 1;
            } else if (ch == TK_F4) {
                pat.steps[cursor_track][cursor_step].tie ^= 1;
            }
        }

        /* ── targeted redraw — avoid full clear to prevent flicker ── */
        if (dirty_all) {
            redraw_all();
        } else {
            /* repaint old cursor cell and new cursor cell */
            if (prev_step  != cursor_step || prev_track != cursor_track)
                draw_step_cell(prev_track, prev_step);
            draw_step_cell(cursor_track, cursor_step);
            draw_status();
            termw_flush();
        }
    }

    termw_cleanup();
    if (seq) snd_seq_close(seq);
    return 0;
}
