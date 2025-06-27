#include "config.h"
#include "conf.h"
#include "logging.h"
#include "tls.h"
#include "util.h"
#include "memory.h"

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/asn.h>

struct TLS_Instance_Record
{
  WOLFSSL *session;
  WOLFSSL_CTX *ctx;
  int server;
  char *server_name;
  char *alpn_name;
};

typedef struct WolfsslCreds {
    char** certs;
    char** keys;
    int n_certs_keys;
    char** trusted_certs;
    int n_trusted_certs;
} WolfsslCreds;

/* ================================================== */

static const size_t MAX_FILE_NAME_SZ = 255;

/* ================================================== */

int
TLS_Initialise(time_t (*get_time)(time_t *t))
{
  int r = wolfSSL_Init();
  if (r != WOLFSSL_SUCCESS) {
    LOG_FATAL("wolfSSL_Init() failed.");
    return 0;
  }

  /* Use our clock instead of the system clock in certificate verification */
  wc_SetTimeCb(get_time);
  return 1;
}

/* ================================================== */

void
TLS_Finalise(void)
{
  wolfSSL_Cleanup();
}

/* ================================================== */

static void
free_credentials(WolfsslCreds* creds) {
  int i;

  if (creds != NULL) {
    if (creds->trusted_certs != NULL) {
      for (i = 0; i < creds->n_trusted_certs; ++i) {
        if (creds->trusted_certs[i] != NULL) {
          free(creds->trusted_certs[i]);
        }
      }
      free(creds->trusted_certs);
    }
    if (creds->keys != NULL) {
      for (i = 0; i < creds->n_certs_keys; ++i) {
        if (creds->keys[i] != NULL) {
          free(creds->keys[i]);
        }
      }
      free(creds->keys);
    }
    if (creds->certs != NULL) {
      for (i = 0; i < creds->n_certs_keys; ++i) {
        if (creds->certs[i] != NULL) {
          free(creds->certs[i]);
        }
      }
      free(creds->certs);
    }
    free(creds);
  }
}

/* ================================================== */

static WolfsslCreds*
allocate_credentials(int n_certs_keys, int n_trusted_certs) {
  WolfsslCreds* creds;
  int err = 0;
  int i;

  creds = (WolfsslCreds*)malloc(sizeof (WolfsslCreds));
  if (creds == NULL) {
    err = 1;
  }
  else {
    memset(creds, 0, sizeof (WolfsslCreds));
  }
  if (!err && n_certs_keys > 0) {
    creds->certs = (char**)malloc(n_certs_keys * sizeof (char*));
    if (creds->certs ==  NULL) {
      err = 1;
    }
    else {
      memset(creds->certs, 0, n_certs_keys * sizeof (char*));
      for (i = 0; i < n_certs_keys; ++i) {
        creds->certs[i] = (char*)malloc(MAX_FILE_NAME_SZ);
        if (creds->certs[i] == NULL) {
          err = 1;
          break;
        }
      }
    }
    if (!err) {
      creds->keys = (char**)malloc(n_certs_keys * sizeof (char*));
      if (creds->keys ==  NULL) {
        err = 1;
      }
      else {
        memset(creds->keys, 0, n_certs_keys * sizeof (char*));
        for (i = 0; i < n_certs_keys; ++i) {
          creds->keys[i] = (char*)malloc(MAX_FILE_NAME_SZ);
          if (creds->keys[i] == NULL) {
            err = 1;
            break;
          }
        }
      }
    }
  }
  if (!err && n_trusted_certs > 0) {
    creds->trusted_certs = (char**)malloc(n_trusted_certs * sizeof (char*));
    if (creds->trusted_certs == NULL) {
      err = 1;
    }
    else {
      memset(creds->trusted_certs, 0, n_trusted_certs * sizeof (char*));
      for (i = 0; i < n_trusted_certs; ++i) {
        creds->trusted_certs[i] = (char*)malloc(MAX_FILE_NAME_SZ);
        if (creds->trusted_certs[i] == NULL) {
          err = 1;
          break;
        }
      }
    }
  }

  if (err) {
    free_credentials(creds);
    creds = NULL;
  }

  return creds;
}

