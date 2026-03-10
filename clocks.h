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

  Header file for clock abstraction and conversions.

  */

#ifndef GOT_CLOCKS_H
#define GOT_CLOCKS_H

struct clk_Driver;
typedef void clk_DriverContext;

typedef struct CLK_Instance_Record *CLK_Instance;

extern void CLK_Initialise(int control);

extern void CLK_Finalise(void);

extern int CLK_IsAnyControlled(void);

extern CLK_Instance CLK_GetMainClock(void);

extern CLK_Instance CLK_GetShadowClock(void);

/* name is CLOCK_REALTIME, CLOCK_MONOTONIC, /dev/ptp*, interface name */
extern CLK_Instance CLK_Open(const char *name, int control);

extern void CLK_Close(CLK_Instance inst);

extern void CLK_SetPoll(CLK_Instance inst, int minpoll, int maxpoll);

extern void CLK_SetSamples(CLK_Instance inst, int min_samples, int max_samples);

extern void CLK_DisableCrossTimestamping(CLK_Instance inst);

extern clockid_t CLK_GetId(CLK_Instance inst);

extern int CLK_GetFd(CLK_Instance inst);

extern const char *CLK_GetName(CLK_Instance inst);

extern int CLK_IsSystem(CLK_Instance inst);

extern double CLK_GetPrecision(CLK_Instance inst);

extern void CLK_ReadTime(CLK_Instance inst, struct timespec *ts);

extern double CLK_GetFrequency(CLK_Instance inst);

extern double CLK_SetFrequency(CLK_Instance inst, double freq);

extern double CLK_AccumulateFrequencyAndOffset(CLK_Instance inst, double dfreq,
                                               double offset, double corr_rate);

extern double CLK_GetOffset(CLK_Instance inst, int direction, struct timespec *ts, double *err);

extern int CLK_ApplyStepOffset(CLK_Instance inst, double offset);

extern int CLK_CanLeap(CLK_Instance inst);

extern void CLK_SetLeap(CLK_Instance inst, int leap, int tai_offset);

extern void CLK_SetSyncStatus(CLK_Instance inst, int synchronised, double est_error,
                              double max_error);

extern void CLK_AddDispersion(CLK_Instance inst, double dispersion);

extern void CLK_CookTime(CLK_Instance inst, struct timespec *raw, struct timespec *cooked,
                         double *err);
extern int CLK_ConvertTime(CLK_Instance from, int from_cooked, struct timespec *from_ts,
                           CLK_Instance to, int to_cooked, struct timespec *to_ts, double *err);
extern int CLK_ConvertInterval(CLK_Instance from, CLK_Instance to, double interval,
                               double *result, double *err);

#endif
