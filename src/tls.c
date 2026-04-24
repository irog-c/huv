#include "internal.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <stdlib.h>
#include <string.h>

/* Cap on the encrypted-inbox size. A single TLS record is ≤ ~16 KiB; in
 * normal traffic the inbox drains after every on_read, so this is mainly a
 * guard against a peer that writes without ever giving us a chance to
 * respond. */
#define HUV_TLS_INBOX_HARD_CAP (256u * 1024u)

struct huv_tls_ctx
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt cert;
    mbedtls_pk_context pkey;
    mbedtls_ssl_config conf;
};

struct huv_tls_conn
{
    mbedtls_ssl_context ssl;
    char *inbox; /* encrypted bytes received from peer, awaiting decrypt */
    size_t inbox_len, inbox_cap, inbox_pos;
    char *outbox; /* encrypted bytes produced by mbedtls, awaiting uv_write */
    size_t outbox_len, outbox_cap;
    bool handshake_done;
    bool fatal;
};

static void log_mbed_err(huv_server_t *s, const char *what, int rc)
{
    char buf[160];
    mbedtls_strerror(rc, buf, sizeof buf);
    huv_log(s, HUV_LOG_ERROR, "%s: %s (-0x%04x)", what, buf, (unsigned)-rc);
}

int huv_tls_ctx_init(huv_server_t *s)
{
    const char *cert_path = s->config.tls_cert_path;
    const char *key_path = s->config.tls_key_path;
    if (!cert_path || !key_path)
        return 0; /* TLS not configured — not an error */

    huv_tls_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    mbedtls_x509_crt_init(&ctx->cert);
    mbedtls_pk_init(&ctx->pkey);
    mbedtls_ssl_config_init(&ctx->conf);

    static const char *pers = "huv-tls";
    int rc = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func,
                                   &ctx->entropy, (const unsigned char *)pers,
                                   strlen(pers));
    if (rc != 0) {
        log_mbed_err(s, "ctr_drbg_seed", rc);
        goto fail;
    }

    rc = mbedtls_x509_crt_parse_file(&ctx->cert, cert_path);
    if (rc != 0) {
        log_mbed_err(s, "x509_crt_parse_file", rc);
        goto fail;
    }

    rc = mbedtls_pk_parse_keyfile(&ctx->pkey, key_path, NULL,
                                  mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (rc != 0) {
        log_mbed_err(s, "pk_parse_keyfile", rc);
        goto fail;
    }

    rc = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        log_mbed_err(s, "ssl_config_defaults", rc);
        goto fail;
    }

    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    rc = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
    if (rc != 0) {
        log_mbed_err(s, "ssl_conf_own_cert", rc);
        goto fail;
    }

    s->tls = ctx;
    return 0;

fail:
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
    free(ctx);
    return -1;
}

void huv_tls_ctx_free(huv_server_t *s)
{
    if (!s->tls)
        return;
    mbedtls_ssl_config_free(&s->tls->conf);
    mbedtls_pk_free(&s->tls->pkey);
    mbedtls_x509_crt_free(&s->tls->cert);
    mbedtls_ctr_drbg_free(&s->tls->ctr_drbg);
    mbedtls_entropy_free(&s->tls->entropy);
    free(s->tls);
    s->tls = NULL;
}

/* BIO recv: called by mbedtls when it wants more encrypted bytes. We feed
 * from the inbox; when empty, return WANT_READ so mbedtls stops pulling and
 * the next on_read callback will top the inbox up. */
static int tls_bio_recv(void *ud, unsigned char *buf, size_t len)
{
    huv_tls_conn_t *t = ud;
    size_t avail = t->inbox_len - t->inbox_pos;
    if (avail == 0)
        return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = len < avail ? len : avail;
    memcpy(buf, t->inbox + t->inbox_pos, n);
    t->inbox_pos += n;
    if (t->inbox_pos == t->inbox_len)
        t->inbox_pos = t->inbox_len = 0;
    return (int)n;
}

/* BIO send: append encrypted bytes to the outbox. Never returns WANT_WRITE —
 * the outbox just grows, then we uv_write it after the ssl_* call returns. */
static int tls_bio_send(void *ud, const unsigned char *buf, size_t len)
{
    huv_tls_conn_t *t = ud;
    if (t->outbox_len + len > t->outbox_cap) {
        size_t newcap = t->outbox_cap ? t->outbox_cap * 2 : 4096;
        while (newcap < t->outbox_len + len)
            newcap *= 2;
        char *nb = realloc(t->outbox, newcap);
        if (!nb)
            return MBEDTLS_ERR_SSL_ALLOC_FAILED;
        t->outbox = nb;
        t->outbox_cap = newcap;
    }
    memcpy(t->outbox + t->outbox_len, buf, len);
    t->outbox_len += len;
    return (int)len;
}

int huv_tls_conn_attach(huv_conn_t *conn)
{
    huv_server_t *s = conn->server;
    if (!s->tls)
        return -1;

    huv_tls_conn_t *t = calloc(1, sizeof(*t));
    if (!t)
        return -1;
    mbedtls_ssl_init(&t->ssl);

    int rc = mbedtls_ssl_setup(&t->ssl, &s->tls->conf);
    if (rc != 0) {
        log_mbed_err(s, "ssl_setup", rc);
        mbedtls_ssl_free(&t->ssl);
        free(t);
        return -1;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, tls_bio_send, tls_bio_recv, NULL);
    conn->tls = t;
    return 0;
}

