/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
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

  Clock abstraction and conversions.

  */

#include "config.h"

#include "sysincl.h"

#include "array.h"
#include "clocks.h"
#include "clocks_priv.h"
#include "conf.h"
#include "hwclock.h"
#include "local.h"
#include "logging.h"
#include "memory.h"
#include "util.h"
#include "sched.h"
#include "sys.h"
#include "sys_null.h"

#define INVALID_CLOCK_FD -1

/* Number of readings per HW clock sample */
#define HWCLOCK_READINGS 25

/* Minimum and maximum interval between PHC readings */
#define MIN_PHC_POLL -6
#define MAX_PHC_POLL 20

#define MAX_FREQUENCY 100000.0

#define NSEC_PER_SEC 1000000000

#define NOCONTROL_CORRECTION_THRESHOLD 10000

struct CLK_Instance_Record {
  char *name;
  clockid_t id;
  int fd;
  int system;
  int control;
  int start_tracking;

  int locked;
  int reference_counter;

  struct clk_Driver driver;
  time_t cook_offset;

  double precision;
  double max_frequency;
  double frequency;

  int minpoll;
  int maxpoll;
  int min_samples;
  int max_samples;
  int nocrossts;

  double last_time_offset;
  double last_freq_offset;

  int in_update;
};

struct tracking {
  CLK_Instance from;
  CLK_Instance to;
  HCL_Instance hwclock;
  SCH_TimeoutID hwclock_timeout_id;
  int reading_mode;
  int in_update;
};

/* Array of struct tracking * */
ARR_Instance tracking_map;

/* ================================================== */

static int control_allowed;

/* Array of CLK_Instance */
static ARR_Instance clocks;

static CLK_Instance main_clock;
static CLK_Instance shadow_clock;

static int step_limit;
static double step_threshold;
static SCH_TimeoutID step_check_timeout_id;

static LOG_FileID log_id;

/* ================================================== */

static int update_tracking(struct tracking *tracking, struct timespec *now);

/* ================================================== */

/* As a fallback, measure how long it takes to read the clock.  It
   typically takes longer than the resolution of the clock (and it
   depends on the CPU speed), i.e. every reading gives a different
   value, but handle also low-resolution clocks that might give
   the same reading multiple times. */

/* Define the number of increments of the system clock that we want
   to see to be fairly sure that we've got something approaching
   the minimum increment.  Even on a crummy implementation that can't
   interpolate between 10ms ticks, we should get this done in
   under 1s of busy waiting. */
#define NITERS 100

static int
measure_read_delay(CLK_Instance inst)
{
  struct timespec ts, old_ts;
  int iters, diff, best;

  CLK_ReadTime(inst, &old_ts);

  /* Assume we must be better than a second */
  best = NSEC_PER_SEC;
  iters = 0;

  do {
    CLK_ReadTime(inst, &ts);

    diff = NSEC_PER_SEC * (ts.tv_sec - old_ts.tv_sec) + (ts.tv_nsec - old_ts.tv_nsec);

    old_ts = ts;
    if (diff > 0) {
      if (diff < best)
        best = diff;
      iters++;
    }
  } while (iters < NITERS);

  assert(best > 0);

  return best;
}

/* ================================================== */

static double
measure_precision(CLK_Instance inst)
{
  int res, delay, prec;

  res = SYS_GetClockResolution(inst);
  delay = measure_read_delay(inst);

  if (res > 0)
    prec = MIN(res, delay);
  else
    prec = delay;

  return prec / 1.0e9;
}

/* ================================================== */

static CLK_Instance
get_record(unsigned int index)
{
  return *(CLK_Instance *)ARR_GetElement(clocks, index);
}

/* ================================================== */

