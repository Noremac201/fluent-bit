/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2019 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/**
 * @name OpenSSL integration
 *
 */

#include "rdkafka_int.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_cert.h"

#ifdef _MSC_VER
#pragma comment (lib, "crypt32.lib")
#endif

#include <openssl/x509.h>



#if WITH_VALGRIND
/* OpenSSL relies on uninitialized memory, which Valgrind will whine about.
 * We use in-code Valgrind macros to suppress those warnings. */
#include <valgrind/memcheck.h>
#else
#define VALGRIND_MAKE_MEM_DEFINED(A,B)
#endif


#if OPENSSL_VERSION_NUMBER < 0x10100000L
static mtx_t *rd_kafka_ssl_locks;
static int    rd_kafka_ssl_locks_cnt;
#endif


/**
 * @brief Close and destroy SSL session
 */
void rd_kafka_transport_ssl_close (rd_kafka_transport_t *rktrans) {
        SSL_shutdown(rktrans->rktrans_ssl);
        SSL_free(rktrans->rktrans_ssl);
        rktrans->rktrans_ssl = NULL;
}


/**
 * @brief Clear OpenSSL error queue to get a proper error reporting in case
 *        the next SSL_*() operation fails.
 */
static RD_INLINE void
rd_kafka_transport_ssl_clear_error (rd_kafka_transport_t *rktrans) {
        ERR_clear_error();
#ifdef _MSC_VER
        WSASetLastError(0);
#else
        rd_set_errno(0);
#endif
}

/**
 * @returns a thread-local single-invocation-use error string for
 *          the last thread-local error in OpenSSL, or an empty string
 *          if no error.
 */
const char *rd_kafka_ssl_last_error_str (void) {
        static RD_TLS char errstr[256];
        unsigned long l;
        const char *file, *data;
        int line, flags;

        l = ERR_peek_last_error_line_data(&file, &line,
                                          &data, &flags);
        if (!l)
                return "";

        rd_snprintf(errstr, sizeof(errstr),
                    "%lu:%s:%s:%s:%d: %s",
                    l,
                    ERR_lib_error_string(l),
                    ERR_func_error_string(l),
                    file, line,
                    ((flags & ERR_TXT_STRING) && data && *data) ?
                    data : ERR_reason_error_string(l));

        return errstr;
}

/**
 * Serves the entire OpenSSL error queue and logs each error.
 * The last error is not logged but returned in 'errstr'.
 *
 * If 'rkb' is non-NULL broker-specific logging will be used,
 * else it will fall back on global 'rk' debugging.
 */
static char *rd_kafka_ssl_error (rd_kafka_t *rk, rd_kafka_broker_t *rkb,
                                 char *errstr, size_t errstr_size) {
        unsigned long l;
        const char *file, *data;
        int line, flags;
        int cnt = 0;

        while ((l = ERR_get_error_line_data(&file, &line, &data, &flags)) != 0) {
                char buf[256];

                if (cnt++ > 0) {
                        /* Log last message */
                        if (rkb)
                                rd_rkb_log(rkb, LOG_ERR, "SSL", "%s", errstr);
                        else
                                rd_kafka_log(rk, LOG_ERR, "SSL", "%s", errstr);
                }

                ERR_error_string_n(l, buf, sizeof(buf));

                rd_snprintf(errstr, errstr_size, "%s:%d: %s: %s",
                            file, line, buf, (flags & ERR_TXT_STRING) ? data : "");

        }

        if (cnt == 0)
                rd_snprintf(errstr, errstr_size, "No error");

        return errstr;
}



/**
 * Set transport IO event polling based on SSL error.
 *
 * Returns -1 on permanent errors.
 *
 * Locality: broker thread
 */
static RD_INLINE int
rd_kafka_transport_ssl_io_update (rd_kafka_transport_t *rktrans, int ret,
                                  char *errstr, size_t errstr_size) {
        int serr = SSL_get_error(rktrans->rktrans_ssl, ret);
        int serr2;

        switch (serr)
        {
        case SSL_ERROR_WANT_READ:
                rd_kafka_transport_poll_set(rktrans, POLLIN);
                break;

        case SSL_ERROR_WANT_WRITE:
        case SSL_ERROR_WANT_CONNECT:
                rd_kafka_transport_poll_set(rktrans, POLLOUT);
                break;

        case SSL_ERROR_SYSCALL:
                serr2 = ERR_peek_error();
                if (!serr2 && !rd_socket_errno)
                        rd_snprintf(errstr, errstr_size, "Disconnected");
                else if (serr2)
                        rd_kafka_ssl_error(NULL, rktrans->rktrans_rkb,
                                           errstr, errstr_size);
                else
                        rd_snprintf(errstr, errstr_size,
                                    "SSL transport error: %s",
                                    rd_strerror(rd_socket_errno));
                return -1;

        case SSL_ERROR_ZERO_RETURN:
                rd_snprintf(errstr, errstr_size, "Disconnected");
                return -1;

        default:
                rd_kafka_ssl_error(NULL, rktrans->rktrans_rkb,
                                   errstr, errstr_size);
                return -1;
        }

        return 0;
}

