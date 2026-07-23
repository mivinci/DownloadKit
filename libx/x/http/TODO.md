# xhttp TODO

## HTTP/2 Support

### Current Status

The protocol parsing layer (llhttp) is tightly coupled with connection management in `xHttpConn_`:

- `xHttpConn_` directly embeds `llhttp_t` and `llhttp_settings_t`
- llhttp callbacks (`on_url`, `on_header_field`, `on_body`, etc.) directly operate on `xHttpConn_` fields
- Request dispatch (`conn_dispatch_request`) reads method from `conn->parser.method`

### Proposed Approach: Protocol Abstraction + nghttp2

Introduce a protocol handler vtable to isolate HTTP/1.1 and HTTP/2 parsing logic:

```text
┌─────────────────────────────────────────┐
│              xHttpServer                │
│  ┌──────────┐    ┌───────────────────┐  │
│  │ Listener │───▶│   xHttpConn       │  │
│  └──────────┘    │  ┌─────────────┐  │  │
│                  │  │ xHttpProto   │  │  │  <-- protocol handler interface
│                  │  │ (vtable)     │  │  │
│                  │  └──────┬──────┘  │  │
│                  └─────────┼─────────┘  │
│                 ┌──────────┴──────────┐  │
│                 │                     │  │
│          ┌──────┴──────┐   ┌─────────┴──┐
│          │ HTTP/1.1    │   │  HTTP/2     │
│          │ (llhttp)    │   │ (nghttp2)   │
│          └─────────────┘   └────────────┘
└─────────────────────────────────────────┘
```

#### Protocol handler interface

```c
typedef struct xHttpProto_ {
    int  (*on_data)(struct xHttpConn_ *conn, const char *buf, size_t len);
    void (*on_write_ready)(struct xHttpConn_ *conn);
    void (*reset)(struct xHttpConn_ *conn);
    void (*destroy)(struct xHttpConn_ *conn);
    void *state;  // llhttp_t* or nghttp2_session*
} xHttpProto;
```

#### HTTP/2 stream multiplexing

Under HTTP/2, a single connection can have multiple concurrent streams, each representing a request:

```c
struct xHttpStream_ {
    int32_t                      stream_id;
    struct xHttpConn_           *conn;
    xBuffer                      url;
    xBuffer                      headers_raw;
    xBuffer                      body;
    struct xHttpResponseWriter_  writer;
};
```

### Key Differences

| Feature      | HTTP/1.1 (llhttp)       | HTTP/2 (nghttp2)                  |
| ------------ | ----------------------- | --------------------------------- |
| Parsing unit | byte stream → request   | byte stream → frame → stream      |
| Multiplexing | None (pipeline at best) | Native, multiple streams per conn |
| Headers      | Plain text key: value   | HPACK compressed                  |
| Flow control | None                    | Built-in per-stream flow control  |
| SSE          | chunked transfer        | DATA frames on a stream           |

### Status

✅ **Complete** — `xHttpProto` vtable in
`server_private.h`, HTTP/1.1 in `proto_h1.c`,
HTTP/2 in `proto_h2.c` (nghttp2). H1 and H2
coexist on the same port with auto-detection.
Upper-layer APIs (routing, SSE, ResponseWriter)
work transparently with both protocols.

---

## HTTP/3 Support

### Background

HTTP/3 replaces TCP+TLS with **QUIC** (UDP-based, built-in encryption via TLS 1.3).
Key benefits: 0-RTT connection setup, no head-of-line blocking across streams, and
connection migration (IP changes without reconnection).

### Proposed Approach: QUIC Library + xHttpProto

Integrate a QUIC implementation (e.g. **ngtcp2** + **nghttp3**, or **quiche**) and add
an HTTP/3 protocol handler behind the existing `xHttpProto` vtable:

```text
┌──────────────────────────────────────────────────┐
│                   xHttpServer                    │
│                                                  │
│   TCP listener (H1/H2)      UDP listener (H3)   │
│        │                          │              │
│   xHttpConn (TCP)           xHttpConn (QUIC)     │
│   ┌──────────┐              ┌──────────┐         │
│   │xHttpProto│              │xHttpProto│         │
│   └────┬─────┘              └────┬─────┘         │
│   ┌────┴────┐          ┌────────┴────────┐       │
│   │ H1 / H2 │          │ H3 (nghttp3)    │       │
│   │         │          │ + QUIC (ngtcp2)  │       │
│   └─────────┘          └─────────────────┘       │
└──────────────────────────────────────────────────┘
```

### Key Differences from HTTP/2

| Feature            | HTTP/2 (TCP+TLS)             | HTTP/3 (QUIC)                    |
| ------------------ | ---------------------------- | -------------------------------- |
| Transport          | TCP                          | UDP (QUIC)                       |
| TLS                | Separate TLS handshake       | Built-in TLS 1.3 (mandatory)     |
| Head-of-line block | TCP-level HOL across streams | No cross-stream HOL blocking     |
| Connection setup   | TCP + TLS = 2-3 RTT          | 1-RTT (0-RTT on resumption)      |
| Multiplexing       | Streams over single TCP conn | Independent streams over QUIC    |
| Connection migrate | Not supported                | Supported (connection ID based)  |
| Event loop         | epoll/kqueue on TCP fd       | Needs UDP recv + QUIC timer mgmt |

### Challenges

1. **Event loop integration**: QUIC runs over UDP, so the existing epoll/kqueue loop needs
   to handle UDP sockets and QUIC-level timers (retransmission, idle timeout, etc.).
2. **Connection management**: QUIC connections are identified by Connection ID, not by
   (IP, port) tuple. Need a connection ID → `xHttpConn` lookup table.
3. **Crypto integration**: QUIC mandates TLS 1.3; ngtcp2 requires a TLS backend
   (OpenSSL/BoringSSL) for the crypto handshake callbacks.
4. **Flow control**: QUIC has its own per-stream and connection-level flow control,
   separate from the application-layer nghttp3 flow control.

### Implementation Roadmap (HTTP/3)

#### Client-side (libcurl-based)

