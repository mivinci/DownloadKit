# tls.h — TLS Configuration Types

## Introduction

`tls.h` defines `xTlsConf`, the unified TLS configuration structure shared across xKit modules. It controls certificate loading, peer verification, and optional ALPN negotiation for both client-side and server-side TLS. `xTlsCtx` is an opaque handle to a server-level TLS context created from `xTlsConf`. These are plain data types with no behavior — the actual TLS handshake is handled by the TLS backend (OpenSSL or mbedTLS) in the transport layer.

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

Opaque handle to a server-level TLS context. Created by `xTlsCtxCreate()`, shared across all connections accepted by a listener. Destroyed by `xTlsCtxDestroy()`.

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

- **xhttp** — The HTTP client and server consume `xTlsConf` via `xHttpClientConf.tls` (at creation time) and `xHttpServerListenTls()`. See the [TLS Deployment Guide](../xhttp/tls.md) for end-to-end examples including certificate generation.
- **xhttp transport layer** — The actual TLS handshake implementation lives in `xhttp/transport_tls_*.c`, supporting both OpenSSL and mbedTLS backends.

## Security Notes

- **Never use `skip_verify = 1` in production.** It disables all certificate validation.
- **Keep private keys secure.** Use restrictive file permissions (`chmod 600`).
- **For mTLS, set `ca` to the signing CA on the server side.** Zero-initialized `skip_verify` means verification is enabled by default.
- **The config struct does not copy strings.** The caller must ensure that file path strings remain valid until `xHttpClientCreate()` or `xHttpServerListenTls()` returns (the library deep-copies them internally).