ssize_t rd_kafka_transport_ssl_send (rd_kafka_transport_t *rktrans,
                                     rd_slice_t *slice,
                                     char *errstr, size_t errstr_size) {
        ssize_t sum = 0;
        const void *p;
        size_t rlen;

        rd_kafka_transport_ssl_clear_error(rktrans);

        while ((rlen = rd_slice_peeker(slice, &p))) {
                int r;
                size_t r2;

                r = SSL_write(rktrans->rktrans_ssl, p, (int)rlen);

                if (unlikely(r <= 0)) {
                        if (rd_kafka_transport_ssl_io_update(rktrans, r,
                                                             errstr,
                                                             errstr_size) == -1)
                                return -1;
                        else
                                return sum;
                }

                /* Update buffer read position */
                r2 = rd_slice_read(slice, NULL, (size_t)r);
                rd_assert((size_t)r == r2 &&
                          *"BUG: wrote more bytes than available in slice");


                sum += r;
                /* FIXME: remove this and try again immediately and let
                 *        the next SSL_write() call fail instead? */
                if ((size_t)r < rlen)
                        break;

        }
        return sum;
}

ssize_t rd_kafka_transport_ssl_recv (rd_kafka_transport_t *rktrans,
                                     rd_buf_t *rbuf,
                                     char *errstr, size_t errstr_size) {
        ssize_t sum = 0;
        void *p;
        size_t len;

        while ((len = rd_buf_get_writable(rbuf, &p))) {
                int r;

                rd_kafka_transport_ssl_clear_error(rktrans);

                r = SSL_read(rktrans->rktrans_ssl, p, (int)len);

                if (unlikely(r <= 0)) {
                        if (rd_kafka_transport_ssl_io_update(rktrans, r,
                                                             errstr,
                                                             errstr_size) == -1)
                                return -1;
                        else
                                return sum;
                }

                VALGRIND_MAKE_MEM_DEFINED(p, r);

                /* Update buffer write position */
                rd_buf_write(rbuf, NULL, (size_t)r);

                sum += r;

                /* FIXME: remove this and try again immediately and let
                 *        the next SSL_read() call fail instead? */
                if ((size_t)r < len)
                        break;

        }
        return sum;

}


/**
 * OpenSSL password query callback
 *
 * Locality: application thread
 */
static int rd_kafka_transport_ssl_passwd_cb (char *buf, int size, int rwflag,
                                             void *userdata) {
        rd_kafka_t *rk = userdata;
        int pwlen;

        rd_kafka_dbg(rk, SECURITY, "SSLPASSWD",
                     "Private key requires password");

        if (!rk->rk_conf.ssl.key_password) {
                rd_kafka_log(rk, LOG_WARNING, "SSLPASSWD",
                             "Private key requires password but "
                             "no password configured (ssl.key.password)");
                return -1;
        }


        pwlen = (int) strlen(rk->rk_conf.ssl.key_password);
        memcpy(buf, rk->rk_conf.ssl.key_password, RD_MIN(pwlen, size));

        return pwlen;
}


/**
 * @brief OpenSSL callback to perform additional broker certificate
 *        verification and validation.
 *
 * @return 1 on success when the broker certificate
 *         is valid and 0 when the certificate is not valid.
 *
 * @sa SSL_CTX_set_verify()
 */
