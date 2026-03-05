/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2017
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

  Null clock driver for operation with no clock control.
  */

#include "config.h"

#include "sysincl.h"

#include "sys_null.h"

#include "clocks_priv.h"
#include "logging.h"
#include "memory.h"
#include "util.h"

/* Minimum interval between updates when frequency is constant */
#define MIN_UPDATE_INTERVAL 1000.0

struct null_context {
  /* Tracked clock */
  CLK_Instance clock;

  /* Current frequency offset of the system clock (in ppm) */
  double freq;

  /* Offset of the system clock at the last update */
  double offset_register;

  /* Time of the last update */
  struct timespec last_update;
};

/* ================================================== */

static void
update_offset(struct null_context *nc)
{
  struct timespec now;
  double duration;

  CLK_ReadTime(nc->clock, &now);
  duration = UTI_DiffTimespecsToDouble(&now, &nc->last_update);
  nc->offset_register += 1.0e-6 * nc->freq * duration;
  nc->last_update = now;

  DEBUG_LOG("%s offset=%e freq=%f", CLK_GetName(nc->clock), nc->offset_register, nc->freq);
}

/* ================================================== */

static double
read_frequency(clk_DriverContext *context)
{
  struct null_context *nc = context;

  return nc->freq;
}

/* ================================================== */

static double
set_frequency(clk_DriverContext *context, double freq_ppm)
{
  struct null_context *nc = context;

  update_offset(nc);
  nc->freq = freq_ppm;

  return nc->freq;
}

/* ================================================== */

static void
accumulate_offset(clk_DriverContext *context, double offset, double corr_rate)
{
  struct null_context *nc = context;

  nc->offset_register += offset;
}

/* ================================================== */

static int
apply_step_offset(clk_DriverContext *context, double offset)
{
  return 0;
}

/* ================================================== */

static double
get_offset(clk_DriverContext *context, int reverse, struct timespec *ts, double *err)
{
  struct null_context *nc = context;
  double duration, offset;

  duration = UTI_DiffTimespecsToDouble(ts, &nc->last_update);

  if (!reverse && duration > MIN_UPDATE_INTERVAL) {
    update_offset(nc);
    duration = 0.0;
  }

  if (err)
    *err = 0.0;

  offset = -1.0e-6 * nc->freq * duration - nc->offset_register;
  if (reverse)
    offset = offset / (1.0 - 1.0e-6 * nc->freq);

#if 0
  if (!reverse) {
    struct timespec cooked;
    UTI_AddDoubleToTimespec(ts, offset, &cooked);

    DEBUG_LOG("raw error: %+.9f", offset - get_offset(context, 1, &cooked, NULL));
  }
#endif

  return offset;
}

/* ================================================== */

void
SYS_Null_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock)
{
  struct null_context *nc;

  nc = MallocNew(struct null_context);

  nc->clock = clock;
  nc->freq = 0.0;
  nc->offset_register = 0.0;
  UTI_ZeroTimespec(&nc->last_update);

  driver->context = nc;
  driver->read_frequency = read_frequency;
  driver->set_frequency = set_frequency;
  driver->accumulate_offset = accumulate_offset;
  driver->get_offset = get_offset;
  driver->apply_step_offset = apply_step_offset;

  LOG(LOGS_INFO, "Disabled control of %s", CLK_GetName(clock));
}

/* ================================================== */

void
SYS_Null_FinaliseClockDriver(struct clk_Driver *driver)
{
  Free(driver->context);
}
