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
#include "privops.h"
#include "sched.h"
#include "util.h"

/* ================================================== */

/* System clock drivers */
static CLK_Instance drv_clock;
static clk_DriverContext *drv_context;
static clk_ReadFrequency drv_read_freq;
static clk_SetFrequency drv_set_freq;
static clk_AccumulateOffset drv_accumulate_offset;
static clk_GetOffset drv_get_offset;
static clk_ApplyStepOffset drv_apply_step_offset;
static clk_SetSyncStatus drv_set_sync_status;
static clk_SetLeap drv_set_leap;

/* Current frequency as requested by the local module (in ppm) */
static double base_freq;

/* Maximum frequency that can be set by drv_set_freq (in ppm) */
static double max_freq;

/* Maximum expected delay in the actual frequency change (e.g. kernel ticks)
   in local time */
static double max_freq_change_delay;

/* Maximum allowed frequency offset relative to the base frequency */
static double max_corr_freq;

/* Amount of outstanding offset to process */
static double offset_register;

/* Minimum offset to correct */
#define MIN_OFFSET_CORRECTION 1.0e-9

/* Current frequency offset between base_freq and the real clock frequency
   as set by drv_set_freq (not in ppm) */
static double slew_freq;

/* Time (raw) of last update of slewing frequency and offset */
static struct timespec slew_start;

/* Limits for the slew length */
#define MIN_SLEW_DURATION 1.0
#define MAX_SLEW_DURATION 1.0e4

/* Scheduler timeout ID for ending of the currently running slew */
static SCH_TimeoutID slew_timeout_id;

/* Scheduled duration of the currently running slew */
static double slew_duration;

/* Expected delay in ending of the slew due to process scheduling and
   execution time, tracked as a decaying maximum value */
static double slew_excess_duration;

/* Maximum accepted excess duration to ignore large jumps after resuming
   suspended system and other reasons (which should be handled in the
   scheduler), a constant to determine the minimum slew duration to avoid
   oscillations due to the excess, and the decay constant */
#define MAX_SLEW_EXCESS_DURATION 100.0
#define MIN_SLEW_DURATION_EXCESS_RATIO 5.0
#define SLEW_EXCESS_DURATION_DECAY 0.9

/* Suggested offset correction rate (correction time * offset) */
static double correction_rate;

/* Maximum expected offset correction error caused by delayed change in the
   real frequency of the clock */
static double slew_error;

/* Minimum offset that the system driver can slew faster than the maximum
   frequency offset that it allows to be set directly */
static double fastslew_min_offset;

/* Maximum slew rate of the system driver */
static double fastslew_max_rate;

/* Flag indicating that the system driver is currently slewing */
static int fastslew_active;

/* ================================================== */

static void handle_end_of_slew(void *anything);
static void update_slew(void);

/* ================================================== */

static void
start_fastslew(void)
{
  if (!drv_accumulate_offset)
    return;

  drv_accumulate_offset(drv_context, offset_register, 0.0);

  DEBUG_LOG("fastslew offset=%e", offset_register);

  offset_register = 0.0;
  fastslew_active = 1;
}

/* ================================================== */

static void
stop_fastslew(struct timespec *now)
{
  double corr;

  if (!drv_get_offset || !fastslew_active)
    return;

  /* Cancel the remaining offset */
  drv_get_offset(drv_context, now, &corr, NULL);
  drv_accumulate_offset(drv_context, corr, 0.0);
  offset_register -= corr;
}

/* ================================================== */

static double
clamp_freq(double freq)
{
  if (freq > max_freq)
    return max_freq;
  if (freq < -max_freq)
    return -max_freq;
  return freq;
}

/* ================================================== */
/* End currently running slew and start a new one */

