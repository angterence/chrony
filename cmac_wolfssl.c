/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2019
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

  Support for AES128 and AES256 CMAC in Nettle.

  */

#include "config.h"

#include "logging.h"
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/cmac.h>

#include "cmac.h"
#include "memory.h"

struct CMC_Instance_Record {
  unsigned char key[AES_256_KEY_SIZE];
  int keySz;
};

int
CMC_GetKeyLength(CMC_Algorithm algorithm)
{
  int ret = 0;

  switch (algorithm) {
    case CMC_AES128:
      ret = AES_128_KEY_SIZE;
      break;
    case CMC_AES256:
      ret = AES_256_KEY_SIZE;
      break;
    default:
      break;
  }

  return ret;
}

/* ================================================== */

CMC_Instance
CMC_CreateInstance(CMC_Algorithm algorithm, const unsigned char *key,
                   int length)
{
  CMC_Instance inst;

  DEBUG_LOG("algorithm = %d, key = %p, length = %d",
    algorithm, key, length);

  if (length <= 0 || length != CMC_GetKeyLength(algorithm)) {
    LOG(LOGS_ERR, "Bad parameter.");
    return NULL;
  }

  inst = MallocNew(struct CMC_Instance_Record);
  if (!inst) { 
    LOG(LOGS_ERR, "MallocNew failed.");
    return NULL;
  }

  memcpy(inst->key, key, length);
  inst->keySz = length;

  return inst;
}

int
CMC_Hash(CMC_Instance inst, const void *in, int in_len, unsigned char *out,
         int out_len)
{
  /* 
   * We need to use a temporary buffer for the CMAC output because wolfCrypt
   * enforces a minimum tag size of 4 bytes. We need to ignore that constraint
   * for the purposes of this function (i.e. we want to permit tags < 4 bytes). 
   */
  unsigned char tmp[WC_CMAC_TAG_MAX_SZ];
  unsigned int tmpSz = WC_CMAC_TAG_MAX_SZ;

  DEBUG_LOG("inst = %p, in = %p, in_len = %d, out = %p, out_len = %d", inst, in,
    in_len, out, out_len);

  if (inst == NULL || in == NULL || in_len < 0 || out == NULL || out_len < 0) {
    LOG(LOGS_ERR, "Bad parameter.");
    return 0;
  }

  if (out_len > WC_CMAC_TAG_MAX_SZ) {
    out_len = WC_CMAC_TAG_MAX_SZ;
  }

  if (wc_AesCmacGenerate(tmp, &tmpSz, in, in_len, inst->key, inst->keySz) != 0) {
    LOG(LOGS_ERR, "wc_AesCmacGenerate failed.");
    return 0;
  }
  else {
    memcpy(out, tmp, out_len);
  }

  return out_len;
}

/* ================================================== */

void
CMC_DestroyInstance(CMC_Instance inst)
{
  Free(inst);
}
