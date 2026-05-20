/*
 * sequencer.c — step sequencer editor for superdupersynth.
 *
 * Standalone binary.  4 tracks × 64 steps (4 pages of 16).
 * Tracker-style note entry on QWERTY piano row; groovebox-style grid nav.
 *
 * Note entry (standard tracker piano, one octave):
 *   q  2  w  3  e  r  5  t  6  y  7  u  i
 *   C  C# D  D# E  F  F# G  G# A  A# B  C(+1)
 *
 * Navigation:
 *   ←→        step left / right (auto-pages)
 *   ↑↓        track up / down
 *   PgUp/Dn   page left / right
 *   [ / ]     octave down / up
 *
 * Step editing:
 *   DEL / BS  clear step (rest)
 *   F2        toggle accent
 *   F3        toggle slide
 *   F4        toggle tie
 *
 * Nerd Font or a Unicode-capable terminal (Konsole, kitty, WezTerm)
 * gives the best display.
 */

#define _GNU_SOURCE
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <alsa/asoundlib.h>

/* ── grid dimensions ───────────────────────────────────────────── */
#define NUM_TRACKS     4
#define NUM_STEPS     64
#define PAGE_STEPS    16
#define NUM_PAGES     (NUM_STEPS / PAGE_STEPS)

/* ── display geometry ──────────────────────────────────────────── */
#define CELL_W         4    /* chars per step cell ("C-4 " or "--- ") */
#define TRACK_LABEL_W  4    /* "T1 ┃" */
#define GRID_LEFT      1    /* left margin */
#define GRID_TOP       3    /* row where step-number header sits */
#define TRACK_H        2    /* rows per track: note row + attr row */

/* ── colour pairs ──────────────────────────────────────────────── */
#define CP_HEADER   1   /* title bar                  */
#define CP_STEPNUM  2   /* step number row             */
#define CP_REST     3   /* empty / rest step           */
#define CP_NOTE     4   /* step with a note            */
#define CP_CURSOR   5   /* cursor highlight            */
#define CP_ACCENT   6   /* accent indicator            */
#define CP_SLIDE    7   /* slide indicator             */
#define CP_TIE      8   /* tie indicator               */
#define CP_STATUS   9   /* status / hint bar           */
#define CP_PAGE     10  /* page indicator dots         */

/* ── data model ────────────────────────────────────────────────── */
typedef struct {
    int note;     /* MIDI note 0-127; -1 = rest */
    int accent;   /* velocity accent flag        */
    int slide;    /* portamento to next note     */
    int tie;      /* extend previous note        */
} Step;

typedef struct {
    Step steps[NUM_TRACKS][NUM_STEPS];
    int  length;    /* active step count (default PAGE_STEPS) */
    char name[32];
} Pattern;

/* ── editor state ──────────────────────────────────────────────── */
static Pattern  pat;
static int cursor_track = 0;   /* 0 .. NUM_TRACKS-1  */
static int cursor_step  = 0;   /* 0 .. NUM_STEPS-1   */
static int current_page = 0;   /* 0 .. NUM_PAGES-1   */
static int entry_octave = 4;   /* octave for note entry (0-9) */

/* ── ALSA seq (output stub for clock and note integration) ─────── */
static snd_seq_t *seq      = NULL;
static int        out_port = -1;

/* ── note helpers ──────────────────────────────────────────────── */
static const char *note_names[12] = {
    "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
};

/*
 * MIDI note → 3-char tracker string.
 * Convention: MIDI 60 = C4 (middle C).  60/12=5, 5-1=4 → "C-4".
 */
static void note_to_str(int note, char buf[4])
{
    if (note < 0) {
        memcpy(buf, "---", 4);
        return;
    }
    int octave   = note / 12 - 1;
    int semitone = note % 12;
    /* octave clamped to 0-9 by entry controls; buf is 4 bytes: "C-4\0" fits */
    buf[0] = note_names[semitone][0];
    buf[1] = note_names[semitone][1];
    buf[2] = (char)('0' + (octave < 0 ? 0 : octave > 9 ? 9 : octave));
    buf[3] = '\0';
}

/*
 * QWERTY piano row → semitone offset (0-12).
 * Returns -1 for keys that are not note keys.
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
    case 'i': return 12;   /* C (octave+1) */
    default:  return -1;
    }
}

