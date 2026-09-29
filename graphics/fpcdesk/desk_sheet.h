/****************************************************************************
 * apps/graphics/fpcdesk/desk_sheet.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Spreadsheet's engine (desk_sheet_eng.c, desk_sheet_io.c): cells kept
 * sparsely in a hash table (only those that hold something), formulas
 * parsed into tokens in reverse Polish order, a dependency index (which
 * formulas read a cell or a range), recalculation of what a change
 * reaches in topological order (circular references found and marked),
 * references adjusted when rows and columns come and go or cells move,
 * CSV and the native text format.  Nothing here draws: the application
 * (desk_sheet.c) and the sheet command's self-test (sheet_main.c) use it.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_SHEET_H
#define __APPS_GRAPHICS_FPCDESK_DESK_SHEET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The sheet: rows 1 to 65536, columns A to ZZ */

#define SH_ROWS         65536
#define SH_COLS         702
#define SH_KEY(r, c)    (((uint32_t)(r) << 10) | (uint32_t)(c))
#define SH_ROW(k)       ((int)((k) >> 10))
#define SH_COL(k)       ((int)((k) & 0x3ff))
#define SH_NOKEY        0xffffffffu

/* What a cell holds (kind) */

#define SK_NONE         0                /* Only a format */
#define SK_NUM          1
#define SK_TEXT         2
#define SK_BOOL         3
#define SK_FORMULA      4

/* Values (vt) */

#define SV_EMPTY        0
#define SV_NUM          1
#define SV_STR          2
#define SV_BOOL         3
#define SV_ERR          4
#define SV_RANGE        5                /* In formulas only */

/* Errors */

#define SE_NONE         0
#define SE_DIV0         1                /* #DIV/0! */
#define SE_REF          2                /* #REF! */
#define SE_NAME         3                /* #NAME? */
#define SE_VALUE        4                /* #VALUE! */
#define SE_CIRC         5                /* #CIRC! */
#define SE_NA           6                /* #N/A */
#define SE_NUM          7                /* #NUM! */
#define SE_COUNT        8

/* Formats: a 32-bit word a cell */

#define SF_NF(f)        ((f) & 0x0f)     /* Number format: */
#define SF_GENERAL      0
#define SF_FIXED        1                /* 1234.50 */
#define SF_THOUSANDS    2                /* 1,234.50 */
#define SF_CURRENCY     3                /* $1,234.50 */
#define SF_PERCENT      4                /* 12.5% */
#define SF_DATE         5                /* 2026-01-02 */
#define SF_DATETIME     6                /* 2026-01-02 09:41 */
#define SF_NFMAX        6
#define SF_DEC(f)       (((f) >> 4) & 0x0f)   /* Decimals, 0 to 10 */
#define SF_ALIGN(f)     (((f) >> 8) & 3)
#define SF_AUTO         0                /* Numbers right, text left */
#define SF_LEFT         1
#define SF_CENTRE       2
#define SF_RIGHT        3
#define SF_BOLD         0x0400
#define SF_FG(f)        (((f) >> 12) & 0x0f)  /* Text colour, 0: the theme's */
#define SF_BG(f)        (((f) >> 16) & 0x0f)  /* Fill, 0: none */
#define SF_COLOURS      9                /* Palette entries, 1 to 8 */

#define SF_MAKE(nf, dec, align) ((uint32_t)(nf) | ((uint32_t)(dec) << 4) | \
                                 ((uint32_t)(align) << 8))
#define SF_SET_NF(f, v)    (((f) & ~0x0fu) | (uint32_t)(v))
#define SF_SET_DEC(f, v)   (((f) & ~0xf0u) | ((uint32_t)(v) << 4))
#define SF_SET_ALIGN(f, v) (((f) & ~0x300u) | ((uint32_t)(v) << 8))
#define SF_SET_FG(f, v)    (((f) & ~0xf000u) | ((uint32_t)(v) << 12))
#define SF_SET_BG(f, v)    (((f) & ~0xf0000u) | ((uint32_t)(v) << 16))

#define SH_INPUT_MAX    1024             /* A cell's text as typed */
#define SH_COLW         80               /* A column's width, pixels */
#define SH_COLW_MIN     16
#define SH_COLW_MAX     600

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sh_formula_s;

