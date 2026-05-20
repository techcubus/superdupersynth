/*
 * patchbrowser.c — modal patch file browser for superdupersynth.
 *
 * Layout (80×24 terminal):
 *
 *   Row  0     ╔══ border ══╗
 *   Row  1     ║ title + mode + current path ║
 *   Row  2     ╠══ separator ══╣
 *   Rows 3-16  ║ two-column file grid (14 rows × 2 cols = 28 entries) ║
 *   Row 17     ╠══ separator ══╣
 *   Row 18     ║ NAME: ...  AUTHOR: ...  ║
 *   Row 19     ║ COMMENT: ...           ║
 *   Row 20     ║ patch values line 1    ║
 *   Row 21     ║ patch values line 2    ║
 *   Row 22     ╠══ separator ══╣
 *   Row 23     ║ key hints ║
 *
 * Navigation:
 *   ↑ ↓        move cursor one row (2 entries) in current column
 *   ← →        switch column on same row
 *   ENTER       enter directory / load file (load mode)
 *               enter directory (save mode)
 *   S           save patch to current directory (save mode only)
 *   N           create new directory in current directory
 *   ESC         cancel / close browser
 *
 * File formats:
 *   .json       Preferred — carries name, author, comment metadata.
 *   anything    Legacy — 17 integers one per line; all values are
 *               range-clamped on read.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "patch.h"
#include "patchbrowser.h"
#include "termw.h"

/* ── layout constants ────────────────────────────────────────────── */
#define GRID_ROWS      14   /* rows 3-16 */
#define ROW_GRID_START  3
#define ROW_PREVIEW    18
#define COL_SEP        40   /* column separator; │ drawn here */
/* left-column content starts at col 2, right at col 42, each 36 chars wide */
#define ENTRY_WIDTH    35   /* usable chars per entry after the cursor glyph */

/* ── storage ─────────────────────────────────────────────────────── */
#define MAX_ENTRIES  512
#define NAME_MAX_LEN 256

typedef struct {
    char name[NAME_MAX_LEN];
    int  is_dir;
} Entry;

/* patches_root + '/' + rel_path must fit in PATH_BUF (520); give each 255 chars max. */
static char  patches_root[255];  /* absolute path of patches/ root — never navigate above */
static char  rel_path[255];      /* path relative to patches_root, e.g. "basses/funk" */
static Entry entries[MAX_ENTRIES];
static int   num_entries = 0;
static int   cursor      = 0;
static int   scroll      = 0;    /* first visible grid row */

/* ── mode ────────────────────────────────────────────────────────── */
typedef enum { PB_LOAD, PB_SAVE } PBMode;
static PBMode mode;

/* ═══════════════════════════════════ path utilities ═════════════ */

/* Returns 1 if currently at the patches root (cannot go further up). */
static int at_root(void)
{
    return rel_path[0] == '\0';
}

/* Max bytes for a full browser path: root(256) + '/' + rel(256) + '\0' = 513.
 * entry paths add '/' + NAME_MAX_LEN(256) = 770 bytes max. */
#define PATH_BUF 520    /* large enough for full_path output */
#define EPATH_BUF 780   /* large enough for entry_path output */

/*
 * Fill out with the full filesystem path to the current browser directory.
 * e.g. patches_root/rel_path  or just patches_root when rel_path is empty.
 * out must be at least PATH_BUF bytes.
 */
static void full_path(char *out, int outsz)
{
    if (rel_path[0])
        snprintf(out, outsz, "%s/%s", patches_root, rel_path);
    else
        snprintf(out, outsz, "%s", patches_root);
}

/*
 * Fill out with the full path to a named entry in the current directory.
 * out must be at least EPATH_BUF bytes.
 */
static void entry_path(char *out, int outsz, const char *name)
{
    char dir[PATH_BUF];
    full_path(dir, sizeof(dir));
    snprintf(out, outsz, "%s/%s", dir, name);
}

/* Navigate into a subdirectory (append to rel_path). */
static void push_dir(const char *name)
{
    if (rel_path[0])
        snprintf(rel_path + strlen(rel_path),
                 sizeof(rel_path) - strlen(rel_path),
                 "/%s", name);
    else
        snprintf(rel_path, sizeof(rel_path), "%s", name);
}

