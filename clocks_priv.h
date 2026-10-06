/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
 * Copyright (C) Miroslav Lichvar  2026
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

  Private include file for clocks.c and system dependent
  driver modules.
  */

#ifndef GOT_CLOCKS_PRIV_H
#define GOT_CLOCKS_PRIV_H

#include "clocks.h"

/* System driver to read the current local frequency, in ppm relative
   to nominal.  A positive value indicates that the local clock runs
   fast when uncompensated. */
typedef double (*clk_ReadFrequency)(clk_DriverContext *context);

/* System driver to set the current local frequency, in ppm relative
   to nominal.  A positive value indicates that the local clock runs
   fast when uncompensated.  Return actual frequency (may be different
   from the requested frequency due to clamping or rounding). */
typedef double (*clk_SetFrequency)(clk_DriverContext *context, double freq_ppm);

/* System driver to accrue an offset. A positive argument means slew
   the clock forwards.  The suggested correction rate of time to correct the
   offset is given in 'corr_rate'. */
typedef void (*clk_AccumulateOffset)(clk_DriverContext *context, double offset, double corr_rate);

/* System driver to convert a raw time to an adjusted (cooked) time.
   The number of seconds returned in 'corr' have to be added to the
   raw time to get the corrected time */
typedef double (*clk_GetOffset)(clk_DriverContext *context, int reverse,
                                struct timespec *ts, double *err);

/* System driver to apply a step offset. A positive argument means step
   the clock forwards. */
typedef int (*clk_ApplyStepOffset)(clk_DriverContext *context, double offset);

/* System driver to schedule leap seconds and set TAI-UTC offset */
typedef void (*clk_SetLeap)(clk_DriverContext *context, int leap, int tai_offset);

/* System driver to set the synchronisation status */
typedef void (*clk_SetSyncStatus)(clk_DriverContext *context, int synchronised,
                                  double est_error, double max_error);

/* Structure containing the whole set of drivers and their contexts to be
   passed to the system-specific initialisation functions */
struct clk_Driver {
  clk_DriverContext *context;

  clk_ReadFrequency read_frequency;
  clk_SetFrequency set_frequency;

  clk_AccumulateOffset accumulate_offset;
  clk_GetOffset get_offset;

  clk_ApplyStepOffset apply_step_offset;

  clk_SetLeap set_leap;

  clk_SetSyncStatus set_sync_status;
};

#endif /* GOT_CLOCKS_PRIV_H */