/* ================================================== */

TLS_Credentials
TLS_CreateCredentials(const char **certs, const char **keys, int n_certs_keys,
                      const char **trusted_certs, uint32_t *trusted_certs_ids,
                      int n_trusted_certs, uint32_t trusted_cert_set)
{
  WolfsslCreds* creds;
  int err = 0;
  int i;
  int j = 0;
  struct stat stat_buf;

  creds = allocate_credentials(n_certs_keys, n_trusted_certs);
  if (creds == NULL) {
    err = 1;
  }

  if (!err && certs && keys) {
    if (trusted_certs || trusted_certs_ids) {
      assert(0);
    }

    if (!err) {
      creds->n_certs_keys = n_certs_keys;
      for (i = 0; i < n_certs_keys; i++) {
        if (stat(certs[i], &stat_buf) != 0) {
          LOG(LOGS_ERR, "Cert %s doesn't exist.", certs[i]);
          err = 1;
          break;
        }
        if (stat(keys[i], &stat_buf) != 0) {
          LOG(LOGS_ERR, "Key %s doesn't exist.", keys[i]);
          err = 1;
          break;
        }

        strncpy(creds->certs[i], certs[i], MAX_FILE_NAME_SZ);
        strncpy(creds->keys[i], keys[i], MAX_FILE_NAME_SZ);
      }
    }
  }
  else {
    if (certs || keys || n_certs_keys > 0) {
      assert(0);
    }

    if (!err && trusted_certs && trusted_certs_ids) {
      for (i = 0; i < n_trusted_certs; i++) {
        if (trusted_certs_ids[i] != trusted_cert_set) {
          continue;
        }
        if (stat(trusted_certs[i], &stat_buf) != 0) {
          LOG(LOGS_ERR, "Cert %s doesn't exist.", trusted_certs[i]);
          err = 1;
          break;
        }

        ++creds->n_trusted_certs;
        strncpy(creds->trusted_certs[j++], trusted_certs[i], MAX_FILE_NAME_SZ);
      }
    }
  }

  if (err) {
    LOG(LOGS_ERR, "Could not set credentials");
    free_credentials(creds);
    creds = NULL;
  }

  return (TLS_Credentials)creds;
}

/* ================================================== */

void TLS_DestroyCredentials(TLS_Credentials credentials)
{
  free_credentials((WolfsslCreds *) credentials);
}

/* ================================================== */

static int server_sni_cb(WOLFSSL* ssl, int* alertReason, void* exArg)
{
  WolfsslCreds* creds = (WolfsslCreds*)exArg;
  char* req_server;
  unsigned short req_server_len;
  int i = 0;
  DerBuffer* der = NULL;
  DecodedCert decoded_cert;
  int ret = 0;

  req_server_len = wolfSSL_SNI_GetRequest(ssl, WOLFSSL_SNI_HOST_NAME,
                                          (void**)&req_server);
  if (req_server_len == 0) {
    /* Not an error if no SNI sent by client. */
    return 0;
  }
  for (; i < creds->n_certs_keys && ret == 0; ++i) {
    ret = wc_PemCertToDer_ex(creds->certs[i], &der);
    if (ret != 0) {
      LOG(LOGS_ERR, "wc_PemCertToDer_ex failed.");
      ret = fatal_return;
    }
    if (ret == 0) {
      wc_InitDecodedCert(&decoded_cert, der->buffer, der->length, NULL);
      ret = wc_ParseCert(&decoded_cert, CERT_TYPE, NO_VERIFY, NULL);
      if (ret != 0) {
        LOG(LOGS_ERR, "wc_ParseCert failed.");
        ret = fatal_return;
      }
    }
    if (ret == 0) {
      DEBUG_LOG("Checking candidate cert with subject name %s against hostname "
                "%s in client SNI extension.", decoded_cert.subjectCN,
                req_server);
      if (memcmp(req_server, decoded_cert.subjectCN, req_server_len) == 0) {
        if (wolfSSL_use_certificate_chain_file(ssl, creds->certs[i])
            != WOLFSSL_SUCCESS) {
          LOG(LOGS_ERR, "Failed to use certificate %s.", creds->certs[i]);
          ret = fatal_return;
        }
        if (wolfSSL_use_PrivateKey_file(ssl, creds->keys[i],
                                        WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
          LOG(LOGS_ERR, "Failed to use private key %s.", creds->keys[i]);
          ret = fatal_return;
        }
        DEBUG_LOG("Cert %s matched.", creds->certs[i]);
      }
      else {
        DEBUG_LOG("Cert %s did NOT match.", creds->certs[i]);
      }
    }
    wc_FreeDecodedCert(&decoded_cert);
    wc_FreeDer(&der);
  }

  return ret;
}

/* ================================================== */

static int override_date_err_cb(int preverify, WOLFSSL_X509_STORE_CTX* store)
{
  if (store->error == ASN_BEFORE_DATE_E || store->error == ASN_AFTER_DATE_E) {
    return 1;
  }

  return 0;
}

/* ================================================== */

static WOLFSSL_CTX*
create_wolfssl_ctx(int server, TLS_Credentials creds)
{
  WOLFSSL_CTX* ctx;

  if (server) {
    ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method_ex(NULL));
    if (ctx) {
      wolfSSL_CTX_set_servername_callback(ctx, server_sni_cb);
      wolfSSL_CTX_set_servername_arg(ctx, creds);
    }
  }
  else {
    ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method_ex(NULL));
  }

  return ctx;
}