/* ── colour init ───────────────────────────────────────────────── */
static void init_colors(void)
{
    start_color();
    use_default_colors();
    init_pair(CP_HEADER,  COLOR_CYAN,    -1);
    init_pair(CP_STEPNUM, COLOR_BLACK,   -1);  /* dim step numbers */
    init_pair(CP_REST,    COLOR_BLACK,   -1);  /* dim empty steps  */
    init_pair(CP_NOTE,    COLOR_GREEN,   -1);  /* lit notes        */
    init_pair(CP_CURSOR,  COLOR_BLACK,   COLOR_GREEN);
    init_pair(CP_ACCENT,  COLOR_YELLOW,  -1);
    init_pair(CP_SLIDE,   COLOR_CYAN,    -1);
    init_pair(CP_TIE,     COLOR_MAGENTA, -1);
    init_pair(CP_STATUS,  COLOR_BLACK,   -1);
    init_pair(CP_PAGE,    COLOR_CYAN,    -1);
}

/* ── drawing ───────────────────────────────────────────────────── */

static void draw_header(void)
{
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvprintw(0, GRID_LEFT, " superdupersynth sequencer");
    attroff(A_BOLD);
    printw("   %.20s", pat.name[0] ? pat.name : "untitled");
    attroff(COLOR_PAIR(CP_HEADER));

    /* page dot indicators: ● current  ○ others */
    attron(COLOR_PAIR(CP_PAGE));
    mvprintw(0, 60, "PG ");
    for (int p = 0; p < NUM_PAGES; p++) {
        if (p == current_page)
            addstr("●");
        else
            addstr("○");
        addch(' ');
    }
    attroff(COLOR_PAIR(CP_PAGE));

    /* separator */
    attron(COLOR_PAIR(CP_STATUS) | A_DIM);
    mvhline(1, GRID_LEFT, ACS_HLINE, COLS - GRID_LEFT * 2);
    attroff(COLOR_PAIR(CP_STATUS) | A_DIM);
}

static void draw_step_numbers(void)
{
    int page_start = current_page * PAGE_STEPS;

    attron(COLOR_PAIR(CP_STEPNUM) | A_DIM);
    mvprintw(GRID_TOP, GRID_LEFT, "    ┃");   /* "    ┃" */
    for (int s = 0; s < PAGE_STEPS; s++) {
        int abs_step = page_start + s;
        /* emphasise beat boundaries (every 4 steps) */
        if (abs_step % 4 == 0)
            attron(A_BOLD);
        else
            attroff(A_BOLD);
        printw("%2d  ", abs_step + 1);
    }
    attroff(COLOR_PAIR(CP_STEPNUM) | A_BOLD | A_DIM);
}

static void draw_track(int track)
{
    int page_start = current_page * PAGE_STEPS;
    int note_row   = GRID_TOP + 1 + track * TRACK_H;
    int attr_row   = note_row + 1;

    /* track label */
    attron(COLOR_PAIR(CP_HEADER));
    mvprintw(note_row, GRID_LEFT, " T%d ┃", track + 1);
    mvprintw(attr_row, GRID_LEFT, "    ┃");
    attroff(COLOR_PAIR(CP_HEADER));

    for (int s = 0; s < PAGE_STEPS; s++) {
        int abs_step = page_start + s;
        Step *st = &pat.steps[track][abs_step];
        int is_cursor = (track == cursor_track && abs_step == cursor_step);
        int col = GRID_LEFT + TRACK_LABEL_W + 1 + s * CELL_W;

        /* ── note cell ── */
        char nbuf[4];
        note_to_str(st->note, nbuf);

        if (is_cursor)
            attron(COLOR_PAIR(CP_CURSOR) | A_BOLD);
        else if (st->note < 0)
            attron(COLOR_PAIR(CP_REST) | A_DIM);
        else
            attron(COLOR_PAIR(CP_NOTE));

        mvprintw(note_row, col, "%-3s ", nbuf);
        attroff(A_BOLD | A_DIM);
        attroff(COLOR_PAIR(CP_CURSOR));
        attroff(COLOR_PAIR(CP_REST));
        attroff(COLOR_PAIR(CP_NOTE));

        /* ── attribute cell ── */
        /* each flag: lit symbol or dim middot */
        move(attr_row, col);   /* position cursor for attribute chars */

        if (st->accent) { attron(COLOR_PAIR(CP_ACCENT)); addstr("a"); attroff(COLOR_PAIR(CP_ACCENT)); }
        else             { attron(A_DIM); addch('.'); attroff(A_DIM); }

        if (st->slide)  { attron(COLOR_PAIR(CP_SLIDE));  addstr("s"); attroff(COLOR_PAIR(CP_SLIDE));  }
        else             { attron(A_DIM); addch('.'); attroff(A_DIM); }

        if (st->tie)    { attron(COLOR_PAIR(CP_TIE));    addstr("t"); attroff(COLOR_PAIR(CP_TIE));    }
        else             { attron(A_DIM); addch('.'); attroff(A_DIM); }

        addch(' ');
    }
}

