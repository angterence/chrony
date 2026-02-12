/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009-2012, 2014-2015, 2017, 2025
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

  Driver for systems that implement the adjtimex()/ntp_adjtime() system call
  */

#include "config.h"

#include "sysincl.h"

#include "clocks_priv.h"
#include "conf.h"
#include "privops.h"
#include "sys_generic.h"
#include "sys_timex.h"
#include "logging.h"
#include "memory.h"
#include "util.h"

#ifdef PRIVOPS_ADJUSTTIMEX
#define NTP_ADJTIME PRV_AdjustTimex
#define NTP_ADJTIME_NAME "ntp_adjtime"
#else
#ifdef LINUX
#define NTP_ADJTIME adjtimex
#define NTP_ADJTIME_NAME "adjtimex"
#else
#define NTP_ADJTIME ntp_adjtime
#define NTP_ADJTIME_NAME "ntp_adjtime"
#endif
#endif

/* Maximum frequency offset accepted by the kernel (in ppm) */
#define MAX_FREQ 500.0

/* Frequency scale to convert from ppm to the timex freq */
#define FREQ_SCALE (double)(1 << 16)

/* Threshold for the timex maxerror when the kernel sets the UNSYNC flag */
#define MAX_SYNC_ERROR 16.0

/* Minimum assumed rate at which the kernel updates the clock frequency */
#define MIN_TICK_RATE 100

struct timex_context {
  /* Saved timex status */
  int status;

  /* Saved TAI-UTC offset */
  int tai_offset;

  long last_constant;
  long last_freq;
  int last_status;
};

/* ================================================== */

static double
convert_timex_frequency(const struct timex *txc)
{
  double freq_ppm;

  freq_ppm = txc->freq / FREQ_SCALE;

  return -freq_ppm;
}

/* ================================================== */

static double
read_frequency(clk_DriverContext *context)
{
  struct timex txc;

  txc.modes = 0;

  SYS_Timex_Adjust(context, &txc, 0);

  return convert_timex_frequency(&txc);
}

/* ================================================== */

static double
set_frequency(clk_DriverContext *context, double freq_ppm)
{
  struct timex txc;

  txc.modes = MOD_FREQUENCY;
  txc.freq = freq_ppm * -FREQ_SCALE;

  SYS_Timex_Adjust(context, &txc, 0);

  return convert_timex_frequency(&txc);
}

/* ================================================== */

static void
set_leap(clk_DriverContext *context, int leap, int tai_offset)
{
  struct timex_context *tc = context;
  struct timex txc;
  int applied, prev_status;

  txc.modes = 0;
  applied = SYS_Timex_Adjust(context, &txc, 0) == TIME_WAIT;

  prev_status = tc->status;
  tc->status &= ~(STA_INS | STA_DEL);

  if (leap > 0)
    tc->status |= STA_INS;
  else if (leap < 0)
    tc->status |= STA_DEL;

  txc.modes = MOD_STATUS;
  txc.status = tc->status;

#ifdef MOD_TAI
  if (tai_offset) {
    txc.modes |= MOD_TAI;
    txc.constant = tai_offset;

    if (applied && !(tc->status & (STA_INS | STA_DEL)))
      tc->tai_offset += prev_status & STA_INS ? 1 : -1;

    if (tc->tai_offset != tai_offset) {
      tc->tai_offset = tai_offset;
      LOG(LOGS_INFO, "System clock TAI offset set to %d seconds", tai_offset);
    }
  }
#endif

  SYS_Timex_Adjust(context, &txc, 0);

  if (prev_status != tc->status) {
    LOG(LOGS_INFO, "System clock status %s leap second",
        leap ? (leap > 0 ? "set to insert" : "set to delete") :
        (applied ? "reset after" : "set to not insert/delete"));
  }
}

/* ================================================== */

