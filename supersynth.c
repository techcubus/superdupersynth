/*
 * supersynth.c  –  Super-Synth for Linux
 *
 * Faithful port of the Commodore 64 BASIC "SUPER-SYNTH" program.
 * Uses:
 *   • ncurses  – terminal UI approximating the VIC-II text screen
 *   • reSID    – 6581 SID chip emulation
 *   • ALSA     – audio output
 *
 * Build:
 *   gcc -O2 -o supersynth supersynth.c \
 *       -lncurses -lresid -lasound -lm
 *
 * Key map  (C64 scan-code → PC key, faithful to original keyboard matrix):
 *
 *   PC number row (black keys):  2 3   5 6 7   9 0   - ` =
 *   PC QWERTY row (white keys):  Q W E R T Y U I O P [ ] \
 *
 *   PC ASDF row   (black keys):  d f   h j k   ; '
 *   PC ZXCV row   (white keys):  Z X C V B N M , . /
 *
 *   The ZXCV row plays the higher octave (Z is one octave above Q).
 *   ` = C64 £,  = = C64 ↑,  [ = C64 @,  ] = C64 *,  \ = C64 =
 *
 *   F1    – Reset to default patch
 *   F3    – New random sound
 *   F5    – Save patch to file
 *   F7    – Load patch from file
 *   F9    – Load scale file
 *   F11   – Save scale file
 *   Enter – Toggle values/keyboard screen
 *   ESC   – Quit
 *
 *   Scale files: one line per note key: key_char Hz
 *   e.g.  "q 261.63"  (see -s flag or F9 in-app)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <ncurses.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include "sixel.h"
#include "midi.h"

/* reSID C++ library – we call it through a thin C wrapper declared below */
#ifdef __cplusplus
extern "C" {
#endif
void *resid_new(void);
void  resid_free(void *sid);
void  resid_reset(void *sid);
int   resid_write(void *sid, int reg, int val);
int   resid_read(void *sid, int reg);
void  resid_clock_delta(void *sid, int cycles, short *buf, int *count);
#ifdef __cplusplus
}
#endif

/* ───────────────────────────────────────── constants ── */
#define SAMPLE_RATE   44100
#define SID_CLOCK     985248   /* PAL */
#define AUDIO_FRAMES  512    /* ~12ms per block; balances latency vs underrun risk */
#define MAX_NOTES     65       /* indices 0-64, index 0 unused */

/* SID register offsets (base address implied) */
#define V1_FREQ_LO    0
#define V1_FREQ_HI    1
#define V1_PW_LO      2
#define V1_PW_HI      3
#define V1_CTRL       4
#define V1_AD         5
#define V1_SR         6
#define V2_FREQ_LO    7
#define V2_FREQ_HI    8
#define V2_PW_LO      9
#define V2_PW_HI      10
#define V2_CTRL       11
#define V2_AD         12
#define V2_SR         13
#define V3_FREQ_LO    14
#define V3_FREQ_HI    15
#define V3_PW_LO      16
#define V3_PW_HI      17
#define V3_CTRL       18
#define V3_AD         19
#define V3_SR         20
#define FC_LO         21
#define FC_HI         22
#define RES_FILT      23
#define MODE_VOL      24

/* ───────────────────────────────────────── note data ── */
/*
 * Original DATA statements, pairs: (key-index, raw-SID-freq-value)
 * The BASIC program stores freq as hi/lo bytes itself; we keep the
 * raw 16-bit value and split on demand.
 */
static const int note_data[][2] = {
    {62,2145},{9,2408},{14,2703},{17,2864},{22,3215},{25,3608},{30,4050},{33,4291},
    {38,4817},{41,5407},{46,5728},{49,6430},{54,7217},{12,8101},{23,8583},{20,9634},
    {31,10814},{28,11457},{39,12860},{36,14435},{47,16203},{44,17167},{55,19269},
    {59,2273},{8,2551},{16,3034},{19,3406},{24,3823},{32,4547},{35,5103},{43,6069},
    {48,6812},{51,7647},{18,9094},{21,10207},{29,12139},{34,13625},{37,15294},
    {45,18188},{50,20415},
    {-1,-1}   /* sentinel */
};

/* ───────────────────────────────────────── patch state ── */
typedef struct {
    int z;          /* voice mode 1-6                         */
    int fl;         /* effect flag 0-2                        */
    int w1, w2;     /* waveform byte voice1, voice2           */
    int at, de;     /* attack, decay  (0-15)                  */
    int su, re;     /* sustain, release (0-15)                */
    int po;         /* filter resonance/routing byte          */
    int xt;         /* sync/sweep step                        */
    int vi;         /* vibrato speed (frequency delta)        */
    int vs;         /* vibrato waveform shape                 */
    int db, dc, dd; /* pulse-width voice 1, 2, 3              */
    int vo;         /* volume/filter mode byte                */
    int sl;         /* sweep limit                            */
} Patch;

/* ───────────────────────────────────────── freq tables ── */
/*
 * note_raw[t] – the raw 16-bit SID frequency value for note index t.
 * Populated from note_data at startup; can be overridden by load_scale().
 * All F/G/H byte-pair arrays are derived from this single source of truth.
 */
static int note_raw[MAX_NOTES];

/* F arrays – normal octave */
static int F1[MAX_NOTES], F2[MAX_NOTES]; /* hi, lo */
/* G arrays – octave up  */
static int G1[MAX_NOTES], G2[MAX_NOTES];
/* H arrays – octave down */
static int H1[MAX_NOTES], H2[MAX_NOTES];
/* Also the M-scaled variants (F3/F4, G3/G4, H3/H4) for voice 2 */
static int F3[MAX_NOTES], F4[MAX_NOTES];
static int G3[MAX_NOTES], G4[MAX_NOTES];
static int H3[MAX_NOTES], H4[MAX_NOTES];

/* ───────────────────────────────────────── globals ── */
static void       *sid   = NULL;
static snd_pcm_t  *pcm   = NULL;
static Patch       patch;

/* key-code → note index mapping  (ncurses keycodes → DATA indices) */
/* We map printable keys to note indices the same way the C64 matrix did. */
/* Upper keyboard row  Q..] and 2,3,5..= */
/* Lower keyboard row  A..;  and Z../ */

