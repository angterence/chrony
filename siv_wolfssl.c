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

  SIV ciphers using wolfSSL
  */

#include "config.h"

#include "sysincl.h"

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/aes.h>

#include "memory.h"
#include "siv.h"

struct SIV_Instance_Record {
  unsigned char key[AES_128_KEY_SIZE*2];
  int keySz;
};

/* ================================================== */

SIV_Instance
SIV_CreateInstance(SIV_Algorithm algorithm)
{
  SIV_Instance instance;

  if (algorithm != AEAD_AES_SIV_CMAC_256)
    return NULL;

  instance = MallocNew(struct SIV_Instance_Record);
  instance->keySz = 0;

  return instance;
}

/* ================================================== */

void
SIV_DestroyInstance(SIV_Instance instance)
{
  Free(instance);
}

/* ================================================== */

int
SIV_GetKeyLength(SIV_Algorithm algorithm)
{
  assert(32 <= SIV_MAX_KEY_LENGTH);

  if (algorithm == AEAD_AES_SIV_CMAC_256)
    return 32;
  return 0;
}

/* ================================================== */

int
SIV_SetKey(SIV_Instance instance, const unsigned char *key, int length)
{
  if (length != 32)
    return 0;

  memcpy(instance->key, key, length);
  instance->keySz = length;

  return 1;
}

/* ================================================== */

int
SIV_GetMinNonceLength(SIV_Instance instance)
{
    return 1;
}

/* ================================================== */

int
SIV_GetMaxNonceLength(SIV_Instance instance)
{
    return INT_MAX;
}

/* ================================================== */

int
SIV_GetTagLength(SIV_Instance instance)
{
  assert(AES_BLOCK_SIZE <= SIV_MAX_TAG_LENGTH);

  return AES_BLOCK_SIZE;
}

/* ================================================== */

int
SIV_Encrypt(SIV_Instance instance,
            const unsigned char *nonce, int nonce_length,
            const void *assoc, int assoc_length,
            const void *plaintext, int plaintext_length,
            unsigned char *ciphertext, int ciphertext_length)
{
  unsigned char iv[AES_BLOCK_SIZE];

  if (instance->keySz == 0)
    return 0;

  if (nonce_length <= 0 || assoc_length < 0 ||
      plaintext_length < 0 || plaintext_length > ciphertext_length ||
      plaintext_length + AES_BLOCK_SIZE != ciphertext_length)
    return 0;

  assert(assoc && plaintext);

  if (wc_AesSivEncrypt(instance->key, instance->keySz, assoc, assoc_length,
                       nonce, nonce_length, plaintext, plaintext_length, iv,
                       ciphertext + AES_BLOCK_SIZE) != 0) {
    return 0;
  }

  memcpy(ciphertext, iv, AES_BLOCK_SIZE);

  return 1;
}

/* ================================================== */

int
SIV_Decrypt(SIV_Instance instance,
            const unsigned char *nonce, int nonce_length,
            const void *assoc, int assoc_length,
            const unsigned char *ciphertext, int ciphertext_length,
            void *plaintext, int plaintext_length)
{
  if (instance->keySz == 0)
    return 0;

  if (nonce_length <= 0 || assoc_length < 0 ||
      plaintext_length < 0 || plaintext_length > ciphertext_length ||
      plaintext_length + AES_BLOCK_SIZE != ciphertext_length)
    return 0;

  assert(assoc && plaintext);

  if (wc_AesSivDecrypt(instance->key, instance->keySz, assoc, assoc_length,
                       nonce, nonce_length, ciphertext + AES_BLOCK_SIZE,
                       ciphertext_length - AES_BLOCK_SIZE,
                       (unsigned char*)ciphertext, plaintext) != 0) {
    return 0;
  }

  return 1;
}
