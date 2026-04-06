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

1. **Step 1**: Evaluate and choose a QUIC library (ngtcp2+nghttp3 vs quiche). Build and
   integrate as a dependency alongside nghttp2.
2. **Step 2**: Add UDP socket support to the event loop (`xSocket`). Implement QUIC timer
   management (retransmission, idle timeout) integrated with the existing loop.
3. **Step 3**: Implement QUIC connection management — Connection ID lookup table,
   connection creation on Initial packet, and connection migration handling.
4. **Step 4**: Implement the HTTP/3 protocol handler (`proto_h3.c`) behind `xHttpProto`,
   mapping nghttp3 callbacks to `xHttpStream_` dispatch, reusing the existing routing
   and ResponseWriter infrastructure.
5. **Step 5**: Add `Alt-Svc` header advertisement on HTTP/1.1 and HTTP/2 responses so
   clients can discover and upgrade to HTTP/3.
6. **Step 6**: Testing — interop testing with curl (--http3), browsers, and h3spec.
