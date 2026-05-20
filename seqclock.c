/*
 * seqclock.c — MIDI clock master stub for superdupersynth sequencer.
 *
 * Standalone binary.  Opens an ALSA sequencer client with two ports:
 *   "clock out"  — sends CLOCK (24 PPQN), START, STOP, CONTINUE
 *   "clock in"   — reserved for future slave mode (external transport)
 *
 * The clock thread uses CLOCK_MONOTONIC with TIMER_ABSTIME wakeups so
 * timing error does not accumulate over ticks.  BPM changes take effect
 * on the very next tick.
 *
 * Keyboard controls:
 *   SPACE      start / stop (sends START or STOP, resets position on START)
 *   c          continue     (sends CONTINUE, resumes without position reset)
 *   + / =      BPM +1
 *   - / _      BPM -1
 *   > (Shift+.) BPM +5
 *   < (Shift+,) BPM -5
 *   q / ESC    quit
 *
 * Nerd Font or a Unicode-capable terminal (Konsole, kitty, WezTerm)
 * recommended for best display.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <termios.h>
#include <sys/select.h>
#include <alsa/asoundlib.h>

/* ── timing constants ──────────────────────────────────────────── */
#define PPQN             24   /* MIDI standard: 24 pulses per quarter note    */
#define BEATS_PER_BAR     4   /* 4/4 time                                     */
#define STEPS_PER_BEAT    4   /* 16th-note resolution                         */
#define TICKS_PER_STEP   (PPQN / STEPS_PER_BEAT)   /* 6 ticks per 16th note  */
#define BPM_MIN          40
#define BPM_MAX         300
#define BPM_DEFAULT     120
#define DISPLAY_INTERVAL_MS  80   /* redraw rate when idle                    */

/* ── shared clock state ─────────────────────────────────────────── */
/* all written by clock thread, read by main/display — volatile is
 * sufficient here since we only need eventual consistency for display */
static volatile int  bpm        = BPM_DEFAULT;
static volatile int  running    = 0;
static volatile int  quit_flag  = 0;

/* position — resets on START, preserved on CONTINUE */
static volatile int  tick_in_beat  = 0;   /* 0 .. PPQN-1            */
static volatile int  beat_in_bar   = 0;   /* 0 .. BEATS_PER_BAR-1   */
static volatile int  bar           = 0;   /* 0-based bar count       */
static volatile int  step          = 0;   /* 0-based 16th-note step  */

/* ── ALSA handles ───────────────────────────────────────────────── */
static snd_seq_t *seq      = NULL;
static int        out_port = -1;
static int        in_port  = -1;

/* ── send a typeless (data-free) event to all subscribers ───────── */
static void send_rt_event(int type)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    ev.type = type;
    snd_seq_ev_set_source(&ev, out_port);
    snd_seq_ev_set_subs(&ev);     /* broadcast to all connected ports */
    snd_seq_ev_set_direct(&ev);   /* bypass queue — real-time delivery */
    snd_seq_event_output_direct(seq, &ev);
}

/* ── clock thread ───────────────────────────────────────────────── */
static void *clock_thread_fn(void *arg)
{
    (void)arg;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!quit_flag) {
        /* recompute interval each tick so BPM edits are immediate */
        long interval_ns = 60000000000L / ((long)bpm * PPQN);

        /* advance absolute wakeup time — no drift accumulation */
        next.tv_nsec += interval_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        if (!running) continue;

        send_rt_event(SND_SEQ_EVENT_CLOCK);

        /* advance position */
        tick_in_beat++;
        if (tick_in_beat % TICKS_PER_STEP == 0)
            step++;   /* every 6 ticks = one 16th-note step */

        if (tick_in_beat >= PPQN) {
            tick_in_beat = 0;
            beat_in_bar++;
            if (beat_in_bar >= BEATS_PER_BAR) {
                beat_in_bar = 0;
                bar++;
            }
        }
    }
    return NULL;
}

/* ── transport helpers ──────────────────────────────────────────── */
static void transport_start(void)
{
    /* reset position counters before setting running so the clock
     * thread never sees a partial reset */
    tick_in_beat = 0;
    beat_in_bar  = 0;
    bar          = 0;
    step         = 0;
    running      = 1;
    send_rt_event(SND_SEQ_EVENT_START);
}

static void transport_stop(void)
{
    running = 0;
    send_rt_event(SND_SEQ_EVENT_STOP);
}

static void transport_continue(void)
{
    /* resume from current position — no counter reset */
    running = 1;
    send_rt_event(SND_SEQ_EVENT_CONTINUE);
}

/* ── display ────────────────────────────────────────────────────── */
/*
 * Header is printed once; subsequent redraws use cursor addressing to
 * overwrite only the data lines — prevents flicker.
 */
#define COL_RESET  "\033[0m"
#define COL_BOLD   "\033[1m"
#define COL_DIM    "\033[2m"
#define COL_GREEN  "\033[1;32m"
#define COL_RED    "\033[1;31m"
#define COL_YELLOW "\033[1;33m"