static struct tracking *
get_tracking(CLK_Instance inst1, CLK_Instance inst2)
{
  CLK_Instance tmp;
  struct tracking *tracking;
  unsigned int i;

  assert(inst1 != inst2);
  assert(inst1->id != inst2->id);

  if ((inst1->system == inst2->system && inst1->id > inst2->id) ||
      (inst1->system && !inst2->system)) {
    tmp = inst1;
    inst1 = inst2;
    inst2 = tmp;
  }

  for (i = 0; i < ARR_GetSize(tracking_map); i++) {
    tracking = *(struct tracking **)ARR_GetElement(tracking_map, i);
    if (tracking->from == inst1 && tracking->to == inst2)
      return tracking;
  }

  tracking = MallocNew(struct tracking);
  tracking->from = inst1;
  tracking->to = inst2;
  tracking->hwclock = HCL_CreateInstance(inst1, inst2,
                                         inst1->min_samples, inst1->max_samples,
                                         UTI_Log2ToDouble(inst1->minpoll),
                                         CLK_GetPrecision(inst1));
  tracking->hwclock_timeout_id = 0;
  tracking->reading_mode = 0;
  tracking->in_update = 0;

  ARR_AppendElement(tracking_map, &tracking);

  DEBUG_LOG("Added %s->%s tracking", inst1->name, inst2->name);

  return tracking;
}

/* ================================================== */

void
CLK_Initialise(int control)
{
  ARR_Instance conf_clocks;
  CLK_Instance clock;
  CNF_Clock *conf;
  unsigned int i;

  control_allowed = control;
  main_clock = shadow_clock = NULL;
  clocks = ARR_CreateInstance(sizeof (CLK_Instance));
  tracking_map = ARR_CreateInstance(sizeof (struct tracking *));

  conf_clocks = CNF_GetClocks();

  for (i = 0; i < ARR_GetSize(conf_clocks); i++) {
    conf = ARR_GetElement(conf_clocks, i);
    clock = CLK_Open(conf->name, !conf->free);
    if (!clock)
      LOG_FATAL("Could not open clock");

    clock->start_tracking = 1;

    if (conf->main)
      main_clock = clock;
    if (conf->shadow)
      shadow_clock = clock;

    CLK_SetPoll(clock, conf->minpoll, conf->maxpoll);
    CLK_SetSamples(clock, conf->min_samples, conf->max_samples);
    if (conf->nocrossts)
      CLK_DisableCrossTimestamping(clock);
  }

  if (!main_clock) {
    main_clock = CLK_Open("SYS:REALTIME", 1);
    if (!main_clock)
      LOG_FATAL("Could not open clock");
  }

  if (!shadow_clock) {
    if (CLK_IsSystem(main_clock)) {
      shadow_clock = main_clock;
    } else {
      shadow_clock = CLK_Open("SYS:REALTIME", 0);
      if (!shadow_clock)
        LOG_FATAL("Could not open clock");
    }
  }

  CNF_GetMakeStep(&step_limit, &step_threshold);
  step_check_timeout_id = 0;

  log_id = CNF_GetLogClocks() ? LOG_FileOpen("clocks",
           "   Date (UTC) Time     TBD")
           : -1;
}

/* ================================================== */

void
CLK_Finalise(void)
{
  if (step_check_timeout_id != 0)
    SCH_RemoveTimeout(step_check_timeout_id);

  if (main_clock != shadow_clock)
    CLK_Close(shadow_clock);
  CLK_Close(main_clock);

  while (ARR_GetSize(clocks) > 0)
    CLK_Close(get_record(0));

  BRIEF_ASSERT(ARR_GetSize(clocks) == 0);
  ARR_DestroyInstance(clocks);
}

/* ================================================== */

void
CLK_StartTracking(void)
{
  CLK_Instance clock;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(clocks); i++) {
    clock = get_record(i);
    if (clock == main_clock || !clock->start_tracking)
      continue;
    update_tracking(get_tracking(main_clock, clock), NULL);
  }
}

/* ================================================== */

int
CLK_IsAnyControlled(void)
{
  unsigned int i;

  for (i = 0; i < ARR_GetSize(clocks); i++) {
    if (get_record(i)->control)
      return 1;
  }

  return 0;
}

/* ================================================== */

CLK_Instance
CLK_GetMainClock(void)
{
  return main_clock;
}

/* ================================================== */

CLK_Instance
CLK_GetShadowClock(void)
{
  return shadow_clock;
}

