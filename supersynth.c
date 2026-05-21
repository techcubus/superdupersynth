/*
 * supersynth.c  –  Super-Synth for Linux
 *
 * Faithful port of the Commodore 64 BASIC "SUPER-SYNTH" program.
 * Uses:
 *   • termw   – raw-ANSI terminal UI (replaces ncurses; Unicode works correctly)
 *   • reSID   – 6581 SID chip emulation
 *   • ALSA    – audio output
 *
 * Build:
 *   gcc -O2 -o supersynth supersynth.c \
 *       -lresid-builder -lasound -lm -lpthread -lstdc++
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
 *
 * Key-up detection:
 *   Raw terminals have no key-up events.  We poll at 5 ms intervals;
 *   held keys repeat at ~30 Hz (33 ms apart).  If no repeat arrives within
 *   RELEASE_MS (60 ms), the key is considered released and the note is gated
 *   off.  This is more than twice as responsive as the old 120 ms ncurses hack.
 */

#define _GNU_SOURCE
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include "termw.h"
#include "sixel.h"
#include "midi.h"
#include "patch.h"
#include "patchbrowser.h"

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
/* Patch typedef lives in patch.h, shared with patchbrowser */

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

/* metadata displayed and edited alongside the patch parameters */
static char patch_name[64]     = "";
static char patch_author[64]   = "";
static char patch_comment[128] = "";

/* ── patch field descriptor (populated by init_pfields) ── */
typedef struct {
    const char *var;    /* BASIC variable name, e.g. "AT" */
    const char *label;  /* short description, e.g. "attack" */
    int        *field;  /* pointer into the global patch struct */
    int         lo, hi; /* inclusive valid range */
    const char *desc;   /* one-line info shown in the editor */
} PField;

#define NPARAMS     17   /* numeric patch fields */
#define NFIELDS     20   /* NPARAMS + 3 metadata fields */
#define FIELD_NAME    17
#define FIELD_AUTHOR  18
#define FIELD_COMMENT 19
#define ED_DIV1       24  /* │ between left and centre param columns */
#define ED_DIV2       45  /* │ between centre column and scope area  */

static PField pfields[NPARAMS];

/* key-code → note index mapping  (ncurses keycodes → DATA indices) */
/* We map printable keys to note indices the same way the C64 matrix did. */
/* Upper keyboard row  Q..] and 2,3,5..= */
/* Lower keyboard row  A..;  and Z../ */

/* raw char value → note index (0 = not a note key) */
static int key_to_note[256];

/*
 * Note index currently gated on (0 = none).  File-scope so draw_keyboard_screen()
 * can highlight the active key without needing a parameter.
 */
static int current_note = 0;

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
static void prompt_line(int row, int col, const char *prompt, char *buf, int maxlen);
static void save_patch(void);
static void load_patch(void);
static int  load_scale(const char *path);
static void save_scale(const char *path);
static void ui_load_scale(void);
static void ui_save_scale(void);
static void audio_tick(void);
static void run_editor(void);
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

/* ───────────────────────────────────────── in-app text input ── */
/*
 * Prompt for a filename / path at (row, col) while in raw mode.
 * Echoes characters as typed, handles backspace.  Completes on Enter;
 * ESC cancels and leaves buf[0]='\0'.
 *
 * We stay in raw mode throughout — no termios toggle needed — because
 * termw_read_key() gives us individual keystrokes and we handle echo
 * and line editing ourselves.
 */
static void prompt_line(int row, int col, const char *prompt, char *buf, int maxlen)
{
    int len = 0;
    buf[0]  = '\0';

    /* clear the target line and print the prompt */
    termw_move(row, col);
    for (int i = 0; i < 72; i++) putchar(' ');
    termw_move(row, col);
    printf(TW_BOLD TW_CYAN "%s" TW_RESET, prompt);
    termw_cursor_show();
    termw_flush();

    int ch;
    while ((ch = termw_read_key()) != TK_ENTER && ch != TK_ESC) {
        if ((ch == TK_BACKSPACE || ch == 127) && len > 0) {
            /* erase last character: move back, overwrite with space, move back */
            len--;
            buf[len] = '\0';
            printf("\b \b");
            termw_flush();
        } else if (ch >= 32 && ch < 127 && len < maxlen - 1) {
            /* printable ASCII — echo and accumulate */
            buf[len++] = (char)ch;
            buf[len]   = '\0';
            putchar(ch);
            termw_flush();
        }
    }

    if (ch == TK_ESC) buf[0] = '\0';   /* ESC = cancel, return empty string */
    termw_cursor_hide();
}

