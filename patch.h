/*
 * patch.h — Patch struct shared between supersynth.c and patchbrowser.c.
 *
 * Field names are kept as original BASIC variable names so the code
 * can be cross-referenced directly against the Compute's Gazette source.
 */

#ifndef PATCH_H
#define PATCH_H

typedef struct {
    int z;          /* voice mode 1-6                              */
    int fl;         /* effect flag: 0=none, 1=vibrato, 2=pw sweep  */
    int w1, w2;     /* waveform control byte for voices 1 and 2    */
    int at, de;     /* ADSR attack, decay  (0-15)                  */
    int su, re;     /* ADSR sustain, release (0-15)                */
    int po;         /* filter resonance / routing byte             */
    int xt;         /* sync/sweep step speed                       */
    int vi;         /* vibrato speed (voice 3 freq lo)             */
    int vs;         /* vibrato waveform shape (voice 3 ctrl)       */
    int db, dc, dd; /* pulse-width high bytes for voices 1, 2, 3  */
    int vo;         /* master volume + filter mode byte            */
    int sl;         /* filter / pitch sweep limit                  */
} Patch;

#endif /* PATCH_H */