/* ================================================== */

CLK_Instance
CLK_Open(const char *name, int control)
{
  struct timespec ts;
  CLK_Instance inst;
  unsigned int i;

  inst = NULL;
  control = control && control_allowed;

  /* Check if this clock is already open */
  for (i = 0; i < ARR_GetSize(clocks); i++) {
    inst = get_record(i);

    if (strcmp(inst->name, name) != 0)
      continue;

    if (inst->control < control) {
      LOG(LOGS_ERR, "Clock control mismatch");
      goto error;
    }

    inst->reference_counter++;
    return inst;
  }

  inst = MallocNew(struct CLK_Instance_Record);
  inst->name = Strdup(name);
  inst->fd = INVALID_CLOCK_FD;
  inst->system = 1;
  inst->control = control;
  inst->start_tracking = 0;
  inst->locked = 1;
  inst->reference_counter = 1;

  if (strcmp(name, "SYS:REALTIME") == 0) {
    inst->id = CLOCK_REALTIME;
  } else if (strcmp(name, "SYS:MONOTONIC") == 0) {
    inst->id = CLOCK_MONOTONIC;
    inst->control = 0;
  } else if (!SYS_OpenClock(name, &inst->control, &inst->id, &inst->fd, &inst->system)) {
    goto error;
  }

  inst->driver.context = NULL;
  inst->driver.read_frequency = NULL;
  inst->driver.set_frequency = NULL;
  inst->driver.accumulate_offset = NULL;
  inst->driver.get_offset = NULL;
  inst->driver.apply_step_offset = NULL;
  inst->driver.set_leap = NULL;
  inst->driver.set_sync_status = NULL;

  inst->cook_offset = 0;

  inst->minpoll = 0;
  inst->maxpoll = 1;
  inst->min_samples = 2;
  inst->max_samples = 16;
  inst->nocrossts = 0;

  inst->last_time_offset = 0.0;
  inst->last_freq_offset = 0.0;

  inst->in_update = 0;

  /* Verify that the clock works */
  CLK_ReadTime(inst, &ts);

  SYS_InitialiseClockDriver(&inst->driver, inst, inst->control);

  ARR_AppendElement(clocks, &inst);

  //TODO: apply to any main clock?
  if (inst->system)
    inst->precision = CNF_GetClockPrecision();
  else
    inst->precision = 100e-9; //hwtimestamp default
    //inst->precision = 0.0;
  if (inst->precision <= 0.0)
    inst->precision = measure_precision(inst);
  inst->precision = CLAMP(1.0e-9, inst->precision, 1.0);
  DEBUG_LOG("%s precision %.9f", inst->name, inst->precision);

  /* Time must never stop or run backwards */
  inst->max_frequency = CNF_GetMaxDrift();
  inst->max_frequency = CLAMP(0.0, inst->max_frequency, MAX_FREQUENCY);

  BRIEF_ASSERT(inst->driver.read_frequency);
  inst->frequency = inst->driver.read_frequency(inst->driver.context);

  return inst;

error:
  if (inst) {
    Free(inst->name);
    Free(inst);
  }
  return NULL;
}

/* ================================================== */

void
CLK_Close(CLK_Instance inst)
{
  struct tracking *tracking;
  unsigned int i;

  if (inst->reference_counter > 1) {
    inst->reference_counter--;
    return;
  }

  for (i = 0; i < ARR_GetSize(tracking_map); i++) {
    tracking = *(struct tracking **)ARR_GetElement(tracking_map, i);
    if (tracking->from != inst && tracking->to != inst)
      continue;
    if (tracking->hwclock) {
      HCL_DestroyInstance(tracking->hwclock);
      SCH_RemoveTimeout(tracking->hwclock_timeout_id);
    }
    Free(tracking);
    ARR_RemoveElement(tracking_map, i--);
  }

  for (i = 0; i < ARR_GetSize(clocks); i++) {
    if (get_record(i) == inst) {
      ARR_RemoveElement(clocks, i);
      break;
    }
  }

  SYS_FinaliseClockDriver(&inst->driver, inst->control);

  if (inst->fd >= 0)
    close(inst->fd);

  Free(inst->name);
  Free(inst);
}

