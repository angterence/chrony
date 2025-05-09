/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2018
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MEretHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  Routines implementing crypto hashing using the nettle library.

  */

#include "config.h"

#include "logging.h"
#include "hash.h"
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/hash.h>

int
HSH_GetHashId(HSH_Algorithm algorithm)
{
  switch (algorithm) {
    case HSH_MD5:
      return WC_HASH_TYPE_MD5;
    case HSH_SHA1:
      return WC_HASH_TYPE_SHA;
    case HSH_SHA256:
      return WC_HASH_TYPE_SHA256;
    case HSH_SHA384:
      return WC_HASH_TYPE_SHA384;
    case HSH_SHA512:
      return WC_HASH_TYPE_SHA512;
    case HSH_SHA3_224:
      return WC_HASH_TYPE_SHA3_224;
    case HSH_SHA3_256:
      return WC_HASH_TYPE_SHA3_256;
    case HSH_SHA3_384:
      return WC_HASH_TYPE_SHA3_384;
    case HSH_SHA3_512:
      return WC_HASH_TYPE_SHA3_512;
    default:
      return -1;
  }
}

int
HSH_Hash(int id, const void *in1, int in1_len, const void *in2, int in2_len,
         unsigned char *out, int out_len)
{
  int hashSize;
  wc_HashAlg hash;
  int ret = out_len;
  unsigned char tmp[WC_MAX_DIGEST_SIZE];

  if (in1_len < 0 || in2_len < 0 || out_len <= 0)
    return 0;

  hashSize = wc_HashGetDigestSize(id);
  if (hashSize <= 0) {
    LOG(LOGS_ERR, "wc_HashGetDigestSize for ID %d failed.", id);
    ret = 0;
  }

  if (ret != 0) {
    if (wc_HashInit(&hash, id) != 0) {
      LOG(LOGS_ERR, "wc_HashInit for ID %d failed.", id);
      ret = 0;
    }
  }

  if (ret != 0) {
    if (wc_HashUpdate(&hash, id, in1, in1_len) != 0) {
      LOG(LOGS_ERR, "wc_HashUpdate 1 for ID %d failed.", id);
      ret = 0;
    }
  }

  if (ret != 0 && in2) {
    if (wc_HashUpdate(&hash, id, in2, in2_len) != 0) {
      LOG(LOGS_ERR, "wc_HashUpdate 2 for ID %d failed.", id);
      ret = 0;
    }
  }

  if (ret != 0) {
    if (wc_HashFinal(&hash, id, tmp) != 0) {
      LOG(LOGS_ERR, "wc_HashFinal for ID %d failed.", id);
      ret = 0;
    }
    else {
      if (out_len > hashSize) {
        out_len = hashSize;
        ret = out_len;
      }
      memcpy(out, tmp, out_len);
    }
  }

  wc_HashFree(&hash, id);

  return ret;
}

void
HSH_Finalise(void)
{
}
