/*
 * logic_detect.h - automatic logic IC detection
 *
 * This file is a part of Minipro.
 *
 * Minipro is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Minipro is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __LOGIC_DETECT_H
#define __LOGIC_DETECT_H

#include "minipro.h"

/* Probe the ZIF socket and try to identify an unknown logic IC
 * (74xx/40xx families) using the test vectors from logicic.xml. */
int logic_ic_detect(minipro_handle_t *handle);

#endif