/* ================================================== */

void
CLK_SetPoll(CLK_Instance inst, int minpoll, int maxpoll)
{
  inst->minpoll = CLAMP(MIN_PHC_POLL, minpoll, MAX_PHC_POLL);
  inst->maxpoll = CLAMP(minpoll, maxpoll, MAX_PHC_POLL);
}

/* ================================================== */

void
CLK_SetSamples(CLK_Instance inst, int min_samples, int max_samples)
{
  inst->min_samples = min_samples;
  inst->max_samples = max_samples;
}

/* ================================================== */

void
CLK_DisableCrossTimestamping(CLK_Instance inst)
{
  inst->nocrossts = 1;
}

/* ================================================== */

int
CLK_GetCrossTimestamping(CLK_Instance inst)
{
  return !inst->nocrossts;
}

/* ================================================== */

clockid_t
CLK_GetId(CLK_Instance inst)
{
  return inst->id;
}

/* ================================================== */

int
CLK_GetFd(CLK_Instance inst)
{
  return inst->fd;
}

/* ================================================== */

const char *
CLK_GetName(CLK_Instance inst)
{
  return inst->name;
}

/* ================================================== */

int
CLK_IsSystem(CLK_Instance inst)
{
  return inst->system;
}

/* ================================================== */

int
CLK_IsTimerAccurate(CLK_Instance inst)
{
  switch (inst->id) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
      return 1;
    default:
      return 0;
  }
}

/* ================================================== */

double
CLK_GetPrecision(CLK_Instance inst)
{
  return inst->precision;
}

/* ================================================== */

void
CLK_ReadTime(CLK_Instance inst, struct timespec *ts)
{
again:
  if (clock_gettime(inst->id, ts) < 0) {
    if (errno == EBUSY)
      goto again;
    LOG_FATAL("Could not read %s : %s", inst->name, strerror(errno));
  }
}

/* ================================================== */

double
CLK_GetFrequency(CLK_Instance inst)
{
  return inst->frequency;
}

/* ================================================== */

static double
get_driver_offset(CLK_Instance inst, int reverse, struct timespec *ts, double *err)
{
  struct timespec ts2;

  BRIEF_ASSERT(inst->driver.get_offset);

  ts2 = *ts;
  if (reverse)
    ts2.tv_sec -= inst->cook_offset;

  return inst->driver.get_offset(inst->driver.context, reverse, &ts2, err);
}

/* ================================================== */

static void
log_change(CLK_Instance inst, struct timespec *cooked, double dfreq, double doffset)
{
  if (log_id == -1)
    return;

  LOG_FileWrite(log_id,
                "%s %-25s %10.3f %10.3e %10.3e %10.3e",
                UTI_TimeToLogForm(cooked->tv_sec), inst->name,
                inst->frequency, dfreq, doffset, get_driver_offset(inst, 1, cooked, NULL));
}

/* ================================================== */

static void
handle_change(CLK_Instance inst, struct timespec *cooked, double dfreq, double doffset)
{
  struct tracking *tracking;
  unsigned int i;

  if (!inst->locked)
    return;

  for (i = 0; i < ARR_GetSize(tracking_map); i++) {
    tracking = *(struct tracking **)ARR_GetElement(tracking_map, i);
    if (!tracking->hwclock)
      continue;
    //TODO: handle CLOCK_MONOTONIC vs CLOCK_REALTIME?
    if (tracking->from == inst && tracking->to->locked)
      HCL_SlewHw(tracking->hwclock, cooked, dfreq, doffset);
    if (tracking->to == inst && tracking->from->locked)
      HCL_SlewLocal(tracking->hwclock, cooked, dfreq, doffset);
  }

  log_change(inst, cooked, dfreq, doffset);
}

/* ================================================== */

