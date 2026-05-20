/*
 * termw.h — raw terminal primitives for superdupersynth TUI tools.
 *
 * Provides raw-mode setup, cursor/screen control, and key reading with
 * full escape sequence parsing.  No ncurses dependency.  Unicode output
 * works correctly because the terminal renders it directly.
 *
 * All coordinates are 0-based; ANSI conversion (1-based) is internal.
 */

#ifndef TERMW_H
#define TERMW_H

/* ── ANSI attribute / colour strings ──────────────────────────── */
#define TW_RESET    "\033[0m"
#define TW_BOLD     "\033[1m"
#define TW_DIM      "\033[2m"
#define TW_REVERSE  "\033[7m"

#define TW_BLACK    "\033[30m"
#define TW_RED      "\033[31m"
#define TW_GREEN    "\033[32m"
#define TW_YELLOW   "\033[33m"
#define TW_BLUE     "\033[34m"
#define TW_MAGENTA  "\033[35m"
#define TW_CYAN     "\033[36m"
#define TW_WHITE    "\033[37m"

/* bright variants */
#define TW_BBLACK   "\033[90m"
#define TW_BRED     "\033[91m"
#define TW_BGREEN   "\033[92m"
#define TW_BYELLOW  "\033[93m"
#define TW_BBLUE    "\033[94m"
#define TW_BMAGENTA "\033[95m"
#define TW_BCYAN    "\033[96m"
#define TW_BWHITE   "\033[97m"

/* background colours */
#define TW_BG_BLACK   "\033[40m"
#define TW_BG_RED     "\033[41m"
#define TW_BG_GREEN   "\033[42m"
#define TW_BG_YELLOW  "\033[43m"
#define TW_BG_BLUE    "\033[44m"
#define TW_BG_MAGENTA "\033[45m"
#define TW_BG_CYAN    "\033[46m"
#define TW_BG_WHITE   "\033[47m"

/* ── key codes returned by termw_read_key*() ──────────────────── */
/* printable ASCII is returned as-is (< 0x100).
 * Control codes: TK_ESC, TK_ENTER, TK_BACKSPACE, TK_TAB.
 * Special keys use values above 0xFF so they never clash with char. */

#define TK_ESC          0x1B
#define TK_ENTER        0x0D
#define TK_TAB          0x09
#define TK_BACKSPACE    0x7F

#define TK_UP           0x100
#define TK_DOWN         0x101
#define TK_LEFT         0x102
#define TK_RIGHT        0x103
#define TK_PGUP         0x104
#define TK_PGDN         0x105
#define TK_HOME         0x106
#define TK_END          0x107
#define TK_INS          0x108
#define TK_DEL          0x109

#define TK_F1           0x110
#define TK_F2           0x111
#define TK_F3           0x112
#define TK_F4           0x113
#define TK_F5           0x114
#define TK_F6           0x115
#define TK_F7           0x116
#define TK_F8           0x117
#define TK_F9           0x118
#define TK_F10          0x119
#define TK_F11          0x11A
#define TK_F12          0x11B

#define TK_SHIFT_UP     0x120
#define TK_SHIFT_DOWN   0x121
#define TK_SHIFT_LEFT   0x122
#define TK_SHIFT_RIGHT  0x123

#define TK_UNKNOWN      0x1FF

/* ── API ───────────────────────────────────────────────────────── */

/* Enter raw mode, hide cursor.  Call before any output. */
void termw_init(void);

/* Restore terminal to state before termw_init(). */
void termw_cleanup(void);

/* Fill *rows and *cols with current terminal dimensions. */
void termw_size(int *rows, int *cols);

/* Clear entire screen and home cursor. */
void termw_clear(void);

/* Move cursor to (row, col), both 0-based. */
void termw_move(int row, int col);

/* Hide / show the text cursor. */
void termw_cursor_hide(void);
void termw_cursor_show(void);

/* Flush stdout. */
void termw_flush(void);

/*
 * Read one key, blocking until a key arrives.
 * Returns a TK_* constant or a plain char value.
 */
int termw_read_key(void);

/*
 * Read one key with a timeout.
 * Returns a TK_* constant / char value, or -1 if no key arrived
 * within `ms` milliseconds.
 */
int termw_read_key_timeout(int ms);

#endif /* TERMW_H */