static int
rd_kafka_transport_ssl_cert_verify_cb (int preverify_ok,
                                       X509_STORE_CTX *x509_ctx) {
        rd_kafka_transport_t *rktrans = rd_kafka_curr_transport;
        rd_kafka_broker_t *rkb;
        rd_kafka_t *rk;
        X509 *cert;
        char *buf = NULL;
        int   buf_size;
        int   depth;
        int   x509_orig_error, x509_error;
        char  errstr[512];
        int   ok;

        rd_assert(rktrans != NULL);
        rkb = rktrans->rktrans_rkb;
        rk = rkb->rkb_rk;

        cert = X509_STORE_CTX_get_current_cert(x509_ctx);
        if (!cert) {
                rd_rkb_log(rkb, LOG_ERR, "SSLCERTVRFY",
                           "Failed to get current certificate to verify");
                return 0;
        }

        depth = X509_STORE_CTX_get_error_depth(x509_ctx);

        x509_orig_error = x509_error = X509_STORE_CTX_get_error(x509_ctx);

        buf_size = i2d_X509(cert, (unsigned char **)&buf);
        if (buf_size < 0 || !buf) {
                rd_rkb_log(rkb, LOG_ERR, "SSLCERTVRFY",
                           "Unable to convert certificate to X509 format");
                return 0;
        }

        *errstr = '\0';

        /* Call application's verification callback. */
        ok = rk->rk_conf.ssl.cert_verify_cb(rk,
                                            rkb->rkb_nodename,
                                            rkb->rkb_nodeid,
                                            &x509_error,
                                            depth,
                                            buf, (size_t)buf_size,
                                            errstr, sizeof(errstr),
                                            rk->rk_conf.opaque);

        OPENSSL_free(buf);

        if (!ok) {
                char subject[128];
                char issuer[128];

                X509_NAME_oneline(X509_get_subject_name(cert),
                                  subject, sizeof(subject));
                X509_NAME_oneline(X509_get_issuer_name(cert),
                                  issuer, sizeof(issuer));
                rd_rkb_log(rkb, LOG_ERR, "SSLCERTVRFY",
                           "Certificate (subject=%s, issuer=%s) verification "
                           "callback failed: %s",
                           subject, issuer, errstr);

                X509_STORE_CTX_set_error(x509_ctx, x509_error);

                return 0; /* verification failed */
        }

        /* Clear error */
        if (x509_orig_error != 0 && x509_error == 0)
                X509_STORE_CTX_set_error(x509_ctx, 0);

        return 1; /* verification successful */
}

/**
 * @brief Set TLSEXT hostname for SNI and optionally enable
 *        SSL endpoint identification verification.
 *
 * @returns 0 on success or -1 on error.
 */
static int
rd_kafka_transport_ssl_set_endpoint_id (rd_kafka_transport_t *rktrans,
                                        char *errstr, size_t errstr_size) {
        char name[RD_KAFKA_NODENAME_SIZE];
        char *t;

        rd_kafka_broker_lock(rktrans->rktrans_rkb);
        rd_snprintf(name, sizeof(name), "%s",
                    rktrans->rktrans_rkb->rkb_nodename);
        rd_kafka_broker_unlock(rktrans->rktrans_rkb);

        /* Remove ":9092" port suffix from nodename */
        if ((t = strrchr(name, ':')))
                *t = '\0';

#if (OPENSSL_VERSION_NUMBER >= 0x0090806fL) && !defined(OPENSSL_NO_TLSEXT)
        /* If non-numerical hostname, send it for SNI */
        if (!(/*ipv6*/(strchr(name, ':') &&
                       strspn(name, "0123456789abcdefABCDEF:.[]%") ==
                       strlen(name)) ||
              /*ipv4*/strspn(name, "0123456789.") == strlen(name)) &&
            !SSL_set_tlsext_host_name(rktrans->rktrans_ssl, name))
                goto fail;
#endif

        if (rktrans->rktrans_rkb->rkb_rk->rk_conf.
            ssl.endpoint_identification == RD_KAFKA_SSL_ENDPOINT_ID_NONE)
                return 0;

#if OPENSSL_VERSION_NUMBER >= 0x10100000
        if (!SSL_set1_host(rktrans->rktrans_ssl, name))
                goto fail;
#elif OPENSSL_VERSION_NUMBER >= 0x1000200fL /* 1.0.2 */
        {
                X509_VERIFY_PARAM *param;

                param = SSL_get0_param(rktrans->rktrans_ssl);

                if (!X509_VERIFY_PARAM_set1_host(param, name, 0))
                        goto fail;
        }
#else
        rd_snprintf(errstr, errstr_size,
                    "Endpoint identification not supported on this "
                    "OpenSSL version (0x%lx)",
                    OPENSSL_VERSION_NUMBER);
        return -1;
#endif

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "ENDPOINT",
                   "Enabled endpoint identification using hostname %s",
                   name);

        return 0;

 fail:
        rd_kafka_ssl_error(NULL, rktrans->rktrans_rkb,
                           errstr, errstr_size);
        return -1;
}


/**
 * @brief Set up SSL for a newly connected connection
 *
 * @returns -1 on failure, else 0.
 */