/* Navigate up one level (strip last component of rel_path). */
static void pop_dir(void)
{
    char *slash = strrchr(rel_path, '/');
    if (slash)
        *slash = '\0';
    else
        rel_path[0] = '\0';   /* was one level deep, now at root */
}

/* ═══════════════════════════════════ directory scanning ═════════ */

static int entry_cmp(const void *a, const void *b)
{
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    /* directories sort before files */
    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

/*
 * Load the contents of the current directory into entries[].
 * Resets cursor and scroll.  Hidden files (dot-files) are excluded.
 * A synthetic ".." entry is prepended when not at the root.
 */
static void scan_dir(void)
{
    num_entries = 0;
    cursor      = 0;
    scroll      = 0;

    /* synthetic parent-directory entry (sandboxed — not shown at root) */
    if (!at_root()) {
        strncpy(entries[0].name, "..", NAME_MAX_LEN - 1);
        entries[0].is_dir = 1;
        num_entries = 1;
    }

    int base = num_entries;   /* real entries start here */

    char dir[PATH_BUF];
    full_path(dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) && num_entries < MAX_ENTRIES) {
        /* skip hidden files and the . / .. hardlinks */
        if (de->d_name[0] == '.') continue;

        Entry *e = &entries[num_entries];
        snprintf(e->name, NAME_MAX_LEN, "%s", de->d_name);

        /* detect directory — check d_type first, fall back to stat */
        if (de->d_type == DT_DIR) {
            e->is_dir = 1;
        } else if (de->d_type == DT_UNKNOWN) {
            char full[EPATH_BUF];
            entry_path(full, sizeof(full), de->d_name);
            struct stat st;
            e->is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        } else {
            e->is_dir = 0;
        }
        num_entries++;
    }
    closedir(d);

    /* sort the real entries; leave the synthetic ".." at index 0 */
    if (num_entries - base > 1)
        qsort(entries + base, num_entries - base, sizeof(Entry), entry_cmp);
}

/* ═══════════════════════════════════ JSON helpers ═══════════════ */

/*
 * Write a JSON string value with minimal escaping (backslash and double-quote).
 * Other control characters are omitted.
 */
static void write_escaped(FILE *f, const char *s)
{
    while (*s) {
        if (*s == '\\' || *s == '"') fputc('\\', f);
        if ((unsigned char)*s >= 32)  fputc(*s, f);
        s++;
    }
}

/*
 * Search one line of JSON for:  "key": "value"
 * Fills out (up to n bytes, NUL-terminated) and returns 1 on success.
 */
static int jstr(const char *line, const char *key, char *out, int n)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    int i = 0;
    while (*p && *p != '"' && i < n - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;    /* consume backslash */
            if (*p == '"' || *p == '\\') out[i++] = *p;
            /* other escape sequences are skipped */
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

/*
 * Search one line of JSON for:  "key": number
 * Sets *out and returns 1 on success.
 */
static int jint(const char *line, const char *key, int *out)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\": ", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    *out = atoi(p);
    return 1;
}

/* ═══════════════════════════════════ patch validation ═══════════ */

/*
 * Clamp all patch fields to their valid ranges.
 * Called after loading any file format to prevent garbage values
 * reaching the SID registers.
 */
static void clamp_patch(Patch *p)
{
#define CLAMP(v, lo, hi) do { \
    if ((v) < (lo)) (v) = (lo); \
    if ((v) > (hi)) (v) = (hi); \
} while (0)
    CLAMP(p->z,  1,   6);
    CLAMP(p->fl, 0,   2);
    CLAMP(p->w1, 0, 255);
    CLAMP(p->w2, 0, 255);
    CLAMP(p->at, 0,  15);
    CLAMP(p->de, 0,  15);
    CLAMP(p->su, 0,  15);
    CLAMP(p->re, 0,  15);
    CLAMP(p->po, 0, 255);
    CLAMP(p->xt, 0, 255);
    CLAMP(p->vi, 0, 255);
    CLAMP(p->vs, 0, 255);
    CLAMP(p->db, 0, 255);
    CLAMP(p->dc, 0, 255);
    CLAMP(p->dd, 0, 255);
    CLAMP(p->vo, 0, 255);
    CLAMP(p->sl, 0, 255);