static double
clamp_frequency(CLK_Instance inst, double freq)
{
  if (freq <= inst->max_frequency && freq >= -inst->max_frequency)
    return freq;

  LOG(LOGS_WARN, "Frequency %.1f ppm exceeds allowed maximum", freq);

  return CLAMP(-inst->max_frequency, freq, inst->max_frequency);
}

/* ================================================== */

static double
set_frequency_and_accumulate_offset(CLK_Instance inst, double freq, double doffset,
                                    double corr_rate, struct timespec *cooked)
{
  double dfreq;

  BRIEF_ASSERT(inst->driver.set_frequency);
  BRIEF_ASSERT(inst->driver.accumulate_offset);
  BRIEF_ASSERT(!inst->in_update);

  inst->in_update = 1;

  freq = clamp_frequency(inst, freq);

  freq = inst->driver.set_frequency(inst->driver.context, freq);
  if (doffset != 0.0)
    inst->driver.accumulate_offset(inst->driver.context, doffset, corr_rate);
  dfreq = (freq - inst->frequency) / (1.0e6 - inst->frequency);

  DEBUG_LOG("%s: old_freq=%+.3f new_freq=%+.3f offset=%.9f",
            inst->name, inst->frequency, freq, doffset);

  inst->frequency = freq;

  handle_change(inst, cooked, dfreq, doffset);

  UTI_AddDoubleToTimespec(cooked, -doffset, cooked);

  if (!inst->control) {
    struct timespec raw;
    double offset;

    //TODO: avoid this call
    CLK_ReadTime(inst, &raw);
#if 1
    offset = get_driver_offset(inst, 0, &raw, NULL);
#else
    offset = get_driver_offset(inst, 1, cooked, NULL);
#endif

    if (fabs(offset) >= NOCONTROL_CORRECTION_THRESHOLD) {
      time_t rounded_offset = round(offset);
      inst->cook_offset += rounded_offset;
      inst->driver.accumulate_offset(inst->driver.context, rounded_offset, corr_rate);
    }
  }

  inst->last_time_offset = doffset;
  inst->last_freq_offset = dfreq;

  inst->in_update = 0;

  return dfreq;
}

/* ================================================== */

static void
step_check_timeout(void *arg)
{
  struct timespec now, cooked;
  CLK_Instance clock;
  unsigned int i;
  double offset;

  step_check_timeout_id = 0;

  for (i = 0; i < ARR_GetSize(clocks); i++) {
    clock = get_record(i);
    if (clock == main_clock)
      continue;
    CLK_ReadTime(clock, &now);
    CLK_CookTime(clock, &now, &cooked, NULL);
    offset = UTI_DiffTimespecsToDouble(&cooked, &now);

    if (fabs(offset) > step_threshold) {
      set_frequency_and_accumulate_offset(clock, clock->frequency, offset, 0.0, &cooked);
      if (CLK_ApplyStepOffset(clock, -offset))
        LOG(LOGS_WARN, "%s was stepped by %.6f seconds", clock->name, -offset);
      else
        //TODO
        ;
    }
  }
}

/* ================================================== */

static void
schedule_step_check(int force)
{
  if (!force) {
    //TODO: this assumes this function is only called through CLK_Accumulate*
    //on every and only reference update
    if (step_limit <= 0)
      return;
    if (step_limit > 0)
      step_limit--;
  }

  if (step_check_timeout_id != 0)
    return;

  step_check_timeout_id = SCH_AddTimeoutByDelay(0.0, step_check_timeout, NULL);
}

/* ================================================== */

static void
update_other_clocks(CLK_Instance inst, struct timespec *cooked)
{
  struct timespec cooked2;
  double freq_ratio, old_freq2, new_freq2, doffset2;
  CLK_Instance inst2;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(clocks); i++) {
    inst2 = get_record(i);
    if (inst2 == inst || !inst2->control || !inst->locked || !inst2->locked)
      continue;

    /* cooked was corrected in set_frequency_and_accumulate_offset() */
    if (!CLK_ConvertTime(inst, 1, cooked, inst2, 1, &cooked2, NULL))
      continue;

    if (!CLK_ConvertInterval(inst, inst2, 1.0, &freq_ratio, NULL))
      continue;

    doffset2 = UTI_DiffTimespecsToDouble(&cooked2, cooked);

    old_freq2 = CLK_GetFrequency(inst2);
    new_freq2 = old_freq2 + (freq_ratio - 1.0) * (1.0e6 - old_freq2);
    set_frequency_and_accumulate_offset(inst2, new_freq2, doffset2, 0.0, &cooked2);

    DEBUG_LOG("%s-%s dfreq %+.6f doffset %+.9f freq %+.6f->%+.6f", CLK_GetName(inst2), CLK_GetName(inst),
              (freq_ratio - 1.0) * 1e6, doffset2, old_freq2, new_freq2);
  }
}