int rd_kafka_transport_ssl_connect (rd_kafka_broker_t *rkb,
                                    rd_kafka_transport_t *rktrans,
                                    char *errstr, size_t errstr_size) {
        int r;

        rktrans->rktrans_ssl = SSL_new(rkb->rkb_rk->rk_conf.ssl.ctx);
        if (!rktrans->rktrans_ssl)
                goto fail;

        if (!SSL_set_fd(rktrans->rktrans_ssl, (int)rktrans->rktrans_s))
                goto fail;

        if (rd_kafka_transport_ssl_set_endpoint_id(rktrans, errstr,
                                                   errstr_size) == -1)
                return -1;

        rd_kafka_transport_ssl_clear_error(rktrans);

        r = SSL_connect(rktrans->rktrans_ssl);
        if (r == 1) {
                /* Connected, highly unlikely since this is a
                 * non-blocking operation. */
                rd_kafka_transport_connect_done(rktrans, NULL);
                return 0;
        }

        if (rd_kafka_transport_ssl_io_update(rktrans, r,
                                             errstr, errstr_size) == -1)
                return -1;

        return 0;

 fail:
        rd_kafka_ssl_error(NULL, rkb, errstr, errstr_size);
        return -1;
}


static RD_UNUSED int
rd_kafka_transport_ssl_io_event (rd_kafka_transport_t *rktrans, int events) {
        int r;
        char errstr[512];

        if (events & POLLOUT) {
                rd_kafka_transport_ssl_clear_error(rktrans);

                r = SSL_write(rktrans->rktrans_ssl, NULL, 0);
                if (rd_kafka_transport_ssl_io_update(rktrans, r,
                                                     errstr,
                                                     sizeof(errstr)) == -1)
                        goto fail;
        }

        return 0;

 fail:
        /* Permanent error */
        rd_kafka_broker_fail(rktrans->rktrans_rkb, LOG_ERR,
                             RD_KAFKA_RESP_ERR__TRANSPORT,
                             "%s", errstr);
        return -1;
}


/**
 * @brief Verify SSL handshake was valid.
 */
static int rd_kafka_transport_ssl_verify (rd_kafka_transport_t *rktrans) {
        long int rl;
        X509 *cert;

        if (!rktrans->rktrans_rkb->rkb_rk->rk_conf.ssl.enable_verify)
                return 0;

        cert = SSL_get_peer_certificate(rktrans->rktrans_ssl);
        X509_free(cert);
        if (!cert) {
                rd_kafka_broker_fail(rktrans->rktrans_rkb, LOG_ERR,
                                     RD_KAFKA_RESP_ERR__SSL,
                                     "Broker did not provide a certificate");
                return -1;
        }

        if ((rl = SSL_get_verify_result(rktrans->rktrans_ssl)) != X509_V_OK) {
                rd_kafka_broker_fail(rktrans->rktrans_rkb, LOG_ERR,
                                     RD_KAFKA_RESP_ERR__SSL,
                                     "Failed to verify broker certificate: %s",
                                     X509_verify_cert_error_string(rl));
                return -1;
        }

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SSLVERIFY",
                   "Broker SSL certificate verified");
        return 0;
}

/**
 * @brief SSL handshake handling.
 * Call repeatedly (based on IO events) until handshake is done.
 *
 * @returns -1 on error, 0 if handshake is still in progress,
 *          or 1 on completion.
 */
int rd_kafka_transport_ssl_handshake (rd_kafka_transport_t *rktrans) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;
        char errstr[512];
        int r;

        r = SSL_do_handshake(rktrans->rktrans_ssl);
        if (r == 1) {
                /* SSL handshake done. Verify. */
                if (rd_kafka_transport_ssl_verify(rktrans) == -1)
                        return -1;

                rd_kafka_transport_connect_done(rktrans, NULL);
                return 1;

        } else if (rd_kafka_transport_ssl_io_update(rktrans, r,
                                                    errstr,
                                                    sizeof(errstr)) == -1) {
                rd_kafka_broker_fail(rkb, LOG_ERR, RD_KAFKA_RESP_ERR__SSL,
                                     "SSL handshake failed: %s%s", errstr,
                                     strstr(errstr, "unexpected message") ?
                                     ": client authentication might be "
                                     "required (see broker log)" : "");
                return -1;
        }

        return 0;
}



/**
 * @brief Parse a PEM-formatted string into an EVP_PKEY (PrivateKey) object.
 *
 * @param str Input PEM string, nul-terminated
 *
 * @remark This method does not provide automatic addition of PEM
 *         headers and footers.
 *
 * @returns a new EVP_PKEY on success or NULL on error.
 */
