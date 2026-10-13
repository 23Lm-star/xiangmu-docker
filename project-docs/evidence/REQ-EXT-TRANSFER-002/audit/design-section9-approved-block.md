## 9. 冻结面逐字批准稿（F2）

在两端 `protocol.h` 的 `MAX_PARALLEL_DOWNLOADS = 3;` 行**之后**、`#endif // PROTOCOL_H` **之前**插入以下 4 行（含空行分隔，两文件逐字节一致）：

```cpp

// ===== REQ-EXT-TRANSFER-002：下载续传起点（两端逐字节一致，仅追加；0 = 从头） =====
static const int DOWNLOAD_RESUME_OFFSET = 32;             // u64；40/105 请求：客户端声明的续传起点
static const int DOWNLOAD_ACCEPTED_OFFSET = 24;           // u64；41/106 响应：服务端实际采纳的起点
```

零删除、零重编号、零改动既有行。`parallel_e2e` 的 P1-frozen-block 正则终点为既有块尾行，插入点在其之后 → 该断言不受影响。

---

