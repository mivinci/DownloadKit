# xbuf

xbuf 提供三种不同设计的字节缓冲区，用户可根据场景按需选择。

## 总览

| | **xBuffer** | **xRingBuffer** | **xIOBuffer** |
| --- | --- | --- | --- |
| 头文件 | `<xbuf/buf.h>` | `<xbuf/ring.h>` | `<xbuf/io.h>` |
| 内存模型 | 连续内存，2x 自动扩容 | 固定大小，power-of-2 掩码索引 | 8 KB block 链，引用计数 |
| 容量 | 动态增长 | 初始化时固定 | 动态增长（按 block 粒度） |
| 零拷贝 split | ✗ | ✗ | ✓ `xIOBufferCut` |
| 零拷贝 append | ✗ | ✗ | ✓ `xIOBufferAppendIOBuffer` |
| writev 支持 | 单段 | 最多 2 段 iov | 天然多段 iov |
| 随机访问 | O(1) | O(1) | O(n) |
| compact 成本 | memmove | 无需 | 无需 |
| block 池化 | — | — | Treiber stack 无锁 freelist |
| 实现复杂度 | ★☆☆ | ★★☆ | ★★★ |
| 适合场景 | 通用 / 简单协议 | 固定吞吐管道 | 高性能网络框架 |

## xBuffer — 线性自动扩容缓冲区

最简单直观的缓冲区。头部元数据和数据区通过 flexible array member 一次 `malloc` 分配，写入时自动 2 倍扩容（`realloc` 整体），读取后可通过 `xBufferCompact` 回收已消费空间。写入类 API 接受 `xBuffer**` 参数，因为扩容可能导致指针变化。

```c
#include <xbuf/buf.h>

xBuffer *buf = xBufferCreate(1024);

xBufferAppend(&buf, data, len);              // 写入，空间不足时自动扩容（可能 realloc）
process(xBufferData(buf), xBufferLen(buf));  // 读取可用数据
xBufferConsume(buf, processed);              // 消费已处理的字节
xBufferCompact(buf);                         // 将未读数据移到头部，回收空间

xBufferDestroy(buf);
```

**适用场景**：大多数通用场景、简单协议解析、单次请求/响应处理。

## xRingBuffer — 固定大小环形缓冲区

固定容量的环形缓冲区，头部元数据和数据区通过 flexible array member 一次 `malloc` 分配。容量自动向上取整为 2 的幂次以使用位掩码替代取模运算。不会重新分配内存，写满时执行部分写入。

```c
#include <xbuf/ring.h>

xRingBuffer *rb = xRingBufferCreate(8192);

size_t n = xRingBufferWrite(rb, data, len); // 写入，返回实际写入字节数（空间不足时部分写入）
size_t n = xRingBufferRead(rb, out, sz); // 读取并消费
xRingBufferPeek(rb, out, sz);            // 只读不消费

// scatter-gather I/O
struct iovec iov[2];
int cnt = xRingBufferReadIov(rb, iov);   // 获取可读 iov（最多 2 段）
writev(fd, iov, cnt);

xRingBufferDestroy(rb);
```

**适用场景**：固定吞吐的生产者-消费者管道、嵌入式或内存受限环境、需要避免 realloc 的实时路径。

## xIOBuffer — 引用计数块链缓冲区

借鉴 brpc IOBuf 的设计，由引用计数的固定大小 block（默认 8 KB）组成链式结构。支持零拷贝的 split 和 append，天然适配 `writev` 的 scatter-gather I/O。

```c
#include <xbuf/io.h>

xIOBuffer io;
xIOBufferInit(&io);

xIOBufferAppend(&io, data, len);          // 写入，按 block 粒度分配

// 零拷贝拆分：将前 N 字节切到另一个 IOBuf
xIOBuffer header;
xIOBufferInit(&header);
xIOBufferCut(&io, &header, header_len);   // 只移动 ref，不拷贝数据

// 零拷贝拼接：将 other 的全部数据移入 io
xIOBufferAppendIOBuffer(&io, &other);        // other 被清空，block 引用转移

// scatter-gather writev
struct iovec iov[64];
int cnt = xIOBufferReadIov(&io, iov, 64);
writev(fd, iov, cnt);

// block 池管理
xIOBlockPoolWarmup(32);                // 启动时预热
xIOBlockPoolDrain();                   // 关闭时清理

xIOBufferDeinit(&io);
```

**适用场景**：高性能网络框架、HTTP/RPC 协议解析（header/body 零拷贝拆分）、需要频繁拼接转发的代理场景。

## 如何选择

```plain
需要自动扩容、API 简单？
  └─ 是 → xBuffer

容量固定、不想 realloc？
  └─ 是 → xRingBuffer

需要零拷贝 split/append、scatter-gather I/O？
  └─ 是 → xIOBuffer
```
