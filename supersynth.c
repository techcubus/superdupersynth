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
 * Key map  (mirrors C64 keyboard layout used in original):
 *   Q W E R T Y U I O P [ \ ]   – white keys (notes)
 *   2 3   5 6 7   9 0   - =     – black keys (sharps/flats)
 *   A S D F G H J K L : ;       – lower white keys (octave 2)
 *   Z X C V B N M , . /         – lower black-key row
 *   F1  – Normal waveform toggle
 *   F3  – New random sound
 *   F5  – Save patch to file
 *   F7  – Load patch from file
 *   Enter – Toggle values/keyboard screen
 *   ESC  – Quit
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

/* reSID C++ library – we call it through a thin C wrapper declared below */
#ifdef __cplusplus
extern "C" {
#endif
void *resid_new(void);
void  resid_free(void *sid);
void  resid_reset(void *sid);
int   resid_write(void *sid, int reg, int val);
void  resid_clock_delta(void *sid, int cycles, short *buf, int *count);
#ifdef __cplusplus
}
#endif

/* ───────────────────────────────────────── constants ── */
#define SAMPLE_RATE   44100
#define SID_CLOCK     985248   /* PAL */
#define AUDIO_FRAMES  2048
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
static void audio_tick(void);   /* push one buffer of silence / tone */
static void run_main_loop(void);

/* ══════════════════════════════════════════════════════════════════
   reSID thin C wrapper  (compiled as C++, linked in)
   ══════════════════════════════════════════════════════════════════ */

/* ───────────────────────────────────────── freq table builder ── */
static void build_freq_tables(void)
{
    const double M = 1.005;
    const int    K = 256;

    for (int i = 0; note_data[i][0] != -1; i++) {
        int  t  = note_data[i][0];
        int  N  = note_data[i][1];
        if (t <= 0 || t >= MAX_NOTES) continue;

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
 * C64 key-matrix index → ncurses keycode
 * Original program uses PEEK(197) which returns the C64 matrix position.
 * We reverse-map the same physical keys.
 *
 * Upper row (white notes):  Q W E R T Y U I O P @ * ^  Z(ctrl)
 * Upper row (black notes):  2 3 _ 5 6 7 _ 9 0 _ - \  _
 * Lower row (white notes):  A S D F G H J K L : ;
 * Lower row (black notes):  Z X C V B N M , . /
 *
 * Note indices come straight from the DATA tables.
 */
static void build_key_map(void)
{
    memset(key_to_note, 0, sizeof(key_to_note));

    /* Upper white keys */
    key_to_note['q'] = 30;  key_to_note['Q'] = 30;
    key_to_note['w'] = 33;  key_to_note['W'] = 33;
    key_to_note['e'] = 38;  key_to_note['E'] = 38;
    key_to_note['r'] = 41;  key_to_note['R'] = 41;
    key_to_note['t'] = 46;  key_to_note['T'] = 46;
    key_to_note['y'] = 49;  key_to_note['Y'] = 49;
    key_to_note['u'] = 54;  key_to_note['U'] = 54;
    key_to_note['i'] = 12;  key_to_note['I'] = 12;
    key_to_note['o'] = 23;  key_to_note['O'] = 23;
    key_to_note['p'] = 20;  key_to_note['P'] = 20;
    key_to_note['['] = 31;  /* @ on C64 */
    key_to_note[']'] = 28;  /* * on C64 */

    /* Upper black keys */
    key_to_note['2'] = 9;
    key_to_note['3'] = 14;
    key_to_note['5'] = 22;
    key_to_note['6'] = 25;
    key_to_note['7'] = 36;
    key_to_note['9'] = 44;
    key_to_note['0'] = 47;
    key_to_note['-'] = 39;
    key_to_note['='] = 55;

    /* Lower white keys */
    key_to_note['a'] = 62;  key_to_note['A'] = 62;
    key_to_note['s'] = 59;  key_to_note['S'] = 59;
    key_to_note['d'] = 8;   key_to_note['D'] = 8;
    key_to_note['f'] = 16;  key_to_note['F'] = 16;
    key_to_note['g'] = 19;  key_to_note['G'] = 19;
    key_to_note['h'] = 24;  key_to_note['H'] = 24;
    key_to_note['j'] = 32;  key_to_note['J'] = 32;
    key_to_note['k'] = 35;  key_to_note['K'] = 35;
    key_to_note['l'] = 43;  key_to_note['L'] = 43;
    key_to_note[';'] = 48;
    key_to_note['\'']= 51;

    /* Lower black keys */
    key_to_note['z'] = 18;  key_to_note['Z'] = 18;
    key_to_note['x'] = 21;  key_to_note['X'] = 21;
    key_to_note['c'] = 29;  key_to_note['C'] = 29;
    key_to_note['v'] = 34;  key_to_note['V'] = 34;
    key_to_note['b'] = 37;  key_to_note['B'] = 37;
    key_to_note['n'] = 45;  key_to_note['N'] = 45;
    key_to_note['m'] = 50;  key_to_note['M'] = 50;
    key_to_note[','] = 17;
    key_to_note['.'] = 62; /* re-used; adjust if collision */
    key_to_note['/'] = 55;
}

/* ───────────────────────────────────────── ALSA init ── */
static void init_alsa(void)
{
    int err;
    snd_pcm_hw_params_t *hw;
    unsigned int rate = SAMPLE_RATE;

    err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) { fprintf(stderr,"ALSA open: %s\n",snd_strerror(err)); exit(1); }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, 1);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
    snd_pcm_hw_params_set_period_size(pcm, hw, AUDIO_FRAMES, 0);
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
    sid_write(V1_AD,  ad);
    sid_write(V1_SR,  sr);
    sid_write(V2_AD,  ad);
    sid_write(V2_SR,  sr);
    sid_write(FC_LO,  patch.db);
    sid_write(V2_PW_LO, patch.dc);
    sid_write(V3_PW_LO, patch.dd);
    sid_write(V3_FREQ_LO, patch.vi);
    sid_write(V3_FREQ_HI, patch.vs);
    sid_write(RES_FILT, patch.po);
    sid_write(MODE_VOL, patch.vo);
}

