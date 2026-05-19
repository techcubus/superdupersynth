/*
 * sixel.h  –  public interface for the sixel oscilloscope module.
 *
 * Three entry points; all internal state lives in sixel.c.
 */

#ifndef SIXEL_H
#define SIXEL_H

/*
 * Open /dev/tty and detect sixel support via the Primary Device Attributes
 * query (ESC[c).  Must be called before initscr() while the terminal is still
 * in normal mode.  Pass force_sixel=1 to skip detection (e.g. via -x flag).
 */
void sixel_init(int force_sixel);

/*
 * Copy `count` samples from `buf` into the scope buffer and, every
 * SCOPE_INTERVAL calls, re-render the oscilloscope to the terminal.
 * Called once per audio tick from audio_tick().
 */
void scope_feed(const short *buf, int count);

/*
 * Force an immediate scope redraw without waiting for the next interval.
 * Call this at the end of draw_keyboard_screen() so the scope is present
 * after every full ncurses refresh.
 */
void scope_redraw(void);

#endif /* SIXEL_H */
