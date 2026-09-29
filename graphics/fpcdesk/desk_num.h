/****************************************************************************
 * apps/graphics/fpcdesk/desk_num.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Numbers to and from text, by hand (desk_num.c): the Calculator's and
 * the Spreadsheet's.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_NUM_H
#define __APPS_GRAPHICS_FPCDESK_DESK_NUM_H

#include <stdbool.h>
#include <stddef.h>

/* 10^n, n >= 0 (exact up to 10^22) */

double num_p10(int n);

/* v with at most prec (1 to 15) significant digits, rounded: no trailing
 * zeros, no "-0"; positional from 1e-5 to 10^prec, else with an exponent
 * (1.5e20, 2e-7).  out: at least 32 bytes.
 */

void num_plain(double v, int prec, char *out, size_t size);

/* A plain number's integer part grouped in thousands (1,234.5) */

void num_group(const char *s, char *out, size_t size);

/* num_plain(), grouped */

void num_format(double v, int prec, char *out, size_t size);

/* v with dec decimals, all shown (1.50), grouped if group */

void num_fixed(double v, int dec, bool group, char *out, size_t size);

/* An integer at p, at least width digits (zeros in front): where it ends
 * (its NUL).  No printf(), little stack.
 */

char *num_int(char *p, long v, int width);

/* [-+]digits[.digits][e[-+]digits] at s: the characters used, 0 if no
 * number is there
 */

int num_parse(const char *s, double *v);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_NUM_H */