/* ───────────────────────────────────────── note on/off ── */
static void play_note(int t)
{
    if (t <= 0 || t >= MAX_NOTES) return;

    switch (patch.z) {
    case 1: /* normal */
        sid_write(V1_FREQ_LO, G1[t]);
        sid_write(V1_FREQ_HI, G2[t]);
        sid_write(V2_FREQ_LO, G3[t]);
        sid_write(V2_FREQ_HI, G4[t]);
        break;
    case 6: /* octave down */
        sid_write(V1_FREQ_LO, H1[t]);
        sid_write(V1_FREQ_HI, H2[t]);
        sid_write(V2_FREQ_LO, H3[t]);
        sid_write(V2_FREQ_HI, H4[t]);
        break;
    default: /* modes 2-5: normal freq + filter variations */
        sid_write(V1_FREQ_LO, F1[t]);
        sid_write(V1_FREQ_HI, F2[t]);
        sid_write(V2_FREQ_LO, F3[t]);
        sid_write(V2_FREQ_HI, F4[t]);
        if (patch.z == 4)
            sid_write(FC_HI, (int)(F1[t] / 0.7));
        else if (patch.z == 5)
            sid_write(FC_HI, F1[t] / 2);
        break;
    }

    sid_write(V1_CTRL, patch.w1);
    sid_write(V2_CTRL, patch.w2);
}

static void release_note(void)
{
    sid_write(V1_CTRL, patch.w1 & ~1);   /* clear gate bit */
    sid_write(V2_CTRL, patch.w2 & ~1);
    sid_write(FC_HI,   0);
}

/* ───────────────────────────────────────── audio tick ── */
/*
 * Clock the SID for AUDIO_FRAMES samples and write to ALSA.
 * Called from the main loop continuously so the SID envelope advances
 * even between key presses.
 */
static void audio_tick(void)
{
    static short buf[AUDIO_FRAMES];
    int cycles_per_sample = SID_CLOCK / SAMPLE_RATE;
    int count = AUDIO_FRAMES;

    resid_clock_delta(sid, cycles_per_sample * AUDIO_FRAMES, buf, &count);

    int written = snd_pcm_writei(pcm, buf, count);
    if (written == -EPIPE) {
        snd_pcm_prepare(pcm);
    }
}