static void
update_slew(void)
{
  double old_slew_freq, total_freq, corr_freq, duration, excess_duration;
  struct timespec now;

  /* Remove currently running timeout */
  SCH_RemoveTimeout(slew_timeout_id);

  CLK_ReadTime(drv_clock, &now);

  /* Adjust the offset register by achieved slew */
  duration = UTI_DiffTimespecsToDouble(&now, &slew_start);
  offset_register -= slew_freq * duration;

  stop_fastslew(&now);

  /* Update the maximum excess duration, decaying even when the slew did
     not time out (i.e. frequency was set or offset accrued), but add a small
     value to avoid denormals */
  slew_excess_duration = (slew_excess_duration + 1.0e-9) * SLEW_EXCESS_DURATION_DECAY;
  excess_duration = duration - slew_duration;
  if (slew_excess_duration < excess_duration &&
      excess_duration <= MAX_SLEW_EXCESS_DURATION)
    slew_excess_duration = excess_duration;

  /* Calculate the duration of the new slew, considering the current correction
     rate and previous delays in stopping of the slew */
  if (fabs(offset_register) < MIN_OFFSET_CORRECTION) {
    duration = MAX_SLEW_DURATION;
  } else {
    duration = correction_rate / fabs(offset_register);
    if (duration < MIN_SLEW_DURATION)
      duration = MIN_SLEW_DURATION;
    if (duration < MIN_SLEW_DURATION_EXCESS_RATIO * slew_excess_duration)
      duration = MIN_SLEW_DURATION_EXCESS_RATIO * slew_excess_duration;
  }

  /* Get frequency offset needed to slew the offset in the duration
     and clamp it to the allowed maximum */
  corr_freq = offset_register / duration;
  if (corr_freq < -max_corr_freq)
    corr_freq = -max_corr_freq;
  else if (corr_freq > max_corr_freq)
    corr_freq = max_corr_freq;

  /* Let the system driver perform the slew if the requested frequency
     offset is too large for the frequency driver */
  if (drv_accumulate_offset && fabs(corr_freq) >= fastslew_max_rate &&
      fabs(offset_register) > fastslew_min_offset) {
    start_fastslew();
    corr_freq = 0.0;
  }

  /* Get the new real frequency and clamp it */
  total_freq = clamp_freq(base_freq + corr_freq * (1.0e6 - base_freq));

  /* Set the new frequency (the actual frequency returned by the call may be
     slightly different from the requested frequency due to rounding) */
  total_freq = drv_set_freq(drv_context, total_freq);

  /* Compute the new slewing frequency, it's relative to the real frequency to
     make the calculation in get_offset() cheaper */
  old_slew_freq = slew_freq;
  slew_freq = (total_freq - base_freq) / (1.0e6 - total_freq);

  /* Compute the dispersion introduced by changing frequency and add it
     to all statistics held at higher levels in the system */
  slew_error = fabs((old_slew_freq - slew_freq) * max_freq_change_delay);
  if (slew_error >= MIN_OFFSET_CORRECTION)
    CLK_AddDispersion(drv_clock, slew_error);

  /* Compute the duration of the slew and clamp it.  If the slewing frequency
     is zero or has wrong sign (e.g. due to rounding in the frequency driver or
     when base_freq is larger than max_freq, or fast slew is active), use the
     maximum timeout and try again on the next update. */
  if (fabs(offset_register) < MIN_OFFSET_CORRECTION ||
      offset_register * slew_freq <= 0.0) {
    duration = MAX_SLEW_DURATION;
  } else {
    duration = offset_register / slew_freq;
    if (duration < MIN_SLEW_DURATION)
      duration = MIN_SLEW_DURATION;
    else if (duration > MAX_SLEW_DURATION)
      duration = MAX_SLEW_DURATION;
  }

  /* Restart timer for the next update */
  slew_timeout_id = SCH_AddTimeoutByDelay(duration, handle_end_of_slew, NULL);
  slew_start = now;
  slew_duration = duration;

  DEBUG_LOG("slew offset=%e corr_rate=%e base_freq=%f total_freq=%f slew_freq=%e"
            " duration=%f excess=%f slew_error=%e",
            offset_register, correction_rate, base_freq, total_freq, slew_freq,
            slew_duration, slew_excess_duration, slew_error);
}

/* ================================================== */

static void
handle_end_of_slew(void *anything)
{
  slew_timeout_id = 0;
  update_slew();
}

/* ================================================== */

static double
read_frequency(clk_DriverContext *context)
{
  return base_freq;
}

/* ================================================== */

static double
set_frequency(clk_DriverContext *context, double freq_ppm)
{
  base_freq = freq_ppm;
  update_slew();

  return base_freq;
}

/* ================================================== */

static void
accumulate_offset(clk_DriverContext *context, double offset, double corr_rate)
{
  offset_register += offset;
  correction_rate = corr_rate;

  update_slew();
}

/* ================================================== */
/* Determine the correction to generate the cooked time for given raw time */