static void draw_status(void)
{
    int status_row = GRID_TOP + 1 + NUM_TRACKS * TRACK_H + 1;

    /* separator */
    attron(A_DIM);
    mvhline(status_row, GRID_LEFT, ACS_HLINE, COLS - GRID_LEFT * 2);
    attroff(A_DIM);

    attron(COLOR_PAIR(CP_STATUS) | A_DIM);
    mvprintw(status_row + 1, GRID_LEFT,
             " Oct:%d  T%d:Step%02d  "
             "[←→↑↓=nav  PgUp/Dn=page  [/]=oct"
             "  F2=accent  F3=slide  F4=tie  DEL=rest  ESC=quit]",
             entry_octave, cursor_track + 1, cursor_step + 1);
    attroff(COLOR_PAIR(CP_STATUS) | A_DIM);
}

static void redraw(void)
{
    draw_header();
    draw_step_numbers();
    for (int t = 0; t < NUM_TRACKS; t++)
        draw_track(t);
    draw_status();
    refresh();
}

/* ── ALSA init (stub — ports ready for clock/note integration) ─── */
static void alsa_init(void)
{
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        seq = NULL;
        return;
    }
    snd_seq_set_client_name(seq, "sequencer");

    /* note output — will send note-on/off events when playback is added */
    out_port = snd_seq_create_simple_port(seq, "notes out",
                   SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                   SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    /* clock/transport input — will receive START/STOP/CLOCK from seqclock */
    snd_seq_create_simple_port(seq, "clock in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void)
{
    setlocale(LC_ALL, "");   /* enable Unicode output */

    alsa_init();

    /* default pattern setup */
    memset(&pat, 0, sizeof(pat));
    pat.length = PAGE_STEPS;
    for (int tr = 0; tr < NUM_TRACKS; tr++)
        for (int st = 0; st < NUM_STEPS; st++)
            pat.steps[tr][st].note = -1;   /* all rests */
    strncpy(pat.name, "untitled", sizeof(pat.name) - 1);

    /* ncurses init */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();

    redraw();

    int ch;
    while ((ch = getch()) != 27 /* ESC */) {

        /* ── navigation ── */
        if (ch == KEY_LEFT) {
            if (cursor_step > 0) {
                cursor_step--;
                current_page = cursor_step / PAGE_STEPS;
            }
        } else if (ch == KEY_RIGHT) {
            if (cursor_step < NUM_STEPS - 1) {
                cursor_step++;
                current_page = cursor_step / PAGE_STEPS;
            }
        } else if (ch == KEY_UP) {
            if (cursor_track > 0) cursor_track--;
        } else if (ch == KEY_DOWN) {
            if (cursor_track < NUM_TRACKS - 1) cursor_track++;

        /* ── page nav ── */
        } else if (ch == KEY_PPAGE) {   /* Page Up   = page left  */
            if (current_page > 0) {
                current_page--;
                cursor_step = current_page * PAGE_STEPS + (cursor_step % PAGE_STEPS);
            }
        } else if (ch == KEY_NPAGE) {   /* Page Down = page right */
            if (current_page < NUM_PAGES - 1) {
                current_page++;
                cursor_step = current_page * PAGE_STEPS + (cursor_step % PAGE_STEPS);
            }

        /* ── octave ── */
        } else if (ch == '[') {
            if (entry_octave > 0) entry_octave--;
        } else if (ch == ']') {
            if (entry_octave < 9) entry_octave++;

        /* ── note entry ── */
        } else {
            int semi = key_to_semitone(ch);
            if (semi >= 0) {
                int oct  = entry_octave + (semi == 12 ? 1 : 0);
                int note = 12 * (oct + 1) + (semi % 12);
                if (note >= 0 && note <= 127) {
                    pat.steps[cursor_track][cursor_step].note = note;
                    /* advance cursor, wrap within page */
                    if (cursor_step < NUM_STEPS - 1) {
                        cursor_step++;
                        current_page = cursor_step / PAGE_STEPS;
                    }
                }
            }
            /* ── clear step ── */
            else if (ch == KEY_DC || ch == KEY_BACKSPACE || ch == 127) {
                pat.steps[cursor_track][cursor_step].note = -1;

            /* ── attribute toggles ── */
            } else if (ch == KEY_F(2)) {
                Step *s = &pat.steps[cursor_track][cursor_step];
                s->accent = !s->accent;
            } else if (ch == KEY_F(3)) {
                Step *s = &pat.steps[cursor_track][cursor_step];
                s->slide = !s->slide;
            } else if (ch == KEY_F(4)) {
                Step *s = &pat.steps[cursor_track][cursor_step];
                s->tie = !s->tie;
            }
        }

        redraw();
    }

    endwin();
    if (seq) snd_seq_close(seq);
    return 0;
}
