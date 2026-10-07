/* leakmain.c: the board's xcc or wcc (xcc_entry.c or wcc_main.c, their
 * main renamed board_main) started as the board starts a module, built
 * with LeakSanitizer (leak.sh).  On the board a module's memory outlives
 * it (a flat NuttX build), so when board_main returns nothing it took may
 * still be allocated: every byte more than before is a leak, listed.
 */

#include <stdio.h>
#include <stdlib.h>

/* The sanitizers' interface (not every gcc installs its headers) */

size_t __sanitizer_get_current_allocated_bytes(void);
int __sanitizer_install_malloc_and_free_hooks(
    void (*malloc_hook)(const volatile void *, size_t),
    void (*free_hook)(const volatile void *));
int __lsan_do_recoverable_leak_check(void);

int board_main(int argc, char *argv[]);

static volatile long g_blocks;

static void on_malloc(const volatile void *p, size_t size)
{
  (void)p;
  (void)size;
  ++g_blocks;
}

static void on_free(const volatile void *p)
{
  (void)p;
  --g_blocks;
}

int main(int argc, char *argv[])
{
  /* The standard streams' buffers are the program's own here, as the
   * task's are on the board (not the compile's)
   */

  static char obuf[BUFSIZ], ebuf[BUFSIZ];
  setvbuf(stdout, obuf, _IOFBF, sizeof(obuf));
  setvbuf(stderr, ebuf, _IOLBF, sizeof(ebuf));
  __sanitizer_install_malloc_and_free_hooks(on_malloc, on_free);

  size_t bytes = __sanitizer_get_current_allocated_bytes();
  long blocks = g_blocks;
  int rc = board_main(argc, argv);
  long nb = g_blocks - blocks;
  size_t nbytes = __sanitizer_get_current_allocated_bytes() - bytes;

  fflush(stdout);
  if (nb != 0 || nbytes != 0)
    {
      fprintf(stderr, "LEAK: %ld blocks, %zu bytes still allocated after", nb, nbytes);
      for (int i = 0; i < argc; ++i)
        fprintf(stderr, " %s", argv[i]);
      fprintf(stderr, "\n");
      fflush(stderr);
      __lsan_do_recoverable_leak_check();
      return 99;
    }

  return rc;
}
