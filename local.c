/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011, 2014-2015, 2025
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

  The routines in this file present a common local (system) clock
  interface to the rest of the software.

  They interface with the system specific driver files in sys_*.c
  */

#include "config.h"

#include "sysincl.h"

#include "conf.h"
#include "clocks.h"
#include "local.h"
#include "memory.h"
#include "smooth.h"
#include "util.h"
#include "logging.h"

/* ================================================== */

/* Variable to store the current frequency, in ppm */
static double current_freq_ppm;

/* Maximum allowed frequency, in ppm */
static double max_freq_ppm;

/* Temperature compensation, in ppm */
static double temp_comp_ppm;

/* ================================================== */

static CLK_Instance local_clock;

/* ================================================== */

/* Types and variables associated with handling the parameter change
   list */

typedef struct _ChangeListEntry {
  struct _ChangeListEntry *next;
  struct _ChangeListEntry *prev;
  LCL_ParameterChangeHandler handler;
  void *anything;
} ChangeListEntry;

static ChangeListEntry change_list;

/* ================================================== */

/* Types and variables associated with handling the parameter change
   list */

typedef struct _DispersionNotifyListEntry {
  struct _DispersionNotifyListEntry *next;
  struct _DispersionNotifyListEntry *prev;
  LCL_DispersionNotifyHandler handler;
  void *anything;
} DispersionNotifyListEntry;

static DispersionNotifyListEntry dispersion_notify_list;

/* ================================================== */

static int precision_log;
static double precision_quantum;

static double max_clock_error;

/* ================================================== */

void
LCL_Initialise(void)
{
  change_list.next = change_list.prev = &change_list;

  dispersion_notify_list.next = dispersion_notify_list.prev = &dispersion_notify_list;

  /* This ought to be set from the system driver layer */
  current_freq_ppm = 0.0;
  temp_comp_ppm = 0.0;

  local_clock = CLK_GetMainClock();

  current_freq_ppm = CLK_GetFrequency(local_clock);

  DEBUG_LOG("Local freq=%.3fppm", current_freq_ppm);

  precision_quantum = CLK_GetPrecision(local_clock);
  precision_log = round(log(precision_quantum) / log(2.0));
  /* NTP code doesn't support smaller log than -30 */
  assert(precision_log >= -30);

  /* This is the maximum allowed frequency offset in ppm, the time must
     never stop or run backwards */
  max_freq_ppm = CNF_GetMaxDrift();
  max_freq_ppm = CLAMP(0.0, max_freq_ppm, 100000.0);

  max_clock_error = CNF_GetMaxClockError() * 1e-6;
}

/* ================================================== */

void
LCL_Finalise(void)
{
  /* Make sure all handlers have been removed */
  BRIEF_ASSERT(change_list.next == &change_list);
  BRIEF_ASSERT(dispersion_notify_list.next == &dispersion_notify_list);

  local_clock = NULL;
}

/* ================================================== */

/* Routine to read the system precision as a log to base 2 value. */
int
LCL_GetSysPrecisionAsLog(void)
{
  return precision_log;
}

/* ================================================== */
/* Routine to read the system precision in terms of the actual time step */

double
LCL_GetSysPrecisionAsQuantum(void)
{
  return precision_quantum;
}

/* ================================================== */

double
LCL_GetMaxClockError(void)
{
  return max_clock_error;
}

/* ================================================== */

void
LCL_AddParameterChangeHandler(LCL_ParameterChangeHandler handler, void *anything)
{
  ChangeListEntry *ptr, *new_entry;

  /* Check that the handler is not already registered */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    BRIEF_ASSERT(ptr->handler != handler || ptr->anything != anything);
  }

  new_entry = MallocNew(ChangeListEntry);

  new_entry->handler = handler;
  new_entry->anything = anything;

  /* Chain it into the list */
  new_entry->next = &change_list;
  new_entry->prev = change_list.prev;
  change_list.prev->next = new_entry;
  change_list.prev = new_entry;
}

