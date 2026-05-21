/*
 * sixel.c  –  sixel oscilloscope for supersynth.
 *
 * Draws a green-phosphor oscilloscope using DEC VT340 sixel graphics.
 * Position and pixel dimensions are runtime-configurable via scope_set_pos()
 * and scope_set_size(); both can be called at any time before the next redraw.
 *
 * Sixel format:  ESC P q <palette> <bands> ESC \
 *   Each "band" is 6 rows tall; one character per column encodes which of
 *   the 6 rows are lit as a bitmask ORed with 0x3F (making it printable).
 *   Multiple colours in one band: use $ (band carriage return) between
 *   colour passes, - (band newline) to advance to the next band.
 *
 * Phosphor glow:
 *   colour 1 = bright green  (trace centre line)
 *   colour 2 = dim green     (±1 px vertical halo around the trace)
 *
 * Detection: Primary Device Attributes query (ESC [ c) before initscr().
 * The response contains "4" as an isolated parameter when sixel is supported.
 * Falls back to a complete no-op on unsupported terminals or if /dev/tty
 * cannot be opened.
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>

#include "sixel.h"

/* ── compile-time limits ───────────────────────────────────────── */
#define MAX_SCOPE_PX   256   /* maximum pixel dimension (w or h)              */
#define SCOPE_INTERVAL   4   /* redraw every N calls to scope_feed()          */

/*
 * Output buffer sized for worst-case MAX_SCOPE_PX × MAX_SCOPE_PX image:
 *   bands = (256+5)/6 = 43;  per band: 2 passes × (256 chars + "#N" + '$'/'-') ≈ 520 bytes
 *   43 × 520 + header/palette ≈ 22 400 bytes  →  32 KB is comfortable.
 */
#define SIXEL_BUFSZ    32768

/* SCOPE_SAMPLES must be >= AUDIO_FRAMES in supersynth.c */
#define SCOPE_SAMPLES  512

/* ── module state (all file-private) ──────────────────────────── */
static int   sixel_ok       = 0;
static int   tty_fd         = -1;
static short scope_buf[SCOPE_SAMPLES];
static int   scope_tick_ctr = 0;

/* runtime-configurable position and size (defaults match the original constants) */
static int   scope_w   = 128;   /* image width in pixels  */
static int   scope_h   = 128;   /* image height in pixels */
static int   scope_row = 4;     /* terminal row, 0-based  */
static int   scope_col = 46;    /* terminal col, 0-based  */

/* ── internal helpers ──────────────────────────────────────────── */

/*
 * Rasterise scope_buf into a scope_w × scope_h pixel bitmap and write it
 * to the terminal as a sixel image positioned at (scope_row, scope_col).
 */
static void render_scope(void)
{
    int bands = (scope_h + 5) / 6;

    /* pixel buffer — indexed [row][col]; static keeps it off the stack */
    static unsigned char px[MAX_SCOPE_PX][MAX_SCOPE_PX];
    /* clear only the rows we will use */
    for (int y = 0; y < scope_h; y++)
        memset(px[y], 0, (size_t)scope_w);

    /* ── rasterise ── */
    int prev_y = scope_h / 2;
    for (int x = 0; x < scope_w; x++) {
        int si = (scope_w > 1)
                 ? (x * (SCOPE_SAMPLES - 1)) / (scope_w - 1)
                 : 0;
        int y = scope_h / 2
                - (int)((scope_buf[si] * (scope_h / 2 - 2)) / 32768);
        if (y <  0)       y = 0;
        if (y >= scope_h) y = scope_h - 1;

        int y0 = prev_y < y ? prev_y : y;
        int y1 = prev_y > y ? prev_y : y;
        for (int vy = y0; vy <= y1; vy++) {
            px[vy][x] = 1;
            if (vy > 0          && px[vy-1][x] == 0) px[vy-1][x] = 2;
            if (vy < scope_h-1  && px[vy+1][x] == 0) px[vy+1][x] = 2;
        }
        prev_y = y;
    }

    /* ── encode sixel ── */
    static char sbuf[SIXEL_BUFSZ];
    int pos = 0;

#define SOUT(...) pos += snprintf(sbuf + pos, SIXEL_BUFSZ - pos, __VA_ARGS__)
#define SCHAR(c)  do { if (pos < SIXEL_BUFSZ - 2) sbuf[pos++] = (char)(c); } while(0)

    SOUT("\033Pq");           /* DCS q — enter sixel mode            */
    SOUT("#0;2;0;0;0");       /* colour 0 = black (terminal bg)      */
    SOUT("#1;2;0;90;10");     /* colour 1 = bright phosphor green    */
    SOUT("#2;2;0;35;4");      /* colour 2 = dim glow                 */

    for (int band = 0; band < bands; band++) {
        int row0 = band * 6;

        SOUT("#1");
        for (int x = 0; x < scope_w; x++) {
            int bits = 0;
            for (int b = 0; b < 6; b++) {
                int vy = row0 + b;
                if (vy < scope_h && px[vy][x] == 1) bits |= (1 << b);
            }
            SCHAR(bits + 0x3F);
        }
        SCHAR('$');   /* carriage return — back to start of this band */

        SOUT("#2");
        for (int x = 0; x < scope_w; x++) {
            int bits = 0;
            for (int b = 0; b < 6; b++) {
                int vy = row0 + b;
                if (vy < scope_h && px[vy][x] == 2) bits |= (1 << b);
            }
            SCHAR(bits + 0x3F);
        }
        SCHAR('-');   /* sixel newline — advance to next band         */
    }

    SOUT("\033\\");   /* ST — end sixel data                          */

#undef SOUT
#undef SCHAR

    /* position cursor then dump the sixel blob */
    char csi[32];
    int clen = snprintf(csi, sizeof(csi),
                        "\033[%d;%dH", scope_row + 1, scope_col + 1);
    write(tty_fd, csi, clen);
    write(tty_fd, sbuf, pos);
}

/* ── public API ────────────────────────────────────────────────── */

void sixel_init(int force_sixel)
{
    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) return;

    if (force_sixel) {
        sixel_ok = 1;
        return;
    }

    struct termios old, raw;
    if (tcgetattr(STDIN_FILENO, &old) < 0) return;
    raw = old;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    write(STDOUT_FILENO, "\033[c", 3);

    struct timeval tv = {0, 250000};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    char buf[128] = {0};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        int n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            for (char *p = buf; *p; p++) {
                if ((*p == '?' || *p == ';') &&
                    *(p+1) == '4' &&
                    (*(p+2) == ';' || *(p+2) == 'c')) {
                    sixel_ok = 1;
                    break;
                }
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old);
}

void scope_set_size(int w, int h)
{
    if (w <  8) w =  8;
    if (w > MAX_SCOPE_PX) w = MAX_SCOPE_PX;
    if (h <  8) h =  8;
    if (h > MAX_SCOPE_PX) h = MAX_SCOPE_PX;
    scope_w = w;
    scope_h = h;
}

void scope_set_pos(int row, int col)
{
    scope_row = row;
    scope_col = col;
}

void scope_feed(const short *buf, int count)
{
    if (!sixel_ok || tty_fd < 0) return;

    int n = count < SCOPE_SAMPLES ? count : SCOPE_SAMPLES;
    memcpy(scope_buf, buf, (size_t)n * sizeof(short));

    if (++scope_tick_ctr >= SCOPE_INTERVAL) {
        render_scope();
        scope_tick_ctr = 0;
    }
}

void scope_redraw(void)
{
    if (!sixel_ok || tty_fd < 0) return;
    render_scope();
}