/* ================================================== */

static WOLFSSL*
create_wolfssl_session(WOLFSSL_CTX* ctx, int server_mode, int sock_fd,
                       const char *server_name, char *alpn_name,
                       const WolfsslCreds* creds, int disable_time_checks)
{
  WOLFSSL* session = NULL;
  int r = WOLFSSL_SUCCESS;
  int i;
  struct stat stat_buf;
  unsigned int verify_flags;

  /* GnuTLS implementation allows date errors at load time. We achieve the same
   * behavior here with WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY. */
  verify_flags = WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY;

  if (!server_mode && disable_time_checks) {
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, override_date_err_cb);
    DEBUG_LOG("Disabled time checks");
  }

  for (i = 0; i < creds->n_trusted_certs; ++i) {
    if (stat(creds->trusted_certs[i], &stat_buf) == 0 &&
        S_ISDIR(stat_buf.st_mode)) {
      r = wolfSSL_CTX_load_verify_locations_ex(ctx, NULL,
                                               creds->trusted_certs[i],
                                               verify_flags);
      if (r != WOLFSSL_SUCCESS) {
        LOG(LOGS_ERR, "Failed to use trusted certificate dir %s.",
            creds->trusted_certs[i]);
        break;
      }
    }
    else {
      r = wolfSSL_CTX_load_verify_locations_ex(ctx, creds->trusted_certs[i],
                                               NULL, verify_flags);
      if (r != WOLFSSL_SUCCESS) {
        LOG(LOGS_ERR, "Failed to use trusted certificate %s. (%d)",
            creds->trusted_certs[i], r);
        break;
      }
    }
  }

  if (r == WOLFSSL_SUCCESS) {
    /* Default to using the first cert/key pair. If there are multiple pairs,
     * the SNI callback, server_sni_cb, will pick the one to use based on the
     * hostname in the SNI extension of the ClientHello. */
    if (creds->n_certs_keys != 0) {
      r = wolfSSL_CTX_use_certificate_chain_file(ctx, creds->certs[0]);
      if (r != WOLFSSL_SUCCESS) {
        LOG(LOGS_ERR, "Failed to use certificate %s.", creds->certs[i]);
      }
      else {
        r = wolfSSL_CTX_use_PrivateKey_file(ctx, creds->keys[0],
                                            WOLFSSL_FILETYPE_PEM);
        if (r != WOLFSSL_SUCCESS) {
          LOG(LOGS_ERR, "Failed to use private key %s.", creds->keys[0]);
        }
      }
    }
  }

  if (r == WOLFSSL_SUCCESS) {
    session = wolfSSL_new(ctx);
    if (session == NULL) {
      LOG(LOGS_ERR, "Could not create TLS session.");
      return NULL;
    }
  }

  if (!server_mode) {
    assert(server_name);

    if (!UTI_IsStringIP(server_name)) {
      r = wolfSSL_UseSNI(session, WOLFSSL_SNI_HOST_NAME, server_name,
                         strlen(server_name));
      if (r != WOLFSSL_SUCCESS) {
        LOG(LOGS_ERR, "Failed to enable SNI with hostname %s.", server_name);
      }
    }
  }

  if (r == WOLFSSL_SUCCESS) {
    /* Continue on mismatch because we check the ALPN manually in check_alpn.
     * This mirrors the behavior with the GnuTLS implementation. */
    r = wolfSSL_UseALPN(session, alpn_name, strlen(alpn_name),
                        WOLFSSL_ALPN_CONTINUE_ON_MISMATCH);
    if (r != WOLFSSL_SUCCESS) {
      LOG(LOGS_ERR, "Failed to enable ALPN.");
    }
  }

  if (r == WOLFSSL_SUCCESS) {
    r = wolfSSL_set_fd(session, sock_fd);
    if (r != WOLFSSL_SUCCESS) {
      LOG(LOGS_ERR, "Failed to associate socket with TLS session.");
    }
  }

  if (r != WOLFSSL_SUCCESS && session != NULL) {
    wolfSSL_free(session);
    session = NULL;
  }

  return session;
}

