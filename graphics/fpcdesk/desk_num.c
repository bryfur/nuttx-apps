/****************************************************************************
 * apps/graphics/fpcdesk/desk_num.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Numbers to and from text, by hand: the C library may be built without
 * floating point in printf() and strtod() (CONFIG_LIBC_FLOATINGPOINT).
 * Shared by the Calculator and the Spreadsheet (desk_num.h).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "desk_num.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Group the integer part of a plain number in thousands */

void num_group(const char *s, char *out, size_t size)
{
  const char *p = s;
  const char *end;
  size_t n = 0;
  int digits;

  if (*p == '-')
    {
      p++;
    }

  for (end = p; *end >= '0' && *end <= '9'; end++)
    {
    }

  if (strchr(s, 'e') != NULL)
    {
      strlcpy(out, s, size);
      return;
    }

  if (*s == '-' && n + 1 < size)
    {
      out[n++] = '-';
    }

  for (digits = end - p; p < end && n + 2 < size; digits--)
    {
      out[n++] = *p++;
      if (digits > 1 && (digits - 1) % 3 == 0)
        {
          out[n++] = ',';
        }
    }

  out[n] = '\0';
  strlcat(out, end, size);
}

/* Numbers to and from text, by hand: the C library may be built without
 * floating point in printf() and strtod() (CONFIG_LIBC_FLOATINGPOINT)
 */

double num_p10(int n)
{
  double p = 1.0;

  while (n-- > 0)
    {
      p *= 10.0;                         /* Exact up to 10^22 */
    }

  return p;
}

/* v with at most prec (<= 15) significant digits, rounded: no trailing
 * zeros, no "-0"; positional from 1e-5 to 10^prec, else with an exponent
 * (1.5e20, 2e-7)
 */

void num_plain(double v, int prec, char *out, size_t size)
{
  char dig[20];
  uint64_t n;
  uint64_t lim;
  double a = v < 0 ? -v : v;
  double m;
  int e = 0;
  int nd;
  int k;
  int i;
  size_t o = 0;

  if (a == 0.0 || size < 32)
    {
      strlcpy(out, "0", size);
      return;
    }

  prec = prec < 1 ? 1 : prec > 15 ? 15 : prec;

  /* The decimal exponent: 10^e <= a < 10^(e + 1) */

  while (a >= num_p10(e + 1) && e < 308)
    {
      e++;
    }

  while (e > -330 && a < (e >= 0 ? num_p10(e) : 1.0 / num_p10(-e)))
    {
      e--;
    }

  /* prec digits as an integer */

  k = prec - 1 - e;
  m = k >= 0 ? (k > 300 ? a * num_p10(k - 300) * num_p10(300) :
                          a * num_p10(k)) : a / num_p10(-k);
  lim = 1;
  for (i = 0; i < prec; i++)
    {
      lim *= 10;
    }

  n = (uint64_t)(m + 0.5);
  if (n >= lim)
    {
      n = (n + 5) / 10;
      e++;
    }

  for (i = prec - 1; i >= 0; i--)
    {
      dig[i] = (char)('0' + n % 10);
      n /= 10;
    }

  nd = prec;
  while (nd > 1 && dig[nd - 1] == '0')
    {
      nd--;
    }

  if (v < 0)
    {
      out[o++] = '-';
    }

  if (e >= -5 && e < prec)
    {
      if (e < 0)
        {
          out[o++] = '0';
          out[o++] = '.';
          for (i = -1; i > e; i--)
            {
              out[o++] = '0';
            }

          for (i = 0; i < nd; i++)
            {
              out[o++] = dig[i];
            }
        }
      else
        {
          for (i = 0; i <= e || i < nd; i++)
            {
              if (i == e + 1)
                {
                  out[o++] = '.';
                }

              out[o++] = i < nd ? dig[i] : '0';
            }
        }

      out[o] = '\0';
    }
  else
    {
      out[o++] = dig[0];
      if (nd > 1)
        {
          out[o++] = '.';
          for (i = 1; i < nd; i++)
            {
              out[o++] = dig[i];
            }
        }

      out[o] = '\0';
      snprintf(out + o, size - o, "e%d", e);
    }
}