void huv_tls_conn_detach(huv_conn_t *conn)
{
    if (!conn->tls)
        return;
    mbedtls_ssl_free(&conn->tls->ssl);
    free(conn->tls->inbox);
    free(conn->tls->outbox);
    free(conn->tls);
    conn->tls = NULL;
}

/* Copy the outbox into a write-buffer and hand it to the raw write path;
 * on_write_end frees it. Outbox is cleared whether or not the write was
 * accepted (on failure the conn is closing anyway). */
static int flush_outbox(huv_conn_t *conn)
{
    huv_tls_conn_t *t = conn->tls;
    if (t->outbox_len == 0)
        return 0;
    size_t n = t->outbox_len;
    char *chunk = huv_conn_alloc_write(n);
    if (!chunk) {
        t->outbox_len = 0;
        return -1;
    }
    memcpy(chunk, t->outbox, n);
    t->outbox_len = 0;
    if (huv_conn_submit_raw_write(conn, chunk, n) < 0) {
        huv_conn_free_write(chunk);
        return -1;
    }
    return 0;
}

/* Append fresh encrypted bytes from libuv into the inbox. */
static int inbox_append(huv_tls_conn_t *t, const char *data, size_t len)
{
    if (t->inbox_len + len > HUV_TLS_INBOX_HARD_CAP)
        return -1;
    if (t->inbox_len + len > t->inbox_cap) {
        size_t newcap = t->inbox_cap ? t->inbox_cap * 2 : 4096;
        while (newcap < t->inbox_len + len)
            newcap *= 2;
        if (newcap > HUV_TLS_INBOX_HARD_CAP)
            newcap = HUV_TLS_INBOX_HARD_CAP;
        char *nb = realloc(t->inbox, newcap);
        if (!nb)
            return -1;
        t->inbox = nb;
        t->inbox_cap = newcap;
    }
    memcpy(t->inbox + t->inbox_len, data, len);
    t->inbox_len += len;
    return 0;
}

/* Drive the SSL state machine as far as the current inbox contents allow.
 *
 * Called from on_read after fresh encrypted bytes arrived. Three phases:
 *   1. handshake (until handshake_done)
 *   2. pull plaintext via ssl_read, feed each chunk to the parser
 *   3. flush outbox so any bytes mbedtls produced reach the wire
 *
 * On fatal SSL error the conn is closed. */
static void tls_pump(huv_conn_t *conn)
{
    huv_tls_conn_t *t = conn->tls;

    if (!t->handshake_done) {
        int rc = mbedtls_ssl_handshake(&t->ssl);
        if (rc == 0) {
            t->handshake_done = true;
        } else if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
                   rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbed_err(conn->server, "ssl_handshake", rc);
            flush_outbox(conn); /* deliver any alert we emitted */
            t->fatal = true;
            huv_conn_close(conn);
            return;
        } else {
            /* Need more bytes in either direction — just flush what we have. */
            flush_outbox(conn);
            return;
        }
    }

    unsigned char buf[4096];
    for (;;) {
        int n = mbedtls_ssl_read(&t->ssl, buf, sizeof buf);
        if (n > 0) {
            huv_conn_feed_parser(conn, (const char *)buf, (size_t)n);
            if (conn->closing)
                return;
        } else if (n == MBEDTLS_ERR_SSL_WANT_READ ||
                   n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            break;
        } else if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            flush_outbox(conn);
            huv_conn_close(conn);
            return;
        } else {
            log_mbed_err(conn->server, "ssl_read", n);
            t->fatal = true;
            flush_outbox(conn);
            huv_conn_close(conn);
            return;
        }
    }

    flush_outbox(conn);
}

void huv_tls_on_read(huv_conn_t *conn, const char *buf, size_t len)
{
    if (conn->tls->fatal || conn->closing)
        return;
    if (inbox_append(conn->tls, buf, len) < 0) {
        huv_log(conn->server, HUV_LOG_WARN,
                "tls inbox over cap — closing connection");
        conn->tls->fatal = true;
        huv_conn_close(conn);
        return;
    }
    tls_pump(conn);
}

int huv_tls_encrypt_and_flush(huv_conn_t *conn, const char *data, size_t len)
{
    if (conn->tls->fatal || conn->closing)
        return -1;

    /* ssl_write may chunk into 16-KiB records; loop until all plaintext is
     * accepted. Each call drives the send BIO, accumulating encrypted bytes
     * into the outbox. We flush once at the end. */
    size_t off = 0;
    while (off < len) {
        int n = mbedtls_ssl_write(&conn->tls->ssl,
                                  (const unsigned char *)data + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == MBEDTLS_ERR_SSL_WANT_READ ||
                   n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* Our BIOs never actually block, so this shouldn't happen post-
             * handshake; treat as fatal to avoid a spin loop. */
            log_mbed_err(conn->server, "ssl_write unexpected WANT", n);
            conn->tls->fatal = true;
            return -1;
        } else {
            log_mbed_err(conn->server, "ssl_write", n);
            conn->tls->fatal = true;
            return -1;
        }
    }
    return flush_outbox(conn);
}