/* ================================================== */

TLS_Instance
TLS_CreateInstance(int server_mode, int sock_fd, const char *server_name, const char *alpn_name,
                   TLS_Credentials credentials, int disable_time_checks)
{
  TLS_Instance inst = MallocNew(struct TLS_Instance_Record);

  inst->ctx = NULL;
  inst->session = NULL;
  inst->server = server_mode;
  inst->server_name = server_name ? Strdup(server_name) : NULL;
  inst->alpn_name = alpn_name ? Strdup(alpn_name) : NULL;

  inst->ctx = create_wolfssl_ctx(server_mode, credentials);
  if (inst->ctx == NULL) {
    LOG(LOGS_ERR, "Failed to create TLS context.");
    goto error;
  }

  inst->session = create_wolfssl_session(inst->ctx, server_mode, sock_fd,
                                            server_name, inst->alpn_name,
                                            (WolfsslCreds *) credentials,
                                            disable_time_checks);
  if (inst->session == NULL) {
    LOG(LOGS_ERR, "Failed to create TLS session.");
    goto error;
  }

  /* Needed to export keying material. */
  wolfSSL_KeepArrays(inst->session);
  return inst;

error:
  TLS_DestroyInstance(inst);
  return NULL;
}

/* ================================================== */

void
TLS_DestroyInstance(TLS_Instance inst)
{
  if (inst->session)
    wolfSSL_free(inst->session);

  if (inst->ctx)
    wolfSSL_CTX_free(inst->ctx);

  if (inst->server_name)
    Free(inst->server_name);

  if (inst->alpn_name)
    Free(inst->alpn_name);

  Free(inst);
}

/* ================================================== */

static int
check_alpn(TLS_Instance inst)
{
  char* protocol_name = NULL;
  unsigned short protocol_name_sz;
  int r;
  int err = 0;

  if (inst->server) {
    r = wolfSSL_ALPN_GetPeerProtocol(inst->session, &protocol_name,
                                     &protocol_name_sz);
  }
  else {
    r = wolfSSL_ALPN_GetProtocol(inst->session, &protocol_name,
                                 &protocol_name_sz);
  }
  if (r != WOLFSSL_SUCCESS) {
    LOG(LOGS_ERR, "Failed to get ALPN protocol name.");
    err = 1;
  }

  if (protocol_name_sz != strlen(inst->alpn_name) ||
      memcmp(protocol_name, inst->alpn_name, protocol_name_sz) != 0) {
    LOG(LOGS_ERR, "ALPN protocol name %s doesn't match expected %s.",
        protocol_name, inst->alpn_name);
    err = 1;
  }

  /* wolfSSL_ALPN_GetPeerProtocol dynamically allocates protocol_name. */
  if (inst->server && protocol_name != NULL) {
    free(protocol_name);
  }

  if (err) {
    return 0;
  }

  return 1;
}