/* ================================================== */

/* Remove a handler */
void LCL_RemoveParameterChangeHandler(LCL_ParameterChangeHandler handler, void *anything)
{

  ChangeListEntry *ptr;
  int ok;

  ptr = NULL;
  ok = 0;

  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    if (ptr->handler == handler && ptr->anything == anything) {
      ok = 1;
      break;
    }
  }

  assert(ok);

  /* Unlink entry from the list */
  ptr->next->prev = ptr->prev;
  ptr->prev->next = ptr->next;

  Free(ptr);
}

/* ================================================== */

int
LCL_IsFirstParameterChangeHandler(LCL_ParameterChangeHandler handler)
{
  return change_list.next->handler == handler;
}

/* ================================================== */

void
LCL_MakeLastParameterChangeHandlerFirst(void)
{
  ChangeListEntry *ptr;

  BRIEF_ASSERT(change_list.next != &change_list);

  /* Remove the last handler from the list */
  ptr = change_list.prev;
  ptr->next->prev = ptr->prev;
  ptr->prev->next = ptr->next;

  /* And add it back to the beginning */
  ptr->prev = &change_list;
  ptr->next = change_list.next;
  change_list.next->prev = ptr;
  change_list.next = ptr;
}

/* ================================================== */

static void
invoke_parameter_change_handlers(struct timespec *raw, struct timespec *cooked,
                                 double dfreq, double doffset,
                                 LCL_ChangeType change_type)
{
  ChangeListEntry *ptr;

  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    (ptr->handler)(raw, cooked, dfreq, doffset, change_type, ptr->anything);
  }
}

/* ================================================== */

void
LCL_AddDispersionNotifyHandler(LCL_DispersionNotifyHandler handler, void *anything)
{
  DispersionNotifyListEntry *ptr, *new_entry;

  /* Check that the handler is not already registered */
  for (ptr = dispersion_notify_list.next; ptr != &dispersion_notify_list; ptr = ptr->next) {
    BRIEF_ASSERT(ptr->handler != handler || ptr->anything != anything);
  }

  new_entry = MallocNew(DispersionNotifyListEntry);

  new_entry->handler = handler;
  new_entry->anything = anything;

  /* Chain it into the list */
  new_entry->next = &dispersion_notify_list;
  new_entry->prev = dispersion_notify_list.prev;
  dispersion_notify_list.prev->next = new_entry;
  dispersion_notify_list.prev = new_entry;
}

/* ================================================== */

/* Remove a handler */
extern 
void LCL_RemoveDispersionNotifyHandler(LCL_DispersionNotifyHandler handler, void *anything)
{

  DispersionNotifyListEntry *ptr;
  int ok;

  ptr = NULL;
  ok = 0;

  for (ptr = dispersion_notify_list.next; ptr != &dispersion_notify_list; ptr = ptr->next) {
    if (ptr->handler == handler && ptr->anything == anything) {
      ok = 1;
      break;
    }
  }

  assert(ok);

  /* Unlink entry from the list */
  ptr->next->prev = ptr->prev;
  ptr->prev->next = ptr->next;

  Free(ptr);
}

/* ================================================== */

CLK_Instance
LCL_GetLocalClock(void)
{
  return local_clock;
}

/* ================================================== */

void
LCL_ReadRawTime(struct timespec *ts)
{
  CLK_ReadTime(local_clock, ts);
}

/* ================================================== */

void
LCL_ReadCookedTime(struct timespec *result, double *err)
{
  struct timespec raw;

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, result, err);
}

/* ================================================== */

void
LCL_CookTime(struct timespec *raw, struct timespec *cooked, double *err)
{
  double correction;

  LCL_GetOffsetCorrection(raw, &correction, err);
  UTI_AddDoubleToTimespec(raw, correction, cooked);
}

/* ================================================== */

