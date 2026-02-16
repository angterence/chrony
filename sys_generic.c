/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2014-2015
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

  Generic driver functions to complete system-specific drivers
  */

#include "config.h"

#include "sysincl.h"

#include "sys_generic.h"

#include "clocks_priv.h"
#include "conf.h"
#include "logging.h"
#include "memory.h"
#include "privops.h"
#include "sched.h"
#include "util.h"

/* ================================================== */

/* Minimum offset to correct */
#define MIN_OFFSET_CORRECTION 1.0e-9

/* Limits for the slew length */
#define MIN_SLEW_DURATION 1.0
#define MAX_SLEW_DURATION 1.0e4

/* Maximum accepted excess duration to ignore large jumps after resuming
   suspended system and other reasons (which should be handled in the
   scheduler), a constant to determine the minimum slew duration to avoid
   oscillations due to the excess, and the decay constant */
#define MAX_SLEW_EXCESS_DURATION 100.0
#define MIN_SLEW_DURATION_EXCESS_RATIO 5.0
#define SLEW_EXCESS_DURATION_DECAY 0.9

struct generic_context {
  /* Controlled clock */
  CLK_Instance clock;

  /* Lower clock driver */
  struct clk_Driver drv;

  /* Current frequency as requested by the local module (in ppm) */
  double base_freq;

  /* Maximum frequency that can be set by drv_set_freq (in ppm) */
  double max_freq;

  /* Maximum expected delay in the actual frequency change (e.g. kernel ticks)
     in local time */
  double max_freq_change_delay;

  /* Maximum allowed frequency offset relative to the base frequency */
  double max_corr_freq;

  /* Amount of outstanding offset to process */
  double offset_register;

  /* Current frequency offset between base_freq and the real clock frequency
     as set by drv_set_freq (not in ppm) */
  double slew_freq;

  /* Time (raw) of last update of slewing frequency and offset */
  struct timespec slew_start;

  /* Scheduler timeout ID for ending of the currently running slew */
  SCH_TimeoutID slew_timeout_id;

  /* Scheduled duration of the currently running slew */
  double slew_duration;

  /* Expected delay in ending of the slew due to process scheduling and
     execution time, tracked as a decaying maximum value */
  double slew_excess_duration;

  /* Suggested offset correction rate (correction time * offset) */
  double correction_rate;

  /* Maximum expected offset correction error caused by delayed change in the
     real frequency of the clock */
  double slew_error;

  /* Minimum offset that the system driver can slew faster than the maximum
     frequency offset that it allows to be set directly */
  double fastslew_min_offset;

  /* Maximum slew rate of the system driver */
  double fastslew_max_rate;

  /* Flag indicating that the system driver is currently slewing */
  int fastslew_active;
};

/* ================================================== */

static void handle_end_of_slew(void *anything);
static void update_slew(struct generic_context *gc);

/* ================================================== */

static void
start_fastslew(struct generic_context *gc)
{
  if (!gc->drv.accumulate_offset)
    return;

  gc->drv.accumulate_offset(gc->drv.context, gc->offset_register, 0.0);

  DEBUG_LOG("fastslew offset=%e", gc->offset_register);

  gc->offset_register = 0.0;
  gc->fastslew_active = 1;
}

/* ================================================== */

static void
stop_fastslew(struct generic_context *gc, struct timespec *now)
{
  double corr;

  if (!gc->drv.get_offset || !gc->fastslew_active)
    return;

  /* Cancel the remaining offset */
  gc->drv.get_offset(gc->drv.context, now, &corr, NULL);
  gc->drv.accumulate_offset(gc->drv.context, corr, 0.0);
  gc->offset_register -= corr;
}

/* ================================================== */

static double
clamp_freq(struct generic_context *gc, double freq)
{
  return CLAMP(-gc->max_freq, freq, gc->max_freq);
}

/* ================================================== */
/* End currently running slew and start a new one */