/* ───────────────────────────────────────── randomise patch ── */
static void randomise_patch(void)
{
    static const int waveforms[] = {17,33,65,129,21,23,85};
    static const int vols[]      = {31,45,79};
    static const int vs_vals[]   = {17,33,65,129};
    static const int po_vals[]   = {240,241,242,243};

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
        mvaddch(r, 0,  ACS_VLINE);
        mvaddch(r, 79, ACS_VLINE);
    }
    mvaddch(0,  0,  ACS_ULCORNER);
    mvaddch(0,  79, ACS_URCORNER);
    mvaddch(23, 0,  ACS_LLCORNER);
    mvaddch(23, 79, ACS_LRCORNER);
    attroff(COLOR_PAIR(CP_CYAN));

    /* ── title bar ── */
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(1, 25, "  *** SUPER-SYNTH ***  ");
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    /* ── function key hints ── */
    attron(COLOR_PAIR(CP_YELLOW));
    mvprintw(2, 4,  "F1");  attroff(COLOR_PAIR(CP_YELLOW));
    printw("-Normal  ");
    attron(COLOR_PAIR(CP_YELLOW));
    printw("F3");           attroff(COLOR_PAIR(CP_YELLOW));
    printw("-New Snd  ");
    attron(COLOR_PAIR(CP_YELLOW));
    printw("F5");           attroff(COLOR_PAIR(CP_YELLOW));
    printw("-Save  ");
    attron(COLOR_PAIR(CP_YELLOW));
    printw("F7");           attroff(COLOR_PAIR(CP_YELLOW));
    printw("-Load  ");
    attron(COLOR_PAIR(CP_YELLOW));
    printw("ENTER");        attroff(COLOR_PAIR(CP_YELLOW));
    printw("-Values  ");
    attron(COLOR_PAIR(CP_YELLOW));
    printw("ESC");          attroff(COLOR_PAIR(CP_YELLOW));
    printw("-Quit");

    /* ── keyboard graphic ── */

    /* top row – black keys above white keys */
    /* row 4: key tops */
    attron(COLOR_PAIR(CP_CYAN));
    mvprintw(4, 4, " ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐");
    /* black key row: 2 3  5 6 7  9 0  - = */
    mvprintw(5, 4, " │");
    attroff(COLOR_PAIR(CP_CYAN));

    const char *top_keys = " 2 3   5 6 7   9 0   - =";
    for (int i = 0; top_keys[i]; i++) {
        if (top_keys[i] != ' ') {
            attron(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
            addch(top_keys[i]);
            attroff(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_CYAN));
            addch(' ');
            attroff(COLOR_PAIR(CP_CYAN));
        }
        attron(COLOR_PAIR(CP_CYAN));
        addch('|');
        attroff(COLOR_PAIR(CP_CYAN));
    }

    /* white key row: Q W E R T Y U I O P [ \ ] */
    attron(COLOR_PAIR(CP_CYAN));
    mvprintw(6, 4, " │");
    attroff(COLOR_PAIR(CP_CYAN));

    const char *upper_keys = "Q W E R T Y U I O P [ \\ ]";
    for (int i = 0; upper_keys[i]; i++) {
        char c = upper_keys[i];
        if (c != ' ') {
            attron(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
            addch(c);
            attroff(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_CYAN));
            addch(' ');
            attroff(COLOR_PAIR(CP_CYAN));
        }
        attron(COLOR_PAIR(CP_CYAN));
        addch('|');
        attroff(COLOR_PAIR(CP_CYAN));
    }

    attron(COLOR_PAIR(CP_CYAN));
    mvprintw(7, 4, " ├──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┤");
    mvprintw(8, 4, " │");
    attroff(COLOR_PAIR(CP_CYAN));

    /* middle row – black-key row above lower white keys */
    const char *mid_black = " D F   H J K   : '";
    for (int i = 0; mid_black[i]; i++) {
        char c = mid_black[i];
        if (c != ' ') {
            attron(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
            addch(c);
            attroff(COLOR_PAIR(CP_BLACK_K) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_CYAN));
            addch(' ');
            attroff(COLOR_PAIR(CP_CYAN));
        }
        attron(COLOR_PAIR(CP_CYAN));
        addch('|');
        attroff(COLOR_PAIR(CP_CYAN));
    }

    attron(COLOR_PAIR(CP_CYAN));
    mvprintw(9, 4, " │");
    attroff(COLOR_PAIR(CP_CYAN));

    /* lower white keys: A S D F G H J K L ; ' */
    const char *lower_keys = "A S D F G H J K L ; '";
    for (int i = 0; lower_keys[i]; i++) {
        char c = lower_keys[i];
        if (c != ' ') {
            attron(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
            addch(c);
            attroff(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_CYAN));
            addch(' ');
            attroff(COLOR_PAIR(CP_CYAN));
        }
        attron(COLOR_PAIR(CP_CYAN));
        addch('|');
        attroff(COLOR_PAIR(CP_CYAN));
    }

    attron(COLOR_PAIR(CP_CYAN));
    mvprintw(10, 4, " ├──┼──┼──┼──┼──┼──┼──┼──┼──┼──┤");
    mvprintw(11, 4, " │");
    attroff(COLOR_PAIR(CP_CYAN));

    /* bottom row: Z X C V B N M , . / */
    const char *bottom_keys = "Z X C V B N M , . /";
    for (int i = 0; bottom_keys[i]; i++) {
        char c = bottom_keys[i];
        if (c != ' ') {
            attron(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
            addch(c);
            attroff(COLOR_PAIR(CP_WHITE_K) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_CYAN));
            addch(' ');
            attroff(COLOR_PAIR(CP_CYAN));
        }
        attron(COLOR_PAIR(CP_CYAN));
        addch('|');
        attroff(COLOR_PAIR(CP_CYAN));
    }

    attron(COLOR_PAIR(CP_CYAN));
    mvprintw(12, 4, " └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘");
    attroff(COLOR_PAIR(CP_CYAN));

    /* ── current patch summary ── */
    attron(COLOR_PAIR(CP_GREEN));
    mvprintw(14, 4, "Mode: %d  FL: %d  W1: %3d  W2: %3d  AT:%2d DE:%2d SU:%2d RE:%2d",
             patch.z, patch.fl, patch.w1, patch.w2,
             patch.at, patch.de, patch.su, patch.re);
    mvprintw(15, 4, "Vol: %2d  Res: %3d  VI: %3d  VS: %3d  SL: %3d",
             patch.vo, patch.po, patch.vi, patch.vs, patch.sl);
    attroff(COLOR_PAIR(CP_GREEN));

    /* ── status bar ── */
    attron(COLOR_PAIR(CP_YELLOW));
    mvprintw(22, 2, " Play keys shown above. Hold key for sustain. ");
    attroff(COLOR_PAIR(CP_YELLOW));

    refresh();
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
    int  current_note = 0;    /* 0 = no note active */

    draw_keyboard_screen();

    for (;;) {
        /* Non-blocking key read */
        int ch = getch();

        if (ch != ERR) {
            /* ── function keys ── */
            if (ch == KEY_F(1)) {
                /* F1 – reset to default patch */
                default_patch();
                apply_patch();
                if (!show_values) draw_keyboard_screen();
                continue;
            }
            if (ch == KEY_F(3)) {
                /* F3 – randomise sound */
                randomise_patch();
                apply_patch();
                if (!show_values) draw_keyboard_screen();
                continue;
            }
            if (ch == KEY_F(5)) {
                /* F5 – save patch */
                save_patch();
                if (!show_values) draw_keyboard_screen();
                else             draw_values_screen();
                continue;
            }
            if (ch == KEY_F(7)) {
                /* F7 – load patch */
                load_patch();
                if (!show_values) draw_keyboard_screen();
                else             draw_values_screen();
                continue;
            }
            if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
                show_values = !show_values;
                if (show_values) draw_values_screen();
                else             draw_keyboard_screen();
                continue;
            }
            if (ch == 27 /* ESC */) {
                release_note();
                break;
            }

            if (show_values) continue;   /* ignore note keys on values screen */

            /* ── note key pressed ── */
            if (ch >= 0 && ch < 256) {
                int note = key_to_note[ch];
                if (note > 0) {
                    if (current_note != note) {
                        release_note();
                        current_note = note;
                        play_note(note);
                    }
                }
            }
        } else {
            /* No key – if a note was being held, check nothing new came in */
            /* (ncurses getch timeout keeps us responsive)                   */
        }

        /* Push audio every iteration */
        audio_tick();
    }
}

/* ───────────────────────────────────────── main ── */
int main(void)
{
    srand((unsigned)time(NULL));

    build_freq_tables();
    build_key_map();
    init_alsa();
    init_sid();
    default_patch();
    apply_patch();

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

    /* cleanup */
    endwin();
    release_note();
    close_alsa();
    if (sid) resid_free(sid);
    return 0;
}
