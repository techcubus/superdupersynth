/*
 * sixel.c  –  sixel oscilloscope for supersynth.
 *
 * Draws a 128×128-pixel green-phosphor oscilloscope to the right of the
 * piano keyboard using sixel terminal graphics (DEC VT340 protocol).
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

/* ── layout constants ──────────────────────────────────────────── */
#define SCOPE_PX        128     /* image width and height in pixels          */
#define SCOPE_ROW         4     /* terminal row of top-left corner (0-based) */
#define SCOPE_COL        46     /* terminal column of top-left corner        */
#define SCOPE_INTERVAL    4     /* redraw every N calls to scope_feed()      */

/* 128 rows / 6 = 21 full bands + 1 partial → 22 bands */
#define SCOPE_BANDS     ((SCOPE_PX + 5) / 6)

/* output buffer: 22 bands × 2 passes × ~130 bytes + header/palette ≈ 6 KB */
#define SIXEL_BUFSZ     8192

/* SCOPE_SAMPLES must match AUDIO_FRAMES in supersynth.c */
#define SCOPE_SAMPLES   512

/* ── module state (all file-private) ──────────────────────────── */
static int   sixel_ok       = 0;                /* 1 when terminal supports sixel    */
static int   tty_fd         = -1;               /* /dev/tty fd for raw terminal I/O  */
static short scope_buf[SCOPE_SAMPLES];          /* last audio buffer snapshot        */
static int   scope_tick_ctr = 0;               /* counts calls since last redraw    */

/* ── internal helpers ──────────────────────────────────────────── */

/*
 * Rasterise scope_buf into a 128×128 pixel bitmap and write it to the
 * terminal as a sixel image positioned at (SCOPE_ROW, SCOPE_COL).
 */
static void render_scope(void)
{
    /* ── rasterise ── */
    /* 0 = black, 1 = bright green (trace), 2 = dim green (glow halo) */
    static unsigned char px[SCOPE_PX][SCOPE_PX];
    memset(px, 0, sizeof(px));

    int prev_y = SCOPE_PX / 2;
    for (int x = 0; x < SCOPE_PX; x++) {
        /* one representative sample per screen column */
        int si = (x * (SCOPE_SAMPLES - 1)) / (SCOPE_PX - 1);
        /* map sample value to pixel row; centre line at SCOPE_PX/2 */
        int y = SCOPE_PX/2 - (int)((scope_buf[si] * (SCOPE_PX/2 - 2)) / 32768);
        y = y <  0         ?  0          : y;
        y = y >= SCOPE_PX  ?  SCOPE_PX-1 : y;

        /* draw vertical segment from prev_y to y — no gaps between columns */
        int y0 = prev_y < y ? prev_y : y;
        int y1 = prev_y > y ? prev_y : y;
        for (int vy = y0; vy <= y1; vy++) {
            px[vy][x] = 1;
            /* glow: dim halo one pixel above and below the bright trace */
            if (vy > 0         && px[vy-1][x] == 0) px[vy-1][x] = 2;
            if (vy < SCOPE_PX-1 && px[vy+1][x] == 0) px[vy+1][x] = 2;
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

    for (int band = 0; band < SCOPE_BANDS; band++) {
        int row0 = band * 6;

        /* pass A: bright trace pixels (colour 1) */
        SOUT("#1");
        for (int x = 0; x < SCOPE_PX; x++) {
            int bits = 0;
            for (int b = 0; b < 6; b++) {
                int vy = row0 + b;
                if (vy < SCOPE_PX && px[vy][x] == 1) bits |= (1 << b);
            }
            SCHAR(bits + 0x3F);  /* sixel char: 0x3F=blank, 0x40=bit0 lit, ... 0x7E=all lit */
        }
        SCHAR('$');   /* carriage return — back to start of this band */

        /* pass B: glow halo pixels (colour 2) */
        SOUT("#2");
        for (int x = 0; x < SCOPE_PX; x++) {
            int bits = 0;
            for (int b = 0; b < 6; b++) {
                int vy = row0 + b;
                if (vy < SCOPE_PX && px[vy][x] == 2) bits |= (1 << b);
            }
            SCHAR(bits + 0x3F);  /* sixel char: 0x3F=blank, 0x40=bit0 lit, ... 0x7E=all lit */
        }
        SCHAR('-');   /* sixel newline — advance to next band         */
    }

    SOUT("\033\\");   /* ST — end sixel data                          */

#undef SOUT
#undef SCHAR

    /* ── write to terminal ── */
    /* position cursor, then dump the sixel blob */
    char csi[32];
    int clen = snprintf(csi, sizeof(csi),
                        "\033[%d;%dH", SCOPE_ROW + 1, SCOPE_COL + 1);
    write(tty_fd, csi, clen);
    write(tty_fd, sbuf, pos);
}

/* ── public API ────────────────────────────────────────────────── */

void sixel_init(int force_sixel)
{
    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) return;

    if (force_sixel) {
        /* -x: trust the user, skip detection */
        sixel_ok = 1;
        return;
    }

    /*
     * Send Primary Device Attributes query and read the response.
     * The terminal must be in raw mode so the response isn't echoed and
     * doesn't require a newline to be flushed.  Restore normal mode after.
     */
    struct termios old, raw;
    if (tcgetattr(STDIN_FILENO, &old) < 0) return;
    raw = old;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    write(STDOUT_FILENO, "\033[c", 3);

    struct timeval tv = {0, 250000};   /* 250 ms timeout */
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    char buf[128] = {0};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        int n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            /* sixel flag = "4" as an isolated parameter: ?4; ?4c ;4; ;4c */
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

void scope_feed(const short *buf, int count)
{
    if (!sixel_ok || tty_fd < 0) return;

    /* snapshot the audio buffer (cap at SCOPE_SAMPLES) */
    int n = count < SCOPE_SAMPLES ? count : SCOPE_SAMPLES;
    memcpy(scope_buf, buf, (size_t)n * sizeof(short));

    if (++scope_tick_ctr >= SCOPE_INTERVAL) {
        render_scope();
        scope_tick_ctr = 0;
    }
}

void scope_redraw(void)
{
    /* force a redraw regardless of the tick counter (e.g. after screen clear) */
    if (!sixel_ok || tty_fd < 0) return;
    render_scope();
}