/* ncurses ch → note index (0 = not a note key) */
static int key_to_note[256];

/* ── FL effect state ──
 * These are advanced each audio_tick() while a note is held (note_active=1).
 *
 * FL=1 vibrato: voice 3 runs as a free LFO; each tick we read its oscillator
 *   output via reSID register 0x1B and write it to voices 1+2 freq LO.
 *   BASIC line 400: POKEV,PEEK(V+27):POKEV+7,PEEK(V+27)
 *   V+27 = SID reg 0x1B = voice 3 oscillator output (read-only on real HW).
 *   reSID exposes this via SID::read(0x1b) → voice[2].wave.readOSC().
 *   Voice 3's waveform and rate were already written by apply_patch (vs, vi).
 *
 * FL=2 filter sweep: FC_HI is stepped from 0 up to patch.sl in increments
 *   of 10 each tick, then held.
 *   BASIC line 410: FORU=1TOSLSTEP10:POKEV+22,U
 */
static int note_active  = 0;   /* 1 while a note is gated on */
static int fl_sweep_pos = 0;   /* current FC_HI value for FL=2 sweep */

/* protects all reSID register access against concurrent MIDI thread writes */
static pthread_mutex_t sid_lock = PTHREAD_MUTEX_INITIALIZER;

/* ───────────────────────────────────────── forward decls ── */
static void build_freq_tables(void);
static void build_key_map(void);
static void init_alsa(void);
static void close_alsa(void);
static void init_sid(void);
static void sid_write(int reg, int val);
static void apply_patch(void);
static void play_note(int t);
static void release_note(void);
static void randomise_patch(void);
static void draw_keyboard_screen(void);
static void draw_values_screen(void);
static void save_patch(void);
static void load_patch(void);
static void load_scale(const char *path);
static void save_scale(const char *path);
static void ui_load_scale(void);
static void ui_save_scale(void);
static void audio_tick(void);   /* push one buffer of silence / tone */
static void run_main_loop(void);

/* ══════════════════════════════════════════════════════════════════
   reSID thin C wrapper  (compiled as C++, linked in)
   ══════════════════════════════════════════════════════════════════ */

/* ───────────────────────────────────────── freq table builder ── */
static void build_freq_tables(void)
{
    /* M is a slight detuning factor for voice 2 (BASIC: M=1.005). */
    const double M = 1.005;
    const int    K = 256;   /* byte split: hi = N/K, lo = N mod K */

    /* Phase 1: seed note_raw[] from the built-in DATA table if not yet set.
     * load_scale() may call us again after overwriting note_raw[], in which
     * case we skip this step so user values are not clobbered. */
    static int seeded = 0;
    if (!seeded) {
        for (int i = 0; note_data[i][0] != -1; i++) {
            int t = note_data[i][0];
            int N = note_data[i][1];
            if (t > 0 && t < MAX_NOTES)
                note_raw[t] = N;
        }
        seeded = 1;
    }

    /* Phase 2: derive F/G/H byte-pair arrays from note_raw[]. */
    for (int t = 1; t < MAX_NOTES; t++) {
        int N = note_raw[t];
        if (N == 0) continue;   /* unused note index */

        int N1 = (int)(N * M);
        int N2 = N * 2;
        int N3 = (int)(N * 2 * M);
        int N4 = N / 2;
        int N5 = (int)(N / 2.0 * M);

        F1[t] = N  / K;   F2[t] = N  - (F1[t] * K);
        F3[t] = N1 / K;   F4[t] = N1 - (F3[t] * K);
        G1[t] = N2 / K;   G2[t] = N2 - (G1[t] * K);
        G3[t] = N3 / K;   G4[t] = N3 - (G3[t] * K);
        H1[t] = N4 / K;   H2[t] = N4 - (H1[t] * K);
        H3[t] = N5 / K;   H4[t] = N5 - (H3[t] * K);
    }
}

/* ───────────────────────────────────────── key map ── */
/*
 * Maps PC keycodes → C64 scan codes (= note_raw[] indices).
 *
 * The C64 used PEEK(197) which returned a keyboard-matrix position.
 * The formula is: scan = PA_bit * 8 + (7 - PB_bit)
 * where PA ($DC00) is the column driven low and PB ($DC01) is the row read.
 *
 * Piano layout on PC keyboard:
 *
 *   number row (black keys):  2  3     5  6  7     9  0     -  `  =
 *   QWERTY row (white keys):  Q  W  E  R  T  Y  U  I  O  P  [  ]  \
 *
 *   ASDF row   (black keys):     d  f     h  j  k     ;  '
 *   ZXCV row   (white keys):  Z  X  C  V  B  N  M  ,  .  /
 *
 * Special mappings:  [ = C64 @   ] = C64 *   \ = C64 =
 *                    ` = C64 £   = = C64 ↑   / = C64 HOME
 *
 * ZXCV row plays a higher octave than QWERTY (Z is one octave above Q).
 * A, S, G, L are not note keys (not present in the original DATA).
 */
