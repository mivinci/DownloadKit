# xKit HTTP Server 性能实测：吊打 Go？？？

姐妹们！我用自研的 C++ 事件循环框架 xKit 做了个完整的 HTTP 服务器性能测试，对比 Go 的 net/http，结果真的惊到我了！！

## 🏁 直接上结论

| 场景 | xKit | Go | 差距 |
|---|---|---|---|
| **HTTP/1.1** | 152K/s | 129K/s | xKit +18% ⚡️ |
| **HTTPS/1.1** | 125K/s | 128K/s | 持平 🤝 |
| **HTTP/2 (h2c)** | 562K/s | 121K/s | **xKit +365%** 🔥 |

## 📊 详细数据

### HTTP/1.1 (明文)
```
连接数   xKit        Go         
 50      152K/s     129K/s    → xKit +18%
100      152K/s     129K/s    → xKit +18%
500      155K/s     125K/s    → xKit +24%
```
单线程事件循环居然赢了 Go 的 goroutine！！

### HTTPS/1.1 (TLS 加密)
```
连接数   xKit        Go         
 50      125K/s     125K/s    → 持平
500      111K/s     122K/s    → Go +10%
```
TLS 加密后大家都被 AES 绑死，单线程/多线程差距消失～

### HTTP/2 (h2c 明文) ⚠️ 重点！
```
连接数   xKit         Go          
 50      576K/s      142K/s    → xKit +307%
100      562K/s      121K/s    → xKit +365%
200      556K/s      110K/s    → xKit +405%
```
**这是最离谱的！** HTTP/2 多路复用让单线程事件循环彻底起飞，吊打 Go 3-4 倍！！

## 🔍 关键发现

1. **HTTP/2 是 xKit 的主场** — 多路复用让单线程优势最大化
2. **HTTPS 拉平差距** — TLS 加密后大家差不多
3. **大 payload 还是 Go 强** — 64KB body 时 Go 反超 +46%

## 🛠️ 技术栈

- 单线程事件循环 (kqueue/epoll)
- 无锁队列 + 内存池
- 直接内存拷贝，不做无用copy

📍 项目：github.com/mivinci/xKit

#编程 #性能优化 #HTTP #Go #C++ #后端开发 #技术 #benchmark