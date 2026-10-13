# BUG-FILE-002 修复方案 — MOVEFILE_REQUEST 响应类型错误

> **优先级**：P1（一般）
> **承接**：同时解决 BUG-ARCH-001-R1（MOVEFILE_RESPOND 枚举值未被引用）
> **裁决人**：等待项目架构师审阅
> **执行人**：项目工程师

---

## 问题

1. `MOVEFILE_REQUEST(38)` 与 `SHAREFILE_AGREE_RESPOND(36)` 共用 `sharefilemove()` 函数
2. `sharefilemove()` 始终返回 `SHAREFILE_FINISH_RESPOND(37)`，而非 `MOVEFILE_RESPOND(39)`
3. `sharefilemove()` 做的是 `QFile::copy`（拷贝），不是真正的移动（rename）
4. Client 端 `handlMsg()` 无 `MOVEFILE_RESPOND` case
5. `MOVEFILE_RESPOND(39)` 枚举值定义后无任何引用（死代码）

## 影响范围

4 个文件：`Server/msghandler.h`、`Server/msghandler.cpp`、`Server/mytcpsocket.cpp`、`Client/client.cpp`

## 修改清单

### 1. `Server/msghandler.h`（第 36 行后）— 新增声明

```diff
     PDU* sharefilemove();//分享文件移动
+    PDU* movefile();      //移动文件（独立处理函数）
     PDU* uploadfileinit();//上传文件初始化
```

### 2. `Server/msghandler.cpp` — 新增 `movefile()` 实现

在 `sharefilemove()` 函数之后新增：

```cpp
// 移动文件（独立处理函数）
PDU* MsgHandler::movefile()
{
    QString sourcePath = pdu->caMsg;
    int index = sourcePath.lastIndexOf('/');
    QString fileName = sourcePath.right(sourcePath.size() - index - 1);
    QString tarPath = QString("%1/%2/%3")
        .arg(Server::getinstance().filesPath)
        .arg(pdu->caData)
        .arg(fileName);

    qDebug() << "movefile source:" << sourcePath << "target:" << tarPath;

    bool ret = false;
    // 优先使用 rename（同一文件系统下为原子操作）
    QFile file(sourcePath);
    if (file.rename(tarPath)) {
        ret = true;
    } else {
        // 跨设备回退到 copy + remove
        if (QFile::copy(sourcePath, tarPath)) {
            QFile::remove(sourcePath);
            ret = true;
        }
    }

    PDU* respdu = mkPDU();
    memcpy(respdu->caData, &ret, sizeof(ret));
    respdu->uiType = ENUM_MSG_TYPE_MOVEFILE_RESPOND;
    return respdu;
}
```

### 3. `Server/msghandler.cpp:sharefilemove()` — 修复拷贝行为

```diff
 PDU* MsgHandler::sharefilemove()
 {
     QString sourcePath = pdu->caMsg;
     int indexi = sourcePath.lastIndexOf('/');
     QString fileName = sourcePath.right(sourcePath.size() - indexi - 1);
     QString tarPath = QString("%1/%2/%3").arg(Server::getinstance().filesPath).arg(pdu->caData).arg(fileName);
     qDebug() << "sourcePath:" << sourcePath << "tarPath:" << tarPath << endl;
-    bool ret = QFile::copy(sourcePath, tarPath);
+    // 优先 rename（同一文件系统下的原子移动）
+    QFile file(sourcePath);
+    bool ret = file.rename(tarPath);
+    if (!ret) {
+        // 跨设备回退到 copy + remove
+        ret = QFile::copy(sourcePath, tarPath);
+        if (ret) {
+            QFile::remove(sourcePath);
+        }
+    }
     PDU* respdu = mkPDU();
     memcpy(respdu->caData, &ret, sizeof(ret));
     respdu->uiType = ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND;
     return respdu;
 }
```

### 4. `Server/mytcpsocket.cpp` — 拆分分发逻辑

```diff
-    //分享文件同意回应
+    // 分享文件同意回应（拷贝到目标目录）
     case ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND:{
         respdu = m_MsgHandler->sharefilemove();
         break;
     }
-    //移动文件请求
+    // 移动文件请求（真正移动到目标目录）
     case ENUM_MSG_TYPE_MOVEFILE_REQUEST:{
-        respdu = m_MsgHandler->sharefilemove();
+        respdu = m_MsgHandler->movefile();
         break;
     }
```

### 5. `Client/client.cpp:handlMsg()` — 新增 MOVEFILE_RESPOND case

在 `case ENUM_MSG_TYPE_CAPTCHA_RESPOND:` 之前添加：

```diff
+    case ENUM_MSG_TYPE_MOVEFILE_RESPOND:{
+        // 移动文件完成 — 收到后刷新文件列表
+        // 调用 m_mhptr->flushfile() 对应的刷新函数
+        // 或直接调用 flushfile 刷新界面
+        m_mhptr->flushfile();
+        QMessageBox::information(&Index::getinstance(), "提示", "文件移动完成");
+        break;
+    }
     case ENUM_MSG_TYPE_CAPTCHA_RESPOND:{
```

> **注意**：如果客户端 `MsgHandler` 没有独立的 `flushfile()` 触发方式，也可以在收到响应后安排一次文件列表刷新。具体调用方式需确认客户端 `MsgHandler` 的接口。

## 验证

```bash
# Server
cd Server && qmake && make clean && make

# Client
cd Client && qmake && make clean && make
```

**通过标准**：
- 两端编译成功，零错误零警告
- `MOVEFILE_RESPOND`(39) 在两端至少有 1 处引用（不再为死代码）
- 移动操作实际执行 `rename`（可观察文件位置变化）
- 客户端收到 `MOVEFILE_RESPOND` 后弹出提示并刷新文件列表

## 手工测试步骤

1. 启动 Server + 两个 Client（用户 A、用户 B）
2. 用户 A 选择一个文件并执行"移动"到用户 B 的目录
3. 预期：用户 A 的原文件消失，用户 B 看到该文件
4. 用户 A 刷新文件列表，确认文件已从原位置移除

## 不在本次范围

- BUG-ARCH-003（用户信息查询功能缺失）— 独立修复
- 其他文件操作功能的改动
