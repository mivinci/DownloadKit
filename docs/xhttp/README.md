# xhttp — Asynchronous HTTP

## Introduction

**xhttp** is xKit's HTTP module, providing both a fully asynchronous HTTP **client** and **server**, all powered by xbase's event loop.

- The **client** uses libcurl's multi-socket API for non-blocking HTTP requests and SSE streaming — ideal for integrating with REST APIs and LLM streaming endpoints. Supports TLS configuration including custom CA certificates, mutual TLS (mTLS), and certificate verification control via `xHttpTlsClientConf`.
- The **server** uses an `xHttpProto` vtable interface for protocol-abstracted parsing, supporting both HTTP/1.1 (llhttp) and HTTP/2 (nghttp2, h2c Prior Knowledge) on the same port. TLS listeners are supported via `xHttpServerListenTls` with `xHttpTlsServerConf`. Single-threaded, event-driven connection handling — ideal for building lightweight HTTP services and APIs.
- **WebSocket** support is built into the server via `xWsUpgrade()`. Call it inside a regular HTTP handler to perform the RFC 6455 upgrade handshake; the library then handles frame codec, ping/pong, fragment reassembly, and close negotiation automatically.

## Design Philosophy

1. **Event Loop Integration** — Instead of blocking threads, xhttp registers libcurl's sockets with [`xEventLoop`](../xbase/event.md) and uses event-driven I/O. All callbacks are dispatched on the event loop thread, eliminating the need for synchronization.

2. **Vtable-Based Request Polymorphism** — Internally, different request types (oneshot HTTP, SSE streaming) share the same curl multi handle but use different vtables for completion and cleanup. This avoids code duplication while supporting diverse response handling patterns.

3. **Zero-Copy Response Delivery** — Response headers and body are accumulated in [`xBuffer`](../xbuf/buf.md) instances and delivered to the callback as pointers. No extra copies are made.

4. **Automatic Resource Management** — Request contexts, curl easy handles, and buffers are automatically cleaned up after the completion callback returns. In-flight requests are cancelled with error callbacks when the client is destroyed.

## Architecture

```mermaid
graph TD
    subgraph "Application"
        APP["User Code"]
    end

    subgraph "xhttp"
        CLIENT["xHttpClient"]
        TLS_CLI["TLS Config<br/>(xHttpTlsClientConf)"]
        ONESHOT["Oneshot Request<br/>(GET/POST/Do)"]
        SSE["SSE Request<br/>(GetSse/DoSse)"]
        PARSER["SSE Parser<br/>(W3C spec)"]
    end

    subgraph "libcurl"
        MULTI["curl_multi"]
        EASY1["curl_easy (req 1)"]
        EASY2["curl_easy (req 2)"]
    end

    subgraph "xbase"
        LOOP["xEventLoop"]
        TIMER["Timer<br/>(curl timeout)"]
        FD["FD Events<br/>(socket I/O)"]
    end

    APP -->|"xHttpClientGet/Post/Do"| ONESHOT
    APP -->|"xHttpClientGetSse/DoSse"| SSE
    APP -->|"xHttpClientSetTls"| TLS_CLI
    SSE --> PARSER
    ONESHOT --> CLIENT
    SSE --> CLIENT
    TLS_CLI --> CLIENT
    CLIENT --> MULTI
    MULTI --> EASY1
    MULTI --> EASY2
    MULTI -->|"CURLMOPT_SOCKETFUNCTION"| FD
    MULTI -->|"CURLMOPT_TIMERFUNCTION"| TIMER
    FD --> LOOP
    TIMER --> LOOP

    style CLIENT fill:#4a90d9,color:#fff
    style LOOP fill:#50b86c,color:#fff
    style MULTI fill:#f5a623,color:#fff
```

## Sub-Module Overview

| File | Description | Doc |
| --- | --- | --- |
| `server.h` | Async HTTP/1.1 & HTTP/2 server (routing, request/response, protocol-abstracted parsing) | [server.md](server.md) |
| `client.h` | Async HTTP client API (GET, POST, Do, SSE, TLS configuration) | [client.md](client.md) |
| `client_sse.c` | SSE stream parser and request handler | [client_sse.md](client_sse.md) |
| `ws.h` | WebSocket server API (upgrade, send, close, callbacks) | [websocket.md](websocket.md) |
| *(guide)* | TLS deployment guide (certificate generation, one-way TLS, mTLS, troubleshooting) | [tls.md](tls.md) |

## Quick Start

```c
#include <stdio.h>
#include <xbase/event.h>
#include <xhttp/client.h>

static void on_response(const xHttpResponse *resp, void *arg) {
    (void)arg;
    if (resp->curl_code == 0) {
        printf("Status: %ld\n", resp->status_code);
        printf("Body: %.*s\n", (int)resp->body_len, resp->body);
    } else {
        printf("Error: %s\n", resp->curl_error);
    }
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();
    xHttpClient client = xHttpClientCreate(loop);

    xHttpClientGet(client, "https://httpbin.org/get", on_response, NULL);

    xEventLoopRun(loop);

    xHttpClientDestroy(client);
    xEventLoopDestroy(loop);
    return 0;
}
```

## Relationship with Other Modules

- **xbase** — Uses [`xEventLoop`](../xbase/event.md) for I/O multiplexing and [`xEventLoopTimerAfter`](../xbase/timer.md) for curl timeout management.
- **xbuf** — Uses [`xBuffer`](../xbuf/buf.md) for response header and body accumulation.
- **libcurl** — External dependency (client). Uses the multi-socket API (`curl_multi_socket_action`) for non-blocking HTTP.
- **llhttp** — External dependency (server). Provides incremental HTTP/1.1 request parsing, isolated behind the `xHttpProto` vtable in `proto_h1.c`.
- **nghttp2** — External dependency (server). Provides HTTP/2 frame processing and HPACK header compression, isolated behind the `xHttpProto` vtable in `proto_h2.c`.