#undef CLAMP
}

/* ═══════════════════════════════════ file I/O ═══════════════════ */

/*
 * Load a JSON patch file.
 * Fills p, name, author, comment (each NUL-terminated).
 * Returns 0 on success, -1 if the file cannot be opened or lacks a patch object.
 */
static int load_json(const char *path, Patch *p,
                     char *name, char *author, char *comment)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* safe defaults if fields are absent */
    strncpy(name,    "untitled", 63);  name[63]    = '\0';
    strncpy(author,  "",         63);  author[63]  = '\0';
    strncpy(comment, "",        127);  comment[127] = '\0';

    int  in_patch = 0;   /* 1 once we have seen the "patch": { line */
    int  found    = 0;   /* set when at least one patch integer was parsed */
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\"patch\"")) { in_patch = 1; continue; }

        if (!in_patch) {
            jstr(line, "name",    name,    64);
            jstr(line, "author",  author,  64);
            jstr(line, "comment", comment, 128);
        } else {
            /* parse each patch field by name */
            found |= jint(line, "z",  &p->z);
            found |= jint(line, "fl", &p->fl);
            found |= jint(line, "w1", &p->w1);
            found |= jint(line, "w2", &p->w2);
            found |= jint(line, "at", &p->at);
            found |= jint(line, "de", &p->de);
            found |= jint(line, "su", &p->su);
            found |= jint(line, "re", &p->re);
            found |= jint(line, "po", &p->po);
            found |= jint(line, "xt", &p->xt);
            found |= jint(line, "vi", &p->vi);
            found |= jint(line, "vs", &p->vs);
            found |= jint(line, "db", &p->db);
            found |= jint(line, "dc", &p->dc);
            found |= jint(line, "dd", &p->dd);
            found |= jint(line, "vo", &p->vo);
            found |= jint(line, "sl", &p->sl);
        }
    }
    fclose(f);
    if (!found) return -1;
    clamp_patch(p);
    return 0;
}

/*
 * Load a legacy plain-text patch file (17 integers, one per line).
 * This matches the format originally saved by the C64 BASIC program.
 * All values are clamped to valid ranges — do not trust untrusted files.
 * Returns 0 on success, -1 if the file is missing or truncated.
 */
static int load_legacy(const char *path, Patch *p)
{
    FILE *f = fopen(path, "rb");  /* binary mode: do not translate \r */
    if (!f) return -1;

    int vals[17];
    int n = 0;

    /* fscanf " %d" skips any whitespace (space, \r, \n) between values,
     * so this handles both Unix (LF) and original C64 (CR-only) line endings */
    while (n < 17 && fscanf(f, " %d", &vals[n]) == 1)
        n++;

    fclose(f);

    if (n < 17) return -1;  /* file was truncated */

    /* assign in the same order as the original save: z fl w1 w2 at de su re
     * po xt vi vs db dc dd vo sl */
    p->z  = vals[0];  p->fl = vals[1];
    p->w1 = vals[2];  p->w2 = vals[3];
    p->at = vals[4];  p->de = vals[5];
    p->su = vals[6];  p->re = vals[7];
    p->po = vals[8];  p->xt = vals[9];
    p->vi = vals[10]; p->vs = vals[11];
    p->db = vals[12]; p->dc = vals[13];
    p->dd = vals[14]; p->vo = vals[15];
    p->sl = vals[16];

    clamp_patch(p);
    return 0;
}

/*
 * Write a JSON patch file.
 * name / author / comment are metadata strings; p is the patch data.
 */
