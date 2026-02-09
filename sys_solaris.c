/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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

  Driver file for illumos operating system (previously Solaris)
  */

#include "config.h"

#include "sysincl.h"

#include "clocks_priv.h"
#include "logging.h"
#include "privops.h"
#include "sys_solaris.h"
#include "sys_timex.h"
#include "util.h"

#include <kvm.h>
#include <nlist.h>

/* ================================================== */

static void
set_dosynctodr(int on_off)
{
  struct nlist nl[] = { {"dosynctodr"}, {NULL} };
  kvm_t *kt;

  kt = kvm_open(NULL, NULL, NULL, O_RDWR, NULL);
  if (!kt)
    LOG_FATAL("Could not open kvm");

  if (kvm_nlist(kt, nl) < 0 || !nl[0].n_value)
    LOG_FATAL("Could not get dosynctodr address");

  if (kvm_kwrite(kt, nl[0].n_value, &on_off, sizeof (on_off)) < 0)
    LOG_FATAL("Could not write to dosynctodr");

  kvm_close(kt);
}

/* ================================================== */

void
SYS_Solaris_InitialiseClockDriver(struct clk_Driver *driver, CLK_Instance clock)
{
  if (CLK_GetId(clock) != CLOCK_REALTIME)
    LOG_FATAL("Unsupported clock");

  /* The kernel keeps the system clock and hardware clock synchronised to each
     other.  The dosynctodr variable needs to be set to zero to prevent the
     the system clock from following the hardware clock when the system clock
     is not adjusted by adjtime() or ntp_adjtime(modes=MOD_OFFSET). */
  set_dosynctodr(0);

  /* The kernel allows the frequency to be set in the full range off int32_t */
  SYS_Timex_InitialiseClockDriver(driver, clock, 32500, 1.0 / 100, 0.0, 0.0);
}

/* ================================================== */

void
SYS_Solaris_FinaliseClockDriver(struct clk_Driver *driver)
{
  SYS_Timex_FinaliseClockDriver(driver);
}

/* ================================================== */

#ifdef FEAT_PRIVDROP
void
SYS_Solaris_DropRoot(uid_t uid, gid_t gid, SYS_ProcessContext context)
{
  if (context == SYS_MAIN_PROCESS)
    PRV_StartHelper();
  UTI_DropRoot(uid, gid);
}
#endif