/* beat pulse — flips each beat for a visual tick indicator */
static const char *beat_chars[] = { "●", "○", "○", "○" };

static void draw_header(void)
{
    printf("\033[2J\033[H");   /* clear and home */
    printf(COL_BOLD " superdupersynth — clock stub" COL_RESET "\n");
    printf(COL_DIM  " ─────────────────────────────────────────" COL_RESET "\n");
    printf("\n");   /* line 3: BPM     */
    printf("\n");   /* line 4: state   */
    printf("\n");   /* line 5: pos     */
    printf("\n");   /* line 6: step    */
    printf("\n");   /* line 7: blank   */
    printf(COL_DIM " SPACE=start/stop  c=continue"
                   "  +/-=bpm  </>=bpm±5  q=quit" COL_RESET "\n");
    fflush(stdout);
}

static void draw_state(void)
{
    /* BPM — line 3 */
    printf("\033[3;1H" COL_BOLD " BPM: " COL_YELLOW "%3d" COL_RESET
           "   [- / +]            ", bpm);

    /* state — line 4 */
    printf("\033[4;1H" COL_BOLD " State: %s%s" COL_RESET "      ",
           running ? COL_GREEN : COL_RED,
           running ? "▶  PLAYING" : "■  STOPPED");

    /* position — line 5 */
    printf("\033[5;1H" COL_DIM " Bar %-4d  Beat %d  Tick %02d" COL_RESET "   ",
           bar + 1, beat_in_bar + 1, tick_in_beat);

    /* beat pulse indicator — line 6 */
    printf("\033[6;1H  %s  %s  %s  %s",
           beat_chars[(beat_in_bar + (running ? 0 : 4)) % 4 == 0 ? 0 : 1],
           beat_chars[(beat_in_bar + (running ? 0 : 4)) % 4 == 1 ? 0 : 1],
           beat_chars[(beat_in_bar + (running ? 0 : 4)) % 4 == 2 ? 0 : 1],
           beat_chars[(beat_in_bar + (running ? 0 : 4)) % 4 == 3 ? 0 : 1]);

    fflush(stdout);
}

/* ── raw terminal helpers ───────────────────────────────────────── */
static struct termios term_saved;

static void term_raw(void)
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &term_saved);
    raw = term_saved;
    cfmakeraw(&raw);
    raw.c_oflag |= OPOST;   /* keep output post-processing (newline translation) */
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &term_saved);
}

/* ── ALSA init ──────────────────────────────────────────────────── */
static int alsa_init(void)
{
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        fprintf(stderr, "seqclock: cannot open ALSA sequencer\n");
        return -1;
    }
    snd_seq_set_client_name(seq, "seqclock");

    /* output: sends CLOCK, START, STOP, CONTINUE to connected synths */
    out_port = snd_seq_create_simple_port(seq, "clock out",
                   SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                   SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    /* input: reserved for external transport / slave mode */
    in_port = snd_seq_create_simple_port(seq, "clock in",
                  SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                  SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);

    if (out_port < 0 || in_port < 0) {
        fprintf(stderr, "seqclock: cannot create ports\n");
        return -1;
    }

    fprintf(stderr, "seqclock: ALSA client %d  out=%d:%d  in=%d:%d\n",
            snd_seq_client_id(seq),
            snd_seq_client_id(seq), out_port,
            snd_seq_client_id(seq), in_port);
    return 0;
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void)
{
    if (alsa_init() < 0) return 1;

    /* start clock thread — loops but sleeps while running==0 */
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, clock_thread_fn, NULL);
    pthread_attr_destroy(&attr);

    term_raw();
    draw_header();
    draw_state();

    struct timeval tv;
    fd_set fds;

    while (!quit_flag) {
        /* block for up to DISPLAY_INTERVAL_MS waiting for a keypress */
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        tv.tv_sec  = 0;
        tv.tv_usec = DISPLAY_INTERVAL_MS * 1000;

        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            int ch = getchar();
            switch (ch) {
            case ' ':
                if (running) transport_stop();
                else         transport_start();
                break;
            case 'c': case 'C':
                if (!running) transport_continue();
                break;
            case '+': case '=':
                if (bpm < BPM_MAX) bpm++;
                break;
            case '-': case '_':
                if (bpm > BPM_MIN) bpm--;
                break;
            case '>':
                bpm = bpm + 5 <= BPM_MAX ? bpm + 5 : BPM_MAX;
                break;
            case '<':
                bpm = bpm - 5 >= BPM_MIN ? bpm - 5 : BPM_MIN;
                break;
            case 'q': case 'Q': case 27 /* ESC */:
                quit_flag = 1;
                if (running) transport_stop();
                break;
            }
        }

        draw_state();
    }

    term_restore();
    printf("\033[H\033[2J");
    snd_seq_close(seq);
    return 0;
}
