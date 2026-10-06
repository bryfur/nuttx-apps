/****************************************************************************
 * apps/interpreters/micropython/agent_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * agent: the AI agent (docs/agent.md) as a command.  MicroPython runs the
 * agent package (agent/, frozen into the firmware: mkfrozen.py) with a
 * heap of CONFIG_INTERPRETERS_MICROPYTHON_AGENT_HEAPSIZE KB (AGENT_HEAP=N
 * in the environment: N KB).  AGENT_PATH=DIR in the environment puts DIR
 * first in sys.path, so a copy of the package there (on the microSD, say)
 * is run instead of the frozen one: the Python half can be changed without
 * a new firmware.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAXARGS         32

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

int MPY_PYTHON_MAIN(int argc, char *argv[]);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char g_boot[] =
  "import sys, os\n"
  "p = os.getenv('AGENT_PATH')\n"
  "if p:\n"
  "    sys.path.insert(0, p)\n"
  "del p\n"
  "import agent.main\n"
  "agent.main.run()\n";

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  char *args[MAXARGS + 6];
  char heap[32];
  const char *env = getenv("AGENT_HEAP");
  long kb = env != NULL ? strtol(env, NULL, 0) :
            CONFIG_INTERPRETERS_MICROPYTHON_AGENT_HEAPSIZE;
  int n = 0;
  int i;

  if (kb < 256)
    {
      kb = 256;
    }

  snprintf(heap, sizeof(heap), "heapsize=%ldK", kb);
  args[n++] = "agent";
  args[n++] = "-X";
  args[n++] = heap;
  args[n++] = "-c";
  args[n++] = (char *)g_boot;
  for (i = 1; i < argc && i <= MAXARGS; i++)
    {
      args[n++] = argv[i];
    }

  args[n] = NULL;
  return MPY_PYTHON_MAIN(n, args);
}
