/****************************************************************************
 * apps/graphics/fpcdesk/sheet_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * sheet: the Spreadsheet's engine from NSH (desk_sheet.h).
 *
 *   sheet --selftest [DIR]  the formula engine's tests (operators,
 *                           functions, references, errors, circular
 *                           references, rows and columns inserted and
 *                           deleted, copies, moves, sorting, files in DIR)
 *   sheet --bench [ROWS [DIR]]  a big sheet: its memory and recalculation
 *                           time (and its files', in DIR)
 *   sheet FILE              a .csv or .sheet file's values, as CSV
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "desk_num.h"
#include "desk_sheet.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_pass;
static int g_fail;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static double fixed_now(void)
{
  return sheet_serial(2026, 1, 2) + (9 * 60 + 41) / 1440.0;
}

static struct sh_cell_s *at(struct sheet_s *sh, const char *ref)
{
  int r;
  int c;

  if (sheet_parse_ref(ref, &r, &c) == 0)
    {
      return NULL;
    }

  return sheet_get(sh, r, c);
}

static void put(struct sheet_s *sh, const char *ref, const char *s)
{
  int r;
  int c;

  if (sheet_parse_ref(ref, &r, &c) == 0 ||
      sheet_set_input(sh, r, c, s, true) < 0)
    {
      printf("  cannot set %s to %s\n", ref, s);
      g_fail++;
    }

  sheet_recalc(sh);
}

static void result(bool ok, int line, const char *what, const char *got)
{
  if (ok)
    {
      g_pass++;
    }
  else
    {
      g_fail++;
      printf("  FAIL line %d: %s: got %s\n", line, what, got);
    }
}

static void got_text(struct sh_cell_s *c, char *buf, size_t size)
{
  if (c == NULL)
    {
      strlcpy(buf, "(empty)", size);
    }
  else if (c->vt == SV_NUM)
    {
      num_plain(c->num, 15, buf, size);
    }
  else if (c->vt == SV_STR)
    {
      snprintf(buf, size, "\"%s\"", c->str != NULL ? c->str : "");
    }
  else if (c->vt == SV_BOOL)
    {
      strlcpy(buf, c->num != 0 ? "TRUE" : "FALSE", size);
    }
  else if (c->vt == SV_ERR)
    {
      strlcpy(buf, sheet_errname(c->err), size);
    }
  else
    {
      strlcpy(buf, "(none)", size);
    }
}

/* A value: a number within 1e-9 (relative), "text", TRUE/FALSE, #ERR */

static void check_cell(int line, struct sh_cell_s *c, const char *want,
                       const char *what)
{
  char got[128];
  bool ok = false;
  double v;

  got_text(c, got, sizeof(got));
  if (want[0] == '#' || strcmp(want, "TRUE") == 0 ||
      strcmp(want, "FALSE") == 0 || want[0] == '"')
    {
      ok = strcmp(got, want) == 0;
    }
  else if (c != NULL && c->vt == SV_NUM && num_parse(want, &v) > 0)
    {
      ok = fabs(c->num - v) <= 1e-9 * (fabs(v) > 1 ? fabs(v) : 1);
    }

  result(ok, line, what, got);
}

static void check_eval(int line, struct sheet_s *sh, const char *expr,
                       const char *want)
{
  struct sh_cell_s c;
  char what[160];

  snprintf(what, sizeof(what), "%s = %s", expr, want);
  if (sheet_eval_text(sh, 99, 99, expr, &c) < 0)
    {
      result(false, line, what, "(does not parse)");
      return;
    }

  check_cell(line, &c, want, what);
  free(c.str);
}

static void check_input(int line, struct sheet_s *sh, const char *ref,
                        const char *want)
{
  char got[256];
  char what[160];

  sheet_input(sh, at(sh, ref), got, sizeof(got));
  snprintf(what, sizeof(what), "%s holds %s", ref, want);
  result(strcmp(got, want) == 0, line, what, got);
}

