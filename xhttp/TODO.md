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

| Feature        | HTTP/1.1 (llhttp)              | HTTP/2 (nghttp2)                    |
|----------------|--------------------------------|-------------------------------------|
| Parsing unit   | byte stream → request          | byte stream → frame → stream        |
| Multiplexing   | None (pipeline at best)        | Native, multiple streams per conn   |
| Headers        | Plain text key: value          | HPACK compressed                    |
| Flow control   | None                           | Built-in per-stream flow control    |
| SSE            | chunked transfer               | DATA frames on a stream             |

### Implementation Roadmap

1. ~~**Step 1 (low cost)**: Extract llhttp-related fields from `xHttpConn_` behind an `xHttpProto`
   interface. Minimal change, no impact on existing functionality.~~ ✅ **Done** — `xHttpProto`
   vtable defined in `server_private.h`, HTTP/1.1 handler isolated in `proto_h1.c`.
2. **Step 2**: Add TLS support (OpenSSL/BoringSSL) with ALPN negotiation, or support h2c
   (cleartext HTTP/2) first for internal service-to-service communication.
3. **Step 3**: Integrate nghttp2, implement the HTTP/2 protocol handler, and plug it into
   the `xHttpProto` interface.

Upper-layer APIs (routing, SSE, ResponseWriter) should require zero changes — user code
works transparently with both HTTP/1.1 and HTTP/2.
