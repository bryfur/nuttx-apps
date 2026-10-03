/****************************************************************************
 * apps/graphics/fpcdesk/desk_monitor.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System Monitor: the load of each CPU with a graph of the last minute,
 * each heap, the uptime, the board, a disk, and a table of all threads
 * (state, priority, CPU, load, stack and heap use) that sorts by any
 * column, scrolls, and can end a task.
 *
 * It samples procfs as ps and free do:
 *
 *   /proc/<pid>/status    Name, Type, Group, CPU, State, Priority
 *   /proc/<pid>/loadavg   the thread's share of all CPUs' time
 *   /proc/<pid>/stack     StackSize, StackUsed (CONFIG_STACK_COLORATION)
 *   /proc/<pid>/heap      AllocSize (CONFIG_MM_BACKTRACE >= 0)
 *   /proc/meminfo         one line per heap
 *   /proc/cpuload         all CPUs' load, if no IDLE thread is found
 *   /proc/gpu             the GPU's memory, link, frames and (a v6 GPU)
 *                         how busy it and its SDRAM are
 *
 * and statfs() of a disk (CONFIG_FPCDESK_SYSMON_DISK).  NuttX counts load
 * against the ticks of all CPUs together, so a thread that keeps one of
 * two CPUs busy has 50%.  Each CPU has an IDLE thread (PID = CPU): a CPU's
 * own load is 100% less its IDLE thread's share times the number of CPUs.
 *
 * Everything is drawn by the GPU into the window's surface (nx_gpuops()),
 * and after the first time only what changed: each text and bar remembers
 * a hash of what it shows and is redrawn (cleared and drawn again) only
 * when that changes; a graph moves left with one COPY within the surface
 * and draws the newest sample's columns; the table redraws the cells whose
 * contents changed and scrolls with a COPY.  Everything is drawn in full
 * only when the window opens, is resized or is exposed.
 *
 * A program of its own (/mnt/sd0/bin/monitor, apps/graphics/deskapps):
 * built into the desktop (DESK_PROGRAM_STUB) this file is only the icon
 * and the name the dock shows (desk_program.c).
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/statfs.h>
#include <sys/utsname.h>

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_FPCDESK_SYSMON_DISK
#  define CONFIG_FPCDESK_SYSMON_DISK      ""
#endif

#ifndef CONFIG_FPCDESK_SYSMON_DISKNAME
#  define CONFIG_FPCDESK_SYSMON_DISKNAME  "Disk"
#endif

#ifdef CONFIG_ARCH_BOARD_CUSTOM
#  define MON_BOARD      CONFIG_ARCH_BOARD_CUSTOM_NAME
#else
#  define MON_BOARD      CONFIG_ARCH_BOARD
#endif

#if defined(CONFIG_MM_BACKTRACE) && CONFIG_MM_BACKTRACE >= 0
#  define MON_HEAPFILES  1              /* /proc/<pid>/heap may exist */
#else
#  define MON_HEAPFILES  0
#endif

/* 1: log the cost of every update (sampling and drawing times, operations,
 * bytes on the GPU link, nx_gpuops() calls)
 */

#define MON_STATS        0

#define MON_MAXPROCS     64             /* Threads shown */
#define MON_MAXCPUS      4              /* CPUs shown */
#define MON_MAXHEAPS     4              /* Heaps shown */
#define MON_NAMELEN      24             /* procfs shows at most 18 */
#define MON_STATELEN     24
#define MON_HISTORY      128            /* Samples kept for the graphs */
#define MON_GRAPHPTS     60             /* Samples across a graph, about */

/* Layout */

#define PAD              12             /* Around and between the cards */
#define CARD_H           170
#define CARD_R           12             /* LUT_R12 */
#define INSET            14             /* Card edge to its contents */
#define FOOTER_H         24
#define CHIP_H           22
#define ROW_H            20
#define HEADER_H         22
#define COL_GAP          14
#define BAR_H            6              /* Bars: LUT_R3 pills */
#define HEAP_ROW         30
#define GPU_H            94             /* The GPU card, across the window */
#define GPU_BAR_H        10
#define GPU_BUFSIZE      2048           /* /proc/gpu */
#define NOLIMIT          10000          /* Text of any width */

/* Colours: the desktop's, and a few of its own */

#define C_GRAPH          THEMED(RGB(0x24, 0x24, 0x28), RGB(0xf4, 0xf4, 0xf7))
#define C_GRID           THEMED(RGB(0x36, 0x36, 0x3b), RGB(0xe0, 0xe0, 0xe5))
#define C_TRACK          THEMED(RGB(0x46, 0x46, 0x4c), RGB(0xdd, 0xdd, 0xe2))
#define C_ROWALT         THEMED(RGB(0x34, 0x34, 0x39), RGB(0xf5, 0xf5, 0xf8))
#define C_HEADER         THEMED(RGB(0x3c, 0x3c, 0x42), RGB(0xec, 0xec, 0xf0))
#define C_SELECT         (g_theme.selection)
#define C_SELDIM         THEMED(RGB(0xb8, 0xcc, 0xe6), g_theme.text_dim)
#define C_CHIP           THEMED(RGB(0x44, 0x44, 0x4a), RGB(0xe6, 0xe6, 0xea))
#define C_CHIPON         colour_mix(C_CARD, C_ACCENT, THEMED(90, 50))
#define C_FAINT          (g_theme.text_faint)
#define C_GREEN          (g_theme.success)
#define C_AMBER          (g_theme.warning)
#define C_RED            THEMED(RGB(0xff, 0x45, 0x3a), RGB(0xe0, 0x35, 0x2b))
#define C_VIOLET         THEMED(RGB(0xbf, 0x5a, 0xf2), RGB(0x9b, 0x4a, 0xd0))
#define C_DANGER         THEMED(RGB(0xc4, 0x2b, 0x22), RGB(0xe0, 0x3c, 0x34))

/* The table's columns */

enum
{
  COL_PID = 0,
  COL_NAME,
  COL_STATE,
  COL_PRIO,
  COL_CORE,
  COL_LOAD,
  COL_STACK,
  COL_HEAP,
  NCOLS
};

/* Kinds of thread, from the "Type:" line of /proc/<pid>/status */

enum
{
  T_TASK = 0,
  T_PTHREAD,
  T_KTHREAD
};

/* What is drawn that changes: each remembers what it shows */

enum
{
  F_CPU_NOTE = 0,
  F_CPU_BIG,
  F_CPU_VAL,                            /* One per CPU */
  F_MEM_NOTE = F_CPU_VAL + MON_MAXCPUS,
  F_MEM_BIG,
  F_MEM_USED,
  F_MEM_TOTAL,
  F_HEAP_NAME,                          /* One per heap */
  F_HEAP_FREE = F_HEAP_NAME + MON_MAXHEAPS,
  F_HEAP_BAR = F_HEAP_FREE + MON_MAXHEAPS,
  F_UPTIME = F_HEAP_BAR + MON_MAXHEAPS,
  F_OS,
  F_BOARD,
  F_THREADS,
  F_DISK_TEXT,
  F_DISK_BAR,
  F_COUNTS,
  F_CHIP_KILL,
  F_CHIP_IVAL,
  F_CHIP_KERN,
  F_HEADER,
  F_THUMB,
  F_FOOTER,
  F_GPU_NOTE,
  F_GPU_MEMTEXT,
  F_GPU_MEMBAR,
  F_GPU_MEMKEY,
  F_GPU_LINK,
  F_GPU_LINKNOTE,
  F_GPU_WAIT,
  F_GPU_BUSY,
  F_GPU_SDBAR,
  F_GPU_SDKEY,
  NFIELDS
};

/* Row cache styles */

#define ROW_INVALID      0xff
#define ROW_EMPTY        0xfe
#define ROW_ODD          0x01
#define ROW_SELECTED     0x02

#ifndef DESK_PROGRAM_STUB

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct mon_proc_s
{
  pid_t    pid;
  pid_t    group;                       /* Process (task group) */
  char     name[MON_NAMELEN];
  char     state[MON_STATELEN];         /* "Running", "Waiting,Semaphore" */
  uint8_t  type;                        /* T_* */
  uint8_t  prio;
  int8_t   cpu;                         /* CPU it runs on, or -1 */
  bool     idle;                        /* A CPU's IDLE thread */
  uint16_t load;                        /* Share of all CPUs, 0.1 % */
  uint32_t stacksize;
  uint32_t stackused;                   /* 0 if unknown */
  int32_t  heap;                        /* Bytes it allocated, -1 unknown */
};

struct mon_heap_s
{
  char     name[12];
  uint32_t total;
  uint32_t used;
  uint32_t maxfree;                     /* Largest free block */
};

/* The GPU (/proc/gpu): its memory in pixels by what holds it, the link,
 * frames, and with a v6 GPU how busy it and its SDRAM are (0.1 %)
 */

#define MON_GPU_MEMS     5              /* Buffers, windows, images, other,
                                         * free */
#define MON_GPU_SDRAM    4              /* Scanout, blitter, DMA, CPU */

struct mon_gpu_s
{
  bool     have;                        /* /proc/gpu was read */
  bool     hw;                          /* The GPU's own counters, clocked */
  uint8_t  version;
  uint32_t space;
  uint32_t mem[MON_GPU_MEMS];
  uint16_t nparked;                     /* Parked, to be drawn again */
  uint16_t nmem;                        /* Windows in memory */
  uint16_t largew;
  uint16_t largeh;
  uint16_t fps;                         /* 0.1 /s */
  uint32_t link;                        /* Bytes/s */
  uint16_t wire;                        /* Of the wire, 0.01 % */
  uint16_t wait;                        /* READY wait, 0.1 % */
  uint16_t busy;                        /* The blitter, 0.1 % */
  uint16_t sdram[MON_GPU_SDRAM];        /* 0.1 % each */
};

struct mon_snap_s
{
  uint32_t uptime;                      /* Seconds */
  uint8_t  ncpus;
  uint16_t cpuload[MON_MAXCPUS];        /* Busy, 0.1 % of that CPU */
  uint16_t totalload;                   /* Busy, 0.1 % of all CPUs */
  uint8_t  nheaps;
  struct mon_heap_s heaps[MON_MAXHEAPS];
  uint32_t memtotal;
  uint32_t memused;
  bool     havedisk;
  uint64_t disktotal;
  uint64_t diskused;
  uint16_t ntasks;
  uint16_t npthreads;
  uint16_t nkthreads;
  uint16_t nrunning;
  bool     haveheap;                    /* /proc/<pid>/heap exists */
  struct mon_gpu_s gpu;
  int      nprocs;
  struct mon_proc_s procs[MON_MAXPROCS];
};

struct mon_rect_s
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

/* A graph: its round background, and the plot inside it (inset from the
 * round corners, so the plot can move within the surface).  The newest
 * sample is at px1, each older one step pixels to the left.
 */

struct mon_graph_s
{
  struct mon_rect_s r;
  int16_t  px0;
  int16_t  px1;
  int16_t  py0;
  int16_t  py1;
  int16_t  step;
  int16_t  npts;
  uint32_t drawn;                       /* Samples when last drawn, 0 */
};

/* Something drawn that changes: what it shows (a hash, 0 = nothing
 * drawn) and where (to clear it)
 */

struct mon_field_s
{
  uint32_t hash;
  struct mon_rect_s r;
};

struct mon_row_s
{
  pid_t    pid;
  uint8_t  style;                       /* ROW_* */
  uint32_t hash[NCOLS];
};

/* One cell of the table, as it is drawn */

struct mon_cell_s
{
  char     text[24];
  uint16_t fg;
  uint16_t mark;                        /* Badge, dot or bar colour */
  uint8_t  kind;                        /* CELL_* */
  uint8_t  badge;                       /* T_* */
  int16_t  bar;                         /* Bar fill, pixels (-1 none) */
};

enum
{
  CELL_TEXT = 0,
  CELL_NAME,
  CELL_STATE,
  CELL_BAR
};

struct mon_s
{
  /* Sampling */

  struct mon_snap_s snap;
  int8_t   haveheap;                    /* -1 not yet known */
  char     buf[512];                    /* A procfs file */
  char     osname[48];
  pid_t    self;
  uint32_t nsamples;
  uint16_t hcpu[MON_MAXCPUS][MON_HISTORY];
  uint16_t hmem[MON_HISTORY];
  uint8_t  ticks;                       /* Seconds since the last sample */

  /* The view */

  uint8_t  sortcol;
  bool     sortdown;
  bool     showkernel;
  bool     confirm;                     /* "Confirm end task" shown */
  uint8_t  ival;                        /* Index in g_intervals */
  pid_t    selected;                    /* -1: none */
  int      scroll;                      /* First row shown */
  int      nrows;
  uint8_t  order[MON_MAXPROCS];         /* Rows: indexes into procs */
  bool     dragging;                    /* The scrollbar's thumb */
  int      dragoff;
  uint8_t  buttons;
  uint8_t  esc;                         /* VT100 sequence state */
  uint8_t  escparam;

  /* The layout, in frame coordinates (the client area starts at
   * TITLE_H), for a client area of lw x lh
   */

