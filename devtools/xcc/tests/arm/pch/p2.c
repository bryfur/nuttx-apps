#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Only through the headers' declarations: types used through others */
static int cmp(const void *a, const void *b) { return *(const int*)a - *(const int*)b; }

int main(int argc, char *argv[]) {
  int v[] = {5, 3, 9, 1};
  qsort(v, 4, sizeof(int), cmp);
  FILE *fp = stdout;
  fprintf(fp, "%d %d %d %d %c\n", v[0], v[1], v[2], v[3], toupper('q'));
  char *s = strdup("pch");
  size_t n = strlen(s);
  printf("%s %u %d\n", s, (unsigned)n, abs(-7));
  free(s);
  return argc > 5 ? EXIT_FAILURE : EXIT_SUCCESS;
}
