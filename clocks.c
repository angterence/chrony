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
#include "hwclock.h"
#include "local.h"
#include "logging.h"
#include "memory.h"
#include "util.h"
#include "sys.h"
#include "sys_null.h"

#if defined(LINUX)
#include "sys_linux.h"
#endif

#define INVALID_CLOCK_FD -1

/* Minimum and maximum interval between PHC readings */
#define MIN_PHC_POLL -6
#define MAX_PHC_POLL 20

struct CLK_Instance_Record {
  char *name;
  clockid_t id;
  int fd;
  int system;
  int control;

  int reference_counter;

  struct clk_Driver driver;

  double frequency;

  int minpoll;
  int maxpoll;
  int min_samples;
  int max_samples;
  int nocrossts;

  HCL_Instance hwclock;
};

/* ================================================== */

static int control_allowed;

/* Array of CLK_Instance */
static ARR_Instance clocks;

static CLK_Instance main_clock;

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
  control_allowed = control;

  clocks = ARR_CreateInstance(sizeof (CLK_Instance));

  main_clock = CLK_Open("SYS:REALTIME", 1);
  if (!main_clock)
    LOG_FATAL("Could not open main clock");
}

/* ================================================== */

void
CLK_Finalise(void)
{
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

  inst->hwclock = NULL;

  /* Verify that the clock works */
  CLK_ReadTime(inst, &ts);

  SYS_InitialiseClockDriver(&inst->driver, inst, inst->control);

  ARR_AppendElement(clocks, &inst);

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
  //TODO
  if (inst->system)
    return 2e-9;
  else
    return 100e-9;
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

void
CLK_GetOffset(CLK_Instance inst, struct timespec *raw, double *corr, double *err)
{
  BRIEF_ASSERT(inst->driver.get_offset);

  inst->driver.get_offset(inst->driver.context, raw, corr, err);
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

  CLK_GetOffset(inst, raw, &offset, err);
  UTI_AddDoubleToTimespec(raw, offset, cooked);
}

/* ================================================== */

int
CLK_ConvertTime(CLK_Instance from, int from_cooked, struct timespec *from_ts,
                CLK_Instance to, int to_cooked, struct timespec *to_ts, double *err)
{
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

  return 0;
}

/* ================================================== */

extern int CLK_ConvertInterval(CLK_Instance from, CLK_Instance to, double interval,
                               double *result, double *err)
{
  return 0;
}
