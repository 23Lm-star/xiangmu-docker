QT       += core gui network sql

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11 utf8_source

# console: 必须有。缺省时 MinGW 目标按 GUI 子系统链接（-Wl,-subsystem,windows），进程没有
# stdout/stderr，Qt 的 qDebug/qWarning 全部走 OutputDebugString —— 服务端运行日志既进不了
# 重定向文件也进不了管道，套件（group-002 p7_slow_conn_dropped）无法断言丢包日志。
# 加回控制台子系统后 `dist/Server.exe > log 2>&1` 与 Popen(stdout=FILE) 均可捕获。
CONFIG += console
# qmake 的 console 会跳过 QMAKE_LIBS_QT_ENTRY（win32-g++ 里是 "-lmingw32 -lqtmain"，生成器
# 只在非 console 时追加），于是 MinGW 的 CRT 启动对象找不到 WinMain：
#   libmingw32.a(crt0_c.o): undefined reference to `WinMain'
# 而 main.cpp 只提供 main()。直接补 LIBS（改 QMAKE_LIBS_QT_ENTRY 无效，已被生成器跳过）。
# 子系统决定控制台是否附着（console → 有 stdout/stderr），入口符号由 qtmain 提供并转调 main()。
win32 {
LIBS += -lmingw32 -L$$[QT_INSTALL_LIBS] -lqtmain
}

# Docker/Linux adaptation:
# utf8_source 在 unix 上会加 -finput-charset=UTF-8，对源码做硬性 UTF-8 校验；
# 本仓库源码为 GBK/UTF-8 混合编码（老文件 GBK 注释），强制校验会直接编译失败。
# Linux 下 g++ 默认按 UTF-8 解析且对注释里的非法序列宽松，去掉该 CONFIG 即可。
unix {
    CONFIG -= utf8_source
}

# Deploy destination policy:
#   Debug   -> stay beside Qt Creator's run config (green triangle works as-is)
#   Release -> converge into repo-root dist/ (the single deployable artifact)
CONFIG(debug, debug|release) {
    DESTDIR =
} else {
    DESTDIR = $$PWD/../dist
}

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    databaseconnectionpool.cpp \
    fileobjectstore.cpp \
    logging.cpp \
    main.cpp \
    msghandler.cpp \
    mytcpserver.cpp \
    mytcpsocket.cpp \
    operatedb.cpp \
    protocol.cpp \
    reclaimscheduler.cpp \
    resumableuploadsession.cpp \
    server.cpp \
    sharelinkservice.cpp \
    uploadwritetask.cpp \
    ThreadCache.cpp \
    CentralCache.cpp \
    PageCache.cpp \
    ConCurrentAlloc.cpp \
    memalloc.cpp

HEADERS += \
    databaseconnectionpool.h \
    fileobjectstore.h \
    logging.h \
    msghandler.h \
    mytcpserver.h \
    mytcpsocket.h \
    operatedb.h \
    protocol.h \
    reclaimscheduler.h \
    resumableuploadsession.h \
    server.h \
    sharelinkservice.h \
    uploadwritetask.h \
    Common.h \
    ThreadCache.h \
    CentralCache.h \
    PageCache.h \
    ObjectPool.h \
    RadixTreeHash.h \
    ConCurrentAlloc.h \
    memalloc.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    config.qrc

DISTFILES +=