/* ───────────────────────────────────────── save / load ── */
/*
 * save_patch / load_patch — delegate to the full patch browser.
 * The browser is a modal overlay; it returns when the user confirms or cancels.
 * load_patch locks sid_lock itself because apply_patch() must not race the audio
 * thread — the mutex is released before we return so the caller need not hold it.
 */
static void save_patch(void)
{
    patchbrowser_open_save(&patch, patch_name, patch_author, patch_comment);
}

/*
 * load_patch — run the modal browser outside sid_lock (the audio thread
 * must keep running during the UI interaction), then swap the patch and
 * write SID registers under the lock.  The caller must NOT hold sid_lock
 * when calling this function.
 */
static void load_patch(void)
{
    Patch tmp = patch;
    char  name[64] = "", author[64] = "", comment[128] = "";
    if (!patchbrowser_open_load(&tmp, name, author, comment))
        return;
    pthread_mutex_lock(&sid_lock);
    patch = tmp;
    apply_patch();
    pthread_mutex_unlock(&sid_lock);
    snprintf(patch_name,    sizeof(patch_name),    "%s", name);
    snprintf(patch_author,  sizeof(patch_author),  "%s", author);
    snprintf(patch_comment, sizeof(patch_comment), "%s", comment);
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
 *
 * Returns 0 on success, -1 if the file could not be opened.
 */
static int load_scale(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open scale file: %s\n", path);
        return -1;
    }

    char line[256];
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
    }
    fclose(f);

    /* rebuild all F/G/H arrays from updated note_raw[] */
    build_freq_tables();
    return 0;
}

static void save_scale(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "ERROR: cannot write scale file: %s\n", path);
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

/* Interactive wrappers — prompt for filename using prompt_line(), then call load/save. */
static void ui_load_scale(void)
{
    char fname[256] = "";
    prompt_line(12, 4, "SCALE FILE TO LOAD: ", fname, 240);
    if (fname[0] == '\0') return;
    if (load_scale(fname) < 0) {
        termw_move(14, 4);
        printf(TW_BOLD TW_RED "ERROR: cannot open scale file." TW_RESET);
        termw_flush();
    }
}

static void ui_save_scale(void)
{
    char fname[256] = "";
    prompt_line(12, 4, "SCALE FILE TO SAVE: ", fname, 240);
    if (fname[0] == '\0') return;
    save_scale(fname);
}

/* ───────────────────────────────────────── UI ── */

/*
 * Draw a horizontal rule row using Unicode box-drawing characters.
 * Each of n cells is 2 terminal columns wide (──).
 * left / sep / right are single-column box-drawing strings.
 *
 * Example for n=3: left + ── + sep + ── + sep + ── + right
 *                  e.g.  ┌──┬──┬──┐
 */
static void kbd_hline(int row, int col, int n,
                      const char *left, const char *sep, const char *right)
{
    termw_move(row, col);
    printf(TW_CYAN "%s", left);
    for (int i = 0; i < n; i++) {
        printf("──");                          /* 2-column cell width */
        printf("%s", i < n - 1 ? sep : right);
    }
    printf(TW_RESET);
}

/*
 * Keyboard screen — mirrors the PETSCII box-art keyboard in the
 * BASIC PRINT statements (lines 540-630).
 *
 * Double-line outer border (╔═╗/║/╚═╝) echoes the C64 PETSCII frame.
 * Single-line inner keyboard grid (┌─┐/│/└─┘) for the key cells.
 *
 * Upper section (rows 4-8):  13 cells — number-row black keys above QWERTY white keys.
 * Lower section (rows 10-14): 10 cells — ASDF black keys above ZXCV white keys.
 *
 * The currently playing key (current_note) is highlighted in bright green.
 * White keys use reverse video; black keys are bold on default background.
 */
