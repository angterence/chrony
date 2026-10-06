/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2015
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

  Header file for a driver based on the adjtimex()/ntp_adjtime() function
  */

#ifndef GOT_SYS_TIMEX_H
#define GOT_SYS_TIMEX_H

#include "clocks.h"

extern void SYS_Timex_Initialise(void);

/* Initialise with some driver functions replaced with special versions */
extern void SYS_Timex_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock,
                                            double max_set_freq_ppm, double max_set_freq_delay,
                                            double min_fastslew_offset, double max_fastslew_rate);

extern void SYS_Timex_FinaliseClockDriver(struct clk_Driver *driver);

/* Wrapper for adjtimex()/ntp_adjtime()/clock_adjtime() */
extern int SYS_Timex_Adjust(clk_DriverContext *context, struct timex *txc, int ignore_error);

/* Wrapper for read-only adjtimex()/ntp_adjtime()/clock_adjtime() */
extern void SYS_Timex_Read(clockid_t clock_id, struct timex *txc);

#endif  /* GOT_SYS_GENERIC_H */
