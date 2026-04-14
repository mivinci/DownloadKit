# xfer — P2P File Transfer

## Introduction

**xfer** is xKit's peer-to-peer file transfer module, providing a high-level API for sending and receiving files over WebRTC DataChannels. Built on top of [xp2p](../xp2p/README.md), it handles the full transfer pipeline — signaling server rendezvous, SDP/ICE exchange, file chunking, integrity verification (SHA-1), progress reporting, and resume support — all driven by the xKit event loop.

The module ships with a built-in signaling server (`xSignalServer`) and client (`xSignalClient`) that handle session creation, peer pairing, and SDP/ICE relay over WebSocket. Applications only need to provide a file path (sender) or a transfer code (receiver) to initiate a transfer. The transfer code (e.g. `AB12CD`) is a short, plain session ID assigned by the signaling server. Both sender and receiver must connect to the same signaling server.

## Design Philosophy

1. **Zero-Configuration P2P** — The sender registers with a signaling server and receives a short transfer code (session ID). The receiver uses this code along with the signaling server URL to connect. NAT traversal, encryption, and chunking are handled automatically.

2. **Event-Driven, Single-Threaded** — All callbacks (state changes, progress, errors) are invoked on the xKit event loop thread, consistent with the rest of the xKit stack.

3. **Resumable Transfers** — The wire protocol includes a `FILE_RESUME` message with a bitmap of received chunks, enabling the sender to skip already-transferred chunks after a reconnection.

4. **Integrity Verification** — Files are SHA-1 hashed before transfer. The receiver verifies the hash after reassembly, detecting corruption or incomplete transfers.

5. **Layered Architecture** — The module is cleanly separated into three layers: the high-level `xTransfer` API, the signaling layer (`xSignalServer` / `xSignalClient`), and the binary wire protocol (`xfer_protocol.h`). Each layer can be used independently.

6. **Pluggable Storage Backend** — All file I/O (reading the source file, writing the received file) goes through a `xTransferVfs` interface. The default implementation uses POSIX `fopen`/`fread`/`fwrite`, but callers can supply a custom VFS for in-memory transfers, encrypted storage, cloud-backed storage, or any other backend.

## Architecture

### Component Stack

```mermaid
graph TD
    subgraph "Application"
        APP["User Application"]
        CUSTOM_VFS["Custom VFS<br/>(optional)"]
    end

    subgraph "xfer"
        XFER["xTransfer<br/>xfer.h"]
        SENDER["Sender Logic<br/>xfer_sender.c"]
        RECEIVER["Receiver Logic<br/>xfer_receiver.c"]
        VFS["xTransferVfs<br/>xfer_vfs.h"]
        VFS_POSIX["POSIX VFS<br/>xfer_vfs_posix.c"]
        SIG_C["xSignalClient<br/>xfer_signal.h"]
        SIG_S["xSignalServer<br/>xfer_signal.h"]
        PROTO["Wire Protocol<br/>xfer_protocol.h"]
    end

    subgraph "xp2p"
        PC["xPeerConnection<br/>peer_connection.h"]
    end

    subgraph "xhttp"
        WS_S["WebSocket Server"]
        WS_C["WebSocket Client"]
    end

    subgraph "xbase"
        EV["xEventLoop<br/>event.h"]
    end

    APP --> XFER
    CUSTOM_VFS -.-> VFS
    XFER --> SENDER
    XFER --> RECEIVER
    SENDER --> VFS
    RECEIVER --> VFS
    VFS --> VFS_POSIX
    XFER --> SIG_C
    XFER --> PC
    XFER --> PROTO
    SIG_S --> WS_S
    SIG_C --> WS_C
    PC --> EV
    WS_S --> EV
    WS_C --> EV

    style XFER fill:#4a90d9,color:#fff
    style SENDER fill:#4a90d9,color:#fff
    style RECEIVER fill:#4a90d9,color:#fff
    style VFS fill:#e74c3c,color:#fff
    style VFS_POSIX fill:#e74c3c,color:#fff
    style CUSTOM_VFS fill:#e74c3c,color:#fff,stroke-dasharray: 5 5
    style SIG_C fill:#50b86c,color:#fff
    style SIG_S fill:#50b86c,color:#fff
    style PROTO fill:#f5a623,color:#fff
    style PC fill:#9b59b6,color:#fff
```