static void draw_keyboard_screen(void)
{
    /*
     * Key layout strings — one char per cell, space = no note at that position.
     *
     * blk_up: number row above QWERTY.  Positions without a sharp (E-R gap,
     *   B-C gap) are spaces.  = (C64 ↑) is a note key but not shown in this row.
     * wht_up: 13 QWERTY-row white note keys.
     *   [ = C64 @,  ] = C64 *,  \ = C64 =
     * blk_lo: ASDF row above ZXCV.  A, S, G, L are control keys (space here).
     * wht_lo: 10 ZXCV-row white note keys.  / = C64 HOME.
     */
    static const char blk_up[] = " 23 567 90 -`";
    static const char wht_up[] = "QWERTYUIOP[\\]";
    static const char blk_lo[] = " DF HJK ;'";
    static const char wht_lo[] = "ZXCVBNM,./";

    const int KX  = 4;    /* left margin for keyboard grid */
    const int NUP = 13;   /* cells in upper keyboard section */
    const int NLO = 10;   /* cells in lower keyboard section */

    termw_clear();

    /* ── outer border: double-line, mirrors C64 PETSCII framing ── */
    printf(TW_CYAN);
    termw_move(0, 0);
    printf("╔");  for (int c = 1; c < 79; c++) printf("═");  printf("╗");
    for (int r = 1; r < 23; r++) {
        termw_move(r,  0); printf("║");
        termw_move(r, 79); printf("║");
    }
    termw_move(23, 0);
    printf("╚");  for (int c = 1; c < 79; c++) printf("═");  printf("╝");
    printf(TW_RESET);

    /* ── title bar ── */
    termw_move(1, 27);
    printf(TW_BOLD TW_BG_BLUE TW_WHITE "  *** SUPER-SYNTH ***  " TW_RESET);

    /* ── function key hints ── */
    termw_move(2, 4);
    printf(TW_YELLOW "F1" TW_RESET "-Normal  "
           TW_YELLOW "F3" TW_RESET "-New  "
           TW_YELLOW "F5" TW_RESET "-Save  "
           TW_YELLOW "F7" TW_RESET "-Load  "
           TW_YELLOW "ENTER" TW_RESET "-Values  "
           TW_YELLOW "ESC" TW_RESET "-Quit");

    /* ── upper keyboard (number row + QWERTY row) ── */
    kbd_hline(4, KX, NUP, "┌", "┬", "┐");

    /* number row — black keys (sharps/flats) */
    termw_move(5, KX);
    printf(TW_CYAN "│" TW_RESET);
    for (int i = 0; i < NUP; i++) {
        char c = blk_up[i];
        int  note = (c != ' ') ? key_to_note[(unsigned char)c] : 0;
        if (c != ' ' && note > 0 && note == current_note)
            printf(TW_BG_GREEN TW_BOLD "%c " TW_RESET, c);  /* active: green highlight */
        else if (c != ' ')
            printf(TW_BOLD "%c " TW_RESET, c);              /* black key: bold */
        else
            printf("  ");                                    /* no key at this position */
        printf(TW_CYAN "│" TW_RESET);
    }

    kbd_hline(6, KX, NUP, "├", "┼", "┤");

    /* QWERTY row — white keys */
    termw_move(7, KX);
    printf(TW_CYAN "│" TW_RESET);
    for (int i = 0; i < NUP; i++) {
        char c    = wht_up[i];
        int  note = key_to_note[(unsigned char)c];
        if (note > 0 && note == current_note)
            printf(TW_BG_GREEN TW_BOLD "%c " TW_RESET, c);  /* active: green highlight */
        else
            printf(TW_REVERSE TW_BOLD "%c " TW_RESET, c);   /* white key: reverse video */
        printf(TW_CYAN "│" TW_RESET);
    }

    kbd_hline(8, KX, NUP, "└", "┴", "┘");

    /* ── lower keyboard (ASDF row + ZXCV row) ── */
    kbd_hline(10, KX, NLO, "┌", "┬", "┐");

    /* ASDF row — black keys */
    termw_move(11, KX);
    printf(TW_CYAN "│" TW_RESET);
    for (int i = 0; i < NLO; i++) {
        char c    = blk_lo[i];
        int  note = (c != ' ') ? key_to_note[(unsigned char)c] : 0;
        if (c != ' ' && note > 0 && note == current_note)
            printf(TW_BG_GREEN TW_BOLD "%c " TW_RESET, c);
        else if (c != ' ')
            printf(TW_BOLD "%c " TW_RESET, c);
        else
            printf("  ");
        printf(TW_CYAN "│" TW_RESET);
    }

    kbd_hline(12, KX, NLO, "├", "┼", "┤");

    /* ZXCV row — white keys */
    termw_move(13, KX);
    printf(TW_CYAN "│" TW_RESET);
    for (int i = 0; i < NLO; i++) {
        char c    = wht_lo[i];
        int  note = key_to_note[(unsigned char)c];
        if (note > 0 && note == current_note)
            printf(TW_BG_GREEN TW_BOLD "%c " TW_RESET, c);
        else
            printf(TW_REVERSE TW_BOLD "%c " TW_RESET, c);
        printf(TW_CYAN "│" TW_RESET);
    }

    kbd_hline(14, KX, NLO, "└", "┴", "┘");

    /* ── patch summary (two lines) ── */
    termw_move(16, 4);
    printf(TW_GREEN "Mode:%-2d  FL:%d  W1:%-3d  W2:%-3d  AT:%-2d  DE:%-2d  SU:%-2d  RE:%-2d"
           TW_RESET,
           patch.z, patch.fl, patch.w1, patch.w2,
           patch.at, patch.de, patch.su, patch.re);
    termw_move(17, 4);
    printf(TW_GREEN "Vol:%-2d  Res:%-3d  VI:%-3d  VS:%-3d  SL:%-3d" TW_RESET,
           patch.vo, patch.po, patch.vi, patch.vs, patch.sl);

    /* ── status / hint bar ── */
    termw_move(22, 2);
    printf(TW_YELLOW " F9=LoadScale  F11=SaveScale  Hold key=sustain  ESC=Quit "
           TW_RESET);

    termw_flush();
    scope_set_pos(4, 46);
    scope_redraw();
}

