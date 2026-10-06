/****************************************************************************
 * apps/graphics/fpcwasm/sdk/examples/hello/hello.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A WASI command line app for the FPC (docs/wasm.md): hello, its
 * arguments, and with file names a small tool, wc: lines, words and bytes
 * of each (the files WASI lets it see: the directory it was started in,
 * "." to it).  Standard C (wasi-libc); one fpc function, the API version.
 *
 *   nsh> wasm run /mnt/sd0/apps/hello/app.wasm
 *   nsh> wasm run hello readme.txt
 ****************************************************************************/

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "fpc_wasm.h"

static int count(const char *name)
{
  FILE *f = fopen(name, "r");
  long lines = 0;
  long words = 0;
  long bytes = 0;
  int inword = 0;
  int c;

  if (f == NULL)
    {
      perror(name);
      return 1;
    }

  while ((c = fgetc(f)) != EOF)
    {
      bytes++;
      lines += c == '\n';
      if (isspace(c))
        {
          inword = 0;
        }
      else if (!inword)
        {
          inword = 1;
          words++;
        }
    }

  fclose(f);
  printf("%7ld %7ld %7ld %s\n", lines, words, bytes, name);
  return 0;
}

int main(int argc, char **argv)
{
  int status = 0;
  int i;

  if (argc < 2)
    {
      printf("Hello from WebAssembly! (fpc API v%d)\n",
             (int)fpc_api_version());
      return 0;
    }

  for (i = 1; i < argc; i++)
    {
      status |= count(argv[i]);
    }

  return status;
}
