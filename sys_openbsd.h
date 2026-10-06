/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Shaun Ren  2021
 * Copyright (C) Thomas Kupper  2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 **********************************************************************

  =======================================================================

  Header file for OpenBSD driver
  */

#ifndef GOT_SYS_OPENBSD_H
#define GOT_SYS_OPENBSD_H

#include "clocks.h"
#include "sys.h"

void SYS_OpenBSD_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock);

void SYS_OpenBSD_FinaliseClockDriver(struct clk_Driver *driver);

void SYS_OpenBSD_DropRoot(uid_t uid, gid_t gid, SYS_ProcessContext context);

void SYS_OpenBSD_EnableSystemCallFilter(int level, SYS_ProcessContext context);

#endif