/* ───────────────────────────────────────── patch editor ── */

static void init_pfields(void)
{
#define PF(i, v, lbl, fp, lo_, hi_, desc_) \
    pfields[i] = (PField){ v, lbl, fp, lo_, hi_, desc_ }
    PF( 0, "Z",  "voice mode",    &patch.z,  1, 6,
        "1=oct up  2-5=normal  6=oct down");
    PF( 1, "FL", "effect",        &patch.fl, 0, 2,
        "0=none  1=vibrato  2=filter sweep");
    PF( 2, "W1", "v1 waveform",   &patch.w1, 0, 255,
        "waveform+gate byte  17=tri 33=saw 65=pulse 129=noise");
    PF( 3, "W2", "v2 waveform",   &patch.w2, 0, 255,
        "waveform+gate byte for voice 2");
    PF( 4, "AT", "attack",        &patch.at, 0, 15,
        "attack time  0=fastest  15=slowest");
    PF( 5, "DE", "decay",         &patch.de, 0, 15,
        "decay time  0=fastest  15=slowest");
    PF( 6, "SU", "sustain",       &patch.su, 0, 15,
        "sustain level  0=silent  15=full");
    PF( 7, "RE", "release",       &patch.re, 0, 15,
        "release time  0=fastest  15=slowest");
    PF( 8, "PO", "resonance",     &patch.po, 0, 255,
        "resonance and filter routing byte");
    PF( 9, "XT", "sync speed",    &patch.xt, 0, 255,
        "sync speed (stored; not yet applied to SID)");
    PF(10, "VI", "vib speed",     &patch.vi, 0, 255,
        "vibrato / LFO speed (voice 3 freq low byte)");
    PF(11, "VS", "vib shape",     &patch.vs, 0, 255,
        "LFO waveform  17=tri 33=saw 65=pulse 129=noise");
    PF(12, "DB", "pulse v1",      &patch.db, 0, 255,
        "pulse-width high byte for voice 1");
    PF(13, "DC", "pulse v2",      &patch.dc, 0, 255,
        "pulse-width high byte for voice 2");
    PF(14, "DD", "pulse v3",      &patch.dd, 0, 255,
        "pulse-width high byte for voice 3");
    PF(15, "VO", "filter/vol",    &patch.vo, 0, 255,
        "filter mode and master volume byte");
    PF(16, "SL", "sweep lim",     &patch.sl, 0, 255,
        "filter sweep upper limit (FL=2)");
#undef PF
}

