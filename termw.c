/*
 * termw.c — raw terminal primitives for superdupersynth TUI tools.
 *
 * Key reading handles the full ANSI/VT escape sequence set:
 *   CSI sequences  ESC [ ...   (arrows, PgUp/Dn, F-keys, Del, Ins, etc.)
 *   SS3 sequences  ESC O ...   (F1-F4 on many terminals)
 *   Shift+arrow    ESC [ 1;2 A/B/C/D
 *
 * After an initial ESC byte, subsequent bytes of the sequence are read
 * with a 50 ms timeout so that a bare ESC keypress is distinguished from
 * the start of a multi-byte sequence.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include "termw.h"

/* ── terminal state ─────────────────────────────────────────────── */
static struct termios saved_termios;
static int termw_active = 0;

void termw_init(void)
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &saved_termios);
    raw = saved_termios;
    cfmakeraw(&raw);
    raw.c_oflag |= OPOST;   /* keep output post-processing (LF→CRLF etc.) */
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    termw_cursor_hide();
    termw_flush();
    termw_active = 1;
}

void termw_cleanup(void)
{
    if (!termw_active) return;
    termw_cursor_show();
    termw_flush();
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    termw_active = 0;
}

/* ── screen / cursor ────────────────────────────────────────────── */

void termw_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (rows) *rows = ws.ws_row;
        if (cols) *cols = ws.ws_col;
    } else {
        if (rows) *rows = 24;
        if (cols) *cols = 80;
    }
}

void termw_clear(void)
{
    fputs("\033[H\033[2J", stdout);
}

void termw_move(int row, int col)
{
    /* ANSI cursor addressing is 1-based */
    printf("\033[%d;%dH", row + 1, col + 1);
}

void termw_cursor_hide(void) { fputs("\033[?25l", stdout); }
void termw_cursor_show(void) { fputs("\033[?25h", stdout); }

void termw_flush(void) { fflush(stdout); }

/* ── key reading ────────────────────────────────────────────────── */

/*
 * Read one byte with a timeout.  Returns the byte value (0-255) or -1
 * if the timeout expired or an error occurred.
 */
static int read_byte_timeout(int ms)
{
    struct timeval tv;
    tv.tv_sec  =  ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return -1;

    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return (int)c;
}

/*
 * Parse a CSI sequence (ESC [ already consumed).
 * Reads bytes until the terminating character, returns a TK_* code.
 */
static int parse_csi(void)
{
    /* accumulate parameter digits and intermediate bytes */
    char buf[16];
    int  len = 0;

    int c;
    while ((c = read_byte_timeout(50)) >= 0 && len < 15) {
        buf[len++] = (char)c;
        /* final byte: letter or '~' */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~')
            break;
    }
    buf[len] = '\0';

    if (len == 0) return TK_UNKNOWN;

    /* single-letter finals (no parameters) */
    if (len == 1) {
        switch (buf[0]) {
        case 'A': return TK_UP;
        case 'B': return TK_DOWN;
        case 'C': return TK_RIGHT;
        case 'D': return TK_LEFT;
        case 'H': return TK_HOME;
        case 'F': return TK_END;
        }
    }

    /* shift+arrow: ESC [ 1 ; 2 A/B/C/D */
    if (len == 5 && buf[0]=='1' && buf[1]==';' && buf[2]=='2') {
        switch (buf[3]) {
        case 'A': return TK_SHIFT_UP;
        case 'B': return TK_SHIFT_DOWN;
        case 'C': return TK_SHIFT_RIGHT;
        case 'D': return TK_SHIFT_LEFT;
        }
    }

    /* tilde-terminated numeric sequences */
    if (buf[len - 1] == '~') {
        /* strip the trailing '~' and parse the number */
        buf[len - 1] = '\0';
        /* handle modifier suffix: e.g. "1;2" — take only the first part */
        char *semi = strchr(buf, ';');
        if (semi) *semi = '\0';
        int n = atoi(buf);
        switch (n) {
        case 1:  return TK_HOME;
        case 2:  return TK_INS;
        case 3:  return TK_DEL;
        case 4:  return TK_END;
        case 5:  return TK_PGUP;
        case 6:  return TK_PGDN;
        case 11: return TK_F1;
        case 12: return TK_F2;
        case 13: return TK_F3;
        case 14: return TK_F4;
        case 15: return TK_F5;
        case 17: return TK_F6;
        case 18: return TK_F7;
        case 19: return TK_F8;
        case 20: return TK_F9;
        case 21: return TK_F10;
        case 23: return TK_F11;
        case 24: return TK_F12;
        }
    }

    return TK_UNKNOWN;
}

/*
 * Parse an SS3 sequence (ESC O already consumed).
 * Used by many terminals for F1-F4 and cursor keys.
 */
static int parse_ss3(void)
{
    int c = read_byte_timeout(50);
    switch (c) {
    case 'A': return TK_UP;
    case 'B': return TK_DOWN;
    case 'C': return TK_RIGHT;
    case 'D': return TK_LEFT;
    case 'H': return TK_HOME;
    case 'F': return TK_END;
    case 'P': return TK_F1;
    case 'Q': return TK_F2;
    case 'R': return TK_F3;
    case 'S': return TK_F4;
    }
    return TK_UNKNOWN;
}

/*
 * Parse whatever follows an ESC byte.
 * `timeout_ms` is how long to wait for the next byte (50ms for mid-sequence,
 * caller-supplied for the initial wait in termw_read_key_timeout).
 */
static int parse_escape(int timeout_ms)
{
    int c2 = read_byte_timeout(timeout_ms);
    if (c2 < 0)    return TK_ESC;   /* bare ESC key — nothing followed */
    if (c2 == '[') return parse_csi();
    if (c2 == 'O') return parse_ss3();
    return TK_UNKNOWN;
}

int termw_read_key(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return TK_UNKNOWN;
    if (c == 0x1B) return parse_escape(50);
    return (int)c;
}

int termw_read_key_timeout(int ms)
{
    int c = read_byte_timeout(ms);
    if (c < 0)     return -1;        /* timeout — no key */
    if (c == 0x1B) return parse_escape(50);
    return c;
}
