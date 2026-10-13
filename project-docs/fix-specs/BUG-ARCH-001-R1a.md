# BUG-ARCH-001-R1a 修复方案

> **优先级**：最高（阻塞 — Client 无法编译）
> **裁决人**：项目架构师
> **裁决时间**：2026-07-24
> **执行人**：项目工程师

---

## 问题

`Client/userinformation.cpp:15` 引用 `ENUM_MSG_TYPE_GETUSERINFORMSTION_REQUEST`，该枚举值在两端 `protocol.h` 中均未定义。

## 影响范围

仅 3 个文件、6 行代码。`setMsg()` 当前无调用者，属死代码，仅需编译通过。Server 端功能缺失另归 BUG-ARCH-003。

## 修改清单

### 1. `Client/protocol.h`（第 58 行后）

```diff
     ENUM_MSG_TYPE_CAPTCHA_REQUEST,//验证码请求 42
     ENUM_MSG_TYPE_CAPTCHA_RESPOND,//验证码响应 43
+    ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST,//获取用户信息请求 44
+    ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND,//获取用户信息响应 45
     ENUM_MSG_TYPE_MAX=99999,
```

### 2. `Server/protocol.h`（第 58 行后）

完全同上，两端保持一致。

### 3. `Client/userinformation.cpp`（第 15 行）

```diff
-    pdu->uiType=ENUM_MSG_TYPE_GETUSERINFORMSTION_REQUEST;
+    pdu->uiType=ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST;
```

## 验证

```bash
# Client
cd Client && qmake && make clean && make

# Server
cd Server && qmake && make clean && make
```

**通过标准**：两端均编译成功，零错误零警告。

## 不在本次范围

- Server 端 userinformation 处理函数 → BUG-ARCH-003
- Client 端 handlMsg 响应分发 → BUG-ARCH-003
- MOVEFILE_REQUEST 响应类型错误 → BUG-FILE-002
