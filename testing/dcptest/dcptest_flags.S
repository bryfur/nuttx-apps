/****************************************************************************
 * apps/testing/dcptest/dcptest_flags.S
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

	.syntax	unified
	.thumb
	.text

/* uint32_t dcpt_callflags(void *fn, double a, double b): calls a
 * flag-setting comparison (__aeabi_cdcmple and its kind); returns its
 * NZCV in bits 31:28 and bit 0 set if it changed any of r0-r3
 */

	.global	dcpt_callflags
	.type	dcpt_callflags, %function
	.thumb_func
dcpt_callflags:
	push	{r4-r8, lr}
	mov	r4, r0
	mov	r0, r2
	mov	r1, r3
	ldrd	r2, r3, [sp, #24]
	mov	r5, r0
	mov	r6, r1
	mov	r7, r2
	mov	r8, r3
	blx	r4
	mrs	r12, apsr
	and	r12, r12, #0xf0000000
	cmp	r0, r5
	it	ne
	orrne	r12, r12, #1
	cmp	r1, r6
	it	ne
	orrne	r12, r12, #1
	cmp	r2, r7
	it	ne
	orrne	r12, r12, #1
	cmp	r3, r8
	it	ne
	orrne	r12, r12, #1
	mov	r0, r12
	pop	{r4-r8, pc}

/* double dcpt_null2(double a, double b): a (the harness's own cost) */

	.global	dcpt_null2
	.type	dcpt_null2, %function
	.thumb_func
dcpt_null2:
	bx	lr