static void build_key_map(void)
{
    memset(key_to_note, 0, sizeof(key_to_note));

    /* QWERTY row — white keys, lower octave */
    key_to_note['q'] = 62;  key_to_note['Q'] = 62;  /* C64 Q  */
    key_to_note['w'] = 9;   key_to_note['W'] = 9;   /* C64 W  */
    key_to_note['e'] = 14;  key_to_note['E'] = 14;  /* C64 E  */
    key_to_note['r'] = 17;  key_to_note['R'] = 17;  /* C64 R  */
    key_to_note['t'] = 22;  key_to_note['T'] = 22;  /* C64 T  */
    key_to_note['y'] = 25;  key_to_note['Y'] = 25;  /* C64 Y  */
    key_to_note['u'] = 30;  key_to_note['U'] = 30;  /* C64 U  */
    key_to_note['i'] = 33;  key_to_note['I'] = 33;  /* C64 I  */
    key_to_note['o'] = 38;  key_to_note['O'] = 38;  /* C64 O  */
    key_to_note['p'] = 41;  key_to_note['P'] = 41;  /* C64 P  */
    key_to_note['['] = 46;                            /* C64 @  */
    key_to_note[']'] = 49;                            /* C64 *  */
    key_to_note['\\']= 54;                            /* C64 =  */

    /* Number row — black keys above QWERTY (sharps/flats) */
    key_to_note['2'] = 59;  /* C64 2  */
    key_to_note['3'] = 8;   /* C64 3  */
    key_to_note['5'] = 16;  /* C64 5  */
    key_to_note['6'] = 19;  /* C64 6  */
    key_to_note['7'] = 24;  /* C64 7  */
    key_to_note['9'] = 32;  /* C64 9  */
    key_to_note['0'] = 35;  /* C64 0  */
    key_to_note['-'] = 43;  /* C64 -  */
    key_to_note['`'] = 48;  /* C64 £ (pound) */
    key_to_note['='] = 51;  /* C64 ↑ (up-arrow) */

    /* ZXCV row — white keys, higher octave */
    key_to_note['z'] = 12;  key_to_note['Z'] = 12;  /* C64 Z    */
    key_to_note['x'] = 23;  key_to_note['X'] = 23;  /* C64 X    */
    key_to_note['c'] = 20;  key_to_note['C'] = 20;  /* C64 C    */
    key_to_note['v'] = 31;  key_to_note['V'] = 31;  /* C64 V    */
    key_to_note['b'] = 28;  key_to_note['B'] = 28;  /* C64 B    */
    key_to_note['n'] = 39;  key_to_note['N'] = 39;  /* C64 N    */
    key_to_note['m'] = 36;  key_to_note['M'] = 36;  /* C64 M    */
    key_to_note[','] = 47;                            /* C64 ,    */
    key_to_note['.'] = 44;                            /* C64 .    */
    key_to_note['/'] = 55;                            /* C64 HOME */

    /* ASDF row — black keys above ZXCV */
    key_to_note['d'] = 18;  key_to_note['D'] = 18;  /* C64 D  */
    key_to_note['f'] = 21;  key_to_note['F'] = 21;  /* C64 F  */
    key_to_note['h'] = 29;  key_to_note['H'] = 29;  /* C64 H  */
    key_to_note['j'] = 34;  key_to_note['J'] = 34;  /* C64 J  */
    key_to_note['k'] = 37;  key_to_note['K'] = 37;  /* C64 K  */
    key_to_note[';'] = 45;                            /* C64 :  */
    key_to_note['\'']= 50;                            /* C64 ;  */
}

/* ───────────────────────────────────────── ALSA init ── */
static void init_alsa(void)
{
    int err;
    snd_pcm_hw_params_t *hw;
    unsigned int rate = SAMPLE_RATE;

    /* Blocking mode: snd_pcm_writei will block until ALSA has consumed one
     * period, which paces the main loop to wall-clock time automatically.
     * Non-blocking caused the loop to spin at CPU speed, filling the buffer
     * instantly and dropping everything thereafter. */
    err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) { fprintf(stderr,"ALSA open: %s\n",snd_strerror(err)); exit(1); }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, 1);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
    snd_pcm_hw_params_set_period_size(pcm, hw, AUDIO_FRAMES, 0);
    snd_pcm_hw_params_set_buffer_size(pcm, hw, AUDIO_FRAMES * 4); /* 4 periods of headroom */
    snd_pcm_hw_params(pcm, hw);
    snd_pcm_prepare(pcm);
}

static void close_alsa(void)
{
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); pcm = NULL; }
}

/* ───────────────────────────────────────── SID helpers ── */
static void init_sid(void)
{
    sid = resid_new();
    resid_reset(sid);
}

static void sid_write(int reg, int val)
{
    if (val < 0)   val = 0;
    if (val > 255) val = 255;
    resid_write(sid, reg, val);
}

/* Compute AD/SR bytes from patch */
static int adsr_ad(void) { return (patch.at & 0xF) << 4 | (patch.de & 0xF); }
static int adsr_sr(void) { return (patch.su & 0xF) << 4 | (patch.re & 0xF); }

static void apply_patch(void)
{
    int ad = adsr_ad(), sr = adsr_sr();
    sid_write(V1_AD,    ad);           /* attack/decay for voice 1 */
    sid_write(V1_SR,    sr);           /* sustain/release for voice 1 */
    sid_write(V2_AD,    ad);           /* attack/decay for voice 2 */
    sid_write(V2_SR,    sr);           /* sustain/release for voice 2 */
    /* BASIC: POKEV+3,DB / V+10,DC / V+17,DD  →  pulse-width HI regs for voices 1-3 */
    sid_write(V1_PW_HI,  patch.db);
    sid_write(V2_PW_HI,  patch.dc);
    sid_write(V3_PW_HI,  patch.dd);
    sid_write(V3_FREQ_LO, patch.vi);  /* vibrato LFO speed (voice 3 freq lo) */
    /* BASIC: POKEV+18,VS  →  V3 control register sets LFO waveform */
    sid_write(V3_CTRL,   patch.vs);
    sid_write(RES_FILT,  patch.po);   /* resonance and filter routing */
    sid_write(MODE_VOL,  patch.vo);   /* filter mode and master volume */
}

/* ───────────────────────────────────────── note on/off ── */
static void play_note(int t)
{
    if (t <= 0 || t >= MAX_NOTES) return;

    /*
     * Freq table layout: F1/G1/H1 = high byte, F2/G2/H2 = low byte.
     * BASIC: V1=V+1 (FREQ_HI), V2=V (FREQ_LO), V3=V+8 (V2 FREQ_HI), V4=V+7 (V2 FREQ_LO)
     * BASIC line 460: POKEV1,F1 / POKEV2,F2 / POKEV3,F3 / POKEV4,F4
     */
    switch (patch.z) {
    case 1: /* octave up — G tables (BASIC line 310) */
        sid_write(V1_FREQ_HI, G1[t]);
        sid_write(V1_FREQ_LO, G2[t]);
        sid_write(V2_FREQ_HI, G3[t]);
        sid_write(V2_FREQ_LO, G4[t]);
        break;
    case 6: /* octave down — H tables (BASIC line 320) */
        sid_write(V1_FREQ_HI, H1[t]);
        sid_write(V1_FREQ_LO, H2[t]);
        sid_write(V2_FREQ_HI, H3[t]);
        sid_write(V2_FREQ_LO, H4[t]);
        break;
    default: /* modes 2-5: normal freq (BASIC line 460 via gosub) */
        sid_write(V1_FREQ_HI, F1[t]);
        sid_write(V1_FREQ_LO, F2[t]);
        sid_write(V2_FREQ_HI, F3[t]);
        sid_write(V2_FREQ_LO, F4[t]);
        /* BASIC line 340: POKEV+15,F1(T)/.7  — V+15 = V3_FREQ_HI */
        if (patch.z == 3)
            sid_write(V3_FREQ_HI, (int)(F1[t] / 0.7));
        /* BASIC line 350: POKEV+15,F1(T)/2 */
        else if (patch.z == 4)
            sid_write(V3_FREQ_HI, F1[t] / 2);
        break;
    }

    /* w1/w2 include the gate bit (bit 0); writing them triggers note-on */
    sid_write(V1_CTRL, patch.w1);
    sid_write(V2_CTRL, patch.w2);

    /* reset FL effect state for the new note */
    note_active  = 1;
    fl_sweep_pos = 0;
}