static EVP_PKEY *rd_kafka_ssl_PKEY_from_string (rd_kafka_t *rk,
                                                const char *str) {
        BIO *bio = BIO_new_mem_buf((void *)str, -1);
        EVP_PKEY *pkey;

        pkey = PEM_read_bio_PrivateKey(bio, NULL,
                                       rd_kafka_transport_ssl_passwd_cb, rk);

        BIO_free(bio);

        return pkey;
}

/**
 * @brief Parse a PEM-formatted string into an X509 object.
 *
 * @param str Input PEM string, nul-terminated
 *
 * @returns a new X509 on success or NULL on error.
 */
static X509 *rd_kafka_ssl_X509_from_string (rd_kafka_t *rk, const char *str) {
        BIO *bio = BIO_new_mem_buf((void *)str, -1);
        X509 *x509;

        x509 = PEM_read_bio_X509(bio, NULL,
                                 rd_kafka_transport_ssl_passwd_cb, rk);

        BIO_free(bio);

        return x509;
}


#if _MSC_VER

/**
 * @brief Attempt load CA certificates from the Windows Certificate Root store.
 */
static int rd_kafka_ssl_win_load_root_certs (rd_kafka_t *rk, SSL_CTX *ctx) {
        HCERTSTORE w_store;
        PCCERT_CONTEXT w_cctx = NULL;
        X509_STORE *store;
        int fail_cnt = 0, cnt = 0;
        char errstr[256];

        w_store = CertOpenStore(CERT_STORE_PROV_SYSTEM,
                                0,
                                0,
                                CERT_SYSTEM_STORE_CURRENT_USER,
                                L"Root");
        if (!w_store) {
                rd_kafka_dbg(rk, SECURITY, "CERTROOT",
                             "Failed to open Windows certificate "
                             "Root store: %s: "
                             "falling back to OpenSSL default CA paths",
                             rd_strerror_w32(GetLastError(), errstr,
                                             sizeof(errstr)));
                return -1;
        }

        /* Get the OpenSSL trust store */
        store = SSL_CTX_get_cert_store(ctx);

        /* Enumerate the Windows certs */
        while ((w_cctx = CertEnumCertificatesInStore(w_store, w_cctx))) {
                X509 *x509;

                cnt++;

                /* Parse Windows cert: DER -> X.509 */
                x509 = d2i_X509(NULL,
                                (const unsigned char **)&w_cctx->pbCertEncoded,
                                (long)w_cctx->cbCertEncoded);
                if (!x509) {
                        fail_cnt++;
                        continue;
                }

                /* Add cert to OpenSSL's trust store */
                if (!X509_STORE_add_cert(store, x509))
                        fail_cnt++;

                X509_free(x509);
        }

        if (w_cctx)
                CertFreeCertificateContext(w_cctx);

        CertCloseStore(w_store, 0);

        rd_kafka_dbg(rk, SECURITY, "CERTROOT",
                     "%d/%d certificate(s) successfully added from "
                     "Windows Certificate Root store",
                     cnt - fail_cnt, cnt);

        return cnt - fail_cnt == 0 ? -1 : 0;
}
#endif /* MSC_VER */

/**
 * @brief Registers certificates, keys, etc, on the SSL_CTX
 *
 * @returns -1 on error, or 0 on success.
 */
