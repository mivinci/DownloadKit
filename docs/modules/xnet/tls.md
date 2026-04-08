# tls.h — TLS Configuration Types

## Introduction

`tls.h` defines `xTlsConf`, the unified TLS configuration structure shared across xKit modules, and `xTlsCtx`, the opaque handle to a server-level TLS context. It controls certificate loading, peer verification, and optional ALPN negotiation for both client-side and server-side TLS. These are the central TLS abstractions — the actual TLS handshake is handled by the TLS backend (OpenSSL or mbedTLS) in the transport layer.

## Design Philosophy

1. **Backend-Agnostic** — The config struct contains only file paths and flags. It works identically whether the TLS backend is OpenSSL or mbedTLS.

2. **Zero-Initialize for Defaults** — A zero-initialized `xTlsConf` uses the system CA bundle with full peer and host verification enabled. This is the secure default for both client and server.

3. **Unified Client/Server** — A single `xTlsConf` struct serves both roles. Client-only fields (`key_password`) and server-only fields (`alpn`) are simply left as `NULL` / zero when unused.

4. **Separation of Concerns** — TLS configuration is defined in xnet (the networking primitives layer) and consumed by xhttp (the HTTP layer). This avoids circular dependencies and allows future modules to reuse the same types.

## API Reference

### xTlsConf

Unified TLS configuration for both client and server.

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `cert` | `const char *` | `NULL` (none) | Path to PEM certificate file |
| `key` | `const char *` | `NULL` (none) | Path to PEM private key file |
| `ca` | `const char *` | `NULL` (system CA) | Path to CA certificate file |
| `key_password` | `const char *` | `NULL` (none) | Private key password (client-side) |
| `alpn` | `const char **` | `NULL` (none) | NULL-terminated ALPN protocol list (server-side) |
| `skip_verify` | `int` | `0` (verify) | Non-zero to skip peer & host verification |

**Backward-compatible aliases:** `xTlsClientConf` and `xTlsServerConf` are `typedef`'d to `xTlsConf`.

### xTlsCtx

Opaque handle to a shared TLS context. Created by `xTlsCtxCreate()`, used by both server-side listeners (`xTcpListenerConf.tls_ctx`) and client-side connectors (`xTcpConnectConf.tls_ctx`, `xWsConnectConf.tls_ctx`). Shared across all connections that use the same context. Destroyed by `xTlsCtxDestroy()`. Supports certificate hot-reload via `xTlsCtxReload()`.

### xTlsCtxCreate

```c
xTlsCtx xTlsCtxCreate(const xTlsConf *conf);
```

Create a shared TLS context. Loads the certificate (if provided), private key (if provided), optional CA, and optional ALPN list. The returned context can be shared across all connections that use the same TLS configuration.

- `conf` — TLS configuration (must not be NULL). For server-side use, `cert` and `key` are required. For client-side use, only `ca` (or defaults) is needed.
- Returns a TLS context handle, or `NULL` on failure.

### xTlsCtxDestroy

```c
void xTlsCtxDestroy(xTlsCtx ctx);
```

Destroy a shared TLS context and release all resources. Safe to call with `NULL` (no-op). Must only be called after all connections using this context have been closed.

### xTlsCtxReload

```c
int xTlsCtxReload(xTlsCtx ctx, const xTlsConf *conf);
```

Hot-reload certificates for an existing TLS context. Atomically replaces the certificate, private key, and optional CA. Existing connections are **not** affected; only new connections will use the updated certificates.

- `ctx` — TLS context to reload (must not be NULL).
- `conf` — New TLS configuration (must not be NULL, `cert` and `key` must not be NULL).
- Returns `0` on success, `-1` on failure (context unchanged).

#### Example: Certificate hot-reload

```c
// Initial setup
xTlsConf tls = {
    .cert = "server.pem",
    .key  = "server-key.pem",
    .alpn = (const char *[]){"h2", "http/1.1", NULL},
};
xTlsCtx ctx = xTlsCtxCreate(&tls);

// ... later, when certificates are renewed ...
xTlsConf new_tls = {
    .cert = "server-new.pem",
    .key  = "server-key-new.pem",
    .alpn = (const char *[]){"h2", "http/1.1", NULL},
};
if (xTlsCtxReload(ctx, &new_tls) == 0) {
    // New connections will use the updated certificates
}
```

### One-Way TLS (Client Verifies Server)

```c
#include <xnet/tls.h>
#include <xhttp/client.h>

// Use system CA bundle (zero-init)
xTlsConf tls = {0};
xHttpClientConf conf = {.tls = &tls};
xHttpClient client = xHttpClientCreate(loop, &conf);

// Or specify a CA file
xTlsConf tls_ca = {0};
tls_ca.ca = "ca.pem";
xHttpClientConf conf_ca = {.tls = &tls_ca};
xHttpClient client2 = xHttpClientCreate(loop, &conf_ca);
```

### Skip Verification (Development Only)

```c
xTlsConf tls = {0};
tls.skip_verify = 1;  // DANGER: disables all checks
xHttpClientConf conf = {.tls = &tls};
xHttpClient client = xHttpClientCreate(loop, &conf);
```

### Mutual TLS (mTLS)

```c
// Server: require client certificate (default: verify enabled)
xTlsConf server_tls = {
    .cert = "server.pem",
    .key  = "server-key.pem",
    .ca   = "ca.pem",
};
xHttpServerListenTls(server, "0.0.0.0", 8443, &server_tls);

// Client: present certificate
xTlsConf client_tls = {0};
client_tls.ca   = "ca.pem";
client_tls.cert = "client.pem";
client_tls.key  = "client-key.pem";
xHttpClientConf client_conf = {
    .tls = &client_tls,
};
xHttpClient client = xHttpClientCreate(loop, &client_conf);
```

### Password-Protected Private Key

```c
xTlsConf tls = {0};
tls.ca           = "ca.pem";
tls.cert         = "client.pem";
tls.key          = "client-key-enc.pem";
tls.key_password = "my-secret";
xHttpClientConf conf = {.tls = &tls};
xHttpClient client = xHttpClientCreate(loop, &conf);
```

## Relationship with Other Modules

- **xnet** — `xTlsCtxCreate()` / `xTlsCtxDestroy()` / `xTlsCtxReload()` are declared in `tls.h` and implemented in the TLS backend files (`transport_openssl.c`, `transport_mbedtls.c`). The TCP listener uses `xTlsCtx` via `xTcpListenerConf.tls_ctx`, and the TCP connector uses it via `xTcpConnectConf.tls_ctx`.
- **xhttp** — The HTTP server calls `xTlsCtxCreate()` internally when `xHttpServerListenTls()` is invoked, automatically setting ALPN to `{"h2", "http/1.1"}`. The HTTP client uses libcurl for TLS management and consumes `xTlsConf` directly. The WebSocket client supports both `xTlsConf` (auto-creates a context) and a pre-created `xTlsCtx` (shared across connections) via `xWsConnectConf.tls_ctx`. See the [TLS Deployment Guide](../xhttp/tls.md) for end-to-end examples.

## Security Notes

- **Never use `skip_verify = 1` in production.** It disables all certificate validation.
- **Keep private keys secure.** Use restrictive file permissions (`chmod 600`).
- **For mTLS, set `ca` to the signing CA on the server side.** Zero-initialized `skip_verify` means verification is enabled by default.
- **The config struct does not copy strings.** The caller must ensure that file path strings remain valid until `xHttpClientCreate()` or `xHttpServerListenTls()` returns (the library deep-copies them internally).