static void release_note(void)
{
    sid_write(V1_CTRL, patch.w1 & ~1);   /* clear gate bit */
    sid_write(V2_CTRL, patch.w2 & ~1);
    sid_write(V3_FREQ_HI, 0);            /* BASIC line 430: POKEV+15,0 */
    sid_write(FC_HI, 0);                 /* reset filter cutoff after sweep */
    note_active = 0;
}

/* ───────────────────────────────────────── MIDI note support ── */

/*
 * Convert a MIDI note number (0–127) to a 16-bit SID frequency word.
 * A4 = MIDI 69 = 440 Hz; SID freq = hz * 2^24 / SID_CLOCK (PAL 985248 Hz).
 */
static unsigned int midi_to_sid_freq(int note)
{
    double hz = 440.0 * pow(2.0, (note - 69) / 12.0);
    double f  = hz * 16777216.0 / (double)SID_CLOCK;
    if (f > 65535.0) f = 65535.0;
    return (unsigned int)(f + 0.5);
}

/*
 * Gate on a note given a raw SID frequency word, applying the current
 * patch waveform and gate bits.  Used by MIDI note-on; bypasses the
 * note_raw[] table so MIDI has the full 128-note range regardless of
 * any loaded user scale.
 * Caller must hold sid_lock.
 */
static void play_note_freq(unsigned int freq)
{
    unsigned char hi = (freq >> 8) & 0xFF;
    unsigned char lo =  freq       & 0xFF;
    sid_write(V1_FREQ_HI, hi);
    sid_write(V1_FREQ_LO, lo);
    sid_write(V2_FREQ_HI, hi);   /* V2 in unison with V1 for MIDI */
    sid_write(V2_FREQ_LO, lo);
    sid_write(V1_CTRL, patch.w1);
    sid_write(V2_CTRL, patch.w2);
    note_active  = 1;
    fl_sweep_pos = 0;
}

/*
 * MIDI thread callbacks — called from the listener thread in midi.c.
 * Both acquire sid_lock before touching SID state.
 */
static void midi_note_on_cb(int midi_note)
{
    unsigned int freq = midi_to_sid_freq(midi_note);
    pthread_mutex_lock(&sid_lock);
    play_note_freq(freq);
    pthread_mutex_unlock(&sid_lock);
}

static void midi_note_off_cb(void)
{
    pthread_mutex_lock(&sid_lock);
    release_note();
    pthread_mutex_unlock(&sid_lock);
}

/* ───────────────────────────────────────── FL effect tick ── */
/*
 * Advance the active FL effect by one audio buffer's worth.
 * Called once per audio_tick() while note_active is set.
 *
 * FL=1 vibrato
 *   Advance an 8-bit LFO phase accumulator by patch.vi each tick.
 *   Convert the phase to a waveform value (matching patch.vs shape codes)
 *   and write it to voices 1+2 freq LO to modulate pitch.
 *   The BASIC read voice-3 oscillator output for this; we replicate the
 *   waveform arithmetic directly since reSID doesn't expose that register.
 *
 * FL=2 filter sweep
 *   Step FC_HI up from 0 toward patch.sl in increments of 10, then hold.
 *   BASIC line 410: FOR U=1 TO SL STEP 10 : POKEV+22,U
 */
static void fl_tick(void)
{
    if (!note_active) return;

    switch (patch.fl) {
    case 1: {
        /* read the real voice 3 oscillator output (SID reg 0x1B).
         * Voice 3 was set up as a free-running LFO by apply_patch (waveform
         * = vs, frequency = vi written to V3_FREQ_LO/HI).  reSID clocks the
         * oscillator inside resid_clock_delta, so each tick produces a fresh
         * value here — exactly what the BASIC does with PEEK(V+27). */
        int val = resid_read(sid, 0x1b);
        /* modulate pitch of voices 1 and 2 by writing to their freq LO bytes */
        sid_write(V1_FREQ_LO, val);
        sid_write(V2_FREQ_LO, val);
        break;
    }
    case 2:
        /* step filter cutoff up to patch.sl then hold */
        if (fl_sweep_pos < patch.sl) {
            fl_sweep_pos += 10;
            if (fl_sweep_pos > patch.sl) fl_sweep_pos = patch.sl;
            sid_write(FC_HI, fl_sweep_pos);
        }
        break;
    default:
        break;
    }
}

/* ───────────────────────────────────────── audio tick ── */
/*
 * Clock the SID for AUDIO_FRAMES samples, advance FL effects, write to ALSA.
 * Called from the main loop continuously so the SID envelope advances
 * even between key presses.
 */
static void audio_tick(void)
{
    static short buf[AUDIO_FRAMES];
    /* integer cycles per buffer: SID_CLOCK/SAMPLE_RATE * AUDIO_FRAMES */
    int cycles_per_sample = SID_CLOCK / SAMPLE_RATE;
    int count = AUDIO_FRAMES;

    /* hold lock for the full SID clock cycle — prevents MIDI callbacks from
     * writing registers mid-computation */
    pthread_mutex_lock(&sid_lock);
    fl_tick();  /* advance vibrato / filter sweep before clocking */
    resid_clock_delta(sid, cycles_per_sample * AUDIO_FRAMES, buf, &count);
    pthread_mutex_unlock(&sid_lock);

    /* hand audio data to the scope module; it throttles and renders internally */
    scope_feed(buf, count);

    int written = snd_pcm_writei(pcm, buf, count);
    if (written == -EPIPE) {
        /* underrun — reset PCM state and retry next tick */
        snd_pcm_prepare(pcm);
    } else if (written < 0) {
        /* other ALSA error — attempt recovery */
        snd_pcm_recover(pcm, written, 1);
    }
}

