/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  The header file for the linux driver
  */

#ifndef GOT_SYS_LINUX_H
#define GOT_SYS_LINUX_H

#include "clocks.h"
#include "sys.h"

extern int SYS_Linux_OpenClock(const char *name, int *control,
                               clockid_t *id, int *fd, int *is_system);

extern int SYS_Linux_GetClockResolution(CLK_Instance clock);

extern int SYS_Linux_GetClockReadings(CLK_Instance clock1, CLK_Instance clock2,
                                      int max_readings, int *mode, struct timespec tss[][3]);

extern void SYS_Linux_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock);

extern void SYS_Linux_FinaliseClockDriver(struct clk_Driver *driver);

extern void SYS_Linux_DropRoot(uid_t uid, gid_t gid, SYS_ProcessContext context);

extern void SYS_Linux_EnableSystemCallFilter(int level, SYS_ProcessContext context);

extern int SYS_Linux_CheckKernelVersion(int req_major, int req_minor);

extern int SYS_Linux_SetPHCExtTimestamping(int fd, int pin, int channel,
                                           int rising, int falling, int enable);

extern int SYS_Linux_ReadPHCExtTimestamp(int fd, struct timespec *phc_ts, int *channel);

#endif  /* GOT_SYS_LINUX_H */