static int save_json(const char *path, const Patch *p,
                     const char *name, const char *author, const char *comment)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"");    write_escaped(f, name);    fprintf(f, "\",\n");
    fprintf(f, "  \"author\": \"");  write_escaped(f, author);  fprintf(f, "\",\n");
    fprintf(f, "  \"comment\": \""); write_escaped(f, comment); fprintf(f, "\",\n");
    fprintf(f, "  \"patch\": {\n");
    fprintf(f, "    \"z\":  %d, \"fl\": %d,\n",    p->z,  p->fl);
    fprintf(f, "    \"w1\": %d, \"w2\": %d,\n",    p->w1, p->w2);
    fprintf(f, "    \"at\": %d, \"de\": %d,\n",    p->at, p->de);
    fprintf(f, "    \"su\": %d, \"re\": %d,\n",    p->su, p->re);
    fprintf(f, "    \"po\": %d, \"xt\": %d,\n",    p->po, p->xt);
    fprintf(f, "    \"vi\": %d, \"vs\": %d,\n",    p->vi, p->vs);
    fprintf(f, "    \"db\": %d, \"dc\": %d, \"dd\": %d,\n", p->db, p->dc, p->dd);
    fprintf(f, "    \"vo\": %d, \"sl\": %d\n",     p->vo, p->sl);
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════ drawing ════════════════════ */

/* Truncate a filename for display, appending "…" if it exceeds maxlen. */
static void trunc_name(const char *src, char *dst, int maxlen)
{
    int len = (int)strlen(src);
    if (len <= maxlen) {
        strncpy(dst, src, maxlen);
        dst[maxlen] = '\0';
    } else {
        strncpy(dst, src, maxlen - 1);
        dst[maxlen - 1] = '\0';
        /* append Unicode ellipsis — fits in 3 UTF-8 bytes, 1 terminal column */
        strcat(dst, "…");
    }
}

/*
 * Draw the outer border, title bar, and hint bar.
 * Called once at the start of each full redraw.
 */
static void draw_chrome(void)
{
    /* outer border */
    printf(TW_CYAN);
    termw_move(0, 0);
    printf("╔");  for (int c = 1; c < 79; c++) printf("═");  printf("╗");
    for (int r = 1; r < 23; r++) {
        termw_move(r,  0); printf("║");
        termw_move(r, 79); printf("║");
    }
    termw_move(23, 0);
    printf("╚");  for (int c = 1; c < 79; c++) printf("═");  printf("╝");

    /* row 2: separator with centre column tee */
    termw_move(2, 0);
    printf("╠");
    for (int c = 1; c < 79; c++) printf(c == COL_SEP ? "╤" : "═");
    printf("╣");

    /* rows 3-16: centre column divider */
    for (int r = ROW_GRID_START; r < ROW_GRID_START + GRID_ROWS; r++) {
        termw_move(r, COL_SEP); printf("│");
    }

    /* row 17: separator collapsing the centre tee */
    termw_move(ROW_GRID_START + GRID_ROWS, 0);
    printf("╠");
    for (int c = 1; c < 79; c++) printf(c == COL_SEP ? "╧" : "═");
    printf("╣");

    /* row 22: separator above hints */
    termw_move(22, 0);
    printf("╠");  for (int c = 1; c < 79; c++) printf("═");  printf("╣");
    printf(TW_RESET);

    /* title: mode + path */
    termw_move(1, 2);
    printf(TW_BOLD "%s" TW_RESET "  " TW_CYAN "patches/%s" TW_RESET,
           mode == PB_LOAD ? "LOAD PATCH" : "SAVE PATCH",
           rel_path);

    /* hints */
    termw_move(23, 2);
    if (mode == PB_LOAD) {
        printf(TW_DIM "↑↓←→ nav  ENTER=open/load  N=new dir  ESC=cancel" TW_RESET);
    } else {
        printf(TW_DIM "↑↓←→ nav  ENTER=open dir  S=save here  N=new dir  ESC=cancel" TW_RESET);
    }
}

/*
 * Draw the two-column file grid for the current scroll position.
 * Entries are laid out left-to-right, top-to-bottom:
 *   entries[0] entries[1]
 *   entries[2] entries[3]
 *   ...
 * The cursor entry is highlighted with reverse video.
 */
