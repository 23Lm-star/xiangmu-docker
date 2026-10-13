# BUG-FILE-001 修复方案 — 文件下载分块传输

> **优先级**：P2（一般）
> **裁决人**：等待项目架构师审阅（需选择分块策略）
> **执行人**：项目工程师

---

## 问题

`Server/msghandler.cpp:484-522` 中 `downloadfile()` 使用 `QFile::readAll()` 一次性读入整个文件到内存，再封装到一个 PDU 中发送。大文件（>100MB）会严重消耗服务端内存。

**当前代码**（第 504-506 行）：
```cpp
QFile file(canonicalFilePath);
if(file.open(QIODevice::ReadOnly)){
    fileData=file.readAll();  // ← 一次性全部读入内存
    file.close();
    ret=true;
}
```

## 影响范围

1 个文件：`Server/msghandler.cpp`

## 分块策略（二选一）

### 方案 A：状态机分块（推荐）

在 `MyTcpSocket` 中维护下载状态，每次 `handlMsg` 返回一个分块，通过 `QTimer` 持续发送后续分块。

**优点**：不改变现有 PDU 生命周期，内存可控。
**缺点**：需要新增状态管理。

**需要改动**：`Server/mytcpsocket.h` + `Server/mytcpsocket.cpp` + `Server/msghandler.cpp`

### 方案 B：直接在 `downloadfile()` 中循环发送

在 `Msghandler::downloadfile()` 中直接循环读取分块并调用 `this->write()` 发送。

**优点**：实现简单，无需额外状态。
**缺点**：破坏了 `handlMsg` → `seedMsg` 的单次 PDU 返回模式，需要调整 PDU 内存管理。

---

## 建议实现（方案 A — 状态机分块）

### 1. `Server/mytcpsocket.h` — 新增下载状态

```cpp
// 下载分块状态
struct DownloadChunkState {
    QString filePath;
    QString fileName;
    QByteArray remainingData;
    qint64 totalSize;
    qint64 sentSize;
    bool active;
};

class MyTcpSocket : public QTcpSocket
{
    // ... 现有成员 ...
    
private:
    DownloadChunkState m_downloadState;
    QTimer* m_chunkTimer;
    
    void sendNextChunk();
    void abortDownload();
};
```

### 2. `Server/mytcpsocket.cpp` — 实现分块发送

```cpp
const qint64 CHUNK_SIZE = 64 * 1024; // 64KB 每块

void MyTcpSocket::sendNextChunk()
{
    if (!m_downloadState.active) {
        m_chunkTimer->stop();
        return;
    }
    
    qint64 toSend = qMin(CHUNK_SIZE, m_downloadState.remainingData.size());
    QByteArray chunk = m_downloadState.remainingData.left(toSend);
    m_downloadState.remainingData.remove(0, toSend);
    
    // 封装为 PDU
    PDU* chunkPdu = mkPDU(chunk.size());
    chunkPdu->uiType = ENUM_MSG_TYPE_DOWNLOADFILE_DATA;  // 需新增枚举
    memcpy(chunkPdu->caData, &m_downloadState.sentSize, sizeof(qint64));
    memcpy(chunkPdu->caData + 8, &m_downloadState.totalSize, sizeof(qint64));
    memcpy(chunkPdu->caMsg, chunk.constData(), chunk.size());
    
    this->write((char*)chunkPdu, chunkPdu->uiTotalLen);
    memfree(chunkPdu);
    
    m_downloadState.sentSize += toSend;
    
    // 判断是否完成
    if (m_downloadState.remainingData.isEmpty()) {
        m_downloadState.active = false;
        m_chunkTimer->stop();
    }
}
```

### 3. `Server/msghandler.cpp:downloadfile()` — 改为启动分块传输

```cpp
PDU* MsgHandler::downloadfile()
{
    // ... 路径校验代码保持不变 ...
    
    QFile file(canonicalFilePath);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray fileData = file.readAll();  // 仍需一次读入（但后续分块发送）
        file.close();
        ret = true;
        
        // 将数据转移到 MyTcpSocket 的分块状态
        // 通过信号/槽或直接调用 MyTcpSocket 的方法
        // 注意：这里需要 MyTcpSocket 实例的引用
    }
    
    // 返回元信息 PDU（文件总大小）
    PDU* respdu = mkPDU();
    respdu->uiType = ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND;
    memcpy(respdu->caData, &ret, sizeof(ret));
    memcpy(respdu->caData + sizeof(qint64), &fileSize, sizeof(qint64));
    return respdu;
}
```

> **⚠️ 注意**：方案 A 的完整实现需要确定 MyTcpSocket 如何从 MsgHandler 获取文件数据。有两个子方案：
> - **A1**：`downloadfile()` 返回特殊标记 PDU，MyTcpSocket 自行打开文件并分块发送
> - **A2**：`downloadfile()` 将文件数据暂存到 MyTcpSocket 成员变量中

### 简化方案（推荐 A1）

`downloadfile()` 只做路径校验，返回文件总大小。`MyTcpSocket` 收到 `DOWNLOADFILE_RESPOND` 后自行读取文件并分块发送：

```cpp
// msghandler.cpp — downloadfile 简化
PDU* MsgHandler::downloadfile()
{
    // 路径校验（与现有代码相同）...
    
    // 只返回文件元信息：是否存在、文件大小、文件名
    qint64 fileSize = ret ? fileInfo.size() : 0;
    
    PDU* respdu = mkPDU();
    respdu->uiType = ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND;
    memcpy(respdu->caData, &ret, sizeof(ret));
    memcpy(respdu->caData + 8, &fileSize, sizeof(qint64));
    memcpy(respdu->caData + 16, fileName.toUtf8().constData(), qMin(fileName.size(), 31));
    return respdu;
}
```

## 验证

```bash
cd Server && qmake && make clean && make
cd Client && qmake && make clean && make
```

**通过标准**：两端编译成功，大文件（>100MB）下载不造成服务端内存激增。

## 手工测试步骤

1. 在服务端文件目录准备一个 200MB 以上的测试文件
2. 启动 Server + Client
3. 从客户端发起下载该文件
4. 观察服务端内存使用，应处于稳定状态（非一次性暴涨）
5. 下载完成后客户端文件完整，MD5 校验与源文件一致

## 不在本次范围

- 下载断点续传 — 未来功能增强
- 上传分块改造 — 上传已使用 `ResumableUploadSession` 实现分块