/* ================================================== */

TLS_Status
TLS_DoHandshake(TLS_Instance inst)
{
  int r;

  if (inst->server) {
    r = wolfSSL_accept(inst->session);
  }
  else {
    r = wolfSSL_connect(inst->session);
  }

  if (r != WOLFSSL_SUCCESS) {
    int err = wolfSSL_get_error(inst->session, 0);
    if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
      LOG(inst->server ? LOGS_DEBUG : LOGS_ERR, "TLS handshake with %s "
                                                "failed : %s", inst->server_name, wc_GetErrorString(err));
      return TLS_FAILED;
    }

    return (err == WOLFSSL_ERROR_WANT_WRITE) ? TLS_AGAIN_OUTPUT : TLS_AGAIN_INPUT;
  }

  if (DEBUG) {
    DEBUG_LOG("Handshake with %s completed", inst->server_name);
  }

  if (!check_alpn(inst)) {
    LOG(inst->server ? LOGS_DEBUG : LOGS_ERR, "NTS-KE not supported by %s", inst->server_name);
    return TLS_FAILED;
  }

  return TLS_SUCCESS;
}

/* ================================================== */

TLS_Status
TLS_Send(TLS_Instance inst, const void *data, int length, int *sent)
{
  int r = wolfSSL_write(inst->session, data, length);
  if (r <= 0) {
    int err = wolfSSL_get_error(inst->session, 0);
    if (err != WOLFSSL_ERROR_WANT_WRITE && err != WOLFSSL_ERROR_WANT_READ) {
      LOG(inst->server ? LOGS_DEBUG : LOGS_ERR,
          "Could not send NTS-KE message to %s : %s", inst->server_name,
          wolfSSL_ERR_error_string(err, NULL));
      return TLS_FAILED;
    }
    return TLS_AGAIN_OUTPUT;
  }

  *sent = r;
  return TLS_SUCCESS;
}

/* ================================================== */

TLS_Status
TLS_Receive(TLS_Instance inst, void *data, int length, int *received)
{
  int r = wolfSSL_read(inst->session, data, length);
  if (r <= 0) {
    int err = wolfSSL_get_error(inst->session, 0);
    if (err != WOLFSSL_ERROR_WANT_WRITE && err != WOLFSSL_ERROR_WANT_READ) {
      LOG(inst->server ? LOGS_DEBUG : LOGS_ERR,
          "Could not receive NTS-KE message from %s : %s", inst->server_name,
          wolfSSL_ERR_error_string(err, NULL));
      return TLS_FAILED;
    }
    return TLS_AGAIN_INPUT;
  }

  *received = r;
  return TLS_SUCCESS;
}

/* ================================================== */

int
TLS_CheckPending(TLS_Instance inst)
{
  return wolfSSL_pending(inst->session) > 0;
}

/* ================================================== */

TLS_Status
TLS_Shutdown(TLS_Instance inst)
{
  int r = wolfSSL_shutdown(inst->session);
  if (r != WOLFSSL_SUCCESS) {
    if (r == WOLFSSL_SHUTDOWN_NOT_DONE) {
      return TLS_AGAIN_INPUT;
    }

    int err = wolfSSL_get_error(inst->session, 0);
    DEBUG_LOG("Shutdown with %s failed : %s", inst->server_name,
              wolfSSL_ERR_error_string(err, NULL));
    return TLS_FAILED;
  }
  return TLS_SUCCESS;
}

/* ================================================== */

int
TLS_ExportKey(TLS_Instance inst, int label_length, const char *label, int context_length, const void *context,
              int key_length, unsigned char *key)
{
  int r;

  if (label_length < 0 || context_length < 0 || key_length < 0)
    return 0;

  r = wolfSSL_export_keying_material(inst->session,
                                     key, key_length, label,
                                     label_length,
                                     (const unsigned char *) context,
                                     context_length, 1);

  return r == WOLFSSL_SUCCESS;
}