static void
get_offset(clk_DriverContext *context, struct timespec *raw, double *corr, double *err)
{
  double duration, fastslew_corr, fastslew_err;

  duration = UTI_DiffTimespecsToDouble(raw, &slew_start);

  if (drv_get_offset && fastslew_active) {
    drv_get_offset(drv_context, raw, &fastslew_corr, &fastslew_err);
    if (fastslew_corr == 0.0 && fastslew_err == 0.0)
      fastslew_active = 0;
  } else {
    fastslew_corr = fastslew_err = 0.0;
  }

  *corr = slew_freq * duration + fastslew_corr - offset_register;

  if (err) {
    *err = fastslew_err;
    if (fabs(duration) <= max_freq_change_delay)
      *err += slew_error;
  }
}

/* ================================================== */
/* Positive means currently fast of true time, i.e. jump backwards */

static int
apply_step_offset(clk_DriverContext *context, double offset)
{
  struct timespec old_time, new_time;
  double err;

  if (drv_apply_step_offset) {
    if (!drv_apply_step_offset(drv_context, offset))
      return 0;
  } else {
    CLK_ReadTime(drv_clock, &old_time);
    UTI_AddDoubleToTimespec(&old_time, -offset, &new_time);

    if (PRV_SetTime(CLOCK_REALTIME, &new_time) < 0) {
      DEBUG_LOG("clock_settime() failed");
      return 0;
    }

    CLK_ReadTime(drv_clock, &old_time);
    err = UTI_DiffTimespecsToDouble(&old_time, &new_time);

    CLK_AddDispersion(drv_clock, fabs(err));
  }

  UTI_AddDoubleToTimespec(&slew_start, -offset, &slew_start);

  return 1;
}

/* ================================================== */

static void
set_leap(clk_DriverContext *context, int leap, int tai_offset)
{
  if (drv_set_leap)
    drv_set_leap(drv_context, leap, tai_offset);
}

/* ================================================== */

static void
set_sync_status(clk_DriverContext *context, int synchronised,
                double est_error, double max_error)
{
  double offset;

  offset = fabs(offset_register);
  if (est_error < offset)
    est_error = offset;
  max_error += offset;

  if (drv_set_sync_status)
    drv_set_sync_status(drv_context, synchronised, est_error, max_error);
}

/* ================================================== */

void
SYS_Generic_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock,
                                  double max_set_freq_ppm, double max_set_freq_delay,
                                  double min_fastslew_offset, double max_fastslew_rate)
{
  max_freq = max_set_freq_ppm;
  max_freq_change_delay = max_set_freq_delay * (1.0 + max_freq / 1.0e6);

  drv_clock = clock;
  drv_context = driver->context;
  drv_read_freq = driver->read_frequency;
  drv_set_freq = driver->set_frequency;
  drv_accumulate_offset = driver->accumulate_offset;
  drv_get_offset = driver->get_offset;
  drv_apply_step_offset = driver->apply_step_offset;
  drv_set_leap = driver->set_leap;
  drv_set_sync_status = driver->set_sync_status;

  base_freq = drv_read_freq(drv_context);
  slew_freq = 0.0;
  offset_register = 0.0;
  slew_excess_duration = 0.0;

  max_corr_freq = CNF_GetMaxSlewRate() / 1.0e6;

  fastslew_min_offset = min_fastslew_offset;
  fastslew_max_rate = max_fastslew_rate / 1.0e6;
  fastslew_active = 0;

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
  struct timespec now;

  /* Must *NOT* leave a slew running - clock could drift way off
     if the daemon is not restarted */

  SCH_RemoveTimeout(slew_timeout_id);
  slew_timeout_id = 0;

  drv_set_freq(drv_context, clamp_freq(base_freq));

  CLK_ReadTime(drv_clock, &now);
  stop_fastslew(&now);

  /* Allow the lower driver to finalise with its original contexts */
  driver->context = drv_context;
  driver->read_frequency = drv_read_freq;
  driver->set_frequency = drv_set_freq;
  driver->accumulate_offset = drv_accumulate_offset;
  driver->get_offset = drv_get_offset;
  driver->apply_step_offset = drv_apply_step_offset;
  driver->set_leap = drv_set_leap;
  driver->set_sync_status = drv_set_sync_status;

  drv_clock = NULL;
}

/* ================================================== */
