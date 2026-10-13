# BUG-ARCH-003 修复方案 — 用户信息查询功能 Server 端缺失

> **优先级**：P1（一般）
> **裁决人**：等待项目架构师审阅
> **执行人**：项目工程师

---

## 问题

Client 端 `userinformation.cpp` 发送 `GETUSERINFORMATION_REQUEST(44)` 查询用户信息，但：

- `Server/msghandler.h` — 无 `getuserinformation()` 声明
- `Server/msghandler.cpp` — 无对应处理函数
- `Server/mytcpsocket.cpp` — 无 `ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST` 分发 case
- `Client/client.cpp:handlMsg()` — 无 `GETUSERINFORMATION_RESPOND(45)` 响应处理

仅有协议枚举值定义（44/45），功能完全不可用。

## 影响范围

4 个文件：`Server/msghandler.h`、`Server/msghandler.cpp`、`Server/mytcpsocket.cpp`、`Client/client.cpp`

> 另需确认 `OperateDB` 是否需要新增查询用户详细信息接口。

## 修改清单

### 1. `Server/msghandler.h`（第 37 行后）— 新增声明

```diff
     PDU* sharefilemove();//分享文件移动
     PDU* movefile();      //移动文件
+    PDU* getuserinformation();//获取用户信息
     PDU* uploadfileinit();//上传文件初始化
```

### 2. `Server/msghandler.cpp` — 新增实现

在 `sharefilemove()` 之后添加：

```cpp
// 获取用户信息
PDU* MsgHandler::getuserinformation()
{
    // 从 caData 提取要查询的用户名（前 32 字节）
    char targetName[32] = {'\0'};
    memcpy(targetName, pdu->caData, 32);
    QString strTargetName = QString::fromUtf8(targetName).trimmed();

    qDebug() << "getuserinformation for:" << strTargetName;

    // 从数据库查询用户信息
    // 注意：当前 user_info 表字段: id, name, pwd, online
    // 返回信息：状态(在线/离线)、注册时间（需确认表结构）
    int ret = OperateDB::instance().handleFindUser(strTargetName.toUtf8().data());

    // 构造响应 PDU
    // caData[0..3]  = ret 状态码
    // caData[4..35] = 用户名（补齐到 32 字节）
    // caMsg         = 可选的详细信息（如注册时间等）
    PDU* respdu = mkPDU();
    memcpy(respdu->caData, &ret, sizeof(ret));
    memcpy(respdu->caData + 32, targetName, 32);
    respdu->uiType = ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND;
    return respdu;
}
```

### 3. `Server/mytcpsocket.cpp` — 新增 case

在 `ENUM_MSG_TYPE_CAPTCHA_REQUEST` case 之前添加：

```diff
+    case ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST:{
+        respdu = m_MsgHandler->getuserinformation();
+        break;
+    }
     case ENUM_MSG_TYPE_CAPTCHA_REQUEST:{
```

### 4. `Client/client.cpp:handlMsg()` — 新增响应处理

在 `ENUM_MSG_TYPE_CAPTCHA_RESPOND` case 之前添加：

```diff
+    case ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND:{
+        m_mhptr->handleGetUserInformation();
+        break;
+    }
     case ENUM_MSG_TYPE_CAPTCHA_RESPOND:{
```

> **注意**：需要在客户端的 `MsgHandler` 类中实现 `handleGetUserInformation()` 方法，解析响应并更新 UI 显示。具体实现取决于客户端 `userinformation.cpp` 期望接收的数据格式。

## 验证

```bash
# Server
cd Server && qmake && make clean && make

# Client
cd Client && qmake && make clean && make
```

**通过标准**：
- 两端编译成功，零错误零警告
- 客户端点击"用户信息"功能后，能显示该用户的信息

## 手工测试步骤

1. 启动 Server + 登录 Client
2. 在好友列表中选中一个用户
3. 右键/"查看用户信息"触发查询
4. 预期：收到响应并在界面显示用户信息（在线状态等）

## 不在本次范围

- 数据库表结构变更（如新增注册时间字段）— 如需扩展请架构师确认
- BUG-FILE-002（移动文件响应类型）— 独立修复