### Transfer Flow

```mermaid
sequenceDiagram
    participant Sender
    participant SignalServer
    participant Receiver

    Note over Sender: xTransferSendFile()
    Sender->>SignalServer: WebSocket connect + "create"
    SignalServer-->>Sender: code = "AB12CD"
    Note over Sender: on_code("AB12CD")

    Note over Receiver: xTransferRecvFile("AB12CD")
    Receiver->>SignalServer: WebSocket connect + "join(AB12CD)"
    SignalServer-->>Sender: peer_joined
    SignalServer-->>Receiver: joined

    Sender->>SignalServer: SDP offer
    SignalServer->>Receiver: SDP offer
    Receiver->>SignalServer: SDP answer
    SignalServer->>Sender: SDP answer

    Note over Sender,Receiver: ICE candidates exchanged via SignalServer

    Note over Sender,Receiver: P2P DataChannel established

    Sender->>Receiver: FILE_META (name, size, sha1)
    loop For each chunk
        Sender->>Receiver: FILE_CHUNK (id, data)
        Note over Receiver: on_progress()
    end
    Sender->>Receiver: FILE_DONE (total_chunks, sha1)
    Receiver->>Sender: FILE_ACK (status)
    Note over Sender: on_state_change(Done)
    Note over Receiver: on_state_change(Done)
```

### Wire Protocol

All messages are sent over the WebRTC DataChannel in binary. Multi-byte integers use network byte order (big-endian).

```text
┌──────────────────────────────────────────────────────────────┐
│  FILE_META   │ type(1B) │ name_len(2B) │ name │ size(8B)    │
│              │          │ chunk_sz(4B) │ sha1(20B)           │
├──────────────────────────────────────────────────────────────┤
│  FILE_CHUNK  │ type(1B) │ chunk_id(4B) │ data(variable)     │
├──────────────────────────────────────────────────────────────┤
│  FILE_DONE   │ type(1B) │ total_chunks(4B) │ sha1(20B)      │
├──────────────────────────────────────────────────────────────┤
│  FILE_ACK    │ type(1B) │ status(1B)                        │
├──────────────────────────────────────────────────────────────┤
│  FILE_RESUME │ type(1B) │ total_chunks(4B) │ bitmap_len(4B) │
│              │ bitmap(variable)                              │
└──────────────────────────────────────────────────────────────┘
```

| Message Type | Value | Direction | Description |
| --- | --- | --- | --- |
| `XFER_MSG_FILE_META` | 0x01 | Sender → Receiver | File metadata (name, size, chunk size, SHA-1) |
| `XFER_MSG_FILE_CHUNK` | 0x02 | Sender → Receiver | File data chunk |
| `XFER_MSG_FILE_DONE` | 0x03 | Sender → Receiver | Transfer complete signal |
| `XFER_MSG_ACK` | 0x04 | Receiver → Sender | Acknowledgement (success/failure) |
| `XFER_MSG_ERROR` | 0x05 | Both | Error message |
| `XFER_MSG_CANCEL` | 0x06 | Both | Cancel transfer |
| `XFER_MSG_FILE_RESUME` | 0x07 | Receiver → Sender | Resume bitmap for skipping received chunks |

## Sub-Module Overview