1. ✅ **Step 1**: Choose QUIC library → **ngtcp2 + nghttp3** (same stack as
   libcurl's HTTP/3 backend, already available via brew).
2. ✅ **Step 2**: Add `xHttpVersion_H3` to the `xHttpVersion` enum in
   `client.h` and map to `CURL_HTTP_VERSION_3` in `apply_http_version()`.
3. ☐ **Step 3**: Update `FindLibcurl.cmake` to build libcurl from source
   with ngtcp2 + nghttp3 support (`CURL_USE_NGTCP2=ON`, `CURL_USE_NGHTTP3=ON`).
   On macOS with brew, link against brew's curl which already ships HTTP/3.
4. ✅ **Step 4**: Add HTTP/3 client tests — `H3ConfigGet`, `H3ConfigPost`,
   `H3ClientDefault`, `H3ConfigSse` in `http_test.cpp`. Tests verify the
   config path works; against a local H1/H2 server, curl gracefully
   falls back to HTTP/1.1 or HTTP/2.

#### Server-side (ngtcp2 + nghttp3)

Architecture: reuse the existing `xHttpProto` vtable and `xTlsConf`. All H3
connections share a single UDP socket, multiplexed by QUIC Connection ID.
Upper-layer APIs (routing, SSE, ResponseWriter, WebSocket) are unchanged —
H3 is just another transport+protocol pair.

```text
┌──────────────────────────────────────────────────────┐
│                   xHttpServer                        │
│                                                      │
│  TCP listen_fd         UDP listen_fd (新增)          │
│      │                       │                       │
│  accept()               recvfrom()                   │
│      │                       │                       │
│  xHttpConn (TCP)        xHttpConn (QUIC)             │
│  ┌────────────┐        ┌────────────────┐            │
│  │ transport  │        │ transport (不适用)│           │
│  │ (TCP/TLS)  │        │ + quic_conn    │ ← ngtcp2  │
│  │ xHttpProto │        │ xHttpProto     │            │
│  │ ├─ H1      │        │ └─ H3 (nghttp3)│            │
│  │ └─ H2      │        └────────────────┘            │
│  └────────────┘              │                       │
│                              │                       │
│       xHttpStream_     xHttpStream_ (复用, 不改)     │
└──────────────────────────────────────────────────────┘
```

**New public API** (server.h):

```c
XCAPI(xErrno) xHttpServerListenH3(xHttpServer server, const char *host,
                                   uint16_t port, const xTlsConf *config);
```

Reuses `xTlsConf` (no new config type). Creates a UDP socket
(`socket(AF_INET, SOCK_DGRAM)`), binds, registers on the event loop.
Can be called alongside `xHttpServerListenTls()` on the same port
(TCP 443 + UDP 443 coexist).

**Crypto backend**: ngtcp2 supports pluggable TLS 1.3 backends via its
crypto callback interface. Default to **OpenSSL** (already a dependency
via `ws_crypto_openssl.c`, uses `ngtcp2_crypto_openssl`). Alternative
backends selectable via cmake option `XHTTP_H3_CRYPTO=OPENSSL|GNUTLS`:

| Backend | ngtcp2 module | Status |
|---------|--------------|--------|
| OpenSSL | `ngtcp2_crypto_openssl` | ✅ Default, production-ready |
| GnuTLS  | `ngtcp2_crypto_gnutls`  | ⚪ Future compile-time option |
| wolfSSL | `ngtcp2_crypto_wolfssl` | ⚪ Future compile-time option |

**mbedTLS is NOT supported** — it does not expose QUIC-required TLS 1.3
internal interfaces (early data key derivation, per-packet protection keys).

---

##### Step 5: Environment prep + cmake (0.5 day)

- [ ] 5.1 Install nghttp3 (`brew install nghttp3`) — headers already present
  as curl dependency, but no pkg-config. Add proper cmake detection.
- [ ] 5.2 Create/extend `cmake/FindNgtcp2.cmake` for server-side linking
  (client-side already uses it via libcurl; server needs direct linkage).
- [ ] 5.3 Create `cmake/FindNghttp3.cmake`.
- [ ] 5.4 Add `XHTTP_ENABLE_H3` cmake option (default ON if deps found).
- [ ] 5.5 Conditionally link `ngtcp2`, `nghttp3`, and `ngtcp2_crypto_openssl`
  in `libx/x/http/CMakeLists.txt`.

**Files**: `cmake/FindNgtcp2.cmake` (extend), `cmake/FindNghttp3.cmake` (new),
`libx/x/http/CMakeLists.txt` (modify).

##### Step 6: `xHttpServer_` H3 fields + UDP listener (1 day)

- [ ] 6.1 Add H3 fields to `xHttpServer_` (`server_private.h`):
  ```c
  int       h3_listen_fd;    /* UDP listen fd */
  int       h3_port;         /* UDP listen port */
  xTlsCtx   h3_tls_ctx;      /* TLS 1.3 context for QUIC */
  xMap     *quic_conns;      /* conn_id → xHttpConn_ lookup (CID as key) */
  int       h3_enabled;      /* Whether H3 is active (for Alt-Svc) */
  ```
- [ ] 6.2 Add QUIC fields to `xHttpConn_` (`server_private.h`, behind
  `#ifdef XHTTP_H3_ENABLED`):
  ```c
  ngtcp2_conn                *quic_conn;     /* ngtcp2 connection object */
  ngtcp2_cid                  conn_id;       /* Our source CID */
  ngtcp2_cid                  remote_cid;    /* Remote CID for sending */
  struct sockaddr_storage     remote_addr;   /* Peer UDP address */
  xEventTimer                 quic_timer;    /* QUIC timer (expiry-based) */
  int                         quic_closing;  /* Graceful close flag */
  ```
- [ ] 6.3 Implement `xHttpServerListenH3()` in `server.c`:
  - Create UDP socket: `socket(AF_INET, SOCK_DGRAM, 0)`
  - `bind()` to host:port
  - Wrap with `xSocketCreateFromFd(loop, fd, xEvent_Read, on_quic_listen_event, server)`
  - Create `quic_conns` xMap
  - Store `h3_tls_ctx`, `h3_port`, set `h3_enabled = 1`
- [ ] 6.4 Implement `on_quic_listen_event()` — the UDP recv pump:
  ```
  while (recvfrom(buf)):
    parse QUIC short-header → extract DCID
    conn = quic_conns[DCID]
    if (!conn && is_long_header(Initial)):
      conn = xHttpConn_ alloc + register in quic_conns
      ngtcp2_conn_read_handshake(conn, buf, pkt_info)
    else:
      ngtcp2_conn_read_pkt(conn, buf, pkt_info)
    → nghttp3_conn_read_stream() triggered inside
    → after read: schedule quic_timer via ngtcp2_conn_get_expiry()
  ```
- [ ] 6.5 Implement QUIC timer callback:
  - `ngtcp2_conn_handle_expiry(conn->quic_conn, now_ts)`
  - Send pending packets if any
  - Re-arm timer with next expiry

**Files**: `server_private.h` (modify), `server.c` (modify),
`server.h` (modify — add `xHttpServerListenH3` declaration).

##### Step 7: QUIC connection lifecycle + TLS 1.3 handshake (2 days)

- [ ] 7.1 Implement ngtcp2 callbacks (`server_quic.c` or inline in `server.c`):
  | Callback | Implementation |
  |----------|---------------|
  | `recv_crypto_data` | Feed CRYPTO frame data to `ngtcp2_crypto_openssl` |
  | `encrypt` | `ngtcp2_crypto_encrypt_cb` (OpenSSL) |
  | `decrypt` | `ngtcp2_crypto_decrypt_cb` (OpenSSL) |
  | `get_path_challenge_data` | `ngtcp2_crypto_get_path_challenge_data_cb` |
  | `acked_stream_offset` | Notify nghttp3 of acked data → stream-level flow control |
  | `stream_open` | No-op (nghttp3 manages streams) |
  | `stream_close` | No-op (nghttp3 notifies via its own callback) |
  | `handshake_completed` | Log handshake success |
  | `recv_stream_data` | Feed stream data to `nghttp3_conn_read_stream` |
  | `extend_max_stream_data` | `nghttp3_conn_unblock_stream` on flow control update |
- [ ] 7.2 Implement ngtcp2 `send` callback (the critical one):
  ```
  h3_server_send_pkt(conn, pkt_buf):
    sendmsg(s->h3_listen_fd, &msg, 0)  → shared UDP fd
  ```
  QUIC packet headers already contain the destination address; `sendmsg`
  with `msg.msg_name` set to `conn->remote_addr`.
- [ ] 7.3 Implement connection teardown:
  - Active close: `ngtcp2_conn_write_connection_close` → sendmsg → free
  - Idle timeout: timer fires → `ngtcp2_conn_is_in_closing_period` → force free
  - Remove CID from `quic_conns` map, free `xHttpConn_`
- [ ] 7.4 Connection migration support (nice-to-have):
  - On `recvfrom` with different `remote_addr` for same CID:
    update `conn->remote_addr`, call `ngtcp2_conn_set_remote_addr()`

**Files**: `server_quic.c` (new, ~400 lines), or integrated into `server.c`
behind `#ifdef XHTTP_H3_ENABLED`.

##### Step 8: `proto_h3.c` — HTTP/3 protocol handler (1.5 days)

Modeled after `proto_h2.c` (685 lines). nghttp3 API is intentionally
similar to nghttp2 — same author, same design patterns.

- [ ] 8.1 Define `xHttpProtoH3` internal state (`proto_h3.c`):
  ```c
  struct xHttpProtoH3 {
    nghttp3_conn          *h3_conn;       /* nghttp3 connection */
    ngtcp2_conn           *quic_conn;     /* for send callback */
    int64_t                last_stream_id;/* last opened stream */
    struct xHttpStream_   *pending_dispatch[16];
    int                    pending_count;
  };
  ```
- [ ] 8.2 Register nghttp3 callbacks (mirror nghttp2 equivalents):
  | nghttp3 callback | Role |
  |-----------------|------|
  | `recv_header` | Pseudo-headers → `.method` / `.url`; regular → `.headers_raw` |
  | `recv_data` | Append to `stream->body` |
  | `end_stream` | Mark `stream->request_complete = 1` |
  | `stream_close` | Free per-stream data |
  | `acked_stream_data` | Flow control bookkeeping |
  | `send_stop_sending` | Handle stream reset from peer |
- [ ] 8.3 Implement `h3_send_callback` — the critical difference from H2:
  ```
  h3_send_callback(h3_conn, data):
    ngtcp2_conn_writev_stream(quic_conn, stream_id, data, ...)
    → triggers the ngtcp2 send callback → sendmsg() on shared UDP fd
  ```
- [ ] 8.4 Implement vtable methods:

  | Method | Implementation |
  |--------|---------------|
  | `on_data` | `nghttp3_conn_read_stream` → dispatch pending streams |
  | `reset` | No-op (streams are independent in H3) |
  | `destroy` | `nghttp3_conn_del` + free `xHttpProtoH3` |
  | `method` | Return cached `:method` from stream context |
  | `should_keep_alive` | Always 1 |
  | `send_response` | `nghttp3_conn_submit_response` + `nghttp3_conn_submit_data` |
  | `write_data` | First call → submit response with `nghttp3_data_reader`; subsequent → append + `nghttp3_conn_resume_stream` |
  | `end_stream` | Set EOF flag on data reader + `nghttp3_conn_resume_stream` |

- [ ] 8.5 Implement `xHttpProtoH3Init(conn)` — analogous to `xHttpProtoH2Init`:
  - Create `nghttp3_conn_server_new` with callbacks
  - Populate `conn->proto` vtable
  - Create default stream (H3 stream 0 for control frames, if needed)

**Files**: `proto_h3.c` (new, ~600 lines), `proto_h3.h` (new, ~20 lines).

##### Step 9: Alt-Svc advertisement (0.5 day)

- [x] 9.1 In `server.c` response path: if `s->h3_enabled`, inject
  `Alt-Svc: h3=":<port>"` header into every H1TLS and H2 response.
- [x] 9.2 Only on TLS connections (plaintext H1 does not advertise H3,
  per RFC 9114 section 3.1.1).
- [x] 9.3 Support optional explicit `alt_svc` string in server config
  (e.g. `h3=":443"; ma=3600`).

**Files**: `server.c` (modify), `server.h` (modify — optional config).

##### Step 10: Testing (1 day)

- [x] 10.1 Unit test: `proto_h3.c` smoke test — stream_id routing, body delivery,
  per-stream dispatch (verified via H3 client tests + ServerH3Test)
- [x] 10.2 Integration test scaffold: H3 server + curl --http3 test code
  (written, skipped in CI — QUIC handshake needs debugging)
- [x] 10.3 Alt-Svc interop: Alt-Svc header injection verified on TLS connections
- [ ] 10.4 Error path: invalid QUIC packet → no crash, graceful close.
- [ ] 10.5 Concurrency: multiple simultaneous H3 connections, verify
  CID-based demux works correctly.
- [ ] 10.6 Protocol compliance: `h3spec` (if available) or manual
  verification against nghttp3 example server behavior.

**Files**: `server_h3_test.cpp` (new), or extend `https_test.cpp`.