void
LCL_GetOffsetCorrection(struct timespec *raw, double *correction, double *err)
{
  /* Call system specific driver to get correction */
  CLK_GetOffset(local_clock, raw, correction, err);
}

/* ================================================== */
/* Return current frequency */

double
LCL_ReadAbsoluteFrequency(void)
{
  double freq;

  freq = current_freq_ppm; 

  /* Undo temperature compensation */
  if (temp_comp_ppm != 0.0) {
    freq = (freq + temp_comp_ppm) / (1.0 - 1.0e-6 * temp_comp_ppm);
  }

  return freq;
}

/* ================================================== */

static double
clamp_freq(double freq)
{
  if (freq <= max_freq_ppm && freq >= -max_freq_ppm)
    return freq;

  LOG(LOGS_WARN, "Frequency %.1f ppm exceeds allowed maximum", freq);

  return CLAMP(-max_freq_ppm, freq, max_freq_ppm);
}

/* ================================================== */

static int
check_offset(struct timespec *now, double offset)
{
  /* Check if the time will be still sane with accumulated offset */
  if (UTI_IsTimeOffsetSane(now, -offset))
      return 1;

  LOG(LOGS_WARN, "Adjustment of %.1f seconds is invalid", -offset);
  return 0;
}

/* ================================================== */

/* This involves both setting the absolute frequency with the
   system-specific driver, as well as calling all notify handlers */

void
LCL_SetAbsoluteFrequency(double afreq_ppm)
{
  struct timespec raw, cooked;
  double dfreq;
  
  afreq_ppm = clamp_freq(afreq_ppm);

  /* Apply temperature compensation */
  if (temp_comp_ppm != 0.0) {
    afreq_ppm = afreq_ppm * (1.0 - 1.0e-6 * temp_comp_ppm) - temp_comp_ppm;
  }

  /* Call the system-specific driver for setting the frequency */
  
  dfreq = CLK_SetFrequency(local_clock, afreq_ppm);

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, dfreq, 0.0, LCL_ChangeAdjust);

  current_freq_ppm = CLK_GetFrequency(local_clock);
}

/* ================================================== */

void
LCL_AccumulateDeltaFrequency(double dfreq)
{
  LCL_AccumulateFrequencyAndOffset(dfreq, 0.0, 0.0);
}

/* ================================================== */

int
LCL_AccumulateOffset(double offset, double corr_rate)
{
  return LCL_AccumulateFrequencyAndOffset(0.0, offset, corr_rate);
}

/* ================================================== */

int
LCL_ApplyStepOffset(double offset)
{
  struct timespec raw, cooked;

  /* In this case, the cooked time to be passed to the notify clients
     has to be the cooked time BEFORE the change was made */

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  if (!check_offset(&raw, offset))
      return 0;

  if (!CLK_ApplyStepOffset(local_clock, offset)) {
    LOG(LOGS_ERR, "Could not step system clock");
    return 0;
  }

  /* Reset smoothing on all clock steps */
  SMT_Reset(&cooked);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, 0.0, offset, LCL_ChangeStep);

  return 1;
}

/* ================================================== */

void
LCL_NotifyExternalTimeStep(struct timespec *raw, struct timespec *cooked,
    double offset, double dispersion)
{
  LCL_CancelOffsetCorrection();

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(raw, cooked, 0.0, offset, LCL_ChangeUnknownStep);

  LCL_InvokeDispersionNotifyHandlers(dispersion);
}

/* ================================================== */

void
LCL_NotifyLeap(int leap)
{
  struct timespec raw, cooked;

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  /* Smooth the leap second out */
  SMT_Leap(&cooked, leap);

  /* Dispatch to all handlers as if the clock was stepped */
  invoke_parameter_change_handlers(&raw, &cooked, 0.0, -leap, LCL_ChangeStep);
}

/* ================================================== */