static void check_shown(int line, struct sheet_s *sh, const char *ref,
                        const char *want)
{
  char got[128];
  char what[160];

  sheet_display(sh, at(sh, ref), 10, got, sizeof(got));
  snprintf(what, sizeof(what), "%s shows %s", ref, want);
  result(strcmp(got, want) == 0, line, what, got);
}

#define EVAL(e, w)    check_eval(__LINE__, sh, e, w)
#define CELL(r, w)    check_cell(__LINE__, at(sh, r), w, r " = " w)
#define INPUT(r, w)   check_input(__LINE__, sh, r, w)
#define SHOWN(r, w)   check_shown(__LINE__, sh, r, w)
#define OK(x)         result((x), __LINE__, #x, "false")

static int selftest(const char *dir)
{
  struct sheet_s *sh = sheet_new();
  struct sh_rect_s r;
  char path[256];
  char buf[256];
  int i;

  if (sh == NULL)
    {
      return -ENOMEM;
    }

  sh->now = fixed_now;

  /* Operators and their precedence */

  EVAL("=1+2*3", "7");
  EVAL("=(1+2)*3", "9");
  EVAL("=2^3^2", "64");
  EVAL("=-2^2", "4");
  EVAL("=2^-1", "0.5");
  EVAL("=10/4", "2.5");
  EVAL("=7-2-1", "4");
  EVAL("=5%", "0.05");
  EVAL("=50%*10", "5");
  EVAL("=--3", "3");
  EVAL("=0.1+0.2", "0.3");
  EVAL("=\"ab\"&\"cd\"", "\"abcd\"");
  EVAL("=1&2", "\"12\"");
  EVAL("=\"say \"\"hi\"\"\"", "\"say \"hi\"\"");
  EVAL("=1+2=3", "TRUE");
  EVAL("=1<2", "TRUE");
  EVAL("=2<=1", "FALSE");
  EVAL("=1<>1", "FALSE");
  EVAL("=\"abc\"=\"ABC\"", "TRUE");
  EVAL("=\"b\">\"a\"", "TRUE");
  EVAL("=1<\"a\"", "TRUE");
  EVAL("=TRUE>1", "TRUE");
  EVAL("=TRUE+1", "2");
  EVAL("=\"3\"+4", "7");
  EVAL("=\"x\"+1", "#VALUE!");
  EVAL("=1/0", "#DIV/0!");
  EVAL("=(1/0)+1", "#DIV/0!");
  EVAL("=1e3+1.5e-1", "1000.15");

  /* Functions */

  put(sh, "A1", "10");
  put(sh, "A2", "20");
  put(sh, "A3", "30");
  put(sh, "A4", "text");
  put(sh, "A5", "TRUE");
  put(sh, "B1", "1");
  put(sh, "B2", "2");
  put(sh, "B3", "3");
  EVAL("=SUM(A1:A5)", "60");
  EVAL("=SUM(A1:A3,5,\"5\",TRUE)", "71");
  EVAL("=SUM(A1:B3)", "66");
  EVAL("=AVERAGE(A1:A5)", "20");
  EVAL("=AVERAGE(C1:C5)", "#DIV/0!");
  EVAL("=MIN(A1:A3,B1:B3)", "1");
  EVAL("=MAX(A1:A3,B1:B3)", "30");
  EVAL("=MAX(C1:C3)", "0");
  EVAL("=COUNT(A1:A5)", "3");
  EVAL("=COUNTA(A1:A5)", "5");
  EVAL("=PRODUCT(B1:B3)", "6");
  EVAL("=IF(A1>5,\"big\",\"small\")", "\"big\"");
  EVAL("=IF(A1>50,\"big\")", "FALSE");
  EVAL("=IF(0,1/0,2)", "2");
  EVAL("=AND(TRUE,1,A5)", "TRUE");
  EVAL("=AND(TRUE,0)", "FALSE");
  EVAL("=OR(FALSE,0,A1>5)", "TRUE");
  EVAL("=NOT(A1=10)", "FALSE");
  EVAL("=ROUND(2.675,2)", "2.68");
  EVAL("=ROUND(-2.5,0)", "-3");
  EVAL("=ROUND(1234.5,-2)", "1200");
  EVAL("=ROUNDUP(1.21,1)", "1.3");
  EVAL("=ROUNDDOWN(-1.29,1)", "-1.2");
  EVAL("=ABS(-4)", "4");
  EVAL("=INT(-1.5)", "-2");
  EVAL("=MOD(-7,3)", "2");
  EVAL("=MOD(7,0)", "#DIV/0!");
  EVAL("=SQRT(16)", "4");
  EVAL("=SQRT(-1)", "#NUM!");
  EVAL("=POWER(2,10)", "1024");
  EVAL("=LEN(\"hello\")", "5");
  EVAL("=LEFT(\"hello\",2)", "\"he\"");
  EVAL("=LEFT(\"hello\")", "\"h\"");
  EVAL("=RIGHT(\"hello\",3)", "\"llo\"");
  EVAL("=MID(\"spreadsheet\",7,5)", "\"sheet\"");
  EVAL("=UPPER(\"abc\")", "\"ABC\"");
  EVAL("=LOWER(A4&\"X\")", "\"textx\"");
  EVAL("=TRIM(\"  a   b  \")", "\"a b\"");
  EVAL("=CONCAT(\"a\",1,TRUE)", "\"a1TRUE\"");
  EVAL("=CONCAT(A1:A2,B1)", "\"10201\"");
  EVAL("=TEXT(1234.567,\"#,##0.00\")", "\"1,234.57\"");
  EVAL("=TEXT(0.256,\"0.0%\")", "\"25.6%\"");
  EVAL("=TEXT(12,\"$0.00\")", "\"$12.00\"");
  EVAL("=TEXT(TODAY(),\"yyyy-mm-dd\")", "\"2026-01-02\"");
  EVAL("=TEXT(NOW(),\"hh:mm\")", "\"09:41\"");
  EVAL("=TODAY()", "46024");
  EVAL("=NOW()-TODAY()>0.4", "TRUE");
  EVAL("=SUMIF(A1:A3,\">15\")", "50");
  EVAL("=SUMIF(A1:A3,\">=20\",B1:B3)", "5");
  EVAL("=COUNTIF(A1:A5,\">10\")", "2");
  EVAL("=COUNTIF(A1:A5,\"te*\")", "1");
  EVAL("=COUNTIF(A1:A5,10)", "1");
  EVAL("=PMT(0.05/12,360,200000)", "-1073.64324602");
  EVAL("=PMT(0,10,1000)", "-100");
  EVAL("=IFERROR(1/0,\"none\")", "\"none\"");
  EVAL("=ISBLANK(C9)", "TRUE");
  EVAL("=ISNUMBER(A1)", "TRUE");
  EVAL("=ISTEXT(A4)", "TRUE");
  EVAL("=PI()", "3.14159265359");
  EVAL("=EXP(0)+LN(1)+LOG10(100)", "3");
  EVAL("=@SUM(A1..A3)", "60");

  /* VLOOKUP: a table of codes and prices */

  put(sh, "D1", "apple");
  put(sh, "E1", "1.5");
  put(sh, "D2", "banana");
  put(sh, "E2", "0.25");
  put(sh, "D3", "cherry");
  put(sh, "E3", "4");
  put(sh, "F1", "10");
  put(sh, "F2", "20");
  put(sh, "F3", "30");
  put(sh, "G1", "A");
  put(sh, "G2", "B");
  put(sh, "G3", "C");
  EVAL("=VLOOKUP(\"banana\",D1:E3,2,FALSE)", "0.25");
  EVAL("=VLOOKUP(\"CHERRY\",D1:E3,2,0)", "4");
  EVAL("=VLOOKUP(\"kiwi\",D1:E3,2,FALSE)", "#N/A");
  EVAL("=VLOOKUP(25,F1:G3,2)", "\"B\"");
  EVAL("=VLOOKUP(5,F1:G3,2)", "#N/A");
  EVAL("=VLOOKUP(99,F1:G3,2,TRUE)", "\"C\"");
  EVAL("=VLOOKUP(20,F1:G3,3,FALSE)", "#REF!");

  /* Errors: unknown names, error literals, references */

  EVAL("=FOO(1)", "#NAME?");
  EVAL("=foo+1", "#NAME?");
  EVAL("=#N/A", "#N/A");
  EVAL("=SUM(A1:A3)+#REF!", "#REF!");
  OK(sheet_set_input(sh, 50, 0, "=1+", true) == -EINVAL);
  OK(sheet_set_input(sh, 50, 0, "=SUM(1", true) == -EINVAL);
  OK(sheet_set_input(sh, 50, 0, "=ROUND(1)", true) == -EINVAL);
  OK(sheet_get(sh, 50, 0) == NULL);

  /* References and recalculation */

  sheet_clear_all(sh);
  put(sh, "A1", "5");
  put(sh, "A2", "7");
  put(sh, "A3", "=A1+A2");
  put(sh, "A4", "=A3*2");
  put(sh, "B1", "=$A$1*10");
  put(sh, "C1", "=SUM(A:A)");
  put(sh, "C2", "=SUM(1:1)");
  CELL("A3", "12");
  CELL("A4", "24");
  CELL("C1", "48");
  CELL("C2", "103");
  put(sh, "A1", "1");
  CELL("A3", "8");
  CELL("A4", "16");
  CELL("B1", "10");
  CELL("C1", "32");
  CELL("C2", "43");
  put(sh, "A2", "");
  CELL("A3", "1");
  put(sh, "A2", "hello");
  CELL("A3", "#VALUE!");
  put(sh, "A2", "2");
  CELL("A4", "6");

  /* Only what a change reaches is recalculated */

  sh->touched.n = 0;
  sh->alltouched = false;
  put(sh, "A2", "3");
  OK(!sh->alltouched && sh->touched.n == 5);   /* A2, A3, A4, C1, C2 */

  /* Circular references: found, and gone when the cycle is broken */

  put(sh, "D1", "=D2+1");
  put(sh, "D2", "=D1+1");
  put(sh, "D3", "=D1*2");
  put(sh, "D4", "=D4");
  CELL("D1", "#CIRC!");
  CELL("D2", "#CIRC!");
  CELL("D3", "#CIRC!");
  CELL("D4", "#CIRC!");
  put(sh, "D2", "5");
  CELL("D1", "6");
  CELL("D3", "12");

  /* A long chain: no recursion (each the one above plus one) */

  put(sh, "F1", "1");
  for (i = 1; i < 20000; i++)
    {
      snprintf(buf, sizeof(buf), "=F%d+1", i);
      sheet_set_input(sh, i, 5, buf, true);
    }

  sheet_recalc(sh);
  CELL("F20000", "20000");
  put(sh, "F1", "101");
  CELL("F20000", "20100");

  /* Formulas' text: as typed, spacing aside */

  sheet_clear_all(sh);
  put(sh, "A1", "= SUM( $A$2:B3 , 4 ) * (2 + -A2%) & \"x\"\"y\"");
  INPUT("A1", "=SUM($A$2:B3,4)*(2+-A2%)&\"x\"\"y\"");
  put(sh, "A2", "=IF(B1<>\"\",ROUND(B1,),0)");
  INPUT("A2", "=IF(B1<>\"\",ROUND(B1,),0)");
  put(sh, "A3", "=SUM(B:$D)+SUM($3:4)");
  INPUT("A3", "=SUM(B:$D)+SUM($3:4)");
  put(sh, "A4", "+B1+B2");
  INPUT("A4", "=+B1+B2");
  put(sh, "A5", "@SUM(B1..B3)");
  INPUT("A5", "=SUM(B1:B3)");

  /* What is typed: numbers, formats they bring, text, booleans */

  put(sh, "B1", "1,234.5");
  CELL("B1", "1234.5");
  SHOWN("B1", "1,234.5");
  put(sh, "B2", "$12");
  CELL("B2", "12");
  SHOWN("B2", "$12.00");
  put(sh, "B3", "15%");
  CELL("B3", "0.15");
  SHOWN("B3", "15%");
  INPUT("B3", "15%");
  put(sh, "B4", "'123");
  CELL("B4", "\"123\"");
  INPUT("B4", "'123");
  put(sh, "B5", "true");
  CELL("B5", "TRUE");
  put(sh, "B6", "2026-01-02");
  CELL("B6", "46024");
  SHOWN("B6", "2026-01-02");
  put(sh, "B7", "1,23");
  CELL("B7", "\"1,23\"");
  put(sh, "B8", "-0.5");
  sheet_set_fmt(sh, 7, 1, SF_MAKE(SF_CURRENCY, 2, 0));
  SHOWN("B8", "-$0.50");
  sheet_set_fmt(sh, 7, 1, SF_MAKE(SF_PERCENT, 1, 0));
  SHOWN("B8", "-50.0%");
  sheet_set_fmt(sh, 7, 1, SF_MAKE(SF_FIXED, 3, 0));
  SHOWN("B8", "-0.500");
  put(sh, "B9", "1234567.891");
  sheet_set_fmt(sh, 8, 1, SF_MAKE(SF_THOUSANDS, 2, 0));
  SHOWN("B9", "1,234,567.89");
  put(sh, "B10", "=NOW()");
  SHOWN("B10", "2026-01-02 09:41");

  /* Copies: relative references move, $ ones stay; off the sheet: #REF! */

  sheet_clear_all(sh);
  put(sh, "A1", "=B1+$C$1+B$1+$D1");
  sheet_set_formula(sh, 1, 1, sheet_formula_dup(at(sh, "A1")->f, 1, 1));
  INPUT("B2", "=C2+$C$1+C$1+$D2");
  sheet_set_formula(sh, 0, 4, sheet_formula_dup(at(sh, "B2")->f, -1, 3));
  INPUT("E1", "=F1+$C$1+F$1+$D1");
  sheet_set_formula(sh, 0, 0, sheet_formula_dup(at(sh, "E1")->f, 0, -6));
  INPUT("A1", "=#REF!+$C$1+#REF!+$D1");
  sheet_recalc(sh);
  CELL("A1", "#REF!");

  /* Rows and columns inserted and deleted: references follow */

  sheet_clear_all(sh);
  put(sh, "A1", "1");
  put(sh, "A2", "2");
  put(sh, "A3", "3");
  put(sh, "A4", "4");
  put(sh, "B1", "=SUM(A1:A4)");
  put(sh, "B2", "=A3*10");
  put(sh, "B3", "=$A$4+A1");
  put(sh, "C1", "=B2");
  sheet_insert(sh, true, 2, 2);                /* Before row 3 */
  INPUT("B1", "=SUM(A1:A6)");
  INPUT("B2", "=A5*10");
  INPUT("B5", "=$A$6+A1");
  INPUT("C1", "=B2");
  CELL("B1", "10");
  CELL("B2", "30");
  put(sh, "A3", "100");
  CELL("B1", "110");
  sheet_delete(sh, true, 2, 2, NULL, NULL);
  INPUT("B1", "=SUM(A1:A4)");
  INPUT("B3", "=$A$4+A1");
  CELL("B1", "10");
  sheet_delete(sh, true, 2, 1, NULL, NULL);     /* Row 3: A3 */
  INPUT("B2", "=#REF!*10");
  CELL("B2", "#REF!");
  INPUT("B1", "=SUM(A1:A3)");
  CELL("B1", "7");
  sheet_insert(sh, false, 0, 1);               /* A column before A */
  INPUT("C1", "=SUM(B1:B3)");
  INPUT("D1", "=C2");
  sheet_delete(sh, false, 1, 1, NULL, NULL);   /* Column B */
  INPUT("B1", "=SUM(#REF!)");
  CELL("B1", "#REF!");
  sheet_clear_all(sh);
  put(sh, "A1", "=SUM(C1:E1)");
  sheet_delete(sh, false, 3, 1, NULL, NULL);   /* Column D */
  INPUT("A1", "=SUM(C1:D1)");
  sheet_delete(sh, false, 2, 2, NULL, NULL);   /* C and D */
  INPUT("A1", "=SUM(#REF!)");

  /* Moves (cut and paste): the cells and what reads them */

  sheet_clear_all(sh);
  put(sh, "A1", "5");
  put(sh, "A2", "=A1*2");
  put(sh, "C1", "=A1+A2");
  r.r0 = 0;
  r.c0 = 0;
  r.r1 = 1;
  r.c1 = 0;
  sheet_move(sh, &r, 3, 1, NULL, NULL);        /* To B4:B5 */
  OK(at(sh, "A1") == NULL);
  INPUT("B5", "=B4*2");
  INPUT("C1", "=B4+B5");
  CELL("C1", "15");

  /* Sorting a range's rows by a column */

  sheet_clear_all(sh);
  put(sh, "A1", "pear");
  put(sh, "B1", "3");
  put(sh, "A2", "apple");
  put(sh, "B2", "1");
  put(sh, "A3", "fig");
  put(sh, "B3", "2");
  put(sh, "C1", "=B1*10");
  put(sh, "C2", "=B2*10");
  put(sh, "C3", "=B3*10");
  r.r0 = 0;
  r.c0 = 0;
  r.r1 = 2;
  r.c1 = 2;
  sheet_sort(sh, &r, 1, false);
  sheet_recalc(sh);
  CELL("A1", "\"apple\"");
  CELL("A3", "\"pear\"");
  INPUT("C1", "=B1*10");
  CELL("C3", "30");
  sheet_sort(sh, &r, 0, true);
  sheet_recalc(sh);
  CELL("A1", "\"pear\"");
  CELL("A2", "\"fig\"");

  /* Files: CSV (values) and the native format (everything) */

  if (dir != NULL)
    {
      sheet_clear_all(sh);
      put(sh, "A1", "Item");
      put(sh, "B1", "Price, each");
      put(sh, "A2", "Tea \"green\"");
      put(sh, "B2", "3.5");
      put(sh, "A3", "Cake");
      put(sh, "B3", "=B2*2");
      put(sh, "B4", "=SUM(B2:B3)");
      put(sh, "D4", "=1/0");
      sheet_set_fmt(sh, 1, 1, SF_MAKE(SF_CURRENCY, 2, SF_RIGHT) | SF_BOLD |
                    (3 << 12) | (5 << 16));
      sh->colw[0] = 120;
      snprintf(path, sizeof(path), "%s/selftest.csv", dir);
      OK(sheet_save_csv(sh, path) == 0);
      snprintf(path, sizeof(path), "%s/selftest.sheet", dir);
      OK(sheet_save_native(sh, path) == 0);

      sheet_clear_all(sh);
      snprintf(path, sizeof(path), "%s/selftest.csv", dir);
      OK(sheet_load_csv(sh, path) == 0);
      CELL("B1", "\"Price, each\"");
      CELL("A2", "\"Tea \"green\"\"");
      CELL("B3", "7");
      INPUT("B3", "7");
      CELL("B4", "10.5");
      CELL("D4", "\"#DIV/0!\"");

      sheet_clear_all(sh);
      snprintf(path, sizeof(path), "%s/selftest.sheet", dir);
      OK(sheet_load_native(sh, path) == 0);
      INPUT("B3", "=B2*2");
      INPUT("B4", "=SUM(B2:B3)");
      CELL("B4", "10.5");
      CELL("D4", "#DIV/0!");
      OK(sheet_fmt(sh, 1, 1) == (SF_MAKE(SF_CURRENCY, 2, SF_RIGHT) |
                                 SF_BOLD | (3 << 12) | (5 << 16)));
      OK(sh->colw[0] == 120);
      put(sh, "B2", "4");
      CELL("B4", "12");
    }

  sheet_free(sh);
  printf("sheet: selftest: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

static long used_bytes(void)
{
  struct mallinfo mi = mallinfo();

  return (long)mi.uordblks;
}

static uint32_t ms_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* A big sheet: rows of four numbers and a formula (their sum), a total */

static int bench(int rows, const char *dir)
{
  long before = used_bytes();
  struct sheet_s *sh = sheet_new();
  char buf[64];
  uint32_t t0;
  uint32_t t1;
  uint32_t t2;
  long after;
  int r;
  int c;

  if (sh == NULL)
    {
      return -ENOMEM;
    }

  t0 = ms_now();
  sh->batch = true;
  for (r = 0; r < rows; r++)
    {
      for (c = 0; c < 4; c++)
        {
          snprintf(buf, sizeof(buf), "%d", r * 4 + c);
          sheet_set_input(sh, r, c, buf, true);
        }

      snprintf(buf, sizeof(buf), "=SUM(A%d:D%d)", r + 1, r + 1);
      sheet_set_input(sh, r, 4, buf, true);
    }

  snprintf(buf, sizeof(buf), "=SUM(E1:E%d)", rows);
  sheet_set_input(sh, rows, 4, buf, true);
  sh->batch = false;
  t1 = ms_now();
  sheet_recalc_all(sh);
  t2 = ms_now();
  after = used_bytes();
  sheet_display(sh, sheet_get(sh, rows, 4), 15, buf, sizeof(buf));
  printf("sheet: %d cells (%d formulas): %ld bytes, %ld a cell; "
         "set in %lu ms, recalculated in %lu ms; total %s\n",
         rows * 5 + 1, rows + 1, after - before,
         (after - before) / (rows * 5 + 1), (unsigned long)(t1 - t0),
         (unsigned long)(t2 - t1), buf);

  sh->touched.n = 0;
  sh->alltouched = false;
  t0 = ms_now();
  sheet_set_input(sh, 0, 0, "1000000", true);
  sheet_recalc(sh);
  t1 = ms_now();
  sheet_display(sh, sheet_get(sh, rows, 4), 15, buf, sizeof(buf));
  printf("sheet: one cell changed: %d recalculated in %lu ms; total %s\n",
         sh->alltouched ? -1 : sh->touched.n, (unsigned long)(t1 - t0),
         buf);

  /* Its files: written and read again */

  if (dir != NULL)
    {
      char path[256];

      snprintf(path, sizeof(path), "%s/bench.sheet", dir);
      t0 = ms_now();
      sheet_save_native(sh, path);
      t1 = ms_now();
      sheet_load_native(sh, path);
      t2 = ms_now();
      printf("sheet: native file: saved in %lu ms, loaded (and "
             "recalculated) in %lu ms\n", (unsigned long)(t1 - t0),
             (unsigned long)(t2 - t1));
      snprintf(path, sizeof(path), "%s/bench.csv", dir);
      t0 = ms_now();
      sheet_save_csv(sh, path);
      t1 = ms_now();
      sheet_load_csv(sh, path);
      t2 = ms_now();
      printf("sheet: CSV: saved in %lu ms, loaded in %lu ms\n",
             (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
    }

  sheet_free(sh);
  return 0;
}

static int show(const char *path)
{
  struct sheet_s *sh = sheet_new();
  struct sh_rect_s r;
  size_t n;
  char *s;
  int ret;

  if (sh == NULL)
    {
      return -ENOMEM;
    }

  ret = sheet_is_native(path) ? sheet_load_native(sh, path) :
                                sheet_load_csv(sh, path);
  if (ret < 0)
    {
      fprintf(stderr, "sheet: %s: %s\n", path, strerror(-ret));
      sheet_free(sh);
      return ret;
    }

  sheet_extent(sh, &r.r1, &r.c1);
  r.r0 = 0;
  r.c0 = 0;
  if (r.r1 >= 0)
    {
      s = sheet_tsv(sh, &r, &n);
      if (s != NULL)
        {
          fwrite(s, 1, n, stdout);
          free(s);
        }
    }

  sheet_free(sh);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
    {
      return selftest(argc >= 3 ? argv[2] : NULL);
    }

  if (argc >= 2 && strcmp(argv[1], "--bench") == 0)
    {
      return bench(argc >= 3 ? atoi(argv[2]) : 10000,
                   argc >= 4 ? argv[3] : NULL) < 0;
    }

  if (argc == 2 && argv[1][0] != '-')
    {
      return show(argv[1]) < 0;
    }

  fprintf(stderr, "usage: sheet --selftest [DIR] | --bench [ROWS [DIR]] | "
                  "FILE\n");
  return 2;
}