| Header / Source | Component | Description |
| --- | --- | --- |
| `xfer.h` | `xTransfer` | High-level file transfer API — send/receive files with progress and state callbacks |
| `xfer_vfs.h` | `xTransferVfs` | Virtual file system interface for pluggable storage backends |
| `xfer_vfs_posix.c` | `xTransferPosixVfs` | Built-in POSIX VFS implementation (`fopen`/`fread`/`fwrite`) |
| `xfer_sender.c` | Sender Logic | Sender-side data flow: file reading, chunking, flow control |
| `xfer_receiver.c` | Receiver Logic | Receiver-side data flow: message parsing, file writing, SHA-1 verification |
| `xfer_private.h` | Internal Header | Shared internal structures and helpers (not part of the public API) |
| `xfer_signal.h` | `xSignalServer` | WebSocket-based signaling server for session management and SDP/ICE relay |
| `xfer_signal.h` | `xSignalClient` | Signaling client for connecting to the server and exchanging SDP/ICE |
| `xfer_protocol.h` | Wire Protocol | Binary message encoding/decoding for file metadata, chunks, and control messages |

## API Reference

### Constants

| Constant | Value | Description |
| --- | --- | --- |
| `XFER_DEFAULT_CHUNK_SIZE` | 64 KB | Default chunk size for file transfer |
| `XFER_MAX_FILENAME_LEN` | 256 | Maximum file name length |
| `XFER_MAX_CODE_LEN` | 128 | Maximum session code length |

### Types

| Type | Description |
| --- | --- |
| `xTransfer` | Opaque handle to a transfer session |
| `xTransferState` | Enum: `Idle`, `WaitingPeer`, `Connecting`, `Transferring`, `Done`, `Failed` |
| `xTransferRole` | Enum: `Sender`, `Receiver` |
| `xTransferConf` | Configuration struct with P2P settings, signaling URL, VFS, and callbacks |
| `xTransferVfs` | Virtual file system interface — function pointers for open/pread/pwrite/close/etc. |

### Callbacks

| Callback | Signature | Description |
| --- | --- | --- |
| `xTransferOnStateChange` | `void (*)(xTransfer, xTransferState, void *ctx)` | State transition notification |
| `xTransferOnProgress` | `void (*)(xTransfer, uint64_t transferred, uint64_t total, void *ctx)` | Progress reporting |
| `xTransferOnCode` | `void (*)(xTransfer, const char *code, void *ctx)` | Sender receives session code |
| `xTransferOnFileMeta` | `void (*)(xTransfer, const char *filename, uint64_t filesize, void *ctx)` | Receiver learns file metadata |
| `xTransferOnError` | `void (*)(xTransfer, xErrno, const char *msg, void *ctx)` | Error notification |
| `xTransferOnIceCandidate` | `void (*)(xTransfer, const char *candidate, void *ctx)` | ICE candidate gathered |

### VFS (Virtual File System)

The `xTransferVfs` struct (defined in `xfer_vfs.h`) abstracts all file I/O. Pass a custom VFS via `xTransferConf.vfs`, or leave it `NULL` to use the default POSIX implementation.

| Field | Signature | Required | Description |
| --- | --- | --- | --- |
| `ctx` | `void *` | — | Opaque context forwarded to all callbacks |
| `open` | `void *(*)(void *ctx, const char *path, const char *mode)` | ✅ | Open a file, returns opaque handle or NULL |
| `pread` | `xErrno (*)(void *ctx, void *handle, uint8_t *buf, size_t len, uint64_t offset, size_t *nread)` | ✅ | Random-access read at offset |
| `pwrite` | `xErrno (*)(void *ctx, void *handle, const uint8_t *buf, size_t len, uint64_t offset, size_t *nwritten)` | ✅ | Random-access write at offset |
| `size` | `xErrno (*)(void *ctx, void *handle, uint64_t *out_size)` | ✅ | Get total file size |
| `truncate` | `xErrno (*)(void *ctx, void *handle, uint64_t size)` | Optional | Pre-allocate / truncate storage |
| `flush` | `xErrno (*)(void *ctx, void *handle)` | ✅ | Flush buffered data to persistent storage |
| `close` | `void (*)(void *ctx, void *handle)` | ✅ | Close the handle |
| `rename` | `xErrno (*)(void *ctx, const char *from, const char *to)` | Optional | Rename a file |
| `remove` | `xErrno (*)(void *ctx, const char *path)` | Optional | Remove a file |

