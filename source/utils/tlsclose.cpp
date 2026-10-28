// ---------------------------------------------------------------------------
// TLS close_notify for libcurl's connections
// ---------------------------------------------------------------------------
// libcurl 8.4's mbedTLS backend ends a TLS connection without a close_notify
// alert: mbedtls_close() frees the context and the socket is closed under it.
// vsftpd 3.0.5 (strict_ssl_read_eof, set to 1 in its tunables.c whatever the
// man page says) reads an upload that ends without one as cut short and
// answers "426 Failure reading network stream" — so no TLS upload could
// succeed, however cleanly the socket itself closes (see _closeGracefully()
// in curl.cpp).
//
// The fix has to sit inside libcurl's teardown, which offers no callback.
// The link wraps two of the mbedTLS calls it makes (-Wl,--wrap in the
// Makefile):
//
//   mbedtls_ssl_setup(ssl, conf)   — remember which context uses which config;
//   mbedtls_ssl_config_free(conf)  — mbedtls_close() calls it while the
//                                    context and its socket are still alive,
//                                    so send close_notify on that context
//                                    first, then free.
//
// At that point libcurl has just read from the same context (its own
// "maybe the server sent close_notify" read), its connection filter has the
// transfer's data saved for the bio callbacks, and the socket filter below
// closes only afterwards — so writing one more record there is what libcurl
// itself would do if it sent the alert.  Only libcurl uses mbedTLS's SSL
// layer in this app; secrets.cpp uses the crypto primitives alone.
//
// Tied to libcurl 8.4's close order and mbedTLS 2.28's ssl->state.  Drop it
// once devkitPro's libcurl sends close_notify itself (curl 8.10 added TLS
// shutdown support).
// ---------------------------------------------------------------------------

#include <mbedtls/ssl.h>

#include <3ds.h>

#include "debug.h"

namespace
{

struct Pairing
{
    const mbedtls_ssl_config *conf;
    mbedtls_ssl_context *ssl;
};

// A run holds a control and a data connection per remote at most; eight is
// room to spare.  Every libcurl call is on the sync worker thread, but the
// lock costs nothing and keeps that from being an assumption.
const int MAX_PAIRINGS = 8;
Pairing g_pairings[MAX_PAIRINGS];
LightLock g_lock;
bool g_lockReady = false;

void lock()
{
    if (!g_lockReady)
    {
        LightLock_Init(&g_lock);
        g_lockReady = true;
    }
    LightLock_Lock(&g_lock);
}

void remember(const mbedtls_ssl_config *conf, mbedtls_ssl_context *ssl)
{
    lock();
    int slot = -1;
    for (int i = 0; i < MAX_PAIRINGS; i++)
    {
        if (g_pairings[i].conf == conf || (slot < 0 && g_pairings[i].conf == NULL))
            slot = i;
        if (g_pairings[i].conf == conf)
            break;
    }
    if (slot >= 0)
    {
        g_pairings[slot].conf = conf;
        g_pairings[slot].ssl = ssl;
    }
    LightLock_Unlock(&g_lock);
    if (slot < 0)
        debugf("TLS: more than %d connections open; this one will close "
               "without close_notify\n", MAX_PAIRINGS);
}

mbedtls_ssl_context *forget(const mbedtls_ssl_config *conf)
{
    mbedtls_ssl_context *ssl = NULL;
    lock();
    for (int i = 0; i < MAX_PAIRINGS; i++)
        if (g_pairings[i].conf == conf)
        {
            ssl = g_pairings[i].ssl;
            g_pairings[i].conf = NULL;
            g_pairings[i].ssl = NULL;
        }
    LightLock_Unlock(&g_lock);
    return ssl;
}

void sendCloseNotify(mbedtls_ssl_context *ssl)
{
    // Nothing to close on a connection whose handshake never finished.
    if (ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER)
        return;

    // The socket is non-blocking; a 31-byte alert goes out at once unless
    // the send buffer is full, so a few short waits are plenty.
    for (int attempt = 0; attempt < 20; attempt++)
    {
        int rc = mbedtls_ssl_close_notify(ssl);
        if (rc == 0)
            return;
        if (rc != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            debugf("TLS: close_notify failed (-0x%04X)\n", (unsigned)-rc);
            return;
        }
        svcSleepThread(10000000LL); // 10 ms
    }
    debugf("TLS: close_notify still pending after 200 ms, giving up\n");
}

} // namespace

extern "C" {

int __real_mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf);
void __real_mbedtls_ssl_config_free(mbedtls_ssl_config *conf);

int __wrap_mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf)
{
    int rc = __real_mbedtls_ssl_setup(ssl, conf);
    if (rc == 0)
        remember(conf, ssl);
    return rc;
}

void __wrap_mbedtls_ssl_config_free(mbedtls_ssl_config *conf)
{
    mbedtls_ssl_context *ssl = forget(conf);
    if (ssl)
        sendCloseNotify(ssl);
    __real_mbedtls_ssl_config_free(conf);
}

} // extern "C"
