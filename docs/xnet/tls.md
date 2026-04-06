# tls.h — TLS Configuration Types

## Introduction

`tls.h` defines the TLS configuration structures shared
across xKit modules. `xTlsClientConf` controls how a client
verifies the server and optionally presents a client
certificate (mTLS). `xTlsServerConf` provides the server's
certificate, private key, and optional client verification
settings. These are plain data types with no behavior — the
actual TLS handshake is handled by the TLS backend
(OpenSSL or mbedTLS) in the xhttp transport layer.

## Design Philosophy

1. **Backend-Agnostic** — The config structs contain only
   file paths and flags. They work identically whether the
   TLS backend is OpenSSL or mbedTLS.

2. **Zero-Initialize for Defaults** — A zero-initialized
   `xTlsClientConf` uses the system CA bundle with full
   peer and host verification enabled. This is the secure
   default.

3. **Separation of Concerns** — TLS configuration is
   defined in xnet (the networking primitives layer) and
   consumed by xhttp (the HTTP layer). This avoids circular
   dependencies and allows future modules to reuse the same
   types.

## API Reference

### xTlsClientConf

Client-side TLS configuration.

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `ca` | `const char *` | `NULL` (system CA) | Path to CA certificate file |
| `cert` | `const char *` | `NULL` (none) | Path to client certificate (for mTLS) |
| `key` | `const char *` | `NULL` (none) | Path to client private key |
| `key_password` | `const char *` | `NULL` (none) | Private key password |
| `skip_verify` | `int` | `0` (verify) | Non-zero to skip peer & host verification |

### xTlsServerConf

Server-side TLS configuration.

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `cert` | `const char *` | (required) | Path to PEM certificate file |
| `key` | `const char *` | (required) | Path to PEM private key file |
| `ca` | `const char *` | `NULL` (none) | Path to CA file for client verification |
| `verify_peer` | `int` | `0` (none) | Peer verification mode (see below) |

### verify_peer Modes

| Value | Mode | Behavior |
| --- | --- | --- |
| `0` | None | No client certificate requested |
| `1` | Optional | Request client cert, allow without |

### One-Way TLS (Client Verifies Server)

```c
#include <xnet/tls.h>
#include <xhttp/client.h>

// Use system CA bundle (zero-init)
xTlsClientConf tls = {0};
xHttpClientConf conf = {.tls = &tls};
xHttpClient client =
    xHttpClientCreate(loop, &conf);

// Or specify a CA file
xTlsClientConf tls_ca = {0};
tls_ca.ca = "ca.pem";
xHttpClientConf conf_ca = {.tls = &tls_ca};
xHttpClient client2 =
    xHttpClientCreate(loop, &conf_ca);
```

### Skip Verification (Development Only)

```c
xTlsClientConf tls = {0};
tls.skip_verify = 1;  // DANGER: disables all checks
xHttpClientConf conf = {.tls = &tls};
xHttpClient client =
    xHttpClientCreate(loop, &conf);
```

### Mutual TLS (mTLS)

```c
// Server: require client certificate
xTlsServerConf server_tls = {
    .cert        = "server.pem",
    .key         = "server-key.pem",
    .ca          = "ca.pem",
    .verify_peer = 2,  // required
};
xHttpServerListenTls(server, "0.0.0.0", 8443,
                     &server_tls);

// Client: present certificate
xTlsClientConf client_tls = {0};
client_tls.ca   = "ca.pem";
client_tls.cert = "client.pem";
client_tls.key  = "client-key.pem";
xHttpClientConf client_conf = {
    .tls = &client_tls,
};
xHttpClient client =
    xHttpClientCreate(loop, &client_conf);
```

### Password-Protected Private Key

```c
xTlsClientConf tls = {0};
tls.ca           = "ca.pem";
tls.cert         = "client.pem";
tls.key          = "client-key-enc.pem";
tls.key_password = "my-secret";
xHttpClientConf conf = {.tls = &tls};
xHttpClient client =
    xHttpClientCreate(loop, &conf);
```

## Relationship with Other Modules

- **xhttp** — The HTTP client and server consume these
  types via `xHttpClientConf.tls` (at creation time)
  and `xHttpServerListenTls()`. See the
  [TLS Deployment Guide](../xhttp/tls.md) for end-to-end
  examples including certificate generation.
- **xhttp transport layer** — The actual TLS handshake
  implementation lives in `xhttp/transport_tls_*.c`,
  supporting both OpenSSL and mbedTLS backends.

## Security Notes

- **Never use `skip_verify = 1` in production.** It
  disables all certificate validation.
- **Keep private keys secure.** Use restrictive file
  permissions (`chmod 600`).
- **Prefer `verify_peer = 2`** for mTLS deployments.
  Mode `1` (optional) allows unauthenticated clients.
- **The config structs do not copy strings.** The caller
  must ensure that file path strings remain valid until
  `xHttpClientCreate()` or `xHttpServerListenTls()`
  returns (the library deep-copies them internally).