static int rd_kafka_ssl_set_certs (rd_kafka_t *rk, SSL_CTX *ctx,
                                   char *errstr, size_t errstr_size) {
        rd_bool_t check_pkey = rd_false;
        int r;

        /*
         * ssl_ca, ssl.ca.location, or Windows cert root store,
         * or default paths.
         */
        if (rk->rk_conf.ssl.ca) {
                /* CA certificate chain set with conf_set_ssl_cert() */
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading CA certificate(s) from memory");

                SSL_CTX_set_cert_store(ctx, rk->rk_conf.ssl.ca->store);

                /* OpenSSL takes ownership of the store */
                rk->rk_conf.ssl.ca->store = NULL;

        } else if (rk->rk_conf.ssl.ca_location) {
                /* CA certificate location, either file or directory. */
                int is_dir = rd_kafka_path_is_dir(rk->rk_conf.ssl.ca_location);

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading CA certificate(s) from %s %s",
                             is_dir ? "directory" : "file",
                             rk->rk_conf.ssl.ca_location);

                r = SSL_CTX_load_verify_locations(ctx,
                                                  !is_dir ?
                                                  rk->rk_conf.ssl.
                                                  ca_location : NULL,
                                                  is_dir ?
                                                  rk->rk_conf.ssl.
                                                  ca_location : NULL);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.ca.location failed: ");
                        return -1;
                }

        } else {
#if _MSC_VER
                /* Attempt to load CA root certificates from the
                 * Windows crypto Root cert store. */
                r = rd_kafka_ssl_win_load_root_certs(rk, ctx);
#else
                r = -1;
#endif
                if (r == -1) {
                        /* Use default CA certificate paths: ignore failures */
                        r = SSL_CTX_set_default_verify_paths(ctx);
                        if (r != 1)
                                rd_kafka_dbg(
                                        rk, SECURITY, "SSL",
                                        "SSL_CTX_set_default_verify_paths() "
                                        "failed: ignoring");
                }
        }

        if (rk->rk_conf.ssl.crl_location) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading CRL from file %s",
                             rk->rk_conf.ssl.crl_location);

                r = SSL_CTX_load_verify_locations(ctx,
                                                  rk->rk_conf.ssl.crl_location,
                                                  NULL);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.crl.location failed: ");
                        return -1;
                }


                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Enabling CRL checks");

                X509_STORE_set_flags(SSL_CTX_get_cert_store(ctx),
                                     X509_V_FLAG_CRL_CHECK);
        }


        /*
         * ssl_cert, ssl.certificate.location and ssl.certificate.pem
         */
        if (rk->rk_conf.ssl.cert) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading public key from memory");

                rd_assert(rk->rk_conf.ssl.cert->x509);
                r = SSL_CTX_use_certificate(ctx, rk->rk_conf.ssl.cert->x509);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl_cert failed: ");
                        return -1;
                }
        }

        if (rk->rk_conf.ssl.cert_location) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading public key from file %s",
                             rk->rk_conf.ssl.cert_location);

                r = SSL_CTX_use_certificate_chain_file(ctx,
                                                       rk->rk_conf.
                                                       ssl.cert_location);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.certificate.location failed: ");
                        return -1;
                }
        }

        if (rk->rk_conf.ssl.cert_pem) {
                X509 *x509;

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading public key from string");

                x509 = rd_kafka_ssl_X509_from_string(rk,
                                                     rk->rk_conf.ssl.cert_pem);
                if (!x509) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.certificate.pem failed: "
                                    "not in PEM format?: ");
                        return -1;
                }

                r = SSL_CTX_use_certificate(ctx, x509);

                X509_free(x509);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.certificate.pem failed: ");
                        return -1;
                }
        }


        /*
         * ssl_key, ssl.key.location and ssl.key.pem
         */
        if (rk->rk_conf.ssl.key) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading private key file from memory");

                rd_assert(rk->rk_conf.ssl.key->pkey);
                r = SSL_CTX_use_PrivateKey(ctx, rk->rk_conf.ssl.key->pkey);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl_key (in-memory) failed: ");
                        return -1;
                }

                check_pkey = rd_true;
        }

        if (rk->rk_conf.ssl.key_location) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading private key file from %s",
                             rk->rk_conf.ssl.key_location);

                r = SSL_CTX_use_PrivateKey_file(ctx,
                                                rk->rk_conf.ssl.key_location,
                                                SSL_FILETYPE_PEM);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.key.location failed: ");
                        return -1;
                }

                check_pkey = rd_true;
        }

        if (rk->rk_conf.ssl.key_pem) {
                EVP_PKEY *pkey;

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading private key from string");

                pkey = rd_kafka_ssl_PKEY_from_string(rk,
                                                     rk->rk_conf.ssl.key_pem);
                if (!pkey) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.key.pem failed: "
                                    "not in PEM format?: ");
                        return -1;
                }

                r = SSL_CTX_use_PrivateKey(ctx, pkey);

                EVP_PKEY_free(pkey);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.key.pem failed: ");
                        return -1;
                }

                /* We no longer need the PEM key (it is cached in the CTX),
                 * clear its memory. */
                rd_kafka_desensitize_str(rk->rk_conf.ssl.key_pem);

                check_pkey = rd_true;
        }


        /*
         * ssl.keystore.location
         */
        if (rk->rk_conf.ssl.keystore_location) {
                FILE *fp;
                EVP_PKEY *pkey;
                X509 *cert;
                STACK_OF(X509) *ca = NULL;
                PKCS12 *p12;

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading client's keystore file from %s",
                             rk->rk_conf.ssl.keystore_location);

                if (!(fp = fopen(rk->rk_conf.ssl.keystore_location, "rb"))) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to open ssl.keystore.location: "
                                    "%s: %s",
                                    rk->rk_conf.ssl.keystore_location,
                                    rd_strerror(errno));
                        return -1;
                }

                p12 = d2i_PKCS12_fp(fp, NULL);
                fclose(fp);
                if (!p12) {
                        rd_snprintf(errstr, errstr_size,
                                    "Error reading PKCS#12 file: ");
                        return -1;
                }

                pkey = EVP_PKEY_new();
                cert = X509_new();
                if (!PKCS12_parse(p12, rk->rk_conf.ssl.keystore_password,
                                  &pkey, &cert, &ca)) {
                        EVP_PKEY_free(pkey);
                        X509_free(cert);
                        PKCS12_free(p12);
                        if (ca != NULL)
                                sk_X509_pop_free(ca, X509_free);
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to parse PKCS#12 file: %s: ",
                                    rk->rk_conf.ssl.keystore_location);
                        return -1;
                }

                if (ca != NULL)
                        sk_X509_pop_free(ca, X509_free);

                PKCS12_free(p12);

                r = SSL_CTX_use_certificate(ctx, cert);
                X509_free(cert);
                if (r != 1) {
                        EVP_PKEY_free(pkey);
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to use ssl.keystore.location "
                                    "certificate: ");
                        return -1;
                }

                r = SSL_CTX_use_PrivateKey(ctx, pkey);
                EVP_PKEY_free(pkey);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to use ssl.keystore.location "
                                    "private key: ");
                        return -1;
                }

                check_pkey = rd_true;
        }

        /* Check that a valid private/public key combo was set. */
        if (check_pkey && SSL_CTX_check_private_key(ctx) != 1) {
                rd_snprintf(errstr, errstr_size,
                            "Private key check failed: ");
                return -1;
        }

        return 0;
}