static void
update_slew(struct generic_context *gc)
{
  double old_slew_freq, total_freq, corr_freq, duration, excess_duration;
  struct timespec now;

  /* Remove currently running timeout */
  SCH_RemoveTimeout(gc->slew_timeout_id);

  CLK_ReadTime(gc->clock, &now);

  /* Adjust the offset register by achieved slew */
  duration = UTI_DiffTimespecsToDouble(&now, &gc->slew_start);
  gc->offset_register -= gc->slew_freq * duration;

  stop_fastslew(gc, &now);

  /* Update the maximum excess duration, decaying even when the slew did
     not time out (i.e. frequency was set or offset accrued), but add a small
     value to avoid denormals */
  gc->slew_excess_duration = (gc->slew_excess_duration + 1.0e-9) * SLEW_EXCESS_DURATION_DECAY;
  excess_duration = duration - gc->slew_duration;
  if (gc->slew_excess_duration < excess_duration &&
      excess_duration <= MAX_SLEW_EXCESS_DURATION)
    gc->slew_excess_duration = excess_duration;

  /* Calculate the duration of the new slew, considering the current correction
     rate and previous delays in stopping of the slew */
  if (fabs(gc->offset_register) < MIN_OFFSET_CORRECTION) {
    duration = MAX_SLEW_DURATION;
  } else {
    duration = gc->correction_rate / fabs(gc->offset_register);
    if (duration < MIN_SLEW_DURATION)
      duration = MIN_SLEW_DURATION;
    if (duration < MIN_SLEW_DURATION_EXCESS_RATIO * gc->slew_excess_duration)
      duration = MIN_SLEW_DURATION_EXCESS_RATIO * gc->slew_excess_duration;
  }

  /* Get frequency offset needed to slew the offset in the duration
     and clamp it to the allowed maximum */
  corr_freq = gc->offset_register / duration;
  corr_freq = CLAMP(-gc->max_corr_freq, corr_freq, gc->max_corr_freq);

  /* Let the system driver perform the slew if the requested frequency
     offset is too large for the frequency driver */
  if (gc->drv.accumulate_offset && fabs(corr_freq) >= gc->fastslew_max_rate &&
      fabs(gc->offset_register) > gc->fastslew_min_offset) {
    start_fastslew(gc);
    corr_freq = 0.0;
  }

  /* Get the new real frequency and clamp it */
  total_freq = clamp_freq(gc, gc->base_freq + corr_freq * (1.0e6 - gc->base_freq));

  /* Set the new frequency (the actual frequency returned by the call may be
     slightly different from the requested frequency due to rounding) */
  total_freq = gc->drv.set_frequency(gc->drv.context, total_freq);

  /* Compute the new slewing frequency, it's relative to the real frequency to
     make the calculation in get_offset() cheaper */
  old_slew_freq = gc->slew_freq;
  gc->slew_freq = (total_freq - gc->base_freq) / (1.0e6 - total_freq);

  /* Compute the dispersion introduced by changing frequency and add it
     to all statistics held at higher levels in the system */
  gc->slew_error = fabs((old_slew_freq - gc->slew_freq) * gc->max_freq_change_delay);
  if (gc->slew_error >= MIN_OFFSET_CORRECTION)
    CLK_AddDispersion(gc->clock, gc->slew_error);

  /* Compute the duration of the slew and clamp it.  If the slewing frequency
     is zero or has wrong sign (e.g. due to rounding in the frequency driver or
     when base_freq is larger than max_freq, or fast slew is active), use the
     maximum timeout and try again on the next update. */
  if (fabs(gc->offset_register) < MIN_OFFSET_CORRECTION ||
      gc->offset_register * gc->slew_freq <= 0.0) {
    duration = MAX_SLEW_DURATION;
  } else {
    duration = gc->offset_register / gc->slew_freq;
    if (duration < MIN_SLEW_DURATION)
      duration = MIN_SLEW_DURATION;
    else if (duration > MAX_SLEW_DURATION)
      duration = MAX_SLEW_DURATION;
  }

  /* Restart timer for the next update */
  gc->slew_timeout_id = SCH_AddTimeoutByDelay(duration, handle_end_of_slew, gc);
  gc->slew_start = now;
  gc->slew_duration = duration;

  DEBUG_LOG("slew offset=%e corr_rate=%e base_freq=%f total_freq=%f slew_freq=%e"
            " duration=%f excess=%f slew_error=%e",
            gc->offset_register, gc->correction_rate, gc->base_freq, total_freq,
            gc->slew_freq, gc->slew_duration, gc->slew_excess_duration, gc->slew_error);
}

/* ================================================== */

static void
handle_end_of_slew(void *anything)
{
  struct generic_context *gc = anything;

  gc->slew_timeout_id = 0;
  update_slew(gc);
}

/* ================================================== */

static double
read_frequency(clk_DriverContext *context)
{
  struct generic_context *gc = context;

  return gc->base_freq;
}

/* ================================================== */

static double
set_frequency(clk_DriverContext *context, double freq_ppm)
{
  struct generic_context *gc = context;

  gc->base_freq = freq_ppm;
  update_slew(gc);

  return gc->base_freq;
}

/* ================================================== */

static void
accumulate_offset(clk_DriverContext *context, double offset, double corr_rate)
{
  struct generic_context *gc = context;

  gc->offset_register += offset;
  gc->correction_rate = corr_rate;

  update_slew(gc);
}

/* ================================================== */
/* Determine the correction to generate the cooked time for given raw time */