| Function | Signature | Description |
| --- | --- | --- |
| `xTransferPosixVfs` | `const xTransferVfs *xTransferPosixVfs(void)` | Return the built-in POSIX VFS (valid for the lifetime of the process) |

### Transfer Lifecycle

| Function | Signature | Description |
| --- | --- | --- |
| `xTransferCreate` | `xTransfer xTransferCreate(xEventLoop loop, const xTransferConf *conf)` | Create a transfer session |
| `xTransferDestroy` | `void xTransferDestroy(xTransfer xfer)` | Destroy and free all resources |
| `xTransferSendFile` | `xErrno xTransferSendFile(xTransfer xfer, const char *filepath)` | Start sending a file |
| `xTransferRecvFile` | `xErrno xTransferRecvFile(xTransfer xfer, const char *code, const char *dest_dir)` | Start receiving a file |
| `xTransferGetState` | `xTransferState xTransferGetState(xTransfer xfer)` | Query current state |
| `xTransferGetRole` | `xTransferRole xTransferGetRole(xTransfer xfer)` | Query role (sender/receiver) |
| `xTransferCancel` | `void xTransferCancel(xTransfer xfer)` | Cancel an in-progress transfer |

### SDP Negotiation (Advanced)

These functions are used internally by the signaling client but are exposed for manual SDP exchange scenarios:

| Function | Signature | Description |
| --- | --- | --- |
| `xTransferCreateOffer` | `char *xTransferCreateOffer(xTransfer xfer)` | Create SDP offer (sender, caller frees) |
| `xTransferCreateAnswer` | `char *xTransferCreateAnswer(xTransfer xfer)` | Create SDP answer (receiver, caller frees) |
| `xTransferSetLocalDescription` | `xErrno xTransferSetLocalDescription(xTransfer xfer, const char *sdp)` | Set local SDP |
| `xTransferSetRemoteDescription` | `xErrno xTransferSetRemoteDescription(xTransfer xfer, const char *sdp)` | Set remote SDP |
| `xTransferGatherCandidates` | `xErrno xTransferGatherCandidates(xTransfer xfer)` | Start ICE gathering |

### Signaling Server

| Function | Signature | Description |
| --- | --- | --- |
| `xSignalServerCreate` | `xSignalServer xSignalServerCreate(xEventLoop loop, const xSignalServerConf *conf)` | Create and start a signaling server |
| `xSignalServerDestroy` | `void xSignalServerDestroy(xSignalServer server)` | Destroy the server |

### Signaling Client

| Function | Signature | Description |
| --- | --- | --- |
| `xSignalClientCreate` | `xSignalClient xSignalClientCreate(xEventLoop loop, const xSignalClientConf *conf)` | Create and connect to signaling server |
| `xSignalClientDestroy` | `void xSignalClientDestroy(xSignalClient client)` | Destroy the client |
| `xSignalClientSendOffer` | `xErrno xSignalClientSendOffer(xSignalClient client, const char *sdp)` | Send SDP offer |
| `xSignalClientSendAnswer` | `xErrno xSignalClientSendAnswer(xSignalClient client, const char *sdp)` | Send SDP answer |
| `xSignalClientSendCandidate` | `xErrno xSignalClientSendCandidate(xSignalClient client, const char *candidate)` | Send ICE candidate |

## State Machine

```mermaid
stateDiagram-v2
    [*] --> Idle: xTransferCreate()
    Idle --> WaitingPeer: xTransferSendFile() / xTransferRecvFile()
    WaitingPeer --> Connecting: Peer joined, SDP exchanged
    Connecting --> Transferring: DataChannel opened
    Transferring --> Done: All chunks transferred + ACK
    Transferring --> Failed: Error / Cancel
    WaitingPeer --> Failed: Signaling error
    Connecting --> Failed: ICE / DTLS failure
    Done --> [*]
    Failed --> [*]
```

## Quick Start

### Sending a File