/**
 * @brief Once per rd_kafka_t handle cleanup of OpenSSL
 *
 * @locality any thread
 *
 * @locks rd_kafka_wrlock() MUST be held
 */
void rd_kafka_ssl_ctx_term (rd_kafka_t *rk) {
        SSL_CTX_free(rk->rk_conf.ssl.ctx);
        rk->rk_conf.ssl.ctx = NULL;
}

/**
 * @brief Once per rd_kafka_t handle initialization of OpenSSL
 *
 * @locality application thread
 *
 * @locks rd_kafka_wrlock() MUST be held
 */
int rd_kafka_ssl_ctx_init (rd_kafka_t *rk, char *errstr, size_t errstr_size) {
        int r;
        SSL_CTX *ctx;

#if OPENSSL_VERSION_NUMBER >= 0x10100000
        rd_kafka_dbg(rk, SECURITY, "OPENSSL", "Using OpenSSL version %s "
                     "(0x%lx, librdkafka built with 0x%lx)",
                     OpenSSL_version(OPENSSL_VERSION),
                     OpenSSL_version_num(),
                     OPENSSL_VERSION_NUMBER);
#else
        rd_kafka_dbg(rk, SECURITY, "OPENSSL", "librdkafka built with OpenSSL "
                     "version 0x%lx", OPENSSL_VERSION_NUMBER);
#endif

        if (errstr_size > 0)
                errstr[0] = '\0';

        ctx = SSL_CTX_new(SSLv23_client_method());
        if (!ctx) {
                rd_snprintf(errstr, errstr_size,
                            "SSLv23_client_method() failed: ");
                goto fail;
        }

#ifdef SSL_OP_NO_SSLv3
        /* Disable SSLv3 (unsafe) */
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3);
#endif

        /* Key file password callback */
        SSL_CTX_set_default_passwd_cb(ctx, rd_kafka_transport_ssl_passwd_cb);
        SSL_CTX_set_default_passwd_cb_userdata(ctx, rk);

        /* Ciphers */
        if (rk->rk_conf.ssl.cipher_suites) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Setting cipher list: %s",
                             rk->rk_conf.ssl.cipher_suites);
                if (!SSL_CTX_set_cipher_list(ctx,
                                             rk->rk_conf.ssl.cipher_suites)) {
                        /* Set a string that will prefix the
                         * the OpenSSL error message (which is lousy)
                         * to make it more meaningful. */
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.cipher.suites failed: ");
                        goto fail;
                }
        }

        /* Set up broker certificate verification. */
        SSL_CTX_set_verify(ctx,
                           rk->rk_conf.ssl.enable_verify ?
                           SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                           rk->rk_conf.ssl.cert_verify_cb ?
                           rd_kafka_transport_ssl_cert_verify_cb : NULL);