static void
get_offset(clk_DriverContext *context, struct timespec *raw, double *corr, double *err)
{
  double duration, fastslew_corr, fastslew_err;
  struct generic_context *gc = context;

  duration = UTI_DiffTimespecsToDouble(raw, &gc->slew_start);

  if (gc->drv.get_offset && gc->fastslew_active) {
    gc->drv.get_offset(gc->drv.context, raw, &fastslew_corr, &fastslew_err);
    if (fastslew_corr == 0.0 && fastslew_err == 0.0)
      gc->fastslew_active = 0;
  } else {
    fastslew_corr = fastslew_err = 0.0;
  }

  *corr = gc->slew_freq * duration + fastslew_corr - gc->offset_register;

  if (err) {
    *err = fastslew_err;
    if (fabs(duration) <= gc->max_freq_change_delay)
      *err += gc->slew_error;
  }
}

/* ================================================== */
/* Positive means currently fast of true time, i.e. jump backwards */

static int
apply_step_offset(clk_DriverContext *context, double offset)
{
  struct generic_context *gc = context;
  struct timespec old_time, new_time;
  double err;

  if (gc->drv.apply_step_offset) {
    if (!gc->drv.apply_step_offset(gc->drv.context, offset))
      return 0;
  } else {
    CLK_ReadTime(gc->clock, &old_time);
    UTI_AddDoubleToTimespec(&old_time, -offset, &new_time);

    if (PRV_SetTime(CLK_GetId(gc->clock), &new_time) < 0) {
      DEBUG_LOG("clock_settime() failed");
      return 0;
    }

    CLK_ReadTime(gc->clock, &old_time);
    err = UTI_DiffTimespecsToDouble(&old_time, &new_time);

    CLK_AddDispersion(gc->clock, fabs(err));
  }

  UTI_AddDoubleToTimespec(&gc->slew_start, -offset, &gc->slew_start);

  return 1;
}

/* ================================================== */

static void
set_leap(clk_DriverContext *context, int leap, int tai_offset)
{
  struct generic_context *gc = context;

  if (gc->drv.set_leap)
    gc->drv.set_leap(gc->drv.context, leap, tai_offset);
}

/* ================================================== */

static void
set_sync_status(clk_DriverContext *context, int synchronised,
                double est_error, double max_error)
{
  struct generic_context *gc = context;
  double offset;

  offset = fabs(gc->offset_register);
  if (est_error < offset)
    est_error = offset;
  max_error += offset;

  if (gc->drv.set_sync_status)
    gc->drv.set_sync_status(gc->drv.context, synchronised, est_error, max_error);
}

/* ================================================== */

void
SYS_Generic_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock,
                                  double max_set_freq_ppm, double max_set_freq_delay,
                                  double min_fastslew_offset, double max_fastslew_rate)
{
  struct generic_context *gc;

  gc = MallocNew(struct generic_context);

  gc->clock = clock;
  gc->drv = *driver;

  gc->base_freq = gc->drv.read_frequency(gc->drv.context);
  gc->max_freq = max_set_freq_ppm;
  gc->max_freq_change_delay = max_set_freq_delay * (1.0 + gc->max_freq / 1.0e6);
  gc->max_corr_freq = CNF_GetMaxSlewRate() / 1.0e6;

  gc->offset_register = 0.0;
  gc->slew_freq = 0.0;
  UTI_ZeroTimespec(&gc->slew_start);
  gc->slew_timeout_id = 0;
  gc->slew_duration = 0.0;
  gc->slew_excess_duration = 0.0;
  gc->correction_rate = 0.0;
  gc->slew_error = 0.0;

  gc->fastslew_min_offset = min_fastslew_offset;
  gc->fastslew_max_rate = max_fastslew_rate / 1.0e6;
  gc->fastslew_active = 0;

  driver->context = gc;
  driver->read_frequency = read_frequency;
  driver->set_frequency = set_frequency;
  driver->accumulate_offset = accumulate_offset;
  driver->get_offset = get_offset;
  driver->apply_step_offset = apply_step_offset;
  driver->set_leap = set_leap;
  driver->set_sync_status = set_sync_status;
}

/* ================================================== */

void
SYS_Generic_FinaliseClockDriver(struct clk_Driver *driver)
{
  struct generic_context *gc = driver->context;
  struct timespec now;

  /* Must *NOT* leave a slew running - clock could drift way off
     if the daemon is not restarted */

  SCH_RemoveTimeout(gc->slew_timeout_id);
  gc->slew_timeout_id = 0;

  gc->drv.set_frequency(gc->drv.context, clamp_freq(gc, gc->base_freq));

  CLK_ReadTime(gc->clock, &now);
  stop_fastslew(gc, &now);

  /* Allow the lower driver to finalise with its original contexts */
  *driver = gc->drv;

  Free(gc);
}

/* ================================================== */