```c
#include <xbase/event.h>
#include <xfer/xfer.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>

static xEventLoop g_loop;
static xTransfer  g_xfer;

static void on_state_change(xTransfer xfer, xTransferState state, void *ctx) {
  (void)xfer; (void)ctx;
  switch (state) {
  case xTransferState_Done:
    printf("\n✅ Transfer complete!\n");
    xEventLoopStop(g_loop);
    return;
  case xTransferState_Failed:
    printf("\n❌ Transfer failed.\n");
    xEventLoopStop(g_loop);
    return;
  default: break;
  }
}

static void on_progress(xTransfer xfer, uint64_t transferred,
                        uint64_t total, void *ctx) {
  (void)xfer; (void)ctx;
  printf("\rProgress: %llu / %llu bytes (%.1f%%)   ",
         (unsigned long long)transferred, (unsigned long long)total,
         total > 0 ? 100.0 * transferred / total : 0.0);
  fflush(stdout);
}

static void on_code(xTransfer xfer, const char *code, void *ctx) {
  (void)xfer; (void)ctx;
  printf("Share this code with the receiver:\n  %s\n", code);
}

int main(void) {
  g_loop = xEventLoopCreate();

  xTransferConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.stun_server     = "stun.l.google.com:19302";
  conf.signal_server   = "ws://127.0.0.1:8080/ws";
  conf.on_state_change = on_state_change;
  conf.on_progress     = on_progress;
  conf.on_code         = on_code;
  conf.vfs             = NULL; /* NULL = default POSIX VFS */

  g_xfer = xTransferCreate(g_loop, &conf);
  xTransferSendFile(g_xfer, "myfile.bin");

  xEventLoopRun(g_loop);

  xTransferDestroy(g_xfer);
  xEventLoopDestroy(g_loop);
  return 0;
}
```

### Receiving a File

```c
#include <xbase/event.h>
#include <xfer/xfer.h>

#include <stdio.h>
#include <string.h>

static xEventLoop g_loop;
static xTransfer  g_xfer;

static void on_state_change(xTransfer xfer, xTransferState state, void *ctx) {
  (void)xfer; (void)ctx;
  switch (state) {
  case xTransferState_Done:
    printf("\n✅ File received!\n");
    xEventLoopStop(g_loop);
    return;
  case xTransferState_Failed:
    printf("\n❌ Transfer failed.\n");
    xEventLoopStop(g_loop);
    return;
  default: break;
  }
}

static void on_progress(xTransfer xfer, uint64_t transferred,
                        uint64_t total, void *ctx) {
  (void)xfer; (void)ctx;
  printf("\rProgress: %llu / %llu bytes (%.1f%%)   ",
         (unsigned long long)transferred, (unsigned long long)total,
         total > 0 ? 100.0 * transferred / total : 0.0);
  fflush(stdout);
}

static void on_file_meta(xTransfer xfer, const char *filename,
                         uint64_t filesize, void *ctx) {
  (void)xfer; (void)ctx;
  printf("Incoming: \"%s\" (%llu bytes)\n",
         filename, (unsigned long long)filesize);
}

int main(void) {
  g_loop = xEventLoopCreate();

  xTransferConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.stun_server     = "stun.l.google.com:19302";
  conf.signal_server   = "ws://127.0.0.1:8080/ws";
  conf.on_state_change = on_state_change;
  conf.on_progress     = on_progress;
  conf.on_file_meta    = on_file_meta;

  g_xfer = xTransferCreate(g_loop, &conf);
  xTransferRecvFile(g_xfer, "AB12CD", "/tmp/received");

  xEventLoopRun(g_loop);

  xTransferDestroy(g_xfer);
  xEventLoopDestroy(g_loop);
  return 0;
}
```

### Running the Examples

The `examples/` directory includes complete sender and receiver programs:

```bash
# Terminal 1: Start the signaling server (built-in)
# The signaling server is started automatically by xfer when needed,
# or you can run a standalone one.
./xfer_signal -p 8080

# Terminal 2: Send a file
./xfer_send -f myfile.bin -u ws://127.0.0.1:8080/ws

# Terminal 3: Receive the file (use the code printed by the sender)
./xfer_recv -c AB12CD -u ws://127.0.0.1:8080/ws -d /tmp/received
```

