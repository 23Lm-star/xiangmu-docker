# xiangmu Server - Docker 版
# 多阶段构建：stage1 用 Ubuntu 22.04 + Qt 5.15 编译，stage2 只带运行时库
# 用法见 docker/README-Docker.md

# ---------- Stage 1: build ----------
FROM ubuntu:22.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
# SERVER_DB_PASSWORD 会被烧进镜像（connect.config 编译进二进制，只能构建期注入）
ARG SERVER_DB_PASSWORD=xiangmu_docker_pass

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make qt5-qmake qtbase5-dev qtbase5-dev-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Server/ /src/Server/

# Docker 版 connect.config：CRLF 行尾（server.cpp 按 "\r\n" 切分，不能是 LF）
# 行1=监听IP(容器内必须 0.0.0.0) 行2=端口 行3=文件存储路径 行4-8=MySQL 配置(主机=compose 服务名)
RUN printf '0.0.0.0\r\n5000\r\n./filesys\r\nmysql\r\n3306\r\nmydb\r\nroot\r\n%s\r\n' \
        "$SERVER_DB_PASSWORD" > /src/Server/connect.config

RUN cd /src/Server && qmake Server.pro CONFIG+=release && make -j"$(nproc)"

# ---------- Stage 2: runtime ----------
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive

# libqt5sql5-mysql 提供 QMYSQL 驱动插件；QApplication 需要.gui 全家桶（offscreen 平台跑）
RUN apt-get update && apt-get install -y --no-install-recommends \
        libqt5core5a libqt5gui5 libqt5network5 libqt5sql5 \
        libqt5sql5-mysql libqt5widgets5 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /app/filesys

WORKDIR /app
COPY --from=build /src/dist/Server /app/Server
COPY Server/reclaim.cfg /app/reclaim.cfg

# 服务器里 QApplication 无头运行必须 offscreen
ENV QT_QPA_PLATFORM=offscreen

VOLUME ["/app/filesys"]
EXPOSE 5000
CMD ["/app/Server"]