/*
 * Draw one numeric param field at its position in the two-column grid.
 * Fields 0-8 go in the left column (rows 3-11, cols 2-ED_DIV1-1).
 * Fields 9-16 go in the centre column (rows 3-10, cols ED_DIV1+1 to ED_DIV2-1).
 */
static void draw_ed_param(int fi, int sel)
{
    int row, scol;
    if (fi < 9) {
        row  = 3 + fi;
        scol = 2;
    } else {
        row  = 3 + (fi - 9);
        scol = ED_DIV1 + 1;
    }
    const PField *pf = &pfields[fi];
    termw_move(row, scol);
    if (fi < 9) {
        /* left column: 11-char label, 4-digit value */
        if (sel)
            printf(TW_REVERSE TW_BOLD "> %-2s %-11.11s%4d" TW_RESET,
                   pf->var, pf->label, *pf->field);
        else
            printf("  %-2s " TW_CYAN "%-11.11s" TW_RESET "%4d",
                   pf->var, pf->label, *pf->field);
    } else {
        /* centre column: 10-char label, 3-digit value */
        if (sel)
            printf(TW_REVERSE TW_BOLD "> %-2s %-10.10s%3d" TW_RESET,
                   pf->var, pf->label, *pf->field);
        else
            printf("  %-2s " TW_CYAN "%-10.10s" TW_RESET "%3d",
                   pf->var, pf->label, *pf->field);
    }
}

/*
 * Draw one metadata field (name / author / comment) at rows 13-15.
 * edit=1: show underscore cursor; edit_buf holds the in-progress text.
 * Metadata rows span the two left param columns (cols 2 to ED_DIV2-2).
 */
static void draw_ed_meta(int fi, int sel, int edit, const char *edit_buf)
{
    static const char *labels[3] = { "NAME   ", "AUTHOR ", "COMMENT" };
    int         mi   = fi - NPARAMS;
    int         row  = 13 + mi;
    const char *text = edit ? edit_buf :
                       (mi == 0 ? patch_name :
                        mi == 1 ? patch_author : patch_comment);
    termw_move(row, 2);
    /* text area: cols 12-43 = 32 chars (2+1+7+1+prefix = col 12, to ED_DIV2-2=43) */
    if (sel && !edit)
        printf(TW_REVERSE TW_BOLD "> %-7s %-32.32s" TW_RESET, labels[mi], text);
    else if (edit)
        printf(TW_REVERSE TW_BOLD "> %-7s %-31.31s_" TW_RESET, labels[mi], text);
    else
        printf("  %-7s " TW_CYAN "%-32.32s" TW_RESET, labels[mi], text);
}

