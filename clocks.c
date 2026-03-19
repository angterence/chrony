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

#if defined(LINUX)
#include "sys_linux.h"
#endif

#define INVALID_CLOCK_FD -1

/* Number of PHC readings per HW clock sample */
#define PHC_READINGS 25

/* Minimum and maximum interval between PHC readings */
#define MIN_PHC_POLL -6
#define MAX_PHC_POLL 20

#define NSEC_PER_SEC 1000000000

struct CLK_Instance_Record {
  char *name;
  clockid_t id;
  int fd;
  int system;
  int control;

  int reference_counter;

  struct clk_Driver driver;

  double precision;
  double frequency;

  int minpoll;
  int maxpoll;
  int min_samples;
  int max_samples;
  int nocrossts;

  CLK_Instance hwclock_reference;
  HCL_Instance hwclock;
  SCH_TimeoutID hwclock_timeout_id;
  int phc_mode;

  int in_poll;
};

/* ================================================== */

static int control_allowed;

/* Array of CLK_Instance */
static ARR_Instance clocks;

static CLK_Instance main_clock;
static CLK_Instance shadow_clock;

/* ================================================== */

static int poll_phc(CLK_Instance inst, struct timespec *now);

/* ================================================== */

/* Ask the system for the resolution of the system clock.  The Linux
   clock_getres() is not usable, because it reports the internal timer
   resolution, which is 1 ns when high-resolution timers are enabled,
   even when using a lower-resolution clocksource. */

static int
get_resolution(CLK_Instance inst)
{
#ifndef LINUX
  struct timespec res;

  if (clock_getres(CLK_GetId(inst), &res) < 0)
    return 0;

  return NSEC_PER_SEC * res.tv_sec + res.tv_nsec;
#else
  return 0;
#endif
}

/* ================================================== */

#if defined(LINUX) && defined(CLOCK_MONOTONIC_RAW)

static int
compare_ints(const void *a, const void *b)
{
  return *(const int *)a - *(const int *)b;
}

#define READINGS 64

/* On Linux, try to measure the actual resolution of the system clock by
   performing a varying amount of busy work between clock readings and finding
   the minimum change in the measured interval.  Require a change of at least
   two nanoseconds to ignore errors caused by conversion to timespec.  If
   measuring a system clock, use the raw monotonic clock to avoid the impact of
   potential frequency changes due to NTP adjustments made by other processes,
   and the kernel dithering of the 32-bit multiplier. */

static int
measure_resolution(CLK_Instance inst)
{
  int i, j, b, busy, diffs[READINGS - 1], diff2, min;
  struct timespec start_ts, ts[READINGS];
  uint32_t acc;
  clockid_t id;

  if (CLK_IsSystem(inst))
    id = CLOCK_MONOTONIC_RAW;
  else
    id = CLK_GetId(inst);

  if (clock_gettime(id, &start_ts) < 0)
    return 0;

  for (acc = 0, busy = 1; busy < 100000; busy = busy * 3 / 2 + 1) {
    for (i = 0, b = busy * READINGS; i < READINGS; i++, b -= busy) {
      if (clock_gettime(id, &ts[i]) < 0)
        return 0;

      for (j = b; j > 0; j--)
        acc += (acc & 1) + (uint32_t)ts[i].tv_nsec;
    }

    /* Give up after 0.1 seconds */
    if (UTI_DiffTimespecsToDouble(&ts[READINGS - 1], &start_ts) > 0.1) {
      DEBUG_LOG("Measurement too slow");
      return 0;
    }

    for (i = 0; i < READINGS - 1; i++) {
      diffs[i] = NSEC_PER_SEC * (ts[i + 1].tv_sec - ts[i].tv_sec) +
                 (ts[i + 1].tv_nsec - ts[i].tv_nsec);

      /* Make sure the differences are sane.  A resolution larger than the
         reading time will be measured in measure_clock_read_delay(). */
      if (diffs[i] <= 0 || diffs[i] > NSEC_PER_SEC)
        return 0;
    }

    /* Sort the differences and keep values unique within 1 ns from the
       first half of the array, which are less likely to be impacted by CPU
       interruptions */
    qsort(diffs, READINGS - 1, sizeof (diffs[0]), compare_ints);
    for (i = 1, j = 0; i < READINGS / 2; i++) {
      if (diffs[j] + 1 < diffs[i])
        diffs[++j] = diffs[i];
    }
    j++;

#if 0
    for (i = 0; i < j; i++)
      DEBUG_LOG("busy %d diff %d %d", busy, i, diffs[i]);
#endif

    /* Require at least three unique differences to be more confident
       with the result */
    if (j < 3)
      continue;

    /* Find the smallest difference between the unique differences */
    for (i = 1, min = 0; i < j; i++) {
      diff2 = diffs[i] - diffs[i - 1];
      if (min == 0 || min > diff2)
        min = diff2;
    }

    if (min == 0)
      continue;

    /* Prevent the compiler from optimising the busy work out */
    if (acc == 0)
      min += 1;

    return min;
  }

  return 0;
}