/* ───────────────────────────────────────── randomise patch ── */
static void randomise_patch(void)
{
    /* waveform control bytes: triangle=17, sawtooth=33, pulse=65, noise=129,
     * tri+pulse=21, saw+pulse=23, all=85  (gate bit not included here) */
    static const int waveforms[] = {17,33,65,129,21,23,85};
    static const int vols[]      = {31,45,79};    /* low-pass=31, band=45, high=79 */
    static const int vs_vals[]   = {17,33,65,129};/* LFO waveform shapes */
    static const int po_vals[]   = {240,241,242,243}; /* resonance/filter routing */

    patch.z   = (rand() % 6) + 1;
    patch.fl  = rand() % 3;
    patch.sl  = (rand() % 255) + 1;

    patch.w1  = waveforms[rand() % 7];
    patch.w2  = waveforms[rand() % 7];

    patch.at  = (rand() % 10) + 1;
    patch.de  = (rand() % 15) + 1;
    patch.su  = (rand() % 15) + 1;
    patch.re  = (rand() % 15) + 1;

    patch.po  = po_vals[rand() % 4];
    patch.xt  = (rand() % 40) + 1;
    patch.vs  = vs_vals[rand() % 4];
    patch.vi  = (rand() % 200) + 55;

    patch.db  = (rand() % 8) + 1;
    patch.dc  = (rand() % 8) + 1;
    patch.dd  = (rand() % 8) + 1;

    patch.vo  = vols[rand() % 3];
}

/* ───────────────────────────────────────── default patch ── */
static void default_patch(void)
{
    patch.z   = 2;
    patch.fl  = 0;
    patch.db  = 8;  patch.dc = 8;  patch.dd = 8;
    patch.vo  = 31;
    patch.vs  = 17;
    patch.vi  = 90;
    patch.xt  = 1;
    patch.po  = 240;
    patch.w1  = 33;
    patch.w2  = 33;
    patch.at  = 8;  patch.de = 8;
    patch.su  = 8;  patch.re = 8;
    patch.sl  = 128;
}

/* ───────────────────────────────────────── save / load ── */
static void save_patch(void)
{
    char fname[256] = "";
    echo();
    move(12, 4); clrtoeol();
    mvprintw(12, 4, "SOUND TO SAVE: ");
    getnstr(fname, 240);
    noecho();
    if (fname[0] == '\0') return;

    FILE *f = fopen(fname, "w");
    if (!f) { mvprintw(14,4,"ERROR: cannot open file."); refresh(); return; }
    fprintf(f,"%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n",
            patch.z,patch.fl,patch.w1,patch.w2,patch.at,patch.de,patch.su,
            patch.re,patch.po,patch.xt,patch.vi,patch.vs,patch.db,patch.dc,
            patch.dd,patch.vo,patch.sl);
    fclose(f);
}

static void load_patch(void)
{
    char fname[256] = "";
    echo();
    move(12, 4); clrtoeol();
    mvprintw(12, 4, "SOUND TO LOAD: ");
    getnstr(fname, 240);
    noecho();
    if (fname[0] == '\0') return;

    FILE *f = fopen(fname, "r");
    if (!f) { mvprintw(14,4,"ERROR: file not found."); refresh(); return; }
    fscanf(f,"%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d",
           &patch.z,&patch.fl,&patch.w1,&patch.w2,&patch.at,&patch.de,
           &patch.su,&patch.re,&patch.po,&patch.xt,&patch.vi,&patch.vs,
           &patch.db,&patch.dc,&patch.dd,&patch.vo,&patch.sl);
    fclose(f);
    apply_patch();
}

/* ───────────────────────────────────────── scale load / save ── */
/*
 * Scale file format — plain text, one note per line:
 *   key_char  frequency_in_Hz
 * e.g.:
 *   q 261.63
 *   2 277.18
 *   w 293.66
 * Lines beginning with # are comments.  Blank lines are ignored.
 * key_char is the PC key character (lower-case).
 * Frequency is in Hz (float); the SID value is computed automatically:
 *   sid_freq = round(hz * 16777216 / SID_CLOCK)
 * Only keys present in the file are updated; the rest keep their current value.
 */

static void load_scale(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        mvprintw(14, 4, "ERROR: cannot open scale file: %s", path);
        refresh();
        return;
    }

    char line[256];
    int  count = 0;
    while (fgets(line, sizeof(line), f)) {
        /* skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char   key_ch;
        double hz;
        if (sscanf(line, " %c %lf", &key_ch, &hz) != 2) continue;
        if (hz <= 0.0) continue;

        /* look up the note index for this PC key character */
        int idx = (unsigned char)key_ch;
        if (idx >= 256) continue;
        int t = key_to_note[idx];
        if (t == 0) {
            /* also try lower-case version */
            if (key_ch >= 'A' && key_ch <= 'Z') t = key_to_note[(int)(key_ch + 32)];
        }
        if (t == 0) continue;   /* key not mapped to a note */

        /* convert Hz to 16-bit SID frequency */
        note_raw[t] = (int)round(hz * 16777216.0 / SID_CLOCK);
        count++;
    }
    fclose(f);

    /* rebuild all F/G/H arrays from updated note_raw[] */
    build_freq_tables();
}

static void save_scale(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        mvprintw(14, 4, "ERROR: cannot write scale file: %s", path);
        refresh();
        return;
    }

    fprintf(f, "# supersynth scale file\n");
    fprintf(f, "# format: key_char  Hz\n");
    fprintf(f, "# SID clock: %d Hz (PAL)\n\n", SID_CLOCK);

    /* iterate note keys in frequency order (ascending note_raw value) */
    static const char note_keys[] = "q2w3er5t6y7ui9o0p[-`=zxcvbnm,./"
                                    "dfhjk;'";
    for (int i = 0; note_keys[i]; i++) {
        int idx = (unsigned char)note_keys[i];
        int t   = key_to_note[idx];
        if (t == 0 || note_raw[t] == 0) continue;
        double hz = (double)note_raw[t] * SID_CLOCK / 16777216.0;
        fprintf(f, "%c %.4f\n", note_keys[i], hz);
    }
    fclose(f);
}