/* ================================================== */

double
CLK_SetFrequency(CLK_Instance inst, double freq)
{
  struct timespec raw, cooked;
  double r;

  assert(inst == main_clock);

  //TODO: get these from the caller, or read only when needed in the handler?
  CLK_ReadTime(inst, &raw);
  CLK_CookTime(inst, &raw, &cooked, NULL);

  r = set_frequency_and_accumulate_offset(inst, freq, 0.0, 0.0, &cooked);

  update_other_clocks(inst, &cooked);

  return r;
}

/* ================================================== */

double
CLK_AccumulateFrequencyAndOffset(CLK_Instance inst, double dfreq, double doffset, double corr_rate)
{
  struct timespec raw, cooked;
  double freq, r;

  assert(inst == main_clock);

  //TODO: get these from the caller, or read only when needed in the handler?
  CLK_ReadTime(inst, &raw);
  CLK_CookTime(inst, &raw, &cooked, NULL);

  /* Work out new absolute frequency.  Note that absolute frequencies
     are handled in units of ppm, whereas the 'dfreq' argument is in
     terms of the gradient of the (offset) v (local time) function. */
  freq = inst->frequency + dfreq * (1.0e6 - inst->frequency);

  r = set_frequency_and_accumulate_offset(inst, freq, doffset, corr_rate, &cooked);

  update_other_clocks(inst, &cooked);

  schedule_step_check(0);

  return r;
}

/* ================================================== */

double
CLK_GetOffset(CLK_Instance inst, int reverse, struct timespec *ts, double *err)
{
  return get_driver_offset(inst, reverse, ts, err) +
         (reverse ? -inst->cook_offset : inst->cook_offset);
}

/* ================================================== */

int
CLK_ApplyStepOffset(CLK_Instance inst, double offset)
{
  struct timespec raw, cooked;

  BRIEF_ASSERT(inst->driver.apply_step_offset);

  //TODO: get these from the caller, or read only when needed in the handler?
  CLK_ReadTime(inst, &raw);
  CLK_CookTime(inst, &raw, &cooked, NULL);

  if (!inst->driver.apply_step_offset(inst->driver.context, offset))
    return 0;

  handle_change(inst, &cooked, 0.0, offset);

  update_other_clocks(inst, &cooked);

  if (inst == main_clock)
    schedule_step_check(1);

  return 1;
}

/* ================================================== */

int
CLK_CanLeap(CLK_Instance inst)
{
  return inst->driver.set_leap != NULL;
}

/* ================================================== */

void
CLK_SetLeap(CLK_Instance inst, int leap, int tai_offset)
{
  BRIEF_ASSERT(inst->driver.set_leap);

  inst->driver.set_leap(inst->driver.context, leap, tai_offset);
}

/* ================================================== */

void
CLK_SetSyncStatus(CLK_Instance inst, int synchronised, double est_error, double max_error)
{
  if (!inst->driver.set_sync_status)
    return;

  inst->driver.set_sync_status(inst->driver.context, synchronised, est_error, max_error);
}

/* ================================================== */

void
CLK_Unlock(CLK_Instance inst)
{
  inst->locked = 0;
}

/* ================================================== */

void
CLK_Lock(CLK_Instance inst)
{
  inst->locked = 1;
}

/* ================================================== */

void
CLK_AddDispersion(CLK_Instance inst, double dispersion)
{
  if (inst == main_clock)
    LCL_InvokeDispersionNotifyHandlers(dispersion);
}