static void draw_grid(void)
{
    int first = scroll * 2;   /* index of first visible entry */

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < 2; col++) {
            int idx = first + row * 2 + col;

            /* column starting position */
            int screen_col = (col == 0) ? 2 : COL_SEP + 2;
            termw_move(ROW_GRID_START + row, screen_col);

            if (idx >= num_entries) {
                /* empty cell — clear to prevent stale content */
                printf("%-*s", ENTRY_WIDTH + 2, "");
                continue;
            }

            const Entry *e   = &entries[idx];
            int          sel = (idx == cursor);

            /* truncated display name; directories get a trailing slash */
            char disp[NAME_MAX_LEN + 4];
            if (e->is_dir) {
                char tmp[NAME_MAX_LEN];
                snprintf(tmp, sizeof(tmp), "%s/", e->name);
                trunc_name(tmp, disp, ENTRY_WIDTH);
            } else {
                trunc_name(e->name, disp, ENTRY_WIDTH);
            }

            if (sel) {
                /* selected entry: reverse video */
                printf(TW_REVERSE TW_BOLD "> %-*s" TW_RESET, ENTRY_WIDTH, disp);
            } else if (e->is_dir) {
                /* directory: cyan */
                printf(TW_CYAN "  %-*s" TW_RESET, ENTRY_WIDTH, disp);
            } else {
                /* check for .json extension */
                const char *dot = strrchr(e->name, '.');
                if (dot && strcmp(dot, ".json") == 0)
                    printf("  %-*s", ENTRY_WIDTH, disp);        /* normal weight */
                else
                    printf(TW_DIM "  %-*s" TW_RESET, ENTRY_WIDTH, disp); /* legacy: dim */
            }
        }
    }
}

/*
 * Draw the preview area (rows 18-21) for the currently selected entry.
 * For JSON files: reads metadata without touching the live patch.
 * For legacy files: shows values only (no metadata).
 * For directories: shows a brief description.
 */
static void draw_preview(void)
{
    /* clear preview rows */
    for (int r = ROW_PREVIEW; r < ROW_PREVIEW + 4; r++) {
        termw_move(r, 1);
        printf("%-77s", "");
    }

    if (cursor < 0 || cursor >= num_entries) return;

    const Entry *e = &entries[cursor];

    if (e->is_dir) {
        termw_move(ROW_PREVIEW, 2);
        printf(TW_CYAN "Directory" TW_RESET);
        return;
    }

    char path[EPATH_BUF];
    entry_path(path, sizeof(path), e->name);

    /* detect format by extension */
    const char *dot = strrchr(e->name, '.');
    int is_json = (dot && strcmp(dot, ".json") == 0);

    Patch      tmp;
    char       name[64]    = "";
    char       author[64]  = "";
    char       comment[128] = "";
    int        ok;

    if (is_json)
        ok = (load_json(path, &tmp, name, author, comment) == 0);
    else
        ok = (load_legacy(path, &tmp) == 0);

    if (!ok) {
        termw_move(ROW_PREVIEW, 2);
        printf(TW_RED "Cannot read file." TW_RESET);
        return;
    }

    if (is_json) {
        /* metadata rows */
        termw_move(ROW_PREVIEW, 2);
        printf(TW_BOLD "NAME:   " TW_RESET "%-32s  "
               TW_BOLD "AUTHOR: " TW_RESET "%s",
               name, author);
        termw_move(ROW_PREVIEW + 1, 2);
        printf(TW_BOLD "COMMENT:" TW_RESET " %.66s", comment);
    } else {
        /* legacy file — no metadata to show */
        termw_move(ROW_PREVIEW, 2);
        printf(TW_DIM "(legacy format — no metadata)" TW_RESET);
    }

    /* abbreviated patch values — two lines */
    termw_move(ROW_PREVIEW + 2, 2);
    printf(TW_DIM "Z:%-2d FL:%d  W1:%-3d W2:%-3d  AT:%-2d DE:%-2d SU:%-2d RE:%-2d"
           TW_RESET,
           tmp.z, tmp.fl, tmp.w1, tmp.w2,
           tmp.at, tmp.de, tmp.su, tmp.re);
    termw_move(ROW_PREVIEW + 3, 2);
    printf(TW_DIM "PO:%-3d XT:%-3d VI:%-3d VS:%-3d  DB:%-3d DC:%-3d DD:%-3d VO:%-3d SL:%-3d"
           TW_RESET,
           tmp.po, tmp.xt, tmp.vi, tmp.vs,
           tmp.db, tmp.dc, tmp.dd, tmp.vo, tmp.sl);
}