  int16_t  lw;
  int16_t  lh;
  bool     laidheap;                    /* Laid out with the Heap column */
  struct mon_rect_s cpucard;            /* w = 0: not shown */
  struct mon_rect_s memcard;
  struct mon_rect_s syscard;
  struct mon_rect_s gpucard;            /* Across the window, below */
  struct mon_rect_s proccard;
  struct mon_rect_s footer;
  struct mon_rect_s chipkill;
  struct mon_rect_s chipival;
  struct mon_rect_s chipkern;
  struct mon_rect_s header;
  struct mon_rect_s rows;               /* Without the scrollbar */
  struct mon_rect_s sbar;
  int16_t  visrows;
  bool     colshown[NCOLS];
  int16_t  colx[NCOLS];                 /* Text: colx .. colx + colw */
  int16_t  colw[NCOLS];
  int16_t  colx0[NCOLS];                /* The cell: colx0 .. colx1 */
  int16_t  colx1[NCOLS];
  int16_t  cpux;                        /* The CPUs' list, right of the % */
  struct mon_graph_s gcpu;
  struct mon_graph_s gmem;

  /* What is drawn */

  bool     force;                       /* Drawing everything */
  int16_t  drawnw;                      /* Size drawn in full */
  int16_t  drawnh;
  int      drawnscroll;
  struct mon_field_s f[NFIELDS];
  struct mon_row_s rowc[MON_MAXPROCS];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;
static NXWINDOW g_hwnd;

#if MON_STATS
static struct
{
  int ops;
  int bytes;
  int calls;
} g_stats;
#endif

/* A CPU's colour in the graphs (the first is the accent's) */

static uint16_t cpu_colour(int i)
{
  switch (i)
    {
      case 0:
        return C_ACCENT;

      case 1:
        return C_ACCENT == C_AMBER ? C_VIOLET : C_AMBER;

      case 2:
        return C_GREEN;

      default:
        return RGB(0xff, 0x37, 0x5f);
    }
}

static const uint8_t g_intervals[] =
{
  1, 2, 5
};

#define NINTERVALS (sizeof(g_intervals) / sizeof(g_intervals[0]))

/* The columns: header, text width (0: what is left), right-aligned, the
 * order they go in when the window is narrow (0: never), and whether the
 * first click sorts largest first
 */

static const struct
{
  const char *label;
  int16_t w;
  bool    right;
  uint8_t drop;
  bool    largest;
} g_cols[NCOLS] =
{
  { "PID",    34, true,  6, false },
  { "Name",    0, false, 0, false },
  { "State", 128, false, 4, false },
  { "Pri",    28, true,  2, true  },
  { "Core",   32, true,  1, false },
  { "CPU",    84, true,  0, true  },
  { "Stack", 108, true,  3, true  },
  { "Heap",   62, true,  5, true  },
};

/****************************************************************************
 * Private Functions: sampling
 ****************************************************************************/

/* The value of a "Key:   value" line, or NULL */

static const char *mon_field(const char *buf, const char *key)
{
  size_t keylen = strlen(key);
  const char *line = buf;

  while (*line != '\0')
    {
      const char *next;

      if (strncmp(line, key, keylen) == 0)
        {
          const char *value = line + keylen;

          while (*value == ' ' || *value == '\t')
            {
              value++;
            }

          return value;
        }

      next = strchr(line, '\n');
      if (next == NULL)
        {
          break;
        }

      line = next + 1;
    }

  return NULL;
}

/* The rest of a line, without trailing blanks */

static void mon_copyline(char *dest, const char *src, size_t size)
{
  size_t len = 0;

  while (src[len] != '\0' && src[len] != '\n' && len < size - 1)
    {
      dest[len] = src[len];
      len++;
    }

  while (len > 0 && (dest[len - 1] == ' ' || dest[len - 1] == '\r'))
    {
      len--;
    }

  dest[len] = '\0';
}

/* "12.5%" as 125 */

static uint16_t mon_percent(const char *str)
{
  unsigned long tenths;
  char *end;

  while (*str == ' ')
    {
      str++;
    }

  tenths = strtoul(str, &end, 10) * 10;
  if (*end == '.' && isdigit((unsigned char)end[1]))
    {
      tenths += end[1] - '0';
    }

  return tenths > 1000 ? 1000 : (uint16_t)tenths;
}

/* A procfs file into m->buf (pid < 0: a file of procfs itself); returns
 * its length, or -1
 */

static ssize_t mon_read(struct mon_s *m, pid_t pid, const char *file)
{
  char path[40];
  ssize_t total = 0;
  int fd;

  if (pid >= 0)
    {
      snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, file);
    }
  else
    {
      snprintf(path, sizeof(path), "/proc/%s", file);
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  while (total < (ssize_t)sizeof(m->buf) - 1)
    {
      ssize_t n = read(fd, &m->buf[total], sizeof(m->buf) - 1 - total);

      if (n <= 0)
        {
          break;
        }

      total += n;
    }

  close(fd);
  m->buf[total] = '\0';
  return total;
}

/* One thread; false if it has gone */

static bool mon_readproc(struct mon_s *m, pid_t pid, struct mon_proc_s *p)
{
  const char *v;

  memset(p, 0, sizeof(*p));
  p->pid   = pid;
  p->group = pid;
  p->cpu   = -1;
  p->heap  = -1;

  if (mon_read(m, pid, "status") <= 0)
    {
      return false;
    }

  if ((v = mon_field(m->buf, "Name:")) != NULL)
    {
      mon_copyline(p->name, v, sizeof(p->name));
    }

  if ((v = mon_field(m->buf, "Type:")) != NULL)
    {
      p->type = strncmp(v, "pthread", 7) == 0 ? T_PTHREAD :
                strncmp(v, "Kthread", 7) == 0 ? T_KTHREAD : T_TASK;
    }

  if ((v = mon_field(m->buf, "Group:")) != NULL)
    {
      p->group = (pid_t)atoi(v);
    }

  if ((v = mon_field(m->buf, "CPU:")) != NULL && isdigit((unsigned char)*v))
    {
      p->cpu = (int8_t)atoi(v);
    }

  if ((v = mon_field(m->buf, "State:")) != NULL)
    {
      mon_copyline(p->state, v, sizeof(p->state));
    }

  if ((v = mon_field(m->buf, "Priority:")) != NULL)
    {
      p->prio = (uint8_t)atoi(v);
    }

  /* The IDLE threads: "CPU<n> IDLE" with SMP, else "Idle_Task" */

  p->idle = pid < MON_MAXCPUS && (strstr(p->name, "IDLE") != NULL ||
                                  strcmp(p->name, "Idle_Task") == 0);

  if (mon_read(m, pid, "loadavg") > 0)
    {
      p->load = mon_percent(m->buf);
    }

  if (mon_read(m, pid, "stack") > 0)
    {
      if ((v = mon_field(m->buf, "StackSize:")) != NULL)
        {
          p->stacksize = (uint32_t)strtoul(v, NULL, 10);
        }

      if ((v = mon_field(m->buf, "StackUsed:")) != NULL)
        {
          p->stackused = (uint32_t)strtoul(v, NULL, 10);
        }
    }

#if MON_HEAPFILES
  /* Once a heap file is missing (procfs without them), stop looking */

  if (m->haveheap != 0)
    {
      if (mon_read(m, pid, "heap") > 0 &&
          (v = mon_field(m->buf, "AllocSize:")) != NULL)
        {
          p->heap = (int32_t)strtol(v, NULL, 10);
          m->haveheap = 1;
        }
      else if (m->haveheap < 0)
        {
          m->haveheap = 0;
        }
    }
#endif

  return true;
}

/* The heaps, from /proc/meminfo:
 *   total  used  free  maxused  maxfree  nused  nfree  name
 */

static void mon_readmem(struct mon_s *m)
{
  struct mon_snap_s *s = &m->snap;
  const char *line;

  s->nheaps   = 0;
  s->memtotal = 0;
  s->memused  = 0;

  if (mon_read(m, -1, "meminfo") <= 0)
    {
      return;
    }

  line = strchr(m->buf, '\n');                  /* Past the header */
  while (line != NULL && s->nheaps < MON_MAXHEAPS)
    {
      unsigned long total;
      unsigned long used;
      unsigned long freemem;
      unsigned long maxused;
      unsigned long maxfree;
      unsigned long nused;
      unsigned long nfree;
      char name[12];

      line++;
      if (sscanf(line, "%lu %lu %lu %lu %lu %lu %lu %11s", &total, &used,
                 &freemem, &maxused, &maxfree, &nused, &nfree, name) == 8 &&
          total > 0)
        {
          struct mon_heap_s *h = &s->heaps[s->nheaps++];

          strlcpy(h->name, name, sizeof(h->name));
          h->total   = (uint32_t)total;
          h->used    = (uint32_t)used;
          h->maxfree = (uint32_t)maxfree;
          s->memtotal += h->total;
          s->memused  += h->used;
        }

      line = strchr(line, '\n');
    }
}

static void mon_readdisk(struct mon_s *m)
{
  struct mon_snap_s *s = &m->snap;
  struct statfs buf;

  s->havedisk = false;
  if (CONFIG_FPCDESK_SYSMON_DISK[0] != '\0' &&
      statfs(CONFIG_FPCDESK_SYSMON_DISK, &buf) == 0 && buf.f_blocks > 0)
    {
      s->havedisk  = true;
      s->disktotal = (uint64_t)buf.f_blocks * buf.f_bsize;
      s->diskused  = (uint64_t)(buf.f_blocks - buf.f_bfree) * buf.f_bsize;
    }
}

/* The CPUs' loads, from their IDLE threads */

static void mon_readloads(struct mon_s *m)
{
  struct mon_snap_s *s = &m->snap;
  uint32_t idle = 0;
  int i;

  s->ncpus = 0;
  for (i = 0; i < s->nprocs; i++)
    {
      s->ncpus += s->procs[i].idle;
    }

  for (i = 0; i < s->nprocs; i++)
    {
      const struct mon_proc_s *p = &s->procs[i];

      if (p->idle)
        {
          int cpu = p->cpu >= 0 ? p->cpu : p->pid;

          if (cpu < MON_MAXCPUS)
            {
              int32_t busy = 1000 - (int32_t)p->load * s->ncpus;

              s->cpuload[cpu] = busy < 0 ? 0 : (uint16_t)busy;
            }

          idle += p->load;
        }
    }

  if (s->ncpus > 0)
    {
      s->totalload = idle > 1000 ? 0 : (uint16_t)(1000 - idle);
    }
  else
    {
      s->ncpus      = 1;
      s->totalload  = mon_read(m, -1, "cpuload") > 0 ?
                      mon_percent(m->buf) : 0;
      s->cpuload[0] = s->totalload;
    }

  if (s->ncpus > MON_MAXCPUS)
    {
      s->ncpus = MON_MAXCPUS;
    }
}

/* /proc/gpu: the numbers after a line's label, in order ("12.3" is 12 and
 * 3), at most max of them; how many (not sscanf(): its stack is large)
 */

static int gpu_nums(const char *buf, const char *key, unsigned long *out,
                    int max)
{
  const char *v = mon_field(buf, key);
  int n = 0;

  while (v != NULL && *v != '\0' && *v != '\n' && n < max)
    {
      if (isdigit((unsigned char)*v))
        {
          unsigned long x = 0;

          while (isdigit((unsigned char)*v))
            {
              x = x * 10 + (*v++ - '0');
            }

          out[n++] = x;
        }
      else
        {
          v++;
        }
    }

  return n;
}

static void mon_readgpu(struct mon_gpu_s *g)
{
  static char buf[GPU_BUFSIZE];
  unsigned long n[12];
  ssize_t total = 0;
  ssize_t r;
  int fd;
  int i;

  memset(g, 0, sizeof(*g));
  fd = open("/proc/gpu", O_RDONLY);
  if (fd < 0)
    {
      return;
    }

  while (total < GPU_BUFSIZE - 1 &&
         (r = read(fd, buf + total, GPU_BUFSIZE - 1 - total)) > 0)
    {
      total += r;
    }

  close(fd);
  buf[total] = '\0';
  g->have = true;

  if (gpu_nums(buf, "Protocol:", n, 1) == 1)
    {
      g->version = (uint8_t)n[0];
    }

  if (gpu_nums(buf, "Space:", n, 3) == 3)
    {
      g->space = n[2];
    }

  if (gpu_nums(buf, "Display:", n, 1) == 1)
    {
      g->mem[0] = n[0];
    }

  if (gpu_nums(buf, "Buffers:", n, 2) == 2)
    {
      g->mem[0] += n[1];
    }

  if (gpu_nums(buf, "Windows:", n, 2) == 2)
    {
      g->mem[1] = n[1];
    }

  if (gpu_nums(buf, "Images:", n, 2) == 2)
    {
      g->mem[2] = n[1];
    }

  if (gpu_nums(buf, "Released:", n, 1) == 1)
    {
      g->nparked = n[0];
    }

  if (gpu_nums(buf, "Other:", n, 2) == 2)
    {
      g->mem[3] = n[1];
    }

  if (gpu_nums(buf, "Free:", n, 3) == 3)
    {
      g->mem[4] = n[0];
      g->largew = n[1];
      g->largeh = n[2];
    }

  if (gpu_nums(buf, "In memory:", n, 1) == 1)
    {
      g->nmem = n[0];
    }

  if (gpu_nums(buf, "Frames:", n, 2) == 2)
    {
      g->fps = n[0] * 10 + n[1];
    }

  if (gpu_nums(buf, "Link:", n, 3) == 3)
    {
      g->link = n[0];
      g->wire = n[1] * 100 + n[2];                  /* 0.01 % */
    }

  if (gpu_nums(buf, "READY wait:", n, 2) == 2)
    {
      g->wait = n[0] * 10 + n[1];
    }

  /* A v6 GPU with a clock: how busy it is, the SDRAM's time by client
   * ("SDRAM: t.t % scanout a.a blitter b.b DMA c.c CPU d.d")
   */

  if (gpu_nums(buf, "GPU:", n, 3) == 3)
    {
      g->hw   = true;
      g->busy = n[1] * 10 + n[2];
      if (gpu_nums(buf, "SDRAM:", n, 10) == 10)
        {
          for (i = 0; i < MON_GPU_SDRAM; i++)
            {
              g->sdram[i] = n[2 + 2 * i] * 10 + n[3 + 2 * i];
            }
        }
    }
}

static void mon_sample(struct mon_s *m)
{
  struct mon_snap_s *s = &m->snap;
  struct dirent *entry;
  struct timespec ts;
  DIR *dir;
  int h;
  int i;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  s->uptime    = (uint32_t)ts.tv_sec;
  s->nprocs    = 0;
  s->ntasks    = 0;
  s->npthreads = 0;
  s->nkthreads = 0;
  s->nrunning  = 0;
  memset(s->cpuload, 0, sizeof(s->cpuload));

  dir = opendir("/proc");
  if (dir != NULL)
    {
      while ((entry = readdir(dir)) != NULL && s->nprocs < MON_MAXPROCS)
        {
          struct mon_proc_s *p = &s->procs[s->nprocs];
          char *end;
          long pid;

          /* The threads are the directories named by a number */

          if (!isdigit((unsigned char)entry->d_name[0]))
            {
              continue;
            }

          pid = strtol(entry->d_name, &end, 10);
          if (*end != '\0' || !mon_readproc(m, (pid_t)pid, p))
            {
              continue;
            }

          s->nprocs++;
          s->ntasks    += p->type == T_TASK;
          s->npthreads += p->type == T_PTHREAD;
          s->nkthreads += p->type == T_KTHREAD;
          s->nrunning  += strncmp(p->state, "Running", 7) == 0;
        }

      closedir(dir);
    }

  s->haveheap = m->haveheap > 0;
  mon_readloads(m);
  mon_readmem(m);
  mon_readdisk(m);
  mon_readgpu(&s->gpu);

  /* The graphs' history */

  h = m->nsamples % MON_HISTORY;
  for (i = 0; i < MON_MAXCPUS; i++)
    {
      m->hcpu[i][h] = i < s->ncpus ? s->cpuload[i] : 0;
    }

  m->hmem[h] = s->memtotal > 0 ?
               (uint16_t)((uint64_t)s->memused * 1000 / s->memtotal) : 0;
  m->nsamples++;
}

/****************************************************************************
 * Private Functions: the rows
 ****************************************************************************/

static int mon_compare(struct mon_s *m, const struct mon_proc_s *a,
                       const struct mon_proc_s *b)
{
  int diff;

  switch (m->sortcol)
    {
      case COL_NAME:
        diff = strcasecmp(a->name, b->name);
        break;

      case COL_STATE:
        diff = strcmp(a->state, b->state);
        break;

      case COL_PRIO:
        diff = (int)a->prio - (int)b->prio;
        break;

      case COL_CORE:
        diff = (int)a->cpu - (int)b->cpu;
        break;

      case COL_LOAD:
        diff = (int)a->load - (int)b->load;
        break;

      case COL_STACK:
        diff = (a->stackused > b->stackused) - (a->stackused < b->stackused);
        break;

      case COL_HEAP:
        diff = (a->heap > b->heap) - (a->heap < b->heap);
        break;

      default:
        diff = 0;
        break;
    }

  if (m->sortdown)
    {
      diff = -diff;
    }

  return diff != 0 ? diff : (int)a->pid - (int)b->pid;      /* Ties */
}

static void mon_clampscroll(struct mon_s *m)
{
  int max = m->nrows - m->visrows;

  if (m->scroll > max)
    {
      m->scroll = max;
    }

  if (m->scroll < 0)
    {
      m->scroll = 0;
    }
}

static void mon_sort(struct mon_s *m)
{
  const struct mon_snap_s *s = &m->snap;
  int i;

  m->nrows = 0;
  for (i = 0; i < s->nprocs; i++)
    {
      if (m->showkernel || s->procs[i].type != T_KTHREAD)
        {
          m->order[m->nrows++] = (uint8_t)i;
        }
    }

  /* Insertion sort: a few dozen rows, mostly in order already */

  for (i = 1; i < m->nrows; i++)
    {
      uint8_t index = m->order[i];
      int j = i - 1;

      while (j >= 0 && mon_compare(m, &s->procs[m->order[j]],
                                   &s->procs[index]) > 0)
        {
          m->order[j + 1] = m->order[j];
          j--;
        }

      m->order[j + 1] = index;
    }

  mon_clampscroll(m);
}

static int mon_selrow(struct mon_s *m)
{
  int i;

  for (i = 0; i < m->nrows; i++)
    {
      if (m->snap.procs[m->order[i]].pid == m->selected)
        {
          return i;
        }
    }

  return -1;
}

static const struct mon_proc_s *mon_selproc(struct mon_s *m)
{
  int row = mon_selrow(m);

  return row >= 0 ? &m->snap.procs[m->order[row]] : NULL;
}

/* Tasks may be ended: not the desktop itself, nor its threads, nor
 * kernel threads
 */

static bool mon_cankill(struct mon_s *m, const struct mon_proc_s *p)
{
  return p != NULL && p->type == T_TASK && !p->idle &&
         p->group != m->self && p->pid != m->self;
}

static void mon_showrow(struct mon_s *m, int row)
{
  if (row < m->scroll)
    {
      m->scroll = row;
    }
  else if (row >= m->scroll + m->visrows)
    {
      m->scroll = row - m->visrows + 1;
    }

  mon_clampscroll(m);
}

static void mon_movesel(struct mon_s *m, int delta)
{
  int row;

  if (m->nrows == 0)
    {
      return;
    }

  row = mon_selrow(m);
  row = row < 0 ? (delta > 0 ? 0 : m->nrows - 1) : row + delta;
  row = row < 0 ? 0 : row >= m->nrows ? m->nrows - 1 : row;
  m->selected = m->snap.procs[m->order[row]].pid;
  m->confirm  = false;
  mon_showrow(m, row);
}

/* The scrollbar's thumb, if the rows do not all fit */

static bool mon_thumb(struct mon_s *m, int *y, int *h)
{
  int track = m->sbar.h;
  int size;

  if (m->nrows <= m->visrows || m->visrows <= 0)
    {
      return false;
    }

  size = track * m->visrows / m->nrows;
  size = size < 20 ? 20 : size;
  *h = size;
  *y = m->sbar.y + (track - size) * m->scroll / (m->nrows - m->visrows);
  return true;
}

/****************************************************************************
 * Private Functions: formatting
 ****************************************************************************/

/* Bytes as "812 B", "12.3 KB", "8.0 MB", "29.7 GB" */

static void fmt_bytes(char *buf, size_t size, uint64_t bytes)
{
  static const char * const units[] =
  {
    "KB", "MB", "GB"
  };

  uint64_t scale = 1024;
  unsigned int tenths;
  int unit = 0;

  if (bytes < 1024)
    {
      snprintf(buf, size, "%u B", (unsigned int)bytes);
      return;
    }

  while (unit < 2 && bytes >= scale * 1024)
    {
      scale *= 1024;
      unit++;
    }

  tenths = (unsigned int)((bytes * 10 + scale / 2) / scale);
  snprintf(buf, size, "%u.%u %s", tenths / 10, tenths % 10, units[unit]);
}

/* A usage colour: green, amber from 60 %, red from 85 % */

static uint16_t level_colour(uint32_t used, uint32_t total)
{
  if (total > 0 && (uint64_t)used * 100 >= (uint64_t)total * 85)
    {
      return C_RED;
    }
  else if (total > 0 && (uint64_t)used * 100 >= (uint64_t)total * 60)
    {
      return C_AMBER;
    }

  return C_GREEN;
}

static uint32_t hash_bytes(uint32_t h, const void *data, size_t n)
{
  const uint8_t *p = data;

  while (n-- > 0)
    {
      h = (h ^ *p++) * 16777619u;
    }

  return h;
}

static uint32_t hash_str(const char *s, uint32_t extra)
{
  uint32_t h = hash_bytes(2166136261u, &extra, sizeof(extra));

  h = hash_bytes(h, s, strlen(s));
  return h != 0 ? h : 1;
}

/* s cut to fit maxw pixels (NOLIMIT: any), with an ellipsis; returns its
 * width
 */

static int fit_text(const struct desk_font_s *f, const char *s, int maxw,
                    char *out, size_t size)
{
  int width = text_width(f, s);
  int ell;
  size_t len;

  if (maxw <= 0)
    {
      out[0] = '\0';
      return 0;
    }

  if (width <= maxw)
    {
      strlcpy(out, s, size);
      return width;
    }

  ell   = text_width(f, DESK_CH_ELLIPSIS);
  width = 0;
  for (len = 0; s[len] != '\0' && len < size - 2; len++)
    {
      char one[2];
      int adv;

      one[0] = s[len];
      one[1] = '\0';
      adv    = text_width(f, one);
      if (width + adv + ell > maxw)
        {
          break;
        }

      width += adv;
    }

  memcpy(out, s, len);
  out[len]     = DESK_CH_ELLIPSIS[0];
  out[len + 1] = '\0';
  return width + ell;
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

/* Operations go into g_o and are sent (flush()) at the end of each part
 * of the window, so that what the server composites again is only around
 * what changed; need() sends them early when g_o would overflow.
 */

static void flush(void)
{
#if MON_STATS
  int i;

  for (i = 0; i < g_o.n; i++)
    {
      g_stats.bytes += g_o.op[i].op == NX_GPUOP_BLEND ? 32 : 16;
    }

  g_stats.ops += g_o.n;
  g_stats.calls += g_o.n > 0;
#endif

  ops_draw(g_hwnd, &g_o);
}

static void need(int n)
{
  if (g_o.n + n > NX_GPUOPS_MAX)
    {
      flush();
    }
}

static void d_fill(int x, int y, int w, int h, uint16_t c)
{
  if (w > 0 && h > 0)
    {
      need(1);
      ops_fill(&g_o, x, y, w, h, c);
    }
}

static void d_rect_fill(const struct mon_rect_s *r, uint16_t c)
{
  d_fill(r->x, r->y, r->w, r->h, c);
}

/* A translucent rectangle (not dithered: flat areas look cleaner) */

static void d_blend(int x, int y, int w, int h, uint16_t c, uint8_t alpha)
{
  struct nx_gpuop_s *op;

  if (w > 0 && h > 0)
    {
      need(1);
      op = ops_add(&g_o);
      op->op     = NX_GPUOP_BLEND;
      op->x      = x;
      op->y      = y;
      op->w      = w;
      op->h      = h;
      op->colour = c;
      op->alpha  = alpha;
      op->mode   = NX_BLEND_SRC_COLOUR;
    }
}

static void d_rrect(int x, int y, int w, int h, int slot, int r, uint16_t c)
{
  if (w > 0 && h > 0)
    {
      need(1);
      ops_rrect(&g_o, x, y, w, h, slot, r, c, 255);
    }
}

/* Only the columns cx .. cx + cw - 1 of a round rectangle: the shape's
 * parameters are absolute, so the piece meets the rest exactly
 */

static void d_rrect_part(int x, int y, int w, int h, int slot, int r,
                         uint16_t c, int cx, int cw)
{
  struct nx_gpuop_s *op;
  int x0 = cx > x ? cx : x;
  int x1 = cx + cw < x + w ? cx + cw : x + w;

  if (x1 <= x0 || h <= 0)
    {
      return;
    }

  need(1);
  op = ops_add(&g_o);
  op->op     = NX_GPUOP_BLEND;
  op->x      = x0;
  op->y      = y;
  op->w      = x1 - x0;
  op->h      = h;
  op->colour = c;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT;
  op->slot   = slot;
  op->p[0]   = x + r;
  op->p[1]   = y + r;
  op->p[2]   = x + w - 1 - r;
  op->p[3]   = y + h - 1 - r;
}

static void d_copy(int sx, int sy, int x, int y, int w, int h)
{
  struct nx_gpuop_s *op;

  if (w > 0 && h > 0)
    {
      need(1);
      op = ops_add(&g_o);
      op->op = NX_GPUOP_COPY;
      op->x  = x;
      op->y  = y;
      op->w  = w;
      op->h  = h;
      op->sx = sx;
      op->sy = sy;
    }
}

static int d_text(const struct desk_font_s *f, int x, int y, const char *s,
                  uint16_t c)
{
  need((int)strlen(s));
  return ops_text(&g_o, f, x, y, s, c, 255);
}

/* A bar: a pill-shaped track, filled from the left */

static void d_bar(int x, int y, int w, int fillw, uint16_t c)
{
  if (w <= 0)
    {
      return;
    }

  d_rrect(x, y, w, BAR_H, LUT_R3, 3, C_TRACK);
  if (fillw > 0)
    {
      d_rrect(x, y, fillw > w ? w : fillw, BAR_H, LUT_R3, 3, c);
    }
}

static int bar_fill(int w, uint64_t used, uint64_t total)
{
  int fw;

  if (total == 0 || used == 0 || w <= 0)
    {
      return 0;
    }

  fw = (int)((uint64_t)w * used / total);
  return fw < BAR_H ? BAR_H : fw > w ? w : fw;   /* A dot, not nothing */
}

/* Whether a field needs drawing: when everything is drawn, or what it
 * shows has changed
 */

static bool field_changed(struct mon_s *m, int id, uint32_t hash)
{
  hash = hash != 0 ? hash : 1;
  if (!m->force && m->f[id].hash == hash)
    {
      return false;
    }

  m->f[id].hash = hash;
  return true;
}

/* Clear what a field showed (nothing to clear when everything is drawn:
 * the background is new)
 */

static void field_clear(struct mon_s *m, int id, uint16_t bg)
{
  if (!m->force)
    {
      d_rect_fill(&m->f[id].r, bg);
    }
}

static void field_set(struct mon_s *m, int id, int x, int y, int w, int h)
{
  m->f[id].r.x = x;
  m->f[id].r.y = y;
  m->f[id].r.w = w;
  m->f[id].r.h = h;
}

/* A text that changes: x is its left edge, or its right edge if right */

static void field_text(struct mon_s *m, int id, const struct desk_font_s *f,
                       int x, int y, bool right, int maxw, const char *s,
                       uint16_t fg, uint16_t bg)
{
  char cut[160];
  int w;

  if (!field_changed(m, id, hash_str(s, fg ^ ((uint32_t)x << 16) ^ maxw)))
    {
      return;
    }

  field_clear(m, id, bg);
  w = fit_text(f, s, maxw, cut, sizeof(cut));
  if (right)
    {
      x -= w;
    }

  if (cut[0] != '\0')
    {
      d_text(f, x, y, cut, fg);
    }

  /* Glyphs may reach a pixel past their advance */

  field_set(m, id, x - 1, y, w + 2, text_height(f));
}

static void field_bar(struct mon_s *m, int id, int x, int y, int w,
                      uint64_t used, uint64_t total, uint16_t c, uint16_t bg)
{
  int fw = bar_fill(w, used, total);

  if (!field_changed(m, id, ((uint32_t)fw << 16 | c) ^
                            ((uint32_t)w << 8) ^ (uint32_t)x))
    {
      return;
    }

  field_clear(m, id, bg);
  d_bar(x, y, w, fw, c);
  field_set(m, id, x, y, w, BAR_H);
}

/****************************************************************************
 * Private Functions: the layout
 ****************************************************************************/

static void rect_set(struct mon_rect_s *r, int x, int y, int w, int h)
{
  r->x = x;
  r->y = y;
  r->w = w > 0 ? w : 0;
  r->h = h > 0 ? h : 0;
}

static bool rect_in(const struct mon_rect_s *r, int x, int y)
{
  return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y &&
         y < r->y + r->h;
}

static void graph_layout(struct mon_graph_s *g, int x, int y, int w, int h)
{
  int pw;

  memset(g, 0, sizeof(*g));
  if (w < 40 || h < 20)
    {
      return;
    }

  rect_set(&g->r, x, y, w, h);
  g->px0  = x + 3;                      /* Clear of the round corners */
  g->px1  = x + w - 4;
  g->py0  = y + 1;
  g->py1  = y + h - 2;
  pw      = g->px1 - g->px0 + 1;
  g->step = pw / MON_GRAPHPTS < 2 ? 2 : pw / MON_GRAPHPTS;
  g->npts = (pw - 1) / g->step + 1;
  if (g->npts > MON_HISTORY)
    {
      g->npts = MON_HISTORY;
      g->px0  = g->px1 - (g->npts - 1) * g->step;
    }
}

/* Lay out for the client area now: the cards across the top (as many as
 * fit), the table of threads below them, the footer at the bottom
 */

static void mon_layout(struct desk_win_s *w, struct mon_s *m)
{
  const struct mon_snap_s *s = &m->snap;
  int cw = w->csize.w;
  int ch = w->csize.h;
  int top = TITLE_H;
  int avail;
  int x;
  int y;
  int i;
  int drop;
  int fixed;
  int left;
  int right;

  m->lw = cw;
  m->lh = ch;
  m->laidheap = s->haveheap;
  memset(&m->cpucard, 0, sizeof(m->cpucard));
  memset(&m->memcard, 0, sizeof(m->memcard));
  memset(&m->syscard, 0, sizeof(m->syscard));
  memset(&m->gpucard, 0, sizeof(m->gpucard));

  /* The cards: three across, or two, or only the CPU's; none if the
   * window is too low for them and some rows
   */

  y = top + PAD;
  if (ch >= CARD_H + 2 * PAD + FOOTER_H + 120)
    {
      avail = cw - 2 * PAD;
      if (cw >= 640)
        {
          int cpuw = (avail - 2 * PAD) * 40 / 100;
          int memw = (avail - 2 * PAD) * 31 / 100;

          rect_set(&m->cpucard, PAD, y, cpuw, CARD_H);
          rect_set(&m->memcard, 2 * PAD + cpuw, y, memw, CARD_H);
          rect_set(&m->syscard, 3 * PAD + cpuw + memw, y,
                   avail - 2 * PAD - cpuw - memw, CARD_H);
        }
      else if (cw >= 420)
        {
          int cpuw = (avail - PAD) * 55 / 100;

          rect_set(&m->cpucard, PAD, y, cpuw, CARD_H);
          rect_set(&m->memcard, 2 * PAD + cpuw, y, avail - PAD - cpuw,
                   CARD_H);
        }
      else
        {
          rect_set(&m->cpucard, PAD, y, avail, CARD_H);
        }

      y += CARD_H + PAD;

      /* The GPU across the window below them, if there is room for it and
       * some rows
       */

      if (s->gpu.have && cw >= 420 &&
          ch >= CARD_H + GPU_H + 3 * PAD + FOOTER_H + 160)
        {
          rect_set(&m->gpucard, PAD, y, avail, GPU_H);
          y += GPU_H + PAD;
        }
    }

  rect_set(&m->footer, 0, top + ch - FOOTER_H, cw, FOOTER_H);
  rect_set(&m->proccard, PAD, y, cw - 2 * PAD, m->footer.y - y);

  /* The CPU card: the list of CPUs right of the big figure, the graph
   * below
   */

  if (m->cpucard.w > 0)
    {
      const struct mon_rect_s *c = &m->cpucard;

      m->cpux = c->x + INSET + text_width(&g_desk_font_big, "100%") + 18;
      graph_layout(&m->gcpu, c->x + INSET, c->y + 92, c->w - 2 * INSET,
                   c->h - 92 - INSET);
    }
  else
    {
      memset(&m->gcpu, 0, sizeof(m->gcpu));
    }

  /* The memory card: a bar per heap, then the graph if there is room */

  memset(&m->gmem, 0, sizeof(m->gmem));
  if (m->memcard.w > 0)
    {
      const struct mon_rect_s *c = &m->memcard;
      int gy = c->y + 92 + s->nheaps * HEAP_ROW + 4;

      graph_layout(&m->gmem, c->x + INSET, gy, c->w - 2 * INSET,
                   c->y + c->h - INSET - gy);
    }

  /* The table's card: its title row with the controls at the right, the
   * header, the rows and the scrollbar
   */

  if (m->proccard.h >= 80 && m->proccard.w >= 120)
    {
      const struct mon_rect_s *c = &m->proccard;
      int cy = c->y + 8;
      int ww;
      int rowsy;

      x  = c->x + c->w - INSET;
      ww = text_width(&g_desk_font_ui, "Confirm end task") + 24;
      rect_set(&m->chipkill, x - ww, cy, ww, CHIP_H);
      x -= ww + 8;
      ww = text_width(&g_desk_font_ui, "Every 5 s") + 24;
      rect_set(&m->chipival, x - ww, cy, ww, CHIP_H);
      x -= ww + 8;
      ww = text_width(&g_desk_font_ui, "Kernel threads") + 36;
      rect_set(&m->chipkern, x - ww, cy, ww, CHIP_H);

      /* Controls that do not fit are not shown */

      if (m->chipkern.x < c->x + 150)
        {
          m->chipkern.w = 0;
        }

      if (m->chipival.x < c->x + 150)
        {
          m->chipival.w = 0;
        }

      if (m->chipkill.x < c->x + 150)
        {
          m->chipkill.w = 0;
        }

      rect_set(&m->header, c->x + 8, c->y + 38, c->w - 16, HEADER_H);
      rowsy = m->header.y + HEADER_H + 4;
      m->visrows = (c->y + c->h - 8 - rowsy) / ROW_H;
      m->visrows = m->visrows < 0 ? 0 : m->visrows;
      rect_set(&m->sbar, c->x + c->w - 8 - 8, rowsy, 6, m->visrows * ROW_H);
      rect_set(&m->rows, c->x + 8, rowsy, m->sbar.x - 4 - (c->x + 8),
               m->visrows * ROW_H);
    }
  else
    {
      memset(&m->chipkill, 0, sizeof(m->chipkill));
      memset(&m->chipival, 0, sizeof(m->chipival));
      memset(&m->chipkern, 0, sizeof(m->chipkern));
      memset(&m->header, 0, sizeof(m->header));
      memset(&m->rows, 0, sizeof(m->rows));
      memset(&m->sbar, 0, sizeof(m->sbar));
      m->visrows = 0;
    }

  /* The columns: drop the least useful until the name has room */

  left  = m->rows.x + 8;
  right = m->rows.x + m->rows.w - 6;
  for (i = 0; i < NCOLS; i++)
    {
      m->colshown[i] = i != COL_HEAP || s->haveheap;
    }

  for (drop = 1; ; drop++)
    {
      fixed = 0;
      for (i = 0; i < NCOLS; i++)
        {
          if (m->colshown[i] && g_cols[i].w > 0)
            {
              fixed += g_cols[i].w + COL_GAP;
            }
        }

      if (right - left - fixed >= 110 || drop > 6)
        {
          break;
        }

      for (i = 0; i < NCOLS; i++)
        {
          if (g_cols[i].drop == drop)
            {
              m->colshown[i] = false;
            }
        }
    }

  x = left;
  for (i = 0; i < NCOLS; i++)
    {
      m->colx[i] = x;
      m->colw[i] = 0;
      if (m->colshown[i])
        {
          m->colw[i] = g_cols[i].w > 0 ? g_cols[i].w :
                       right - left - fixed > 0 ? right - left - fixed : 0;
          x += m->colw[i] + COL_GAP;
        }
    }

  /* Each cell reaches half way to its neighbours; the first and last to
   * the ends of the row
   */

  for (i = 0; i < NCOLS; i++)
    {
      m->colx0[i] = m->colx[i] - COL_GAP / 2;
      m->colx1[i] = m->colx[i] + m->colw[i] + COL_GAP / 2;
    }

  for (i = 0; i < NCOLS && !m->colshown[i]; i++)
    {
    }

  if (i < NCOLS)
    {
      m->colx0[i] = m->rows.x;
    }

  for (i = NCOLS - 1; i >= 0 && !m->colshown[i]; i--)
    {
    }

  if (i >= 0)
    {
      m->colx1[i] = m->rows.x + m->rows.w;
    }

  mon_clampscroll(m);
}

/****************************************************************************
 * Private Functions: the graphs
 ****************************************************************************/

/* Series s of a graph at age a samples (0: the newest) */

static int graph_hist(struct mon_s *m, bool cpu, int s, int a)
{
  int i = (int)((m->nsamples - 1 - a) % MON_HISTORY);

  return cpu ? m->hcpu[s][i] : m->hmem[i];
}

/* The value at pixel column px, 0.1 %, or -1 where there is no history */

static int graph_val(struct mon_s *m, struct mon_graph_s *g, bool cpu,
                     int s, int px)
{
  int d = g->px1 - px;
  int a = d / g->step;
  int f = d % g->step;
  int count = m->nsamples < MON_HISTORY ? (int)m->nsamples : MON_HISTORY;
  int v0;
  int v1;

  if (d < 0 || a >= count || (f != 0 && a + 1 >= count))
    {
      return -1;
    }

  v0 = graph_hist(m, cpu, s, a);
  if (f == 0)
    {
      return v0;
    }

  v1 = graph_hist(m, cpu, s, a + 1);
  return v0 + (v1 - v0) * f / g->step;
}

static int graph_y(struct mon_graph_s *g, int v)
{
  return g->py1 - v * (g->py1 - g->py0) / 1000;
}

/* Draw pixel columns pxa .. pxb (on the graph's background): the grid,
 * then each series' tinted area, then each line; runs of equal columns
 * are one operation
 */

static void graph_plot(struct mon_s *m, struct mon_graph_s *g, bool cpu,
                       int pxa, int pxb)
{
  int nseries = cpu ? m->snap.ncpus : 1;
  int ph = g->py1 - g->py0 + 1;
  int pass;
  int px;
  int s;
  int i;

  for (i = 1; i < 4; i++)
    {
      d_fill(pxa, g->py0 + ph * i / 4, pxb - pxa + 1, 1, C_GRID);
    }

  /* A vertical line every ten samples: they move with the samples */

  for (px = pxa; px <= pxb; px++)
    {
      int d = g->px1 - px;

      if (d % g->step == 0 && (uint32_t)(d / g->step) < m->nsamples &&
          (m->nsamples - 1 - (uint32_t)(d / g->step)) % 10 == 0)
        {
          d_fill(px, g->py0, 1, ph, C_GRID);
        }
    }

  for (pass = 0; pass < 2; pass++)
    {
      for (s = nseries - 1; s >= 0; s--)
        {
          uint16_t c = cpu ? cpu_colour(s) : C_VIOLET;
          int runx = -1;
          int runa = 0;
          int runb = 0;

          for (px = pxa; px <= pxb + 1; px++)
            {
              int a = -1;
              int b = -1;

              if (px <= pxb)
                {
                  int v = graph_val(m, g, cpu, s, px);

                  if (v >= 0)
                    {
                      int y = graph_y(g, v);
                      int vl = graph_val(m, g, cpu, s, px - 1);
                      int yl = vl >= 0 && px > g->px0 ? graph_y(g, vl) : y;

                      if (pass == 0)
                        {
                          a = y + 1;                     /* The area */
                          b = g->py1;
                        }
                      else
                        {
                          a = y < yl ? y : yl;           /* The line */
                          b = (y > yl ? y : yl) + 1;
                          b = b > g->py1 ? g->py1 : b;
                        }
                    }
                }

              if (runx >= 0 && (a != runa || b != runb))
                {
                  if (runb >= runa)
                    {
                      if (pass == 0)
                        {
                          d_blend(runx, runa, px - runx, runb - runa + 1, c,
                                  60);
                        }
                      else
                        {
                          d_fill(runx, runa, px - runx, runb - runa + 1, c);
                        }
                    }

                  runx = -1;
                }

              if (runx < 0 && a >= 0)
                {
                  runx = px;
                  runa = a;
                  runb = b;
                }
            }
        }
    }
}

/* A graph: moved left by the samples taken since it was drawn (a COPY
 * within the surface) and the new columns drawn, or all of it
 */

static void graph_draw(struct mon_s *m, struct mon_graph_s *g, bool cpu)
{
  int shift;
  int pw;

  if (g->r.w == 0)
    {
      return;
    }

  pw    = g->px1 - g->px0 + 1;
  shift = (int)(m->nsamples - g->drawn) * g->step;
  if (m->force || g->drawn == 0 || shift >= pw)
    {
      d_rrect(g->r.x, g->r.y, g->r.w, g->r.h, LUT_R3, 3, C_GRAPH);
      graph_plot(m, g, cpu, g->px0, g->px1);
    }
  else if (shift > 0)
    {
      d_copy(g->px0 + shift, g->py0, g->px0, g->py0, pw - shift,
             g->py1 - g->py0 + 1);
      d_fill(g->px1 - shift + 1, g->py0, shift, g->py1 - g->py0 + 1,
             C_GRAPH);
      graph_plot(m, g, cpu, g->px1 - shift + 1, g->px1);
    }

  g->drawn = m->nsamples;
}

/****************************************************************************
 * Private Functions: the cards
 ****************************************************************************/

static void card_bg(const struct mon_rect_s *c, const char *caption)
{
  if (c->w > 0)
    {
      d_rrect(c->x, c->y, c->w, c->h, LUT_R12, CARD_R, C_CARD);
      d_text(&g_desk_font_small, c->x + INSET, c->y + 10, caption,
             C_TEXT_DIM);
    }
}

static void draw_cpu(struct mon_s *m)
{
  const struct mon_snap_s *s = &m->snap;
  const struct mon_rect_s *c = &m->cpucard;
  int x0 = c->x + INSET;
  int right = c->x + c->w - INSET;
  char buf[32];
  unsigned int span;
  int i;

  if (c->w == 0)
    {
      return;
    }

  span = (m->gcpu.npts > 1 ? m->gcpu.npts - 1 : MON_GRAPHPTS) *
         g_intervals[m->ival];
  if (span >= 120)
    {
      snprintf(buf, sizeof(buf), "last %u min", span / 60);
    }
  else
    {
      snprintf(buf, sizeof(buf), "last %u s", span);
    }

  field_text(m, F_CPU_NOTE, &g_desk_font_small, right, c->y + 10, true,
             c->w / 2 - INSET, buf, C_TEXT_DIM, C_CARD);

  snprintf(buf, sizeof(buf), "%u%%", (s->totalload + 5) / 10);
  field_text(m, F_CPU_BIG, &g_desk_font_big, x0, c->y + 22, false,
             m->cpux - 12 - x0, buf, C_TEXT, C_CARD);

  /* Each CPU in its graph colour, two to a column */

  for (i = 0; i < s->ncpus; i++)
    {
      int cx = m->cpux + (i / 2) * 104;
      int cy = c->y + 38 + (i % 2) * 22;

      if (cx + 88 > right)
        {
          break;
        }

      if (m->force)
        {
          need(1);
          ops_disc(&g_o, cx + 4, cy + 9, LUT_R3, 3, cpu_colour(i), 255);
          snprintf(buf, sizeof(buf), "CPU%d", i);
          d_text(&g_desk_font_ui, cx + 13, cy, buf, C_TEXT_DIM);
        }

      snprintf(buf, sizeof(buf), "%u%%", (s->cpuload[i] + 5) / 10);
      field_text(m, F_CPU_VAL + i, &g_desk_font_uib, cx + 88, cy, true,
                 NOLIMIT, buf, C_TEXT, C_CARD);
    }

  flush();
  graph_draw(m, &m->gcpu, true);
  flush();
}

static void draw_memory(struct mon_s *m)
{
  const struct mon_snap_s *s = &m->snap;
  const struct mon_rect_s *c = &m->memcard;
  int x0 = c->x + INSET;
  int iw = c->w - 2 * INSET;
  int bx;
  char buf[48];
  char buf2[24];
  int i;

  if (c->w == 0)
    {
      return;
    }

  snprintf(buf, sizeof(buf), "%d heap%s", s->nheaps,
           s->nheaps == 1 ? "" : "s");
  field_text(m, F_MEM_NOTE, &g_desk_font_small, x0 + iw, c->y + 10, true,
             iw / 2, buf, C_TEXT_DIM, C_CARD);

  snprintf(buf, sizeof(buf), "%u%%", s->memtotal > 0 ?
           (unsigned int)(((uint64_t)s->memused * 100 + s->memtotal / 2) /
                          s->memtotal) : 0);
  field_text(m, F_MEM_BIG, &g_desk_font_big, x0, c->y + 22, false,
             iw / 2, buf, C_TEXT, C_CARD);

  /* Beside the figure: what is used of what there is, shorter if that
   * does not fit
   */

  bx = m->f[F_MEM_BIG].r.x + m->f[F_MEM_BIG].r.w + 14;
  fmt_bytes(buf2, sizeof(buf2), s->memused);
  snprintf(buf, sizeof(buf), "%s used", buf2);
  if (text_width(&g_desk_font_ui, buf) > x0 + iw - bx)
    {
      strlcpy(buf, buf2, sizeof(buf));
    }

  field_text(m, F_MEM_USED, &g_desk_font_ui, bx, c->y + 38, false,
             x0 + iw - bx, buf, C_TEXT, C_CARD);
  fmt_bytes(buf2, sizeof(buf2), s->memtotal);
  snprintf(buf, sizeof(buf), "of %s", buf2);
  field_text(m, F_MEM_TOTAL, &g_desk_font_ui, bx, c->y + 60, false,
             x0 + iw - bx, buf, C_TEXT_DIM, C_CARD);

  /* A bar per heap, as many as fit */

  for (i = 0; i < s->nheaps; i++)
    {
      const struct mon_heap_s *h = &s->heaps[i];
      int y = c->y + 88 + i * HEAP_ROW;
      int nw;

      if (y + HEAP_ROW > c->y + c->h - 8)
        {
          break;
        }

      field_text(m, F_HEAP_NAME + i, &g_desk_font_uib, x0, y, false, iw / 2,
                 h->name, C_TEXT, C_CARD);
      nw = text_width(&g_desk_font_uib, h->name) + 12;
      fmt_bytes(buf2, sizeof(buf2), h->maxfree);
      snprintf(buf, sizeof(buf), "largest free %s", buf2);
      if (nw + text_width(&g_desk_font_small, buf) > iw)
        {
          strlcpy(buf, buf2, sizeof(buf));
        }

      field_text(m, F_HEAP_FREE + i, &g_desk_font_small, x0 + iw, y + 2,
                 true, iw - nw, buf, C_TEXT_DIM, C_CARD);
      field_bar(m, F_HEAP_BAR + i, x0, y + 19, iw, h->used, h->total,
                level_colour(h->used, h->total), C_CARD);
    }

  flush();
  graph_draw(m, &m->gmem, false);
  flush();
}

static void draw_system(struct mon_s *m)
{
  static const char * const labels[4] =
  {
    "Uptime", "OS", "Board", "Threads"
  };

  const struct mon_snap_s *s = &m->snap;
  const struct mon_rect_s *c = &m->syscard;
  int x0 = c->x + INSET;
  int iw = c->w - 2 * INSET;
  int vx = x0 + 60;
  int vw = x0 + iw - vx;
  uint32_t up = s->uptime;
  char buf[48];
  int i;
  int y;

  if (c->w == 0)
    {
      return;
    }

  if (m->force)
    {
      for (i = 0; i < 4; i++)
        {
          d_text(&g_desk_font_ui, x0, c->y + 32 + i * 20, labels[i],
                 C_TEXT_DIM);
        }
    }

  if (up >= 86400)
    {
      snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu",
               (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
               (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
    }
  else
    {
      snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu",
               (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
               (unsigned long)(up % 60));
    }

  field_text(m, F_UPTIME, &g_desk_font_uib, vx, c->y + 32, false, vw, buf,
             C_TEXT, C_CARD);
  field_text(m, F_OS, &g_desk_font_ui, vx, c->y + 52, false, vw, m->osname,
             C_TEXT, C_CARD);
  snprintf(buf, sizeof(buf), "%s, %d x %s", MON_BOARD, s->ncpus,
           CONFIG_ARCH_CHIP);
  field_text(m, F_BOARD, &g_desk_font_ui, vx, c->y + 72, false, vw, buf,
             C_TEXT, C_CARD);
  snprintf(buf, sizeof(buf), "%d, %d running", s->nprocs, s->nrunning);
  field_text(m, F_THREADS, &g_desk_font_ui, vx, c->y + 92, false, vw, buf,
             C_TEXT, C_CARD);

  /* The disk at the bottom: "1.2 GB of 29.7 GB", or its percentage if
   * that does not fit; "not mounted" when statfs() fails
   */

  y = c->y + c->h - INSET - 26;
  if (m->force)
    {
      d_text(&g_desk_font_uib, x0, y, CONFIG_FPCDESK_SYSMON_DISKNAME,
             C_TEXT);
    }

  i = text_width(&g_desk_font_uib, CONFIG_FPCDESK_SYSMON_DISKNAME) + 12;
  if (s->havedisk)
    {
      char used[16];
      char total[16];

      fmt_bytes(used, sizeof(used), s->diskused);
      fmt_bytes(total, sizeof(total), s->disktotal);
      snprintf(buf, sizeof(buf), "%s of %s", used, total);
      if (i + text_width(&g_desk_font_small, buf) > iw)
        {
          snprintf(buf, sizeof(buf), "%u%%",
                   (unsigned int)(s->diskused * 100 / s->disktotal));
        }
    }
  else
    {
      strlcpy(buf, CONFIG_FPCDESK_SYSMON_DISK[0] != '\0' ? "not mounted" :
              "none", sizeof(buf));
    }

  field_text(m, F_DISK_TEXT, &g_desk_font_small, x0 + iw, y + 2, true,
             iw - i, buf, C_TEXT_DIM, C_CARD);
  field_bar(m, F_DISK_BAR, x0, y + 19, iw, s->havedisk ? s->diskused : 0,
            s->havedisk ? s->disktotal : 1, C_ACCENT, C_CARD);
  flush();
}

/****************************************************************************
 * Private Functions: the GPU card
 ****************************************************************************/

/* A bar made of parts (a stacked bar): the pill-shaped track, then each
 * part in its colour, left to right, the round ends shared by the first
 * and last (the shape's parameters are the whole bar's)
 */

static void d_stackbar(int x, int y, int w, int h, int n,
                       const uint32_t *val, const uint16_t *colour,
                       uint64_t total)
{
  uint64_t acc = 0;
  int px = 0;
  int i;

  d_rrect(x, y, w, h, LUT_R3, 3, C_TRACK);
  for (i = 0; i < n && total > 0; i++)
    {
      int end;

      acc += val[i];
      end  = (int)((uint64_t)w * acc / total);
      end  = end > w ? w : end;
      if (val[i] > 0 && end <= px)
        {
          end = px + 1;                 /* Something: a sliver */
        }

      if (end > px)
        {
          need(1);
          d_rrect_part(x, y, w, h, LUT_R3, 3, colour[i], x + px, end - px);
          px = end;
        }
    }
}

/* A key for a stacked bar: a dot of each part's colour and its label,
 * as many as fit maxw, redrawn only when they change
 */

static void field_key(struct mon_s *m, int id, int x, int y, int maxw,
                      int n, const char * const *label,
                      const uint16_t *colour, uint16_t bg)
{
  uint32_t hash = 2166136261u;
  int cx = x;
  int i;

  for (i = 0; i < n; i++)
    {
      hash = hash_bytes(hash, label[i], strlen(label[i]));
      hash = hash_bytes(hash, &colour[i], sizeof(colour[i]));
    }

  if (!field_changed(m, id, hash ^ (uint32_t)maxw))
    {
      return;
    }

  field_clear(m, id, bg);
  for (i = 0; i < n; i++)
    {
      int tw = text_width(&g_desk_font_small, label[i]);

      if (cx + 10 + tw > x + maxw)
        {
          break;
        }

      need(2);
      ops_disc(&g_o, cx + 3, y + 7, LUT_R3, 3, colour[i], 255);
      d_text(&g_desk_font_small, cx + 10, y, label[i], C_TEXT_DIM);
      cx += 10 + tw + 12;
    }

  field_set(m, id, x - 1, y, cx - x + 2, text_height(&g_desk_font_small));
}

/* Pixels of the GPU's memory as bytes */

static void fmt_px(char *buf, size_t size, uint32_t px)
{
  fmt_bytes(buf, size, (uint64_t)px * 2);
}

/* The GPU: its memory (what holds it, what is free, the largest free
 * rectangle), the link (bytes a second, of the wire, the time the CPU
 * waited for the GPU), and with a v6 GPU how busy its blitter is and
 * whose the SDRAM's time is; frames a second above
 */

static void draw_gpu(struct mon_s *m)
{
  const uint16_t memcol[MON_GPU_MEMS] =                /* The theme's */
  {
    C_ACCENT, C_VIOLET, RGB(0x22, 0xd0, 0xb0), C_FAINT, C_TRACK
  };

  const uint16_t sdcol[MON_GPU_SDRAM] =
  {
    C_ACCENT, C_VIOLET, C_GREEN, C_AMBER
  };

  static const char * const sdname[MON_GPU_SDRAM] =
  {
    "scanout", "blitter", "DMA", "CPU"
  };

  const struct mon_gpu_s *g = &m->snap.gpu;
  const struct mon_rect_s *c = &m->gpucard;
  static char keybuf[MON_GPU_MEMS][64];   /* Static: the desktop's stack is
                                          * small */
  const char *key[MON_GPU_MEMS];
  uint16_t kc[MON_GPU_MEMS];
  char buf[64];
  char b1[16];
  uint32_t v[MON_GPU_SDRAM];
  int x0 = c->x + INSET;
  int iw = c->w - 2 * INSET;
  int colw[3];
  int colx[3];
  int lw;
  int nk = 0;
  int i;

  if (c->w == 0)
    {
      return;
    }

  /* Three columns: the memory, the link, the GPU's time */

  colw[0] = (iw - 40) * 50 / 100;
  colw[1] = (iw - 40) * 20 / 100;
  colw[2] = iw - 40 - colw[0] - colw[1];
  colx[0] = x0;
  colx[1] = colx[0] + colw[0] + 20;
  colx[2] = colx[1] + colw[1] + 20;

  if (m->force)
    {
      d_text(&g_desk_font_uib, colx[0], c->y + 30, "Memory", C_TEXT);
      d_text(&g_desk_font_uib, colx[1], c->y + 30, "Link", C_TEXT);
      d_text(&g_desk_font_uib, colx[2], c->y + 30, "GPU busy", C_TEXT);
    }

  snprintf(buf, sizeof(buf), "protocol v%u, %u.%u frames/s", g->version,
           g->fps / 10, g->fps % 10);
  field_text(m, F_GPU_NOTE, &g_desk_font_small, x0 + iw, c->y + 10, true,
             iw / 2, buf, C_TEXT_DIM, C_CARD);

  /* The memory: what is free and the largest free rectangle, a bar of
   * what holds the rest, its key
   */

  lw = colw[0] - text_width(&g_desk_font_uib, "Memory") - 12;
  fmt_px(b1, sizeof(b1), g->mem[4]);
  snprintf(buf, sizeof(buf), "%s free, largest %u x %u", b1, g->largew,
           g->largeh);
  if (text_width(&g_desk_font_small, buf) > lw)
    {
      snprintf(buf, sizeof(buf), "%s free", b1);
    }

  field_text(m, F_GPU_MEMTEXT, &g_desk_font_small, colx[0] + colw[0],
             c->y + 32, true, lw, buf, C_TEXT_DIM, C_CARD);

  if (field_changed(m, F_GPU_MEMBAR,
                    hash_bytes(2166136261u, g->mem, sizeof(g->mem)) ^
                    (uint32_t)colw[0]))
    {
      field_clear(m, F_GPU_MEMBAR, C_CARD);
      d_stackbar(colx[0], c->y + 54, colw[0], GPU_BAR_H, MON_GPU_MEMS - 1,
                 g->mem, memcol, g->space);
      field_set(m, F_GPU_MEMBAR, colx[0], c->y + 54, colw[0], GPU_BAR_H);
    }

  fmt_px(b1, sizeof(b1), g->mem[0]);
  snprintf(keybuf[nk], sizeof(keybuf[nk]), "Display %s", b1);
  kc[nk++] = memcol[0];
  fmt_px(b1, sizeof(b1), g->mem[1]);
  snprintf(keybuf[nk], sizeof(keybuf[nk]), "Windows %s", b1);
  kc[nk++] = memcol[1];
  if (g->nparked > 0)
    {
      /* Windows parked: they take nothing, and are drawn again when seen */

      snprintf(keybuf[nk], sizeof(keybuf[nk]), "%u parked", g->nparked);
      kc[nk++] = C_AMBER;
    }

  if (g->mem[2] > 0)
    {
      fmt_px(b1, sizeof(b1), g->mem[2]);
      snprintf(keybuf[nk], sizeof(keybuf[nk]), "Images %s", b1);
      kc[nk++] = memcol[2];
    }

  if (g->nmem > 0)
    {
      snprintf(keybuf[nk], sizeof(keybuf[nk]), "%u in PSRAM", g->nmem);
      kc[nk++] = C_RED;
    }

  for (i = 0; i < nk; i++)
    {
      key[i] = keybuf[i];
    }

  field_key(m, F_GPU_MEMKEY, colx[0], c->y + 70, colw[0], nk, key, kc,
            C_CARD);

  /* The link: bytes a second, of the wire; the time the CPU waited for
   * room in the GPU (it was saturated)
   */

  if (g->link >= 1024 * 1024)
    {
      snprintf(buf, sizeof(buf), "%lu.%lu MB/s",
               (unsigned long)(g->link / (1024 * 1024)),
               (unsigned long)(g->link % (1024 * 1024) * 10 /
                               (1024 * 1024)));
    }
  else
    {
      snprintf(buf, sizeof(buf), "%lu KB/s",
               (unsigned long)((g->link + 512) / 1024));
    }

  field_text(m, F_GPU_LINK, &g_desk_font_uib, colx[1] + colw[1], c->y + 30,
             true, colw[1] - text_width(&g_desk_font_uib, "Link") - 8, buf,
             C_TEXT, C_CARD);
  snprintf(buf, sizeof(buf), "%u.%02u %% of the wire", g->wire / 100,
           g->wire % 100);
  field_text(m, F_GPU_LINKNOTE, &g_desk_font_small, colx[1], c->y + 52,
             false, colw[1], buf, C_TEXT_DIM, C_CARD);
  snprintf(buf, sizeof(buf), "waiting for the GPU %u.%u %%", g->wait / 10,
           g->wait % 10);
  field_text(m, F_GPU_WAIT, &g_desk_font_small, colx[1], c->y + 70, false,
             colw[1], buf, C_TEXT_DIM, C_CARD);

  /* The GPU's time (a v6 GPU with a clock): its blitter's share, the
   * SDRAM's by client; without, what there is not
   */

  if (g->hw)
    {
      snprintf(buf, sizeof(buf), "%u.%u %%", g->busy / 10, g->busy % 10);
    }
  else
    {
      strlcpy(buf, "-", sizeof(buf));
    }

  field_text(m, F_GPU_BUSY, &g_desk_font_uib, colx[2] + colw[2], c->y + 30,
             true, colw[2] - text_width(&g_desk_font_uib, "GPU busy") - 8,
             buf, C_TEXT, C_CARD);

  for (i = 0; i < MON_GPU_SDRAM; i++)
    {
      v[i] = g->hw ? g->sdram[i] : 0;
    }

  if (field_changed(m, F_GPU_SDBAR,
                    hash_bytes(2166136261u, v, sizeof(v)) ^
                    (uint32_t)colw[2] ^ (g->hw ? 0x8000 : 0)))
    {
      field_clear(m, F_GPU_SDBAR, C_CARD);
      d_stackbar(colx[2], c->y + 54, colw[2], GPU_BAR_H, MON_GPU_SDRAM, v,
                 sdcol, 1000);
      field_set(m, F_GPU_SDBAR, colx[2], c->y + 54, colw[2], GPU_BAR_H);
    }

  if (g->hw)
    {
      for (i = 0; i < MON_GPU_SDRAM; i++)
        {
          snprintf(keybuf[i], sizeof(keybuf[i]), "%s %u%%", sdname[i],
                   (g->sdram[i] + 5) / 10);
          key[i] = keybuf[i];
        }

      field_key(m, F_GPU_SDKEY, colx[2], c->y + 70, colw[2], MON_GPU_SDRAM,
                key, sdcol, C_CARD);
    }
  else
    {
      key[0] = g->version >= 6 ? "SDRAM: the GPU has no clock" :
                                 "SDRAM: a v6 GPU tells";
      field_key(m, F_GPU_SDKEY, colx[2], c->y + 70, colw[2], 1, key,
                &memcol[3], C_CARD);
    }

  flush();
}

/****************************************************************************
 * Private Functions: the table
 ****************************************************************************/

static void chip(const struct mon_rect_s *r, const char *label, uint16_t fg,
                 uint16_t bg)
{
  d_fill(r->x, r->y, r->w, r->h, C_CARD);
  d_rrect(r->x, r->y, r->w, r->h, LUT_R6, 6, bg);
  if (label != NULL)
    {
      ops_text_centre(&g_o, &g_desk_font_ui, r->x + r->w / 2,
                      r->y + (r->h - text_height(&g_desk_font_ui)) / 2,
                      label, fg, 255);
    }
}

static void draw_controls(struct mon_s *m)
{
  const struct mon_snap_s *s = &m->snap;
  const struct mon_rect_s *c = &m->proccard;
  const struct mon_proc_s *sel = mon_selproc(m);
  bool killable = mon_cankill(m, sel);
  char buf[64];
  int tx;

  tx = c->x + INSET + text_width(&g_desk_font_small, "PROCESSES") + 12;
  snprintf(buf, sizeof(buf), "%d threads: %u tasks, %u pthreads, %u kernel",
           s->nprocs, s->ntasks, s->npthreads, s->nkthreads);
  field_text(m, F_COUNTS, &g_desk_font_small, tx, c->y + 10, false,
             (m->chipkern.w > 0 ? m->chipkern.x : c->x + c->w - INSET) -
             12 - tx, buf, C_TEXT_DIM, C_CARD);

  if (m->chipkill.w > 0 &&
      field_changed(m, F_CHIP_KILL, 1 + killable + 2 * m->confirm))
    {
      need(24);
      if (m->confirm && killable)
        {
          chip(&m->chipkill, "Confirm end task", C_WHITE, C_DANGER);
        }
      else
        {
          chip(&m->chipkill, "End task", killable ? C_TEXT : C_FAINT,
               C_CHIP);
        }
    }

  if (m->chipival.w > 0 && field_changed(m, F_CHIP_IVAL, 1 + m->ival))
    {
      snprintf(buf, sizeof(buf), "Every %u s", g_intervals[m->ival]);
      need(16);
      chip(&m->chipival, buf, C_TEXT, C_CHIP);
    }

  if (m->chipkern.w > 0 && field_changed(m, F_CHIP_KERN, 1 + m->showkernel))
    {
      const struct mon_rect_s *r = &m->chipkern;

      need(20);
      chip(r, NULL, 0, m->showkernel ? C_CHIPON : C_CHIP);
      ops_disc(&g_o, r->x + 15, r->y + r->h / 2, LUT_R3, 3,
               m->showkernel ? C_ACCENT : C_FAINT, 255);
      ops_text(&g_o, &g_desk_font_ui, r->x + 25,
               r->y + (r->h - text_height(&g_desk_font_ui)) / 2,
               "Kernel threads", m->showkernel ? C_TEXT : C_TEXT_DIM, 255);
    }

  flush();
}

/* The header: the sort column brighter, with an arrow */

static void draw_header(struct mon_s *m)
{
  const struct mon_rect_s *h = &m->header;
  int hy = h->y + (h->h - text_height(&g_desk_font_uib)) / 2;
  int i;

  if (h->w == 0 ||
      !field_changed(m, F_HEADER, 1 + m->sortcol * 2 + m->sortdown))
    {
      return;
    }

  d_fill(h->x, h->y, h->w, h->h, C_CARD);
  d_rrect(h->x, h->y, h->w, h->h, LUT_R6, 6, C_HEADER);
  for (i = 0; i < NCOLS; i++)
    {
      const char *label = g_cols[i].label;
      int tw = text_width(&g_desk_font_uib, label);
      bool sorted = i == m->sortcol;
      int lx;
      int ax;
      int k;

      if (!m->colshown[i])
        {
          continue;
        }

      lx = g_cols[i].right ? m->colx[i] + m->colw[i] - tw : m->colx[i];
      lx += i == COL_NAME ? 21 : 0;
      d_text(&g_desk_font_uib, lx, hy, label, sorted ? C_TEXT : C_TEXT_DIM);

      if (sorted)
        {
          /* A triangle of five rows, pointing down for largest first */

          ax = g_cols[i].right ? lx - 8 : lx + tw + 8;
          for (k = 0; k < 4; k++)
            {
              int half = m->sortdown ? 3 - k : k;

              d_fill(ax - half, h->y + h->h / 2 - 2 + k, 2 * half + 1, 1,
                     C_ACCENT);
            }
        }
    }

  flush();
}

/* What a cell shows */

static void cell_of(struct mon_s *m, const struct mon_proc_s *p, int col,
                    bool selected, struct mon_cell_s *cell)
{
  memset(cell, 0, sizeof(*cell));
  cell->fg  = C_TEXT;
  cell->bar = -1;

  switch (col)
    {
      case COL_PID:
        snprintf(cell->text, sizeof(cell->text), "%d", (int)p->pid);
        cell->fg = selected ? C_SELDIM : C_TEXT_DIM;
        break;

      case COL_NAME:
        strlcpy(cell->text, p->name, sizeof(cell->text));
        cell->kind  = CELL_NAME;
        cell->badge = p->type;
        cell->fg    = p->idle ? (selected ? C_SELDIM : C_TEXT_DIM) : C_TEXT;
        break;

      case COL_STATE:
        cell->kind = CELL_STATE;
        cell->mark = C_FAINT;
        if (strncmp(p->state, "Running", 7) == 0)
          {
            cell->mark = C_GREEN;
          }
        else if (strncmp(p->state, "Ready", 5) == 0 ||
                 strncmp(p->state, "Assigned", 8) == 0)
          {
            cell->mark = C_AMBER;
          }
        else if (strncmp(p->state, "Waiting", 7) != 0)
          {
            cell->mark = C_RED;
          }

        if (strncmp(p->state, "Waiting,", 8) == 0)
          {
            snprintf(cell->text, sizeof(cell->text), "Wait: %s",
                     p->state + 8);
          }
        else
          {
            strlcpy(cell->text, p->state, sizeof(cell->text));
          }

        if (cell->mark == C_FAINT)
          {
            cell->fg = selected ? C_SELDIM : C_TEXT_DIM;
          }
        break;

      case COL_PRIO:
        snprintf(cell->text, sizeof(cell->text), "%u", p->prio);
        break;

      case COL_CORE:
        if (p->cpu >= 0)
          {
            snprintf(cell->text, sizeof(cell->text), "%d", p->cpu);
          }
        else
          {
            strlcpy(cell->text, "-", sizeof(cell->text));
          }
        break;

      case COL_LOAD:
        snprintf(cell->text, sizeof(cell->text), "%u.%u%%", p->load / 10,
                 p->load % 10);
        cell->kind = CELL_BAR;
        cell->bar  = bar_fill(36, p->load, 1000);
        cell->mark = C_ACCENT;
        cell->fg   = p->load > 0 ? C_TEXT :
                     selected ? C_SELDIM : C_TEXT_DIM;
        break;

      case COL_STACK:
        if (p->stackused > 0)
          {
            snprintf(cell->text, sizeof(cell->text), "%lu/%lu",
                     (unsigned long)p->stackused,
                     (unsigned long)p->stacksize);
            cell->kind = CELL_BAR;
            cell->bar  = bar_fill(24, p->stackused, p->stacksize);
            cell->mark = level_colour(p->stackused, p->stacksize);
          }
        else
          {
            snprintf(cell->text, sizeof(cell->text), "%lu",
                     (unsigned long)p->stacksize);
          }
        break;

      case COL_HEAP:
        if (p->heap >= 0)
          {
            fmt_bytes(cell->text, sizeof(cell->text), (uint64_t)p->heap);
          }
        else
          {
            strlcpy(cell->text, "-", sizeof(cell->text));
          }
        break;
    }
}

static uint32_t cell_hash(const struct mon_cell_s *cell)
{
  uint32_t h = hash_bytes(2166136261u, cell->text, strlen(cell->text));

  h = hash_bytes(h, &cell->fg, sizeof(cell->fg));
  h = hash_bytes(h, &cell->mark, sizeof(cell->mark));
  h = hash_bytes(h, &cell->kind, sizeof(cell->kind));
  h = hash_bytes(h, &cell->badge, sizeof(cell->badge));
  h = hash_bytes(h, &cell->bar, sizeof(cell->bar));
  return h != 0 ? h : 1;
}

/* A row's background: the stripe of odd rows, the selection */

static void row_bg(struct mon_s *m, int y, uint8_t style, int x0, int x1)
{
  const struct mon_rect_s *r = &m->rows;

  d_fill(x0, y, x1 - x0, ROW_H, (style & ROW_ODD) != 0 && style != ROW_EMPTY ?
         C_ROWALT : C_CARD);
  if (style != ROW_EMPTY && (style & ROW_SELECTED) != 0)
    {
      d_rrect_part(r->x, y, r->w, ROW_H, LUT_R6, 6, C_SELECT, x0, x1 - x0);
    }
}

static void cell_draw(struct mon_s *m, const struct mon_cell_s *cell,
                      int col, int y)
{
  static const uint16_t badges[3] =
  {
    RGB(0x2f, 0x5f, 0xa8), RGB(0x1f, 0x7a, 0x6a), RGB(0x55, 0x55, 0x5c)
  };

  const struct desk_font_s *f = &g_desk_font_ui;
  int x = m->colx[col];
  int w = m->colw[col];
  int ty = y + (ROW_H - text_height(f)) / 2;
  int my = y + ROW_H / 2;
  char cut[32];
  int tw;

  switch (cell->kind)
    {
      case CELL_NAME:
        d_rrect(x, my - 7, 15, 14, LUT_R3, 3, badges[cell->badge % 3]);
        need(1);
        ops_text_centre(&g_o, &g_desk_font_small, x + 8,
                        my - text_height(&g_desk_font_small) / 2,
                        cell->badge == T_TASK ? "T" :
                        cell->badge == T_PTHREAD ? "P" : "K", C_WHITE, 255);
        x += 21;
        w -= 21;
        break;

      case CELL_STATE:
        need(1);
        ops_disc(&g_o, x + 3, my, LUT_R3, 3, cell->mark, 255);
        x += 12;
        w -= 12;
        break;

      case CELL_BAR:
        d_bar(x, my - BAR_H / 2, col == COL_LOAD ? 36 : 24, cell->bar,
              cell->mark);
        break;

      default:
        break;
    }

  tw = fit_text(f, cell->text, w, cut, sizeof(cut));
  if (g_cols[col].right)
    {
      x += w - tw;
    }

  d_text(f, x, ty, cut, cell->fg);
}

/* The rows: moved with a COPY when the table has scrolled, then each cell
 * whose contents changed drawn again (a whole row if its background
 * changed)
 */

static void draw_rows(struct mon_s *m)
{
  const struct mon_rect_s *r = &m->rows;
  int nvis = m->visrows < MON_MAXPROCS ? m->visrows : MON_MAXPROCS;
  int i;
  int c;

  if (m->visrows <= 0)
    {
      return;
    }

  if (m->force)
    {
      for (i = 0; i < MON_MAXPROCS; i++)
        {
          m->rowc[i].style = ROW_INVALID;
        }
    }
  else if (m->scroll != m->drawnscroll)
    {
      int d = m->scroll - m->drawnscroll;
      int n = nvis - (d > 0 ? d : -d);

      if (n > 0)
        {
          if (d > 0)
            {
              d_copy(r->x, r->y + d * ROW_H, r->x, r->y, r->w, n * ROW_H);
              memmove(&m->rowc[0], &m->rowc[d], n * sizeof(m->rowc[0]));
              for (i = n; i < nvis; i++)
                {
                  m->rowc[i].style = ROW_INVALID;
                }
            }
          else
            {
              d_copy(r->x, r->y, r->x, r->y - d * ROW_H, r->w, n * ROW_H);
              memmove(&m->rowc[-d], &m->rowc[0], n * sizeof(m->rowc[0]));
              for (i = 0; i < -d; i++)
                {
                  m->rowc[i].style = ROW_INVALID;
                }
            }
        }
      else
        {
          for (i = 0; i < nvis; i++)
            {
              m->rowc[i].style = ROW_INVALID;
            }
        }
    }

  m->drawnscroll = m->scroll;

  for (i = 0; i < nvis; i++)
    {
      struct mon_row_s *rc = &m->rowc[i];
      int idx = m->scroll + i;
      int y = r->y + i * ROW_H;
      const struct mon_proc_s *p;
      bool whole;
      uint8_t style;

      if (idx >= m->nrows)
        {
          if (rc->style != ROW_EMPTY)
            {
              if (!m->force)
                {
                  row_bg(m, y, ROW_EMPTY, r->x, r->x + r->w);
                }

              rc->style = ROW_EMPTY;
            }

          continue;
        }

      p     = &m->snap.procs[m->order[idx]];
      style = (idx & 1 ? ROW_ODD : 0) |
              (p->pid == m->selected ? ROW_SELECTED : 0);
      whole = rc->style != style;
      if (whole)
        {
          if (!m->force || style != 0)
            {
              row_bg(m, y, style, r->x, r->x + r->w);
            }

          rc->style = style;
        }

      rc->pid = p->pid;
      for (c = 0; c < NCOLS; c++)
        {
          struct mon_cell_s cell;
          uint32_t h;

          if (!m->colshown[c])
            {
              continue;
            }

          cell_of(m, p, c, (style & ROW_SELECTED) != 0, &cell);
          h = cell_hash(&cell);
          if (whole || h != rc->hash[c])
            {
              if (!whole)
                {
                  row_bg(m, y, style, m->colx0[c], m->colx1[c]);
                }

              cell_draw(m, &cell, c, y);
              rc->hash[c] = h;
            }
        }
    }

  /* Rows past the cache (a very tall window) are only ever empty */

  if (m->force && m->visrows > nvis)
    {
      d_fill(r->x, r->y + nvis * ROW_H, r->w, (m->visrows - nvis) * ROW_H,
             C_CARD);
    }

  flush();
}

static void draw_scrollbar(struct mon_s *m)
{
  const struct mon_rect_s *s = &m->sbar;
  int ty = 0;
  int th = 0;
  bool shown;

  if (s->h == 0)
    {
      return;
    }

  shown = mon_thumb(m, &ty, &th);
  if (!field_changed(m, F_THUMB, shown ? (uint32_t)ty << 16 | th << 1 |
                                         m->dragging : 1))
    {
      return;
    }

  d_fill(s->x, s->y, s->w, s->h, C_CARD);
  if (shown)
    {
      d_rrect(s->x, s->y, s->w, s->h, LUT_R3, 3, C_GRAPH);
      d_rrect(s->x, ty, s->w, th, LUT_R3, 3,
              m->dragging ? C_TEXT_DIM : C_FAINT);
    }

  flush();
}

/* The footer: the selected thread in full, or what can be done */

static void draw_footer(struct mon_s *m)
{
  static const char * const types[3] =
  {
    "task", "pthread", "kernel thread"
  };

  const struct mon_proc_s *p = mon_selproc(m);
  const struct mon_rect_s *f = &m->footer;
  char buf[160];
  char heap[24];

  if (p != NULL)
    {
      heap[0] = '\0';
      if (p->heap >= 0)
        {
          strlcpy(heap, ", heap ", sizeof(heap));
          fmt_bytes(heap + 7, sizeof(heap) - 7, (uint64_t)p->heap);
        }

      snprintf(buf, sizeof(buf), "%s (PID %d): %s of process %d, "
               "priority %u, stack %lu of %lu bytes (%u%%)%s",
               p->name, (int)p->pid, types[p->type % 3], (int)p->group,
               p->prio, (unsigned long)p->stackused,
               (unsigned long)p->stacksize,
               p->stacksize > 0 ? (unsigned int)((uint64_t)p->stackused *
                                                 100 / p->stacksize) : 0,
               heap);
    }
  else
    {
      strlcpy(buf, "Click a column to sort.  Up/Down, PgUp/PgDn select, "
                   "Delete ends the selected task, K hides kernel threads.",
              sizeof(buf));
    }

  field_text(m, F_FOOTER, &g_desk_font_small, PAD + 4,
             f->y + (f->h - text_height(&g_desk_font_small)) / 2 - 1, false,
             f->w - 2 * PAD - 8, buf, C_TEXT_DIM, C_WINDOW_BG);
  flush();
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

/* Draw what changed (everything if m->force) */

static void mon_paint(struct desk_win_s *w)
{
  struct mon_s *m = w->priv;

  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(&g_o);
  draw_cpu(m);
  draw_memory(m);
  draw_system(m);
  draw_gpu(m);
  if (m->proccard.w > 0 && m->proccard.h >= 80)
    {
      draw_controls(m);
      draw_header(m);
      draw_rows(m);
      draw_scrollbar(m);
    }

  draw_footer(m);
}

/* Everything, for the client area now */

static void mon_draw(struct desk_win_s *w)
{
  struct mon_s *m = w->priv;
#if MON_STATS
  uint32_t t0 = desk_ms();
#endif

  if (m == NULL)
    {
      return;
    }

  if (m->lw != w->csize.w || m->lh != w->csize.h ||
      m->laidheap != m->snap.haveheap)
    {
      mon_layout(w, m);
    }

  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(&g_o);
  d_fill(0, TITLE_H, w->csize.w, w->csize.h, C_WINDOW_BG);
  card_bg(&m->cpucard, "CPU");
  card_bg(&m->memcard, "MEMORY");
  card_bg(&m->syscard, "SYSTEM");
  card_bg(&m->gpucard, "GPU");
  card_bg(&m->proccard, "PROCESSES");
  flush();

  memset(m->f, 0, sizeof(m->f));
  m->gcpu.drawn = 0;
  m->gmem.drawn = 0;
  m->force = true;
  mon_paint(w);
  m->force = false;
  m->drawnw = w->csize.w;
  m->drawnh = w->csize.h;

#if MON_STATS
  syslog(LOG_INFO, "sysmon: full %lu ms, %d ops, %d bytes, %d calls\n",
         (unsigned long)(desk_ms() - t0), g_stats.ops, g_stats.bytes,
         g_stats.calls);
  memset(&g_stats, 0, sizeof(g_stats));
#endif
}

/* Draw what changed, unless the layout is out of date (being resized) */

static void mon_update(struct desk_win_s *w)
{
  struct mon_s *m = w->priv;

  if (m->laidheap != m->snap.haveheap)
    {
      mon_draw(w);
    }
  else if (m->lw == w->csize.w && m->lh == w->csize.h &&
           m->drawnw == w->csize.w && m->drawnh == w->csize.h)
    {
      mon_paint(w);
    }
}

static void mon_resize(struct desk_win_s *w)
{
  mon_draw(w);
}

/* New territory of a window that grew: while it is being resized, only
 * the background; else everything if it has not been drawn at this size
 */

static void mon_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  if (g_desk.resize == w)
    {
      g_hwnd = (NXWINDOW)w->hwnd;
      ops_reset(&g_o);
      d_fill(x, y + TITLE_H, cw, ch, C_WINDOW_BG);
      flush();
    }
  else
    {
      /* Its pixels are gone (moved to memory, say): all of it again */

      mon_draw(w);
    }
}

static void mon_tick(struct desk_win_s *w)
{
  struct mon_s *m = w->priv;
#if MON_STATS
  struct timespec t0;
  struct timespec t1;
  struct timespec t2;
#endif

  if (++m->ticks < g_intervals[m->ival])
    {
      return;
    }

  m->ticks = 0;

#if MON_STATS
  clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
  mon_sample(m);
  mon_sort(m);
#if MON_STATS
  clock_gettime(CLOCK_MONOTONIC, &t1);
#endif
  mon_update(w);
#if MON_STATS
  clock_gettime(CLOCK_MONOTONIC, &t2);
  syslog(LOG_INFO, "sysmon: sample %ld us, draw %ld us, %d ops, "
         "%d bytes, %d calls\n",
         (long)((t1.tv_sec - t0.tv_sec) * 1000000 +
                (t1.tv_nsec - t0.tv_nsec) / 1000),
         (long)((t2.tv_sec - t1.tv_sec) * 1000000 +
                (t2.tv_nsec - t1.tv_nsec) / 1000),
         g_stats.ops, g_stats.bytes, g_stats.calls);
  memset(&g_stats, 0, sizeof(g_stats));
#endif
}

static void mon_kill(struct desk_win_s *w)
{
  struct mon_s *m = w->priv;
  const struct mon_proc_s *p = mon_selproc(m);

  m->confirm = false;
  if (p != NULL && mon_cankill(m, p) && task_delete(p->pid) < 0)
    {
      fprintf(stderr, "sysmon: task_delete(%d): %d\n", (int)p->pid, errno);
    }

  mon_sample(m);
  mon_sort(m);
}

static void mon_scrollto(struct mon_s *m, int y)
{
  int ty;
  int th;

  if (mon_thumb(m, &ty, &th))
    {
      int travel = m->sbar.h - th;
      int range = m->nrows - m->visrows;
      int top = y - m->dragoff - m->sbar.y;

      m->scroll = travel > 0 ? (top * range + travel / 2) / travel : 0;
      mon_clampscroll(m);
    }
}

static void mon_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct mon_s *m = w->priv;
  uint8_t pressed = buttons & ~m->buttons;
  int ty;
  int th;
  int i;

  m->buttons = buttons;
  y += TITLE_H;

  if (m->dragging)
    {
      if ((buttons & NX_MOUSE_LEFTBUTTON) == 0)
        {
          m->dragging = false;
        }
      else
        {
          mon_scrollto(m, y);
        }

      mon_update(w);
      return;
    }

  if ((pressed & NX_MOUSE_LEFTBUTTON) == 0)
    {
      return;
    }

  /* A click anywhere but on the button takes back "Confirm end task" */

  if (m->confirm && !rect_in(&m->chipkill, x, y))
    {
      m->confirm = false;
    }

  if (rect_in(&m->chipkill, x, y))
    {
      if (mon_cankill(m, mon_selproc(m)))
        {
          if (m->confirm)
            {
              mon_kill(w);
            }
          else
            {
              m->confirm = true;
            }
        }
    }
  else if (rect_in(&m->chipival, x, y))
    {
      m->ival = (m->ival + 1) % NINTERVALS;
      m->ticks = 0;
    }
  else if (rect_in(&m->chipkern, x, y))
    {
      m->showkernel = !m->showkernel;
      mon_sort(m);
    }
  else if (rect_in(&m->header, x, y))
    {
      for (i = 0; i < NCOLS; i++)
        {
          if (m->colshown[i] && x >= m->colx0[i] && x < m->colx1[i])
            {
              if (i == m->sortcol)
                {
                  m->sortdown = !m->sortdown;
                }
              else
                {
                  m->sortcol  = i;
                  m->sortdown = g_cols[i].largest;
                }

              mon_sort(m);
              break;
            }
        }
    }
  else if (m->sbar.h > 0 && x >= m->sbar.x - 4 &&
           x < m->sbar.x + m->sbar.w + 6 && y >= m->sbar.y &&
           y < m->sbar.y + m->sbar.h)
    {
      if (mon_thumb(m, &ty, &th))
        {
          if (y >= ty && y < ty + th)
            {
              m->dragging = true;
              m->dragoff  = y - ty;
            }
          else
            {
              m->scroll += y < ty ? -m->visrows : m->visrows;
              mon_clampscroll(m);
            }
        }
    }
  else if (rect_in(&m->rows, x, y))
    {
      int row = m->scroll + (y - m->rows.y) / ROW_H;

      m->selected = row < m->nrows ? m->snap.procs[m->order[row]].pid : -1;
    }

  mon_update(w);
}

/* Keys: the arrows, PgUp/PgDn, Home/End move the selection; Delete ends
 * the selected task (asking first); K shows or hides kernel threads
 */

static void mon_key1(struct mon_s *m, struct desk_win_s *w, uint8_t ch,
                     bool last)
{
  if (m->esc == 1)
    {
      m->esc      = ch == '[' ? 2 : ch == 'O' ? 3 : 0;
      m->escparam = 0;
      return;
    }

  if (m->esc == 3)
    {
      m->esc = 0;
      switch (ch)
        {
          case 'H':
            mon_movesel(m, -MON_MAXPROCS);
            break;

          case 'F':
            mon_movesel(m, MON_MAXPROCS);
            break;
        }

      return;
    }

  if (m->esc == 2)
    {
      if (ch >= '0' && ch <= '9')
        {
          m->escparam = (uint8_t)(m->escparam * 10 + (ch - '0'));
          return;
        }

      m->esc = 0;
      switch (ch)
        {
          case 'A':
            mon_movesel(m, -1);
            break;

          case 'B':
            mon_movesel(m, 1);
            break;

          case 'H':
            mon_movesel(m, -MON_MAXPROCS);
            break;

          case 'F':
            mon_movesel(m, MON_MAXPROCS);
            break;

          case '~':
            switch (m->escparam)
              {
                case 1:
                  mon_movesel(m, -MON_MAXPROCS);
                  break;

                case 4:
                  mon_movesel(m, MON_MAXPROCS);
                  break;

                case 5:
                  mon_movesel(m, -(m->visrows > 1 ? m->visrows - 1 : 1));
                  break;

                case 6:
                  mon_movesel(m, m->visrows > 1 ? m->visrows - 1 : 1);
                  break;

                case 3:                         /* Delete */
                  if (mon_cankill(m, mon_selproc(m)))
                    {
                      if (m->confirm)
                        {
                          mon_kill(w);
                        }
                      else
                        {
                          m->confirm = true;
                        }
                    }
                  break;
              }
            break;
        }

      return;
    }

  switch (ch)
    {
      case 0x1b:

        /* Escape on its own takes back "Confirm end task"; one that
         * starts a sequence (Delete: ESC [ 3 ~) does not
         */

        m->esc = 1;
        if (last)
          {
            m->confirm = false;
          }
        break;

      case 'k':
      case 'K':
        m->showkernel = !m->showkernel;
        mon_sort(m);
        break;
    }
}

static void mon_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct mon_s *m = w->priv;
  int i;

  for (i = 0; i < n; i++)
    {
      mon_key1(m, w, ch[i], i == n - 1);
    }

  mon_update(w);
}

static struct desk_win_s *mon_open(void)
{
  struct desk_win_s *w;
  struct utsname info;
  struct mon_s *m;

  m = calloc(1, sizeof(*m));
  if (m == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_MONITOR, "System Monitor", 860, 600);
  if (w == NULL)
    {
      free(m);
      return NULL;
    }

  w->priv       = m;
  m->haveheap   = MON_HEAPFILES ? -1 : 0;
  m->self       = getpid();
  m->sortcol    = COL_LOAD;
  m->sortdown   = true;
  m->showkernel = true;
  m->selected   = -1;
  m->lw         = -1;
  if (uname(&info) == 0)
    {
      snprintf(m->osname, sizeof(m->osname), "%s %s", info.sysname,
               info.release);
    }

  mon_sample(m);
  mon_sort(m);
  mon_draw(w);
  return w;
}

#endif /* !DESK_PROGRAM_STUB */

/* The icon: bars on a teal tile (few operations: every icon shares the
 * dock's and the launcher's display lists)
 */

static void mon_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  static const uint8_t bars[4] =
  {
    40, 70, 55, 85
  };

  int bw = s / 9;
  int i;

  icon_tile(o, x, y, s, RGB(0x22, 0xd0, 0xb0), RGB(0x08, 0x76, 0x88), a);
  for (i = 0; i < 4; i++)
    {
      int h = (s * 6 / 10) * bars[i] / 100;
      int bx = x + s / 5 + i * (bw + bw / 2 + 1);

      ops_blend(o, bx, y + s * 4 / 5 - h, bw, h, C_WHITE,
                (uint8_t)(a * 235 / 255));
    }

  ops_blend(o, x + s / 6, y + s * 4 / 5, s * 2 / 3, 1, C_WHITE,
            (uint8_t)(a * 150 / 255));
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef DESK_PROGRAM_STUB
DESK_PROGRAM_APP(g_app_monitor, "System Monitor", mon_icon, "monitor", true);
#else
const struct desk_app_s g_app_monitor =
{
  .name   = "System Monitor",
  .icon   = mon_icon,
  .open   = mon_open,
  .draw   = mon_draw,
  .expose = mon_expose,
  .resize = mon_resize,
  .tick   = mon_tick,
  .mouse  = mon_mouse,
  .key    = mon_key,
  .single = true,
};
#endif
