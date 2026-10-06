/****************************************************************************
 * apps/interpreters/micropython/modagent.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The _agent module (docs/agent.md): the C half of the agent, what is
 * slow or impossible in MicroPython.  modagent.c (the module, the
 * terminal, text helpers), agent_http.c (HTTP/1.1 and server-sent events
 * over the network coprocessor, TLS on it), agent_proc.c (NSH commands
 * with their output captured), agent_search.c (grep, glob).
 *
 * Nothing is kept in static variables: several interpreters may run at
 * once (mphalport.c), each with its own objects.
 ****************************************************************************/

#ifndef __APPS_INTERPRETERS_MICROPYTHON_MODAGENT_H
#define __APPS_INTERPRETERS_MICROPYTHON_MODAGENT_H

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

/* Milliseconds, monotonic */

uint32_t agent_ms(void);

/* Wait up to ms (-1: for ever) for events on fd, watching the terminal:
 * Ctrl-C (and Esc while _agent.interrupt_keys(True)) raises
 * KeyboardInterrupt.  The revents, 0 when the time is up.
 */

int agent_wait_fd(int fd, short events, int ms);

/* Now and then (every 20 ms at most): the terminal looked at, a pending
 * KeyboardInterrupt raised.  For long loops in C.
 */

void agent_check_interrupt(void);

/* OSError with a message */

NORETURN void agent_raise(int err, const char *fmt, ...);

/* The types and functions of the other files, in the module's table */

extern const mp_obj_type_t agent_http_type;
extern const mp_obj_type_t agent_proc_type;

MP_DECLARE_CONST_FUN_OBJ_KW(agent_grep_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(agent_glob_obj);
MP_DECLARE_CONST_FUN_OBJ_2(agent_fnmatch_obj);
MP_DECLARE_CONST_FUN_OBJ_0(agent_netstate_obj);

#endif /* __APPS_INTERPRETERS_MICROPYTHON_MODAGENT_H */