static void draw_editor_screen(int sel, int edit_mode, const char *edit_buf)
{
    termw_clear();

    /* outer border */
    printf(TW_YELLOW);
    termw_move(0, 0);
    printf("╔"); for (int c = 1; c < 79; c++) printf("═"); printf("╗");
    for (int r = 1; r < 23; r++) {
        termw_move(r,  0); printf("║");
        termw_move(r, 79); printf("║");
    }
    termw_move(23, 0);
    printf("╚"); for (int c = 1; c < 79; c++) printf("═"); printf("╝");

    /* rows 2 and 12: plain full-width separators */
    termw_move(2, 0);
    printf("╠"); for (int c = 1; c < 79; c++) printf("═"); printf("╣");
    termw_move(12, 0);
    printf("╠"); for (int c = 1; c < 79; c++) printf("═"); printf("╣");

    /* rows 16 and 22: full-width separators */
    termw_move(16, 0);
    printf("╠"); for (int c = 1; c < 79; c++) printf("═"); printf("╣");
    termw_move(22, 0);
    printf("╠"); for (int c = 1; c < 79; c++) printf("═"); printf("╣");
    printf(TW_RESET);

    /* title — fits within left+centre columns */
    termw_move(1, 2);
    printf(TW_BOLD TW_BG_BLUE TW_WHITE " PATCH EDITOR " TW_RESET);
    if (patch_name[0]) {
        termw_move(1, 18);
        printf(TW_CYAN "%.25s" TW_RESET, patch_name);  /* cols 18-44 */
    }

    /* all numeric params */
    for (int i = 0; i < NPARAMS; i++)
        draw_ed_param(i, i == sel);

    /* metadata rows */
    for (int i = NPARAMS; i < NFIELDS; i++)
        draw_ed_meta(i, i == sel,
                     (edit_mode && i == sel) ? 1 : 0,
                     (edit_mode && i == sel) ? edit_buf : "");

    /* info area — kept within cols 2-44 to avoid scope area */
    termw_move(17, 2);
    if (sel < NPARAMS) {
        printf(TW_DIM "Range %d-%d  " TW_RESET "%.40s",
               pfields[sel].lo, pfields[sel].hi, pfields[sel].desc);
    } else {
        static const char *meta_hints[3] = {
            "Patch name stored in .pjs metadata",
            "Author name stored in .pjs metadata",
            "Freeform comment stored in .pjs metadata",
        };
        printf("%.43s", meta_hints[sel - NPARAMS]);
    }

    /* hints bar */
    termw_move(23, 2);
    if (edit_mode)
        printf(TW_DIM "Type to edit  ENTER=confirm  ESC=cancel" TW_RESET);
    else
        printf(TW_DIM
               "↑↓ nav  ←→ adjust  ENTER=edit  F5=save  ESC=back"
               TW_RESET);

    /* sixel scope overlay — right-aligned against the right border */
    termw_flush();
    scope_set_pos(4, 63);
    scope_redraw();
}