#if OPENSSL_VERSION_NUMBER >= 0x1000200fL && !defined(LIBRESSL_VERSION_NUMBER)
        /* Curves */
        if (rk->rk_conf.ssl.curves_list) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Setting curves list: %s",
                             rk->rk_conf.ssl.curves_list);
                if (!SSL_CTX_set1_curves_list(ctx,
                                              rk->rk_conf.ssl.curves_list)) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.curves.list failed: ");
                        goto fail;
                }
        }

        /* Certificate signature algorithms */
        if (rk->rk_conf.ssl.sigalgs_list) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Setting signature algorithms list: %s",
                             rk->rk_conf.ssl.sigalgs_list);
                if (!SSL_CTX_set1_sigalgs_list(ctx,
                                               rk->rk_conf.ssl.sigalgs_list)) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.sigalgs.list failed: ");
                        goto fail;
                }
        }
#endif

        /* Register certificates, keys, etc. */
        if (rd_kafka_ssl_set_certs(rk, ctx, errstr, errstr_size) == -1)
                goto fail;


        SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);

        rk->rk_conf.ssl.ctx = ctx;

        return 0;

 fail:
        r = (int)strlen(errstr);
        rd_kafka_ssl_error(rk, NULL, errstr+r,
                           (int)errstr_size > r ? (int)errstr_size - r : 0);
        SSL_CTX_free(ctx);

        return -1;
}


#if OPENSSL_VERSION_NUMBER < 0x10100000L
static RD_UNUSED void
rd_kafka_transport_ssl_lock_cb (int mode, int i, const char *file, int line) {
        if (mode & CRYPTO_LOCK)
                mtx_lock(&rd_kafka_ssl_locks[i]);
        else
                mtx_unlock(&rd_kafka_ssl_locks[i]);
}
#endif

static RD_UNUSED unsigned long rd_kafka_transport_ssl_threadid_cb (void) {
#ifdef _MSC_VER
        /* Windows makes a distinction between thread handle
         * and thread id, which means we can't use the
         * thrd_current() API that returns the handle. */
        return (unsigned long)GetCurrentThreadId();
#else
        return (unsigned long)(intptr_t)thrd_current();
#endif
}

#ifdef HAVE_OPENSSL_CRYPTO_THREADID_SET_CALLBACK
static void rd_kafka_transport_libcrypto_THREADID_callback(CRYPTO_THREADID *id)
{
        unsigned long thread_id = rd_kafka_transport_ssl_threadid_cb();

        CRYPTO_THREADID_set_numeric(id, thread_id);
}
#endif

/**
 * @brief Global OpenSSL cleanup.
 */
void rd_kafka_ssl_term (void) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        int i;

        if (CRYPTO_get_locking_callback() == &rd_kafka_transport_ssl_lock_cb) {
                CRYPTO_set_locking_callback(NULL);
#ifdef HAVE_OPENSSL_CRYPTO_THREADID_SET_CALLBACK
                CRYPTO_THREADID_set_callback(NULL);
#else
                CRYPTO_set_id_callback(NULL);
#endif

                for (i = 0 ; i < rd_kafka_ssl_locks_cnt ; i++)
                        mtx_destroy(&rd_kafka_ssl_locks[i]);

                rd_free(rd_kafka_ssl_locks);
        }
#endif
}


/**
 * @brief Global (once per process) OpenSSL init.
 */
void rd_kafka_ssl_init (void) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        int i;

        if (!CRYPTO_get_locking_callback()) {
                rd_kafka_ssl_locks_cnt = CRYPTO_num_locks();
                rd_kafka_ssl_locks = rd_malloc(rd_kafka_ssl_locks_cnt *
                                               sizeof(*rd_kafka_ssl_locks));
                for (i = 0 ; i < rd_kafka_ssl_locks_cnt ; i++)
                        mtx_init(&rd_kafka_ssl_locks[i], mtx_plain);

                CRYPTO_set_locking_callback(rd_kafka_transport_ssl_lock_cb);

#ifdef HAVE_OPENSSL_CRYPTO_THREADID_SET_CALLBACK
                CRYPTO_THREADID_set_callback(rd_kafka_transport_libcrypto_THREADID_callback);
#else
                CRYPTO_set_id_callback(rd_kafka_transport_ssl_threadid_cb);
#endif
        }

        /* OPENSSL_init_ssl(3) and OPENSSL_init_crypto(3) say:
         * "As of version 1.1.0 OpenSSL will automatically allocate
         * all resources that it needs so no explicit initialisation
         * is required. Similarly it will also automatically
         * deinitialise as required."
         */
        SSL_load_error_strings();
        SSL_library_init();

        ERR_load_BIO_strings();
        ERR_load_crypto_strings();
        OpenSSL_add_all_algorithms();
#endif
}