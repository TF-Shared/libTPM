/*
 * Copyright (c) 2025, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Temporary header file to provide prototypes until functional logging
 * is implemented.
 */

#ifndef DEBUG_H
#define DEBUG_H

void ERROR(const char *fmt, ...);
void NOTICE(const char *fmt, ...);
void WARNING(const char *fmt, ...);
void INFO(const char *fmt, ...);
void VERBOSE(const char *fmt, ...);

#endif /* DEBUG_H */
