# 变更摘要

### 修改文件清单
| 文件 | 变更类型 | 变更说明 |
|------|---------|---------|
| Server/msghandler.cpp | 修改 | 将 `logoff()` 中的 `char[32]` 裸指针及 `memcpy` 替换为 `fixedString()`，防止密码 MD5 为 32 字符时缺少 null 终止符导致越界读取。 |

### 未修改但受影响的文件
| 文件 | 影响说明 |
|------|---------|
| Server/operatedb.cpp | 本次修复确保传入 `handleLogoff` 的密码参数是合法的以 null 结尾的 C 字符串。 |

### 变更前后对比（关键逻辑）
# 修改前：
char userName[32]={'\0'};
char userPwd[32]={'\0'};
memcpy(userName,pdu->caData,32);
memcpy(userPwd,pdu->caData+32,32);

qDebug()<<"logoff request for:"<<QString::fromUtf8(userName);

bool ret=OperateDB::instance().handleLogoff(userName,userPwd);

# 修改后：
QString userName = fixedString(pdu->caData, 32);
QString userPwd  = fixedString(pdu->caData + 32, 32);

qDebug() << "logoff request for:" << userName;

bool ret = OperateDB::instance().handleLogoff(
    userName.toUtf8().constData(),
    userPwd.toUtf8().constData()
);

### 自检结果
- [ ] 单元测试通过 (本地逻辑转换，符合同文件 login 惯用法)
- [ ] 边界测试通过 (验证码/密码处理一致)
- [ ] 接口签名未变化 (返回类型 PDU* 保持不变)
- [ ] 已知限制：无