int
LCL_AccumulateFrequencyAndOffset(double dfreq, double doffset, double corr_rate)
{
  struct timespec raw, cooked;

  LCL_ReadRawTime(&raw);
  /* Due to modifying the offset, this has to be the cooked time prior
     to the change we are about to make */
  LCL_CookTime(&raw, &cooked, NULL);

  if (!check_offset(&cooked, doffset))
    return 0;

  /* Call the system-specific driver for setting the frequency */
  dfreq = CLK_AccumulateFrequencyAndOffset(local_clock, dfreq, doffset, corr_rate);
  current_freq_ppm = CLK_GetFrequency(local_clock);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, dfreq, doffset, LCL_ChangeAdjust);

  return 1;
}

/* ================================================== */

int
LCL_AccumulateFrequencyAndOffsetNoHandlers(double dfreq, double doffset, double corr_rate)
{
  ChangeListEntry *first_handler;
  int r;

  first_handler = change_list.next;
  change_list.next = &change_list;

  r = LCL_AccumulateFrequencyAndOffset(dfreq, doffset, corr_rate);

  change_list.next = first_handler;

  return r;
}

/* ================================================== */

void
LCL_InvokeDispersionNotifyHandlers(double dispersion)
{
  DispersionNotifyListEntry *ptr;

  for (ptr = dispersion_notify_list.next; ptr != &dispersion_notify_list; ptr = ptr->next) {
    (ptr->handler)(dispersion, ptr->anything);
  }

}

/* ================================================== */
/* Look at the current difference between the system time and the NTP
   time, and make a step to cancel it. */

int
LCL_MakeStep(void)
{
  struct timespec raw;
  double correction;

  LCL_ReadRawTime(&raw);
  LCL_GetOffsetCorrection(&raw, &correction, NULL);

  if (!check_offset(&raw, -correction))
      return 0;

  /* Cancel remaining slew and make the step */
  LCL_AccumulateOffset(correction, 0.0);
  if (!LCL_ApplyStepOffset(-correction)) {
    /* Revert the correction */
    LCL_AccumulateOffset(-correction, 0.0);
    return 0;
  }

  LOG(LOGS_WARN, "System clock was stepped by %.6f seconds", correction);

  return 1;
}

/* ================================================== */

void
LCL_CancelOffsetCorrection(void)
{
  struct timespec raw;
  double correction;

  LCL_ReadRawTime(&raw);
  LCL_GetOffsetCorrection(&raw, &correction, NULL);
  LCL_AccumulateOffset(correction, 0.0);
}

/* ================================================== */

int
LCL_CanSystemLeap(void)
{
  return CLK_CanLeap(local_clock);
}

/* ================================================== */

void
LCL_SetSystemLeap(int leap, int tai_offset)
{
  CLK_SetLeap(local_clock, leap, tai_offset);
}

/* ================================================== */

double
LCL_SetTempComp(double comp)
{
  double uncomp_freq_ppm;

  if (temp_comp_ppm == comp)
    return comp;

  /* Undo previous compensation */
  current_freq_ppm = (current_freq_ppm + temp_comp_ppm) /
    (1.0 - 1.0e-6 * temp_comp_ppm);

  uncomp_freq_ppm = current_freq_ppm;

  /* Apply new compensation */
  current_freq_ppm = current_freq_ppm * (1.0 - 1.0e-6 * comp) - comp;

  /* Call the system-specific driver for setting the frequency */
  CLK_SetFrequency(local_clock, current_freq_ppm);
  current_freq_ppm = CLK_GetFrequency(local_clock);

  temp_comp_ppm = (uncomp_freq_ppm - current_freq_ppm) /
    (1.0e-6 * uncomp_freq_ppm + 1.0);

  return temp_comp_ppm;
}

/* ================================================== */

void
LCL_SetSyncStatus(int synchronised, double est_error, double max_error)
{
  CLK_SetSyncStatus(local_clock, synchronised, est_error, max_error);
}

/* ================================================== */
