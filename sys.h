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

  This is the header for the file that links in the operating system-
  specific parts of the software

*/

#ifndef GOT_SYS_H
#define GOT_SYS_H

#include "clocks.h"

/* Open a system-specific clock (control is an in-out parameter) */
extern int SYS_OpenClock(const char *name, int *control, clockid_t *id, int *fd, int *is_system);

/* Get resolution of a clock in nanoseconds (also for system clocks
   not returned by SYS_OpenClock()), 0 indicates unknown */
extern int SYS_GetClockResolution(CLK_Instance clock);

/* Read time from two clocks repeatedly, providing triplets of timestamps
   from (clock1, clock2, clock1) (mode is an in-out parameter) */
extern int SYS_GetClockReadings(CLK_Instance clock1, CLK_Instance clock2,
                                int max_readings, int *mode, struct timespec tss[][3]);

/* Called at the start of the run to do initialisation */
extern void SYS_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock,
                                      int control);

/* Called at the end of the run to do final clean-up */
extern void SYS_FinaliseClockDriver(struct clk_Driver *driver, int control);

typedef enum {
  SYS_MAIN_PROCESS,
  SYS_NTSKE_HELPER,
  SYS_PRIVOPS_HELPER,
} SYS_ProcessContext;

/* Switch to the specified user and group in given context */
extern void SYS_DropRoot(uid_t uid, gid_t gid, SYS_ProcessContext context);

/* Enable a system call filter to allow only system calls
   which chronyd normally needs after initialization */
extern void SYS_EnableSystemCallFilter(int level, SYS_ProcessContext context);

extern void SYS_SetScheduler(int SchedPriority);
extern void SYS_LockMemory(void);

#endif /* GOT_SYS_H */