/* Interactive wrappers that prompt in the ncurses window */
static void ui_load_scale(void)
{
    char fname[256] = "";
    echo();
    move(12, 4); clrtoeol();
    mvprintw(12, 4, "SCALE FILE TO LOAD: ");
    getnstr(fname, 240);
    noecho();
    if (fname[0] == '\0') return;
    load_scale(fname);
}

static void ui_save_scale(void)
{
    char fname[256] = "";
    echo();
    move(12, 4); clrtoeol();
    mvprintw(12, 4, "SCALE FILE TO SAVE: ");
    getnstr(fname, 240);
    noecho();
    if (fname[0] == '\0') return;
    save_scale(fname);
}

/* ───────────────────────────────────────── UI ── */

/* Colour pairs */
#define CP_NORMAL   1   /* white on black */
#define CP_CYAN     2   /* cyan on black  */
#define CP_YELLOW   3   /* yellow on black */
#define CP_GREEN    4   /* green on black */
#define CP_WHITE_K  5   /* white key highlight */
#define CP_BLACK_K  6   /* black key highlight */
#define CP_TITLE    7   /* bright white on black */
#define CP_ACTIVE   8   /* playing key highlight */

static void init_colors(void)
{
    start_color();
    use_default_colors();
    init_pair(CP_NORMAL,  COLOR_WHITE,   COLOR_BLACK);
    init_pair(CP_CYAN,    COLOR_CYAN,    COLOR_BLACK);
    init_pair(CP_YELLOW,  COLOR_YELLOW,  COLOR_BLACK);
    init_pair(CP_GREEN,   COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_WHITE_K, COLOR_BLACK,   COLOR_WHITE);
    init_pair(CP_BLACK_K, COLOR_WHITE,   COLOR_BLACK);
    init_pair(CP_TITLE,   COLOR_WHITE,   COLOR_BLUE);
    init_pair(CP_ACTIVE,  COLOR_BLACK,   COLOR_CYAN);
}

/*
 *  Keyboard screen – mirrors the PETSCII box-art keyboard in the
 *  BASIC PRINT statements (lines 540-630).
 *
 *  Layout (approximate):
 *
 *   ╔══════════════════════════════════════╗
 *   ║         KEYBOARD SCREEN              ║
 *   ║  F1-Normal  F3-New  F5-Save  F7-Load ║
 *   ║  ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐    ║
 *   ║  │ │2│3│ │5│6│7│ │9│0│ │-│=│ │    ║
 *   ║  │Q│W│E│R│T│Y│U│I│O│P│[│\│]│  │    ║
 *   ║  ├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤    ║
 *   ║  │A│S│D│F│G│H│J│K│L│;│'│  │        ║
 *   ║  ├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤          ║
 *   ║  │Z│X│C│V│B│N│M│,│.│/│            ║
 *   ║  └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘            ║
 *   ║                                      ║
 *   ╚══════════════════════════════════════╝
 */

/*
 * Draw one horizontal border row using ACS line-drawing chars.
 * left/sep/right are the ACS corner/tee chars for that row type.
 * Each of the `n` cells is 2 chars wide; total width = 3n+1.
 */
static void kbd_hline(int y, int x, int n, chtype left, chtype sep, chtype right)
{
    attron(COLOR_PAIR(CP_CYAN));
    mvaddch(y, x, left);
    for (int i = 0; i < n; i++) {
        addch(ACS_HLINE);
        addch(ACS_HLINE);
        addch(i < n - 1 ? sep : right);
    }
    attroff(COLOR_PAIR(CP_CYAN));
}