/* ================================================== */

void
CLK_CookTime(CLK_Instance inst, struct timespec *raw, struct timespec *cooked, double *err)
{
  double offset;

  offset = get_driver_offset(inst, 0, raw, err);
  UTI_AddDoubleToTimespec(raw, offset, cooked);
  cooked->tv_sec += inst->cook_offset;
}

/* ================================================== */

static void
uncook_time(CLK_Instance inst, struct timespec *cooked, struct timespec *raw, double *err)
{
  struct timespec cooked2;
  double offset;

  cooked2 = *cooked;
  cooked2.tv_sec -= inst->cook_offset;

  offset = get_driver_offset(inst, 1, &cooked2, err);
  UTI_AddDoubleToTimespec(cooked, -offset, raw);
}

/* ================================================== */

static int get_clock_readings(CLK_Instance inst1, CLK_Instance inst2,
                              int max_readings, struct timespec tss[][3])
{
  int i;

  for (i = 0; i < max_readings; i++) {
    CLK_ReadTime(inst1, &tss[i][0]);
    CLK_ReadTime(inst2, &tss[i][1]);
  }

  for (i = 0; i < max_readings; i++)
    tss[i][2] = tss[i][0];

  return max_readings;
}

/* ================================================== */

int
CLK_ReadTimePair(CLK_Instance inst1, CLK_Instance inst2,
                 struct timespec *raw1, struct timespec *raw2, double *err, int *quality)
{
  struct timespec readings[HWCLOCK_READINGS][3];
  struct tracking *tracking;
  int n_readings;

  tracking = get_tracking(inst2, inst1);

  if (inst2 != tracking->from) {
    struct timespec *tmp;
    assert(inst1 == tracking->from);
    assert(inst2 == tracking->to);
    /* Swap */
    inst1 = tracking->to;
    inst2 = tracking->from;
    tmp = raw1;
    raw1 = raw2;
    raw2 = tmp;
  } else {
    assert(inst1 == tracking->to);
  }

  if (inst1->system && !inst2->system) {
    n_readings = SYS_GetClockReadings(inst1, inst2, HWCLOCK_READINGS,
                                      &tracking->reading_mode, readings);
  } else {
    n_readings = get_clock_readings(inst1, inst2, HWCLOCK_READINGS, readings);
  }

  if (n_readings <= 0)
    return 0;

  if (!HCL_ProcessReadings(tracking->hwclock, n_readings, readings,
                           raw2, raw1, err, quality))
    return 0;

  if (*quality > 0) {
    struct timespec cooked1, cooked2;
    double err1, err2;

    CLK_CookTime(inst1, raw1, &cooked1, &err1);
    if (HCL_NeedsNewSample(tracking->hwclock, &cooked1)) {
      CLK_CookTime(inst2, raw2, &cooked2, &err2);
      HCL_AccumulateSample(tracking->hwclock, &cooked2, &cooked1, *err + err1 + err2);
    }
  }

  return 1;
}

/* ================================================== */

static void
poll_timeout(void *arg)
{
  struct tracking *tracking = arg;
  struct timespec now;

  tracking->hwclock_timeout_id = 0;

  SCH_GetLastEventTime(&now, NULL, NULL);
  update_tracking(tracking, &now);
}

/* ================================================== */

