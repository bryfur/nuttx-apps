/****************************************************************************
 * apps/testing/strbench/strcheck.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* The memory and string functions against byte-at-a-time references:
 * shared by the board's strbench (the firmware's functions) and
 * tests/string (NuttX's ARMv8-M assembly under qemu-arm).
 */

#ifndef __APPS_TESTING_STRBENCH_STRCHECK_H
#define __APPS_TESTING_STRBENCH_STRCHECK_H

#include <stddef.h>

struct strcheck_fns
{
  void *(*memset)(void *d, int c, size_t n);
  void *(*memcpy)(void *d, const void *s, size_t n);
  void *(*memmove)(void *d, const void *s, size_t n);
  void *(*memchr)(const void *s, int c, size_t n);
  size_t (*strlen)(const char *s);
  int (*strcmp)(const char *a, const char *b);
  char *(*strcpy)(char *d, const char *s);
};

/* Run every check (a NULL function is skipped); print each failure (the
 * first few of each function) and a line a function with its case count.
 * Returns the number of failures.
 */

int strcheck(const struct strcheck_fns *f);

#endif /* __APPS_TESTING_STRBENCH_STRCHECK_H */