Command-line options:

| Option | `xfer_send` | `xfer_recv` | Description |
| --- | --- | --- | --- |
| `-f <file>` | ✅ Required | — | File to send |
| `-c <code>` | — | ✅ Required | Transfer code from sender (plain session ID) |
| `-d <dir>` | — | Optional | Destination directory (default: `/tmp/xfer_recv`) |
| `-u <url>` | ✅ Required | ✅ Required | Signaling server URL |
| `-s <host:port>` | Optional | Optional | STUN server (default: `stun.l.google.com:19302`) |
| `-6` | Optional | Optional | Enable IPv6 candidates |

## Relationship with Other Modules

- **[xp2p](../xp2p/README.md)** — Uses `xPeerConnection` for the full WebRTC DataChannel stack (ICE + DTLS + SCTP + DataChannel). xfer creates a PeerConnection internally and sends/receives file data over a DataChannel.
- **[xhttp](../xhttp/README.md)** — The signaling server and client use xhttp's WebSocket server and client for SDP/ICE relay.
- **[xbase](../xbase/README.md)** — Uses [`xEventLoop`](../xbase/event.md) for I/O multiplexing and the single-threaded callback model.
- **[xcrypto](../xcrypto/README.md)** — Uses SHA-1 for file integrity verification.
- **[xnet](../xnet/README.md)** — Uses URL parsing for signaling server addresses.

## Custom VFS Example

The following example shows how to implement a minimal in-memory VFS for testing:

```c
#include <xfer/xfer_vfs.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  uint64_t size;
  uint64_t capacity;
} MemFile;

static void *mem_open(void *ctx, const char *path, const char *mode) {
  (void)ctx; (void)path; (void)mode;
  MemFile *f = calloc(1, sizeof(MemFile));
  return f;
}

static xErrno mem_pread(void *ctx, void *handle, uint8_t *buf,
                        size_t len, uint64_t offset, size_t *nread) {
  (void)ctx;
  MemFile *f = handle;
  if (offset >= f->size) { *nread = 0; return xErrno_Ok; }
  size_t avail = (size_t)(f->size - offset);
  size_t n = len < avail ? len : avail;
  memcpy(buf, f->data + offset, n);
  *nread = n;
  return xErrno_Ok;
}

static xErrno mem_pwrite(void *ctx, void *handle, const uint8_t *buf,
                         size_t len, uint64_t offset, size_t *nwritten) {
  (void)ctx;
  MemFile *f = handle;
  uint64_t end = offset + len;
  if (end > f->capacity) {
    f->data = realloc(f->data, (size_t)end);
    f->capacity = end;
  }
  memcpy(f->data + offset, buf, len);
  if (end > f->size) f->size = end;
  *nwritten = len;
  return xErrno_Ok;
}

static xErrno mem_size(void *ctx, void *handle, uint64_t *out) {
  (void)ctx;
  *out = ((MemFile *)handle)->size;
  return xErrno_Ok;
}

static xErrno mem_flush(void *ctx, void *handle) {
  (void)ctx; (void)handle;
  return xErrno_Ok; /* no-op for in-memory */
}

static void mem_close(void *ctx, void *handle) {
  (void)ctx;
  MemFile *f = handle;
  if (f) { free(f->data); free(f); }
}

static const xTransferVfs g_mem_vfs = {
  .ctx      = NULL,
  .open     = mem_open,
  .pread    = mem_pread,
  .pwrite   = mem_pwrite,
  .size     = mem_size,
  .truncate = NULL,  /* optional */
  .flush    = mem_flush,
  .close    = mem_close,
  .rename   = NULL,  /* optional */
  .remove   = NULL,  /* optional */
};

/* Usage: */
xTransferConf conf;
memset(&conf, 0, sizeof(conf));
conf.vfs = &g_mem_vfs;
/* ... set other fields ... */
xTransfer xfer = xTransferCreate(loop, &conf);
```