static int
update_tracking(struct tracking *tracking, struct timespec *now)
{
  int quality, on_timeout, early;
  struct timespec ts, ts1, ts2;
  double err, interval;

  if (!SCH_IsInitialised())
    return 0;

  if (tracking->in_update || tracking->from->in_update || tracking->to->in_update)
    return 0;
  tracking->in_update = 1;

  if (!now) {
    now = &ts;
    CLK_ReadTime(tracking->to, now);
    CLK_CookTime(tracking->to, now, now, NULL);
  }

  early = !HCL_NeedsNewSample(tracking->hwclock, now);
  on_timeout = tracking->hwclock_timeout_id == 0;

  /* Add timeout for the next poll in case no HW timestamp will be captured
     between the minpoll and maxpoll.  Separate reading of different PHCs to
     avoid long intervals between handling I/O events. */
  if (!early || on_timeout) {
    SCH_RemoveTimeout(tracking->hwclock_timeout_id);
    interval = UTI_Log2ToDouble(tracking->from->maxpoll);
    tracking->hwclock_timeout_id = SCH_AddTimeoutInClass(interval, interval /
                                                         ARR_GetSize(clocks) / 4, 0.1,
                                                         SCH_PhcPollClass, poll_timeout, tracking);
  }

  if (early) {
    DEBUG_LOG("early");
    tracking->in_update = 0;
    return 1;
  }

  DEBUG_LOG("Polling %s-%s%s",
            tracking->from->name, tracking->to->name, !on_timeout ? " before timeout" : "");

  if (!CLK_ReadTimePair(tracking->to, tracking->from, &ts1, &ts2, &err, &quality))
    ;
  tracking->in_update = 0;

  return 1;
}

/* ================================================== */

int
CLK_ConvertTime(CLK_Instance from, int from_cooked, struct timespec *from_ts,
                CLK_Instance to, int to_cooked, struct timespec *to_ts, double *err)
{
  struct tracking *tracking;
  struct timespec ts1, ts2;
  double err1, err2, err3;

  if (from->id == to->id) {
    if (from_cooked == to_cooked) {
      *to_ts = *from_ts;
      if (err)
        *err = 0.0;
    } else if (!from_cooked && to_cooked) {
      CLK_CookTime(to, from_ts, to_ts, err);
    } else {
      return 0;
    }
    return 1;
  }

  tracking = get_tracking(from, to);

  if (!update_tracking(tracking, NULL))
    return 0;

  if (from_cooked) {
    ts1 = *from_ts;
    err1 = 0.0;
  } else {
    CLK_CookTime(from, from_ts, &ts1, &err1);
  }

  if (from == tracking->from && to == tracking->to) {
    if (!HCL_CookTime(tracking->hwclock, &ts1, &ts2, &err2))
      return 0;
  } else if (from == tracking->to && to == tracking->from) {
    if (!HCL_UncookTime(tracking->hwclock, &ts1, &ts2, &err2))
      return 0;
  } else {
    assert(0);
  }

  if (to_cooked) {
    *to_ts = ts2;
    err3 = 0.0;
  } else {
    uncook_time(to, &ts2, to_ts, &err3);
  }

  if (err)
    *err = err1 + err2 + err3;

  return 1;
}

/* ================================================== */

extern int CLK_ConvertInterval(CLK_Instance from, CLK_Instance to, double interval,
                               double *result, double *err)
{
  struct tracking *tracking;
  double freq;

  if (from->id == to->id) {
    *result = interval;
    return 1;
  }

  tracking = get_tracking(from, to);

  if (!HCL_GetFrequency(tracking->hwclock, &freq))
    return 0;

  if (from == tracking->from && to == tracking->to) {
    *result = interval * (1.0 - freq / 1.0e6);
    return 1;
  } else if (from == tracking->to && to == tracking->from) {
    *result = interval * (1.0 + freq / (1.0e6 + freq));
    return 1;
  } else {
    assert(0);
  }

  return 1;
}

/* ================================================== */

int
CLK_GetNumberOfClocks(void)
{
  return ARR_GetSize(clocks);
}

/* ================================================== */

int
CLK_GetReport(int index, RPT_ClockReport *report)
{
  struct timespec now;
  CLK_Instance inst;

  if (index < 0 || index >= ARR_GetSize(clocks))
    return 0;

  inst = get_record(index);

  CLK_ReadTime(inst, &now);

  report->name = CLK_GetName(inst);
  report->cook_offset = inst->cook_offset;
  report->precision = inst->precision;
  report->raw_offset = get_driver_offset(inst, 0, &now, NULL);
  report->last_time_adj = inst->last_time_offset;
  report->freq_ppm = CLK_GetFrequency(inst);
  report->last_freq_adj_ppm = inst->last_freq_offset * 1.0e6;

  return 1;
}