static void
set_sync_status(clk_DriverContext *context, int synchronised, double est_error,
                double max_error)
{
  struct timex_context *tc = context;
  struct timex txc;

  if (synchronised) {
    if (est_error > MAX_SYNC_ERROR)
      est_error = MAX_SYNC_ERROR;
    if (max_error >= MAX_SYNC_ERROR) {
      max_error = MAX_SYNC_ERROR;
      synchronised = 0;
    }
  } else {
    est_error = max_error = MAX_SYNC_ERROR;
  }

#ifdef LINUX
  /* On Linux clear the UNSYNC flag only if rtcsync is enabled */
  if (!CNF_GetRtcSync())
    synchronised = 0;
#endif

  if (synchronised)
    tc->status &= ~STA_UNSYNC;
  else
    tc->status |= STA_UNSYNC;

  txc.modes = MOD_STATUS | MOD_ESTERROR | MOD_MAXERROR;
  txc.status = tc->status;
  txc.esterror = est_error * 1.0e6;
  txc.maxerror = max_error * 1.0e6;

  if (SYS_Timex_Adjust(context, &txc, 1) < 0)
    ;
}

/* ================================================== */

static void
initialise_timex(struct timex_context *tc)
{
  struct timex txc;

  tc->status = STA_UNSYNC;
  tc->tai_offset = 0;
  tc->last_status = -1;

  /* Reset PLL offset */
  txc.modes = MOD_OFFSET | MOD_STATUS;
  txc.status = STA_PLL | tc->status;
  txc.offset = 0;
  SYS_Timex_Adjust(tc, &txc, 0);

  /* Turn PLL off */
  txc.modes = MOD_STATUS;
  txc.status = tc->status;
  SYS_Timex_Adjust(tc, &txc, 0);

#ifdef LINUX
  /* Reset adjtime() offset */
  txc.modes = ADJ_OFFSET_SINGLESHOT;
  txc.offset = 0;
  SYS_Timex_Adjust(tc, &txc, 0);
#endif
}

/* ================================================== */

void
SYS_Timex_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock,
                                double max_set_freq_ppm, double max_set_freq_delay,
                                double min_fastslew_offset, double max_fastslew_rate)
{
  struct timex_context *tc;

  if (CLK_GetId(clock) != CLOCK_REALTIME)
    LOG_FATAL("Unsupported clock");

  tc = MallocNew(struct timex_context);

  initialise_timex(tc);

  /* The lower drivers must not have their own context.  They will get the
     context of this driver to be able to call SYS_Timex_Adjust(), or they
     have to be stateless. */
  BRIEF_ASSERT(!driver->context);
  driver->context = tc;

  if (!driver->read_frequency)
    driver->read_frequency = read_frequency;
  if (!driver->set_frequency)
    driver->set_frequency = set_frequency;
  driver->set_leap = set_leap;
  driver->set_sync_status = set_sync_status;

  SYS_Generic_InitialiseClockDriver(driver, clock, max_set_freq_ppm, max_set_freq_delay,
                                    min_fastslew_offset, max_fastslew_rate);
}

/* ================================================== */

void
SYS_Timex_FinaliseClockDriver(struct clk_Driver *driver)
{
  SYS_Generic_FinaliseClockDriver(driver);
  Free(driver->context);
}

/* ================================================== */

int
SYS_Timex_Adjust(clk_DriverContext *context, struct timex *txc, int ignore_error)
{
  struct timex_context *tc = context;
  int state;

#ifdef SOLARIS
  /* The kernel seems to check the constant even when it's not being set */
  if (!(txc->modes & MOD_TIMECONST))
    txc->constant = 10;
#endif

  state = NTP_ADJTIME(txc);

  if (state < 0) {
    LOG(ignore_error ? LOGS_DEBUG : LOGS_FATAL,
        NTP_ADJTIME_NAME"(0x%x) failed : %s", txc->modes, strerror(errno));
    return state;
  }

  /* This a good place to verify that nothing else is touching the clock,
     without making an additional timex call.  A clock update is normally
     expected to have four driver calls:
     - set_sync_status - primarily updating leap status
     - set_frequency - correcting frequency error
     - set_frequency - correcting phase error
     - set_sync_status - updating leap and estimated/maximum error */
  if (tc->last_status != -1 &&
      (((tc->last_status ^ txc->status) & STA_PLL && !(txc->modes & MOD_STATUS)) ||
       (tc->last_constant != txc->constant && !(txc->modes & MOD_TIMECONST)) ||
       (tc->last_freq != txc->freq && !(txc->modes & MOD_FREQUENCY))))
    LOG(LOGS_WARN, "System clock interference detected (another NTP client?)");
  tc->last_status = txc->status;
  tc->last_constant = txc->constant;
  tc->last_freq = txc->freq;

  return state;
}
