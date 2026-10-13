# fix-client-filepath-after-reconnect — 客户端先启动/重连后登录，文件页停在存储根目录

## 档位
**SMALL**。理由：客户端单文件路径初始化顺序缺陷；不触协议、不触数据、不触安全/系统边界；
改动可逐字回滚（一个新增方法 + 一处调用 + 构造函数一行替换）；爆炸半径限于文件页的
用户根/当前路径两个成员。

## 来源
2026-10-12 全功能审计「需人工验证清单」A-4，主人复验描述：
> 客户端先启动、服务端后启动 → 心跳超时弹出重连窗口 → 重连后登录成功 →
> **文件界面进入的是 filesys 这个文件存储的根目录**，导致文件操作目录不对。

## 根因（代码锚点）
1. `Client/file.cpp` 构造函数原第 71-72 行：
   `m_strUserPath = <root>/<m_LoginName>`；`m_strCurPath = m_strUserPath`。
   即**用户根在构造时一次性定格**。
2. `Client/client.cpp:400` `forceReLogin()` 为了隐藏主界面调用 `Index::getinstance().hide()`，
   这会**在登录之前**构造 Index → File。此刻 `Client::m_LoginName` 为空（本次会话从未登录过），
   于是 `m_strUserPath` 退化为 `"<root>/"`，`m_strCurPath` 同样是 `<root>/`。
3. 正常流程之所以看不出问题：服务端在线时，Index/File 首次被构造发生在登录成功后的
   1500 ms 定时器里（`Client/msghandler.cpp:90`），那时 `m_LoginName` 已有值。
4. 副产物：空名构造还会执行 `dir.mkdir("<root>/")` 与 `dir.mkdir("<root>//<yyyy-MM>")`，
   在**存储根**下建出一个 `2026-10` 之类的月份目录（污染服务端数据根）。

## 修复
- `Client/file.h/.cpp`：新增 `File::resetUserPath()`，按**当时的**登录身份重算
  `m_strUserPath`/`m_strCurPath`、刷新路径显示、创建用户目录与当月目录；
  **登录名为空时直接返回**（不创建任何目录）。
- 构造函数改为调用 `resetUserPath()`（唯一实现，去掉重复代码）。
- `Client/msghandler.cpp` 登录成功分支（1500 ms 定时器内、`Index::show()` 之前）
  调用 `Index::getinstance().getFile()->resetUserPath()`——每次登录都重算，
  同时覆盖"重连后登录"与"注销后换账号登录"两种路径。

## 验收
1. 静态：登录成功路径存在 `resetUserPath()` 调用；构造函数不再直接拼 `m_LoginName`。
2. 静态：`resetUserPath()` 在登录名为空时提前返回（不建目录）。
3. 回归：全量 14 套件与基线一致（本改动不触及任何 socket 面）。
4. **人工（GUI）**：客户端先启动 → 服务端后启动 → 等心跳超时 → 重连 → 登录 →
   文件页路径栏应为 `<root>/<用户名>`，且上传/新建/删除落在该目录下；
   存储根下不再出现月份目录。

## 证据
`build/evidence/current/{build,test,audit}-<hash>-<run-id>.*` + 本文档 §验收 的静态检查输出。