/* Full redraw of the browser screen. */
static void draw_all(void)
{
    termw_clear();
    draw_chrome();
    draw_grid();
    draw_preview();
    termw_flush();
}

/* ═══════════════════════════════════ in-browser text input ══════ */

/*
 * Prompt for a line of text within the browser's preview area at
 * (row, col).  Echoes characters, handles backspace.
 * Enter confirms; ESC cancels and returns buf[0]='\0'.
 */
static void pb_prompt(int row, int col, const char *prompt, char *buf, int maxlen)
{
    int len = 0;
    buf[0]  = '\0';

    termw_move(row, col);
    for (int i = 0; i < 76 - col; i++) putchar(' ');  /* clear line */
    termw_move(row, col);
    printf(TW_BOLD TW_CYAN "%s" TW_RESET, prompt);
    termw_cursor_show();
    termw_flush();

    int ch;
    while ((ch = termw_read_key()) != TK_ENTER && ch != TK_ESC) {
        if ((ch == TK_BACKSPACE || ch == 127) && len > 0) {
            len--;
            buf[len] = '\0';
            printf("\b \b");
            termw_flush();
        } else if (ch >= 32 && ch < 127 && len < maxlen - 1) {
            buf[len++] = (char)ch;
            buf[len]   = '\0';
            putchar(ch);
            termw_flush();
        }
    }

    if (ch == TK_ESC) buf[0] = '\0';
    termw_cursor_hide();
}

/* ═══════════════════════════════════ save flow ══════════════════ */

/*
 * Run the interactive save flow: prompt for filename and metadata in the
 * preview area, then write the JSON file.  Rescans the directory on success.
 * Returns 1 if saved, 0 if cancelled at any prompt.
 */
static int do_save(const Patch *p)
{
    char fname[64]    = "";
    char name[64]     = "";
    char author[64]   = "";
    char comment[128] = "";

    /* clear preview rows for prompt display */
    for (int r = ROW_PREVIEW; r < ROW_PREVIEW + 4; r++) {
        termw_move(r, 1); printf("%-77s", "");
    }

    pb_prompt(ROW_PREVIEW,     2, "Filename (no ext): ", fname,   62);
    if (!fname[0]) return 0;

    pb_prompt(ROW_PREVIEW + 1, 2, "Name:              ", name,    62);
    if (!name[0]) return 0;

    pb_prompt(ROW_PREVIEW + 2, 2, "Author:            ", author,  62);
    /* author is optional — don't cancel on empty */

    pb_prompt(ROW_PREVIEW + 3, 2, "Comment:           ", comment, 126);
    /* comment is optional too */

    /* build the full path: current dir / filename.json */
    char path[EPATH_BUF];
    {
        char dir[PATH_BUF];
        full_path(dir, sizeof(dir));
        snprintf(path, sizeof(path), "%s/%s.json", dir, fname);
    }

    if (save_json(path, p, name, author, comment) < 0) {
        /* write failed — show error in preview row and wait for a key */
        termw_move(ROW_PREVIEW, 2);
        printf(TW_RED TW_BOLD "ERROR: could not write file." TW_RESET);
        termw_flush();
        termw_read_key();
        return 0;
    }

    scan_dir();
    return 1;
}

/* ═══════════════════════════════════ main browser loop ══════════ */

/*
 * Adjust scroll so the cursor row is visible within the grid.
 * Grid shows rows scroll..scroll+GRID_ROWS-1; cursor is at row cursor/2.
 */
static void ensure_visible(void)
{
    int crow = cursor / 2;
    if (crow < scroll)
        scroll = crow;
    else if (crow >= scroll + GRID_ROWS)
        scroll = crow - GRID_ROWS + 1;
}

/*
 * Core browser event loop.
 * Returns 1 if a patch was loaded / saved, 0 if cancelled.
 * In LOAD mode, fills *result on success.
 * In SAVE mode, result may be NULL.
 */