static void draw_keyboard_screen(void)
{
    clear();

    /* ── outer border ── */
    attron(COLOR_PAIR(CP_CYAN));
    for (int c = 1; c < 79; c++) {
        mvaddch(0,  c, ACS_HLINE);
        mvaddch(23, c, ACS_HLINE);
    }
    for (int r = 1; r < 23; r++) {
        mvaddch(r,  0, ACS_VLINE);
        mvaddch(r, 79, ACS_VLINE);
    }
    mvaddch(0,   0, ACS_ULCORNER);  mvaddch(0,  79, ACS_URCORNER);
    mvaddch(23,  0, ACS_LLCORNER);  mvaddch(23, 79, ACS_LRCORNER);
    attroff(COLOR_PAIR(CP_CYAN));

    /* ── title ── */
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(1, 25, "  *** SUPER-SYNTH ***  ");
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    /* ── function key hints ── */
    move(2, 4);
    attron(COLOR_PAIR(CP_YELLOW));  addstr("F1");    attroff(COLOR_PAIR(CP_YELLOW));
    addstr("-Normal  ");
    attron(COLOR_PAIR(CP_YELLOW));  addstr("F3");    attroff(COLOR_PAIR(CP_YELLOW));
    addstr("-New  ");
    attron(COLOR_PAIR(CP_YELLOW));  addstr("F5");    attroff(COLOR_PAIR(CP_YELLOW));
    addstr("-Save  ");
    attron(COLOR_PAIR(CP_YELLOW));  addstr("F7");    attroff(COLOR_PAIR(CP_YELLOW));
    addstr("-Load  ");
    attron(COLOR_PAIR(CP_YELLOW));  addstr("ENTER"); attroff(COLOR_PAIR(CP_YELLOW));
    addstr("-Values  ");
    attron(COLOR_PAIR(CP_YELLOW));  addstr("ESC");   attroff(COLOR_PAIR(CP_YELLOW));
    addstr("-Quit");

    /*
     * Keyboard layout — two sections, each cell 2 chars wide + 1 border.
     * Total width per section: 3n+1 chars, starting at column KX=4.
     *
     * Upper (13 cells): number row (black keys) above QWERTY row (white keys)
     *   rows 4-8,  width = 40 cols
     *
     * Lower (11 cells): A-row (white) above Z-row (extra notes)
     *   rows 10-14, width = 34 cols
     *
     * Space in a key string = no key / blank cell.
     */
    const int KX  = 4;
    const int NUP = 13;   /* 13 white keys across QWERTY row */
    const int NLO = 10;   /* 10 white keys across ZXCV row   */

    /*
     * One char per cell; space = no note key at that position.
     * blk_up: number row keys that sit above the QWERTY white keys.
     *   Positions with no sharp (like between E-R and U-I) are spaces.
     *   ` = C64 £,  = = C64 ↑  (both mapped to number-row keys)
     * wht_up: the 13 QWERTY-row white note keys.
     *   [ = C64 @,  ] = C64 *,  \ = C64 =
     * blk_lo: ASDF-row keys that sit above the ZXCV white keys.
     *   Only D F H J K ; ' are note keys; A S G L are control keys (space).
     * wht_lo: the 10 ZXCV-row white note keys.
     *   / = C64 HOME (last key in this row)
     */
    static const char blk_up[] = " 23 567 90 -`";  /* number row (13 positions); = (C64 ↑) not shown */
    static const char wht_up[] = "QWERTYUIOP[\\]";  /* QWERTY row (13 keys)     */
    static const char blk_lo[] = " DF HJK ;'";      /* ASDF row  (10 positions) */
    static const char wht_lo[] = "ZXCVBNM,./";      /* ZXCV row  (10 keys)      */

    /* ── upper keyboard ── */
    kbd_hline(4, KX, NUP, ACS_ULCORNER, ACS_TTEE, ACS_URCORNER);

    attron(COLOR_PAIR(CP_CYAN));
    mvaddch(5, KX, ACS_VLINE);
    attroff(COLOR_PAIR(CP_CYAN));
    for (int i = 0; i < NUP; i++) {
        char c = blk_up[i];
        if (c != ' ') {
            attron(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
            addch(c); addch(' ');
            attroff(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
        } else {
            addch(' '); addch(' ');
        }
        attron(COLOR_PAIR(CP_CYAN));
        addch(ACS_VLINE);
        attroff(COLOR_PAIR(CP_CYAN));
    }

    kbd_hline(6, KX, NUP, ACS_LTEE, ACS_PLUS, ACS_RTEE);

    attron(COLOR_PAIR(CP_CYAN));
    mvaddch(7, KX, ACS_VLINE);
    attroff(COLOR_PAIR(CP_CYAN));
    for (int i = 0; i < NUP; i++) {
        attron(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
        addch(wht_up[i]); addch(' ');
        attroff(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
        attron(COLOR_PAIR(CP_CYAN));
        addch(ACS_VLINE);
        attroff(COLOR_PAIR(CP_CYAN));
    }

    kbd_hline(8, KX, NUP, ACS_LLCORNER, ACS_BTEE, ACS_LRCORNER);

    /* ── lower keyboard ── */
    /* Upper row = ASDF black keys (CP_BLACK_K); lower row = ZXCV white keys (CP_WHITE_K) */
    kbd_hline(10, KX, NLO, ACS_ULCORNER, ACS_TTEE, ACS_URCORNER);

    attron(COLOR_PAIR(CP_CYAN));
    mvaddch(11, KX, ACS_VLINE);
    attroff(COLOR_PAIR(CP_CYAN));
    for (int i = 0; i < NLO; i++) {
        char c = blk_lo[i];
        if (c != ' ') {
            attron(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
            addch(c); addch(' ');
            attroff(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
        } else {
            addch(' '); addch(' ');
        }
        attron(COLOR_PAIR(CP_CYAN));
        addch(ACS_VLINE);
        attroff(COLOR_PAIR(CP_CYAN));
    }

    kbd_hline(12, KX, NLO, ACS_LTEE, ACS_PLUS, ACS_RTEE);

    attron(COLOR_PAIR(CP_CYAN));
    mvaddch(13, KX, ACS_VLINE);
    attroff(COLOR_PAIR(CP_CYAN));
    for (int i = 0; i < NLO; i++) {
        attron(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
        addch(wht_lo[i]); addch(' ');
        attroff(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
        attron(COLOR_PAIR(CP_CYAN));
        addch(ACS_VLINE);
        attroff(COLOR_PAIR(CP_CYAN));
    }

    kbd_hline(14, KX, NLO, ACS_LLCORNER, ACS_BTEE, ACS_LRCORNER);

    /* ── patch summary ── */
    attron(COLOR_PAIR(CP_GREEN));
    mvprintw(16, 4, "Mode:%d FL:%d W1:%3d W2:%3d  AT:%2d DE:%2d SU:%2d RE:%2d",
             patch.z, patch.fl, patch.w1, patch.w2,
             patch.at, patch.de, patch.su, patch.re);
    mvprintw(17, 4, "Vol:%2d Res:%3d VI:%3d VS:%3d  SL:%3d",
             patch.vo, patch.po, patch.vi, patch.vs, patch.sl);
    attroff(COLOR_PAIR(CP_GREEN));

    /* ── status bar ── */
    attron(COLOR_PAIR(CP_YELLOW));
    mvprintw(22, 2, " F9=LoadScale  F11=SaveScale  Hold key=sustain  ESC=Quit ");
    attroff(COLOR_PAIR(CP_YELLOW));

    refresh();

    /* overlay scope after ncurses has flushed — sixel persists until next clear() */
    scope_redraw();
}

static void draw_values_screen(void)
{
    clear();
    bkgd(COLOR_PAIR(CP_YELLOW));

    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(1, 28, "  VALUES SCREEN  ");
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    attron(COLOR_PAIR(CP_NORMAL));
    int r = 3;
#define VROW(label, val) mvprintw(r++, 4, "%-22s = %d", label, val)
    VROW("Z (voice mode)",     patch.z);
    VROW("FL (effect)",        patch.fl);
    VROW("VOICE 1 WAVEFORM",   patch.w1);
    VROW("VOICE 2 WAVEFORM",   patch.w2);
    VROW("ATTACK",             patch.at);
    VROW("DECAY",              patch.de);
    VROW("SUSTAIN",            patch.su);
    VROW("RELEASE",            patch.re);
    VROW("RESONANCE",          patch.po);
    VROW("SYNC SPEED",         patch.xt);
    VROW("VIBRATO SPEED",      patch.vi);
    VROW("VIBRATO SHAPE",      patch.vs);
    VROW("PULSE SHAPE VOICE 1",patch.db);
    VROW("PULSE SHAPE VOICE 2",patch.dc);
    VROW("PULSE SHAPE VOICE 3",patch.dd);
    VROW("FILTER/VOLUME",      patch.vo);
    VROW("STEP LIMIT",         patch.sl);
#undef VROW
    attroff(COLOR_PAIR(CP_NORMAL));

    attron(COLOR_PAIR(CP_YELLOW) | A_BOLD);
    mvprintw(r+1, 4, "Press ENTER to return to keyboard screen.");
    attroff(COLOR_PAIR(CP_YELLOW) | A_BOLD);

    bkgd(0);
    refresh();
}

/* ───────────────────────────────────────── main loop ── */
static void run_main_loop(void)
{
    int  show_values  = 0;
    int  current_note = 0;             /* note index currently gated on, 0 = none */
    struct timespec last_key_ts = {0, 0}; /* time of last note-key event */

    /*
     * Terminal key-up workaround: ncurses has no key-up events.
     * We timestamp every note-key event (including repeat, ~30ms apart).
     * If no event arrives for RELEASE_MS, assume key was released and gate off.
     * 120ms sits comfortably above typical key-repeat interval (~30ms) while
     * staying short enough to feel responsive. The 500ms initial-repeat delay
     * means notes get cut off if held between ~120ms and ~500ms — accepted
     * limitation until MIDI input replaces keyboard.
     */
#define RELEASE_MS 120L

    draw_keyboard_screen();

    for (;;) {
        /* Non-blocking key read */
        int ch = getch();

        if (ch != ERR) {
            /* ── function keys ── */
            if (ch == KEY_F(1)) {
                /* F1 – reset to default patch */
                default_patch();
                pthread_mutex_lock(&sid_lock);
                apply_patch();
                pthread_mutex_unlock(&sid_lock);
                if (!show_values) draw_keyboard_screen();
                continue;
            }
            if (ch == KEY_F(3)) {
                /* F3 – randomise sound */
                randomise_patch();
                pthread_mutex_lock(&sid_lock);
                apply_patch();
                pthread_mutex_unlock(&sid_lock);
                if (!show_values) draw_keyboard_screen();
                continue;
            }
            if (ch == KEY_F(5)) {
                /* F5 – save patch */
                save_patch();
                if (!show_values) draw_keyboard_screen();
                else              draw_values_screen();
                continue;
            }
            if (ch == KEY_F(7)) {
                /* F7 – load patch; apply_patch() inside load_patch() writes SID regs */
                pthread_mutex_lock(&sid_lock);
                load_patch();
                pthread_mutex_unlock(&sid_lock);
                if (!show_values) draw_keyboard_screen();
                else              draw_values_screen();
                continue;
            }
            if (ch == KEY_F(9)) {
                /* F9 – load scale file */
                ui_load_scale();
                if (!show_values) draw_keyboard_screen();
                else              draw_values_screen();
                continue;
            }
            if (ch == KEY_F(11)) {
                /* F11 – save scale file */
                ui_save_scale();
                if (!show_values) draw_keyboard_screen();
                else              draw_values_screen();
                continue;
            }
            if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
                show_values = !show_values;
                if (show_values) draw_values_screen();
                else             draw_keyboard_screen();
                continue;
            }
            if (ch == 27 /* ESC */) {
                pthread_mutex_lock(&sid_lock);
                release_note();
                pthread_mutex_unlock(&sid_lock);
                current_note = 0;
                break;
            }

            if (show_values) continue;   /* ignore note keys on values screen */

            /* ── note key pressed ── */
            if (ch >= 0 && ch < 256) {
                int note = key_to_note[ch];
                if (note > 0) {
                    clock_gettime(CLOCK_MONOTONIC, &last_key_ts);
                    if (current_note != note) {
                        pthread_mutex_lock(&sid_lock);
                        release_note();
                        current_note = note;
                        play_note(note);
                        pthread_mutex_unlock(&sid_lock);
                    }
                }
            }
        }

        /* Gate off if no note-key event for RELEASE_MS */
        if (current_note > 0 && last_key_ts.tv_sec > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec  - last_key_ts.tv_sec)  * 1000L
                    + (now.tv_nsec - last_key_ts.tv_nsec) / 1000000L;
            if (ms >= RELEASE_MS) {
                pthread_mutex_lock(&sid_lock);
                release_note();
                pthread_mutex_unlock(&sid_lock);
                current_note = 0;
            }
        }

        audio_tick();
    }
#undef RELEASE_MS
}

/* ───────────────────────────────────────── main ── */
int main(int argc, char **argv)
{
    const char *scale_file = NULL;

    int force_sixel = 0;

    /* parse CLI flags: -s <scale_file>  -x (force sixel) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            scale_file = argv[++i];
        else if (strcmp(argv[i], "-x") == 0)
            force_sixel = 1;
        else {
            fprintf(stderr, "Usage: %s [-s scale_file] [-x]\n", argv[0]);
            return 1;
        }
    }

    srand((unsigned)time(NULL));

    build_freq_tables();   /* seeds note_raw[] from built-in DATA */
    build_key_map();       /* must run before load_scale() */
    if (scale_file)
        load_scale(scale_file);  /* override note_raw[] + rebuild tables */
    init_alsa();
    init_sid();
    default_patch();
    apply_patch();

    /* sixel detection — must happen before initscr() takes over the terminal */
    sixel_init(force_sixel);

    /* MIDI input — also before initscr() so the aconnect hint prints cleanly */
    midi_init(midi_note_on_cb, midi_note_off_cb);

    /* ncurses init */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);   /* non-blocking getch */
    curs_set(0);
    init_colors();

    /* splash */
    attron(COLOR_PAIR(CP_CYAN) | A_BOLD);
    mvprintw(10, 20, "WELCOME TO SUPER-SYNTH");
    attroff(COLOR_PAIR(CP_CYAN) | A_BOLD);
    mvprintw(12, 14, "Initialising SID emulation, please wait...");
    refresh();
    napms(800);

    run_main_loop();

    /* cleanup — stop MIDI thread before touching SID on the main thread */
    midi_cleanup();
    endwin();
    release_note();
    close_alsa();
    if (sid) resid_free(sid);
    return 0;
}