static void run_editor(void)
{
    init_pfields();

    int  sel       = 0;
    int  edit_mode = 0;
    char orig_meta[128] = "";
    char edit_buf[128]  = "";
    int  edit_len       = 0;

    struct timespec last_key_ts = {0, 0};
#define RELEASE_MS 60L

    draw_editor_screen(sel, 0, "");

    for (;;) {
        int ch = termw_read_key_timeout(5);

        if (ch != -1) {
            if (edit_mode) {
                /* ── text edit mode ── */
                if (ch == TK_ESC) {
                    /* cancel: restore saved original */
                    int mi = sel - NPARAMS;
                    char *dst = (mi == 0) ? patch_name :
                                (mi == 1) ? patch_author : patch_comment;
                    size_t maxd = (mi == 2) ? sizeof(patch_comment) : sizeof(patch_name);
                    snprintf(dst, maxd, "%s", orig_meta);
                    edit_mode = 0;
                    draw_editor_screen(sel, 0, "");
                } else if (ch == TK_ENTER || ch == '\r') {
                    edit_mode = 0;
                    draw_editor_screen(sel, 0, "");
                } else if ((ch == TK_BACKSPACE || ch == 127) && edit_len > 0) {
                    edit_buf[--edit_len] = '\0';
                    int mi = sel - NPARAMS;
                    char *dst = (mi == 0) ? patch_name :
                                (mi == 1) ? patch_author : patch_comment;
                    strncpy(dst, edit_buf, (size_t)edit_len + 1);
                    draw_ed_meta(sel, 1, 1, edit_buf);
                    termw_flush();
                } else if (ch >= 32 && ch < 127) {
                    int mi     = sel - NPARAMS;
                    int maxlen = (mi == 2) ? 127 : 63;
                    if (edit_len < maxlen) {
                        edit_buf[edit_len++] = (char)ch;
                        edit_buf[edit_len]   = '\0';
                        char *dst = (mi == 0) ? patch_name :
                                    (mi == 1) ? patch_author : patch_comment;
                        strncpy(dst, edit_buf, (size_t)edit_len + 1);
                        draw_ed_meta(sel, 1, 1, edit_buf);
                        termw_flush();
                    }
                }
            } else {
                /* ── navigation mode ── */
                if (ch == TK_ESC) {
                    /* gate off any held note and return to keyboard screen */
                    pthread_mutex_lock(&sid_lock);
                    release_note();
                    pthread_mutex_unlock(&sid_lock);
                    current_note = 0;
                    break;
                }

                if (ch == TK_F5) {
                    patchbrowser_open_save(&patch,
                                           patch_name, patch_author, patch_comment);
                    draw_editor_screen(sel, 0, "");
                    goto ed_tick;
                }

                if (ch == TK_UP) {
                    if (sel > 0) { sel--; draw_editor_screen(sel, 0, ""); }
                } else if (ch == TK_DOWN) {
                    if (sel < NFIELDS - 1) { sel++; draw_editor_screen(sel, 0, ""); }
                } else if (ch == TK_LEFT || ch == TK_PGDN) {
                    if (sel < NPARAMS) {
                        int step = (ch == TK_PGDN) ? 10 : 1;
                        pthread_mutex_lock(&sid_lock);
                        *pfields[sel].field -= step;
                        if (*pfields[sel].field < pfields[sel].lo)
                            *pfields[sel].field = pfields[sel].lo;
                        apply_patch();
                        pthread_mutex_unlock(&sid_lock);
                        draw_ed_param(sel, 1);
                        termw_flush();
                    }
                } else if (ch == TK_RIGHT || ch == TK_PGUP) {
                    if (sel < NPARAMS) {
                        int step = (ch == TK_PGUP) ? 10 : 1;
                        pthread_mutex_lock(&sid_lock);
                        *pfields[sel].field += step;
                        if (*pfields[sel].field > pfields[sel].hi)
                            *pfields[sel].field = pfields[sel].hi;
                        apply_patch();
                        pthread_mutex_unlock(&sid_lock);
                        draw_ed_param(sel, 1);
                        termw_flush();
                    }
                } else if ((ch == TK_ENTER || ch == '\r') && sel >= NPARAMS) {
                    /* enter text edit mode for the selected metadata field */
                    int mi = sel - NPARAMS;
                    const char *src = (mi == 0) ? patch_name :
                                      (mi == 1) ? patch_author : patch_comment;
                    size_t maxd = (mi == 2) ? sizeof(orig_meta) : sizeof(orig_meta);
                    strncpy(orig_meta, src, maxd - 1);
                    orig_meta[maxd - 1] = '\0';
                    strncpy(edit_buf, src, sizeof(edit_buf) - 1);
                    edit_buf[sizeof(edit_buf) - 1] = '\0';
                    edit_len  = (int)strlen(edit_buf);
                    edit_mode = 1;
                    draw_ed_meta(sel, 1, 1, edit_buf);
                    termw_move(23, 2);
                    printf("%-76s", "");
                    termw_move(23, 2);
                    printf(TW_DIM "Type to edit  ENTER=confirm  ESC=cancel" TW_RESET);
                    termw_flush();
                }

                /* note keys work the same as on the keyboard screen */
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
        }

        /* key-up detection: release note if no repeat within RELEASE_MS */
        if (!edit_mode && current_note > 0 && last_key_ts.tv_sec > 0) {
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

ed_tick:
        audio_tick();
    }
#undef RELEASE_MS
}

/* ───────────────────────────────────────── main loop ── */
static void run_main_loop(void)
{
    struct timespec last_key_ts = {0, 0}; /* time of last note-key event */

    /*
     * Key-up detection via repeat timeout.
     *
     * Raw terminals generate no key-up events.  Held keys repeat at ~30 Hz
     * (interval ≈ 33 ms) after a ~250 ms initial delay.  We poll at 5 ms;
     * each time a note key arrives we reset last_key_ts.  If no event arrives
     * within RELEASE_MS, the key must have been released.
     *
     * 60 ms is safely above the 33 ms repeat interval, but half the old 120 ms
     * value — notes decay noticeably faster when lifted.  The ~250 ms "dead zone"
     * (between initial press and first repeat) means a very short tap may not
     * repeat at all; in that case the note is cut after 60 ms, which is fine.
     *
     * MIDI input bypasses this entirely (midi.c fires true key-up events).
     */
#define RELEASE_MS 60L

    draw_keyboard_screen();

    for (;;) {
        /* poll with 5 ms timeout — fast enough to feel instant, slow enough
         * not to waste CPU, and still lets audio_tick() pace the loop via ALSA */
        int ch = termw_read_key_timeout(5);

        if (ch != -1) {
            /* ── function keys ── */
            if (ch == TK_F1) {
                /* F1 – reset to default patch */
                default_patch();
                patch_name[0] = patch_author[0] = patch_comment[0] = '\0';
                pthread_mutex_lock(&sid_lock);
                apply_patch();
                pthread_mutex_unlock(&sid_lock);
                draw_keyboard_screen();
                goto next_tick;
            }
            if (ch == TK_F3) {
                /* F3 – randomise sound */
                randomise_patch();
                pthread_mutex_lock(&sid_lock);
                apply_patch();
                pthread_mutex_unlock(&sid_lock);
                draw_keyboard_screen();
                goto next_tick;
            }
            if (ch == TK_F5) {
                /* F5 – save patch */
                save_patch();
                draw_keyboard_screen();
                goto next_tick;
            }
            if (ch == TK_F7) {
                /* F7 – open patch browser */
                load_patch();
                draw_keyboard_screen();
                goto next_tick;
            }
            if (ch == TK_F9) {
                /* F9 – load user scale file */
                ui_load_scale();
                draw_keyboard_screen();
                goto next_tick;
            }
            if (ch == TK_F11) {
                /* F11 – save current scale to file */
                ui_save_scale();
                draw_keyboard_screen();
                goto next_tick;
            }
            if (ch == TK_ENTER || ch == '\r' || ch == '\n') {
                /* ENTER – open patch editor */
                run_editor();
                draw_keyboard_screen();
                goto next_tick;
            }
            if (ch == TK_ESC) {
                /* ESC – gate off any held note and quit */
                pthread_mutex_lock(&sid_lock);
                release_note();
                pthread_mutex_unlock(&sid_lock);
                current_note = 0;
                break;
            }

            /* ── note key ── */
            if (ch >= 0 && ch < 256) {
                int note = key_to_note[ch];
                if (note > 0) {
                    /* update the repeat timestamp every time the key event arrives */
                    clock_gettime(CLOCK_MONOTONIC, &last_key_ts);
                    if (current_note != note) {
                        /* key changed — release previous note then gate on new one */
                        pthread_mutex_lock(&sid_lock);
                        release_note();
                        current_note = note;
                        play_note(note);
                        pthread_mutex_unlock(&sid_lock);
                        draw_keyboard_screen();   /* repaint to highlight new key */
                    }
                }
            }
        }

        /* ── key-up detection: gate off if no repeat within RELEASE_MS ── */
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
                draw_keyboard_screen();   /* remove green highlight */
            }
        }

next_tick:
        audio_tick();   /* clock the SID and push audio — must run every iteration */
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

    /* locale needed for UTF-8 box-drawing characters to render correctly */
    setlocale(LC_ALL, "");
    srand((unsigned)time(NULL));

    /* create patches/ directory if absent — must happen before termw_init() */
    patchbrowser_init();

    build_freq_tables();   /* seeds note_raw[] from built-in DATA */
    build_key_map();       /* must run before load_scale() */
    if (scale_file)
        load_scale(scale_file);  /* override note_raw[] + rebuild tables */
    init_alsa();
    init_sid();
    default_patch();
    apply_patch();

    /* sixel detection — must happen before termw_init() takes over the terminal */
    sixel_init(force_sixel);

    /* MIDI input — also before termw_init() so the aconnect hint prints cleanly */
    midi_init(midi_note_on_cb, midi_note_off_cb);

    /* enter raw mode and show splash */
    termw_init();
    termw_clear();
    termw_move(10, 20);
    printf(TW_BOLD TW_CYAN "WELCOME TO SUPER-SYNTH" TW_RESET);
    termw_move(12, 14);
    printf("Initialising SID emulation, please wait...");
    termw_flush();
    /* 800 ms pause so the splash is readable */
    struct timespec splash_wait = {0, 800000000L};
    nanosleep(&splash_wait, NULL);

    run_main_loop();

    /* cleanup — stop MIDI thread before touching SID on the main thread */
    midi_cleanup();
    termw_cleanup();
    pthread_mutex_lock(&sid_lock);
    release_note();
    pthread_mutex_unlock(&sid_lock);
    close_alsa();
    if (sid) resid_free(sid);
    return 0;
}