static int run_browser(Patch *result, const Patch *to_save)
{
    scan_dir();
    draw_all();

    for (;;) {
        int ch = termw_read_key();

        if (ch == TK_ESC) return 0;

        if (ch == TK_UP) {
            /* move up one row: -2 entries */
            int col = cursor % 2;
            int row = cursor / 2;
            if (row > 0) cursor = (row - 1) * 2 + col;
            if (cursor >= num_entries) cursor = num_entries - 1;

        } else if (ch == TK_DOWN) {
            /* move down one row: +2 entries */
            int col = cursor % 2;
            int row = cursor / 2;
            int next = (row + 1) * 2 + col;
            if (next < num_entries) cursor = next;

        } else if (ch == TK_LEFT) {
            /* switch to left column if in right column */
            if (cursor % 2 == 1) cursor--;

        } else if (ch == TK_RIGHT) {
            /* switch to right column if in left column and entry exists */
            if (cursor % 2 == 0 && cursor + 1 < num_entries) cursor++;

        } else if (ch == TK_ENTER || ch == '\r') {
            if (cursor < 0 || cursor >= num_entries) continue;
            const Entry *e = &entries[cursor];

            if (e->is_dir) {
                /* ".." goes up; anything else goes down */
                if (strcmp(e->name, "..") == 0)
                    pop_dir();
                else
                    push_dir(e->name);
                scan_dir();

            } else if (mode == PB_LOAD) {
                /* load the selected file */
                char path[EPATH_BUF];
                entry_path(path, sizeof(path), e->name);

                const char *dot = strrchr(e->name, '.');
                int is_json = (dot && strcmp(dot, ".json") == 0);

                Patch     tmp;
                char      name[64], author[64], comment[128];
                int       ok = is_json
                    ? load_json(path, &tmp, name, author, comment)
                    : load_legacy(path, &tmp);

                if (ok < 0) {
                    /* show brief error then stay in browser */
                    termw_move(ROW_PREVIEW, 2);
                    printf(TW_RED TW_BOLD "ERROR: cannot load file." TW_RESET);
                    termw_flush();
                    termw_read_key();
                } else {
                    *result = tmp;
                    return 1;
                }
            }
            /* in SAVE mode, Enter on a file does nothing — use S to save */

        } else if (ch == 'n' || ch == 'N') {
            /* create a new directory in the current location */
            char dname[64] = "";
            pb_prompt(ROW_PREVIEW, 2, "New directory name: ", dname, 62);
            if (dname[0]) {
                char path[EPATH_BUF];
                {
                    char dir[PATH_BUF];
                    full_path(dir, sizeof(dir));
                    snprintf(path, sizeof(path), "%s/%s", dir, dname);
                }
                if (mkdir(path, 0755) < 0 && errno != EEXIST) {
                    termw_move(ROW_PREVIEW, 2);
                    printf(TW_RED TW_BOLD "ERROR: could not create directory." TW_RESET);
                    termw_flush();
                    termw_read_key();
                } else {
                    scan_dir();
                }
            }

        } else if ((ch == 's' || ch == 'S') && mode == PB_SAVE) {
            if (do_save(to_save)) return 1;
        }

        ensure_visible();
        draw_all();
    }
}

/* ═══════════════════════════════════ public API ═════════════════ */

/*
 * Initialise the patches directory.  Creates patches/ relative to the
 * current working directory if it does not exist.
 * Call once at startup before entering raw terminal mode.
 */
void patchbrowser_init(void)
{
    /* build absolute path so we can safely detect root later */
    if (!realpath("patches", patches_root)) {
        /* patches/ doesn't exist yet — build the path manually then create */
        char cwd[246];   /* 255 - len("/patches") = 246 bytes max for cwd */
        if (getcwd(cwd, sizeof(cwd)))
            snprintf(patches_root, sizeof(patches_root), "%s/patches", cwd);
        else
            snprintf(patches_root, sizeof(patches_root), "patches");
    }

    mkdir(patches_root, 0755);   /* no-op if already exists */
    rel_path[0] = '\0';
}

int patchbrowser_open_load(Patch *p)
{
    mode       = PB_LOAD;
    rel_path[0] = '\0';   /* always start at root */
    return run_browser(p, NULL);
}

int patchbrowser_open_save(const Patch *p)
{
    mode       = PB_SAVE;
    rel_path[0] = '\0';
    Patch dummy;
    return run_browser(&dummy, p);
}