#else
static int
measure_clock_resolution(void)
{
  return 0;
}
#endif

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

  res = get_resolution(inst);
  if (res <= 0)
    res = measure_resolution(inst);

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

  conf_clocks = CNF_GetClocks();

  for (i = 0; i < ARR_GetSize(conf_clocks); i++) {
    conf = ARR_GetElement(conf_clocks, i);
    clock = CLK_Open(conf->name, !conf->free);
    if (!clock)
      LOG_FATAL("Could not open clock");

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
}

/* ================================================== */

void
CLK_Finalise(void)
{
  if (main_clock != shadow_clock)
    CLK_Close(shadow_clock);
  CLK_Close(main_clock);

  BRIEF_ASSERT(ARR_GetSize(clocks) == 0);
  ARR_DestroyInstance(clocks);
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
  inst->reference_counter = 1;

  if (strcmp(name, "SYS:REALTIME") == 0) {
    inst->id = CLOCK_REALTIME;
  } else if (strcmp(name, "SYS:MONOTONIC") == 0) {
    inst->id = CLOCK_MONOTONIC;
    inst->control = 0;
#ifdef CLOCK_MONOTONIC_RAW
  } else if (strcmp(name, "SYS:MONOTONIC_RAW") == 0) {
    inst->id = CLOCK_MONOTONIC_RAW;
    inst->control = 0;
#endif
#ifdef LINUX
  } else if (strncmp(name, "SYS:AUX", 7) == 0) {
    /* CLOCK_AUX0-CLOCK_AUX7 following linux/time.h */
    if (strlen(name + 7) != 1 || name[7] < '0' || name[7] > '7') {
      LOG(LOGS_ERR, "Invalid clock %s", name);
      goto error;
    }
    inst->id = 16 + (name[7] - '0');
#endif
#if defined(LINUX) && (defined(FEAT_PHC) || defined(HAVE_LINUX_TIMESTAMPING))
  } else if (strncmp(name, "PHC:", 4) == 0) {
    inst->fd = SYS_Linux_OpenPHC(name + 4, control ? O_RDWR : O_RDONLY);
    if (inst->fd < 0) {
      LOG(LOGS_ERR, "Could not open PHC clock %s", name + 4);
      goto error;
    }

    /* Following linux/posix-timers_types.h */
    inst->id = (((unsigned int)~inst->fd) << 3) | 3;
    inst->system = 0;
#endif
  } else {
    LOG(LOGS_ERR, "Unsupported clock %s", name);
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

  inst->minpoll = 0;
  inst->maxpoll = 1;
  inst->min_samples = 2;
  inst->max_samples = 16;
  inst->nocrossts = 0;

  inst->hwclock_reference = NULL;
  inst->hwclock = NULL;
  inst->hwclock_timeout_id = 0;
  inst->phc_mode = 0;

  inst->in_poll = 0;

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
  unsigned int i;

  if (inst->reference_counter > 1) {
    inst->reference_counter--;
    return;
  }

  for (i = 0; i < ARR_GetSize(clocks); i++) {
    if (get_record(i) == inst) {
      ARR_RemoveElement(clocks, i);
      break;
    }
  }

  SYS_FinaliseClockDriver(&inst->driver, inst->control);

  if (inst->hwclock)
    HCL_DestroyInstance(inst->hwclock);

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

static void
handle_change(CLK_Instance inst, struct timespec *cooked, double dfreq, double doffset)
{
  CLK_Instance inst2;
  unsigned int i;

  if (inst->hwclock) {
    HCL_SlewHw(inst->hwclock, cooked, dfreq, doffset);
  }

  for (i = 0; i < ARR_GetSize(clocks); i++) {
    inst2 = get_record(i);
    if (inst2->hwclock_reference != inst)
      continue;

    HCL_SlewLocal(inst2->hwclock, cooked, dfreq, doffset);
  }
}

/* ================================================== */

static double
set_frequency_and_accumulate_offset(CLK_Instance inst, double freq, double doffset,
                                    double corr_rate, struct timespec *cooked)
{
  double dfreq;

  BRIEF_ASSERT(inst->driver.set_frequency);
  BRIEF_ASSERT(inst->driver.accumulate_offset);

  //TODO: clamp freq

  freq = inst->driver.set_frequency(inst->driver.context, freq);
  if (doffset != 0.0)
    inst->driver.accumulate_offset(inst->driver.context, doffset, corr_rate);
  dfreq = (freq - inst->frequency) / (1.0e6 - inst->frequency);

  DEBUG_LOG("%s: old_freq=%+.3f new_freq=%+.3f offset=%.9f",
            inst->name, inst->frequency, freq, doffset);

  inst->frequency = freq;

  handle_change(inst, cooked, dfreq, doffset);

  return dfreq;
}

/* ================================================== */


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


  return r;
}

/* ================================================== */

double
CLK_GetOffset(CLK_Instance inst, int reverse, struct timespec *ts, double *err)
{
  BRIEF_ASSERT(inst->driver.get_offset);

  return inst->driver.get_offset(inst->driver.context, reverse, ts, err);
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

  offset = CLK_GetOffset(inst, 0, raw, err);
  UTI_AddDoubleToTimespec(raw, offset, cooked);
}

/* ================================================== */

static void
uncook_time(CLK_Instance inst, struct timespec *cooked, struct timespec *raw, double *err)
{
  double offset;

  offset = CLK_GetOffset(inst, 1, cooked, err);
  UTI_AddDoubleToTimespec(cooked, -offset, raw);
}

/* ================================================== */

static void
poll_timeout(void *arg)
{
  CLK_Instance inst = arg;
  struct timespec now;

  inst->hwclock_timeout_id = 0;

  SCH_GetLastEventTime(&now, NULL, NULL);
  poll_phc(inst, &now);
}

/* ================================================== */

static int
poll_phc(CLK_Instance inst, struct timespec *now)
{
  struct timespec sample_phc_ts, sample_sys_ts, sample_local_ts;
  struct timespec phc_readings[PHC_READINGS][3], ts;
  double phc_err, local_err, hw_err, interval;
  int n_readings, quality;

  if (!SCH_IsInitialised())
    return 0;

  if (inst->in_poll)
    return 0;
  inst->in_poll = 1;

  if (!inst->hwclock)
    inst->hwclock = HCL_CreateInstance(inst, inst->hwclock_reference,
                                       inst->min_samples, inst->max_samples,
                                       UTI_Log2ToDouble(inst->minpoll),
                                       CLK_GetPrecision(inst));

  if (!now) {
    now = &ts;
    CLK_ReadTime(inst->hwclock_reference, now);
    CLK_CookTime(inst->hwclock_reference, now, now, NULL);
  }

  if (!HCL_NeedsNewSample(inst->hwclock, now)) {
    inst->in_poll = 0;
    return 1;
  }

  DEBUG_LOG("Polling PHC on %s%s",
            inst->name, inst->hwclock_timeout_id != 0 ? " before timeout" : "");

  n_readings = SYS_Linux_GetPHCReadings(inst->fd, inst->hwclock_reference->id, inst->nocrossts,
                                        &inst->phc_mode, PHC_READINGS, phc_readings);

  /* Add timeout for the next poll in case no HW timestamp will be captured
     between the minpoll and maxpoll.  Separate reading of different PHCs to
     avoid long intervals between handling I/O events. */
  SCH_RemoveTimeout(inst->hwclock_timeout_id);
  interval = UTI_Log2ToDouble(inst->maxpoll);
  inst->hwclock_timeout_id = SCH_AddTimeoutInClass(interval, interval /
                                                     ARR_GetSize(clocks) / 4, 0.1,
                                                   SCH_PhcPollClass, poll_timeout, inst);

  if (n_readings <= 0) {
    inst->in_poll = 0;
    return 0;
  }

  if (!HCL_ProcessReadings(inst->hwclock, n_readings, phc_readings,
                           &sample_phc_ts, &sample_sys_ts, &phc_err, &quality) ||
      quality <= 0) {
    inst->in_poll = 0;
    return 1;
  }

  CLK_CookTime(inst->hwclock_reference, &sample_sys_ts, &sample_local_ts, &local_err);
  CLK_CookTime(inst, &sample_phc_ts, &sample_phc_ts, &hw_err);
  HCL_AccumulateSample(inst->hwclock, &sample_phc_ts, &sample_local_ts,
                       phc_err + local_err + hw_err);

  inst->in_poll = 0;

  return 1;
}

/* ================================================== */

int
CLK_ConvertTime(CLK_Instance from, int from_cooked, struct timespec *from_ts,
                CLK_Instance to, int to_cooked, struct timespec *to_ts, double *err)
{
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

  if (!from->system && to->system) {
    if (!from->hwclock_reference)
      from->hwclock_reference = to;
    if (from->hwclock_reference != to)
      LOG_FATAL("Conflict");

    if (!poll_phc(from, NULL))
      return 0;

    if (from_cooked) {
      ts1 = *from_ts;
      err1 = 0.0;
    } else {
      CLK_CookTime(from, from_ts, &ts1, &err1);
    }

    if (!HCL_CookTime(from->hwclock, &ts1, &ts2, &err2))
      return 0;

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

  if (from->system && !to->system) {
    if (!to->hwclock_reference)
      to->hwclock_reference = from;
    if (to->hwclock_reference != from)
      LOG_FATAL("Conflict");

    if (!poll_phc(to, NULL))
      return 0;

    if (from_cooked) {
      ts1 = *from_ts;
      err1 = 0.0;
    } else {
      CLK_CookTime(from, from_ts, &ts1, &err1);
    }

    if (!HCL_UncookTime(to->hwclock, &ts1, &ts2, &err2))
      return 0;

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

  return 0;
}

/* ================================================== */

extern int CLK_ConvertInterval(CLK_Instance from, CLK_Instance to, double interval,
                               double *result, double *err)
{
  return 0;
}
