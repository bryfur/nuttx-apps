/*
 * The prefix: comments, defines, conditionals and includes,
 * a continued line
 */
#define _GNU_SOURCE \
  1
#include <stdio.h>
#include <stdlib.h>   /* a comment after */
#include <string.h>  // and another
#ifdef __XCC
#include <stdarg.h>
#else
#error "not xcc"
#endif
#include "local.h"
#include "once.h"
#undef NDEBUG
#define STR(x) #x
#define CAT(a, b) a ## b
#define LOG(fmt, ...) printf("[%s] " fmt "\n", STR(log), ##__VA_ARGS__)
/* the rest */ int prefix_ended = 1;

#include <stdio.h>
#include "once.h"
#include "local.h"

static int twice(int x) { return 2 * x; }
static const entry_t entries[] = { {"twice", twice} };

static int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += va_arg(ap, int);
  va_end(ap);
  return s;
}

int main(void) {
  struct point p = {3, -4};
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", manhattan(p));
  LOG("manhattan %s, local %d, line %d", buf, LOCAL_VALUE, __LINE__);
  LOG("no args");
  int CAT(va, lue) = entries[0].fn(21);
  printf("%s %d %d %zu %d\n", entries[0].name, value, sum(3, 1, 2, 3), strlen(STR(abc)), prefix_ended);
  long l = labs(-17L);
  printf("%ld %d %d\n", l / 5, atoi("17") % 5, EOF);
  return 0;
}