struct sh_cell_s
{
  uint32_t key;                          /* SH_KEY(row, column) */
  uint32_t fmt;                          /* SF_* */
  uint8_t  kind;                         /* SK_* */
  uint8_t  vt;                           /* SV_*: its value */
  uint8_t  err;                          /* SE_*, vt SV_ERR */
  uint8_t  mark;                         /* Recalculation's */
  uint32_t indeg;                        /* ... */
  double   num;                          /* SV_NUM, SV_BOOL (0 or 1) */
  char    *str;                          /* SK_TEXT's text, a formula's
                                          * string */
  struct sh_formula_s *f;                /* SK_FORMULA */
};

/* A range in a formula's value, or a rectangle */

struct sh_rect_s
{
  int r0;
  int c0;
  int r1;
  int c1;
};

/* A cell as it was (undo, the clipboard): its text as typed and format */

struct sh_snap_s
{
  uint32_t key;
  uint32_t fmt;
  char    *input;                        /* NULL: empty */
};

/* A growing array of keys */

struct sh_keys_s
{
  uint32_t *k;
  int n;
  int cap;
};

/* Ranges read by formulas, by column (struct sh_rdep_s: the engine's) */

struct sh_rvec_s
{
  struct sh_rdep_s *d;
  int n;
  int cap;
};

struct sh_dnode_s;
struct sh_slab_s;

struct sheet_s
{
  /* The cells: open addressing, a power of two */

  struct sh_cell_s **tab;
  uint32_t cap;
  uint32_t n;
  struct sh_cell_s *free;                /* Cells to reuse */
  struct sh_slab_s *slabs;

  /* Who reads what: a cell's readers (hash of key -> keys), ranges'
   * readers by column (or wide: more than a few columns), and the
   * formulas that change by themselves (NOW, TODAY)
   */

  struct sh_dnode_s *dtab;
  uint32_t dcap;
  uint32_t dn;
  struct sh_rvec_s rcol[SH_COLS];
  struct sh_rvec_s rwide;
  struct sh_keys_s vol;

  /* Recalculation: the cells changed since, and afterwards all the cells
   * whose value may have changed (touched; all: too many to say)
   */

  struct sh_keys_s changed;
  struct sh_keys_s touched;
  bool alltouched;
  bool batch;                            /* Loading: recalculate at the end */

  /* Evaluation's strings */

  char *arena;
  int arenan;
  void *stack;

  /* The columns' widths (0: SH_COLW) */

  uint16_t colw[SH_COLS];

  /* Used: the last row and column with anything, -1 none (stale: work it
   * out again)
   */

  int maxrow;
  int maxcol;
  bool extstale;

  /* The time now, as a serial day number (days since 1899-12-30) */

