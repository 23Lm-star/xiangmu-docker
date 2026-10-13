QT       += core gui network concurrent

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11 utf8_source

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
    chat.cpp \
    file.cpp \
    friend.cpp \
    group.cpp \
    groupchat.cpp \
    groupfile.cpp \
    index.cpp \
    logging.cpp \
    main.cpp \
    client.cpp \
    msghandler.cpp \
    onlineuser.cpp \
    protocol.cpp \
    sharefile.cpp \
    sharelinkdialog.cpp \
    folderselectdialog.cpp \
    reconnectdialog.cpp \
    transfertask.cpp \
    uploader.cpp \
    simplecaptcha.cpp \
    userinformation.cpp

HEADERS += \
    chat.h \
    client.h \
    logging.h \
    file.h \
    friend.h \
    group.h \
    groupchat.h \
    groupfile.h \
    index.h \
    msghandler.h \
    onlineuser.h \
    protocol.h \
    sharefile.h \
    sharelinkdialog.h \
    folderselectdialog.h \
    reconnectdialog.h \
    transfertask.h \
    uploader.h \
    simplecaptcha.h \
    userinformation.h

FORMS += \
    chat.ui \
    client.ui \
    file.ui \
    friend.ui \
    index.ui \
    onlineuser.ui \
    sharefile.ui \
    sharelinkdialog.ui \
    userinformation.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    config.qrc \
    images.qrc

DISTFILES += \
    file.png \
    index.png \
    屏幕截图 2024-10-27 150601.png