void num_format(double v, int prec, char *out, size_t size)
{
  char buf[40];

  num_plain(v, prec, buf, sizeof(buf));
  num_group(buf, out, size);
}

/* [-]digits[.digits][e[-]digits]; returns the characters used, 0 if no
 * number
 */

int num_parse(const char *s, double *v)
{
  const char *p = s;
  double m = 0.0;
  int scale = 0;
  int digits = 0;
  bool neg = false;
  bool point = false;
  int ex = 0;

  if (*p == '-' || *p == '+')
    {
      neg = *p++ == '-';
    }

  for (; (*p >= '0' && *p <= '9') || (*p == '.' && !point); p++)
    {
      if (*p == '.')
        {
          point = true;
          continue;
        }

      if (digits < 17)
        {
          m = m * 10.0 + (*p - '0');
          scale -= point;
        }
      else
        {
          scale += !point;
        }

      digits++;
    }

  if (digits == 0)
    {
      return 0;
    }

  if (*p == 'e' || *p == 'E')
    {
      const char *q = p + 1;
      bool eneg = false;
      int n = 0;

      if (*q == '-' || *q == '+')
        {
          eneg = *q++ == '-';
        }

      if (*q >= '0' && *q <= '9')
        {
          while (*q >= '0' && *q <= '9')
            {
              n = n < 1000 ? n * 10 + (*q - '0') : n;
              q++;
            }

          ex = eneg ? -n : n;
          p  = q;
        }
    }

  scale += ex;
  if (scale >= 0)
    {
      m *= num_p10(scale > 400 ? 400 : scale);
    }
  else
    {
      m /= num_p10(-scale > 400 ? 400 : -scale);
    }

  *v = neg ? -m : m;
  return p - s;
}

/* v rounded to dec (0 to 12) decimals, all of them shown; grouped in
 * thousands if group.  Beyond what 64 bits hold: num_plain()
 */

void num_fixed(double v, int dec, bool group, char *out, size_t size)
{
  char buf[48];
  char dig[24];
  double a = v < 0 ? -v : v;
  double m;
  uint64_t n;
  int nd = 0;
  int o = 0;
  int i;

  dec = dec < 0 ? 0 : dec > 12 ? 12 : dec;
  m = a * num_p10(dec);
  if (!(m < 1.8e19))
    {
      num_plain(v, 15, buf, sizeof(buf));
      if (group)
        {
          num_group(buf, out, size);
        }
      else
        {
          strlcpy(out, buf, size);
        }

      return;
    }

  /* Half away from zero, forgiving the last bit (2.675 is 2.68) */

  n = (uint64_t)(m + 0.5 + m * 4e-16);
  do
    {
      dig[nd++] = (char)('0' + n % 10);
      n /= 10;
    }
  while (n > 0);

  while (nd <= dec)
    {
      dig[nd++] = '0';
    }

  if (v < 0)
    {
      for (i = 0; i < nd; i++)
        {
          if (dig[i] != '0')
            {
              buf[o++] = '-';
              break;
            }
        }
    }

  for (i = nd - 1; i >= 0; i--)
    {
      buf[o++] = dig[i];
      if (i == dec && dec > 0)
        {
          buf[o++] = '.';
        }
    }

  buf[o] = '\0';
  if (group)
    {
      num_group(buf, out, size);
    }
  else
    {
      strlcpy(out, buf, size);
    }
}

char *num_int(char *p, long v, int width)
{
  char dig[24];
  unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
  int n = 0;

  do
    {
      dig[n++] = (char)('0' + u % 10);
      u /= 10;
    }
  while (u > 0 && n < (int)sizeof(dig));

  while (n < width && n < (int)sizeof(dig))
    {
      dig[n++] = '0';
    }

  if (v < 0)
    {
      *p++ = '-';
    }

  while (n > 0)
    {
      *p++ = dig[--n];
    }

  *p = '\0';
  return p;
}