  double (*now)(void);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Sheets */

struct sheet_s *sheet_new(void);
void sheet_free(struct sheet_s *sh);
void sheet_clear_all(struct sheet_s *sh);

/* Cells: NULL if it holds nothing */

struct sh_cell_s *sheet_get(struct sheet_s *sh, int r, int c);

/* Set what is typed: "" empties it (its format stays), "=..." a formula,
 * "'..." text, numbers (1,234.5, $12, 15%, 2026-01-02 give the format
 * too), TRUE / FALSE, else text.  A formula that does not parse: -EINVAL
 * if strict (nothing changes), else it becomes text.
 */

int  sheet_set_input(struct sheet_s *sh, int r, int c, const char *s,
                     bool strict);

/* Its text as typed (a formula's with its references as they are now);
 * "" if empty
 */

void sheet_input(struct sheet_s *sh, struct sh_cell_s *cell, char *buf,
                 size_t size);

/* Formats; empty cells with no format go */

void sheet_set_fmt(struct sheet_s *sh, int r, int c, uint32_t fmt);
uint32_t sheet_fmt(struct sheet_s *sh, int r, int c);

/* A rectangle's contents cleared (and formats too) */

void sheet_clear_rect(struct sheet_s *sh, const struct sh_rect_s *r,
                      bool formats);

/* A formula of a cell copied to (r, c): its relative references moved by
 * (dr, dc) ('$' ones stay), off the sheet: #REF!.  Takes f.
 */

struct sh_formula_s *sheet_formula_dup(const struct sh_formula_s *f,
                                       int dr, int dc);
void sheet_formula_free(struct sh_formula_s *f);
void sheet_set_formula(struct sheet_s *sh, int r, int c,
                       struct sh_formula_s *f);
int  sheet_formula_text(const struct sh_formula_s *f, char *buf,
                        size_t size);

/* Recalculate what changed since the last time (sheet_recalc()), or
 * every formula
 */

void sheet_recalc(struct sheet_s *sh);
void sheet_recalc_all(struct sheet_s *sh);

/* What a cell shows: its value in its format.  prec: the significant
 * digits of the General format (the application makes it fit).  Returns
 * the alignment (SF_LEFT, _CENTRE, _RIGHT).
 */

int  sheet_display(struct sheet_s *sh, struct sh_cell_s *cell, int prec,
                   char *buf, size_t size);
bool sheet_is_number(struct sh_cell_s *cell);   /* Its value is */
const char *sheet_errname(int err);

/* Names: column 0 is "A", (0, 0) "A1"; parsing "B7" (or "$B$7"): the
 * characters used, 0 if none
 */

void sheet_colname(int c, char *buf);
void sheet_refname(int r, int c, char *buf);
int  sheet_parse_ref(const char *s, int *r, int *c);

/* The last row and column used (-1: none) */

void sheet_extent(struct sheet_s *sh, int *maxr, int *maxc);

/* Every cell (in no order); return false to stop */

void sheet_each(struct sheet_s *sh, bool (*fn)(struct sh_cell_s *cell,
                                              void *arg), void *arg);

/* The cells with something in a rectangle, in no order; return false to
 * stop
 */

void sheet_each_in(struct sheet_s *sh, const struct sh_rect_s *r,
                   bool (*fn)(void *arg, struct sh_cell_s *cell), void *arg);

/* The cells in a rectangle, row by row (an array to free), n in *n */

struct sh_cell_s **sheet_cells_in(struct sheet_s *sh,
                                  const struct sh_rect_s *r, int *n);

/* Rows or columns inserted before at, or deleted from at: cells move,
 * references follow (to deleted ones: #REF!).  snap, if not NULL, is
 * given each cell (before the change) that is deleted or whose formula
 * changes, at its old place.
 */

typedef void (*sheet_snap_t)(void *arg, struct sh_cell_s *cell);

void sheet_insert(struct sheet_s *sh, bool rows, int at, int n);
void sheet_delete(struct sheet_s *sh, bool rows, int at, int n,
                  sheet_snap_t snap, void *arg);

/* The cells of src moved by (dr, dc) (cut and paste): what was there
 * goes, references to them follow.  snap as above for every formula
 * elsewhere that changes.
 */

void sheet_move(struct sheet_s *sh, const struct sh_rect_s *src, int dr,
                int dc, sheet_snap_t snap, void *arg);

/* Sort a rectangle's rows by column col (ascending or not); formulas
 * move as copies do
 */

void sheet_sort(struct sheet_s *sh, const struct sh_rect_s *r, int col,
                bool desc);

/* Snapshots (undo) */

void sheet_snap(struct sheet_s *sh, uint32_t key, struct sh_snap_s *s);
void sheet_snap_cell(struct sheet_s *sh, struct sh_cell_s *cell,
                     struct sh_snap_s *s);
void sheet_unsnap(struct sheet_s *sh, const struct sh_snap_s *s);
void sheet_snap_free(struct sh_snap_s *s);

/* Evaluate a formula's text as if in cell (r, c) (tests): its value into
 * the cell given (not in the sheet); -EINVAL if it does not parse
 */

int  sheet_eval_text(struct sheet_s *sh, int r, int c, const char *s,
                     struct sh_cell_s *out);

/* Files (desk_sheet_io.c): CSV (values), the native text format
 * (formulas, formats, widths).  0 or a negated errno.
 */

int  sheet_load_csv(struct sheet_s *sh, const char *path);
int  sheet_save_csv(struct sheet_s *sh, const char *path);
int  sheet_load_native(struct sheet_s *sh, const char *path);
int  sheet_save_native(struct sheet_s *sh, const char *path);
bool sheet_is_native(const char *path);       /* By its name */

/* TSV: the rectangle's values (the clipboard), and back; malloc'd */

char *sheet_tsv(struct sheet_s *sh, const struct sh_rect_s *r, size_t *n);

/* Days: a serial day number from a date, and back */

double sheet_serial(int y, int m, int d);
void sheet_date(double serial, int *y, int *m, int *d, int *hh, int *mm,
                int *ss);

/* Keys */

int  sheet_keys_add(struct sh_keys_s *ks, uint32_t k);
void sheet_keys_free(struct sh_keys_s *ks);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_SHEET_H */
