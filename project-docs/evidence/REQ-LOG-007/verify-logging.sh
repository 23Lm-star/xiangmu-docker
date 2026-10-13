#!/bin/bash
# REQ-LOG-007 人工验证：Server.exe 为 -subsystem,windows，Qt 日志经 OutputDebugString 输出
# （无控制台时 Qt 默认 handler 不写重定向 stderr），故用 ods_listener.py 抓 DBWIN_BUFFER 流。
set -u
EV="$(cd "$(dirname "$0")" && pwd)"
WIN_EV="$EV"
EXE="/e/qt/xiangmu/build/build-Server/release/Server.exe"
export PATH="/c/Qt/5.14.2/mingw73_64/bin:/c/Qt/Tools/mingw730_64/bin:$PATH"
cd /e/qt/xiangmu/Server
taskkill //F //IM Server.exe >/dev/null 2>&1
rm -f "$EV"/*.ods.log "$EV"/*.stderr.log

run_case() {
  local name="$1" rules="$2"
  py -3.13 "$WIN_EV/ods_listener.py" "$EV/$name.ods.log" 14 > "$EV/$name.listener.err" 2>&1 &
  local lp=$!
  sleep 1
  if [ "$rules" = "__none__" ]; then unset QT_LOGGING_RULES
  else export QT_LOGGING_RULES="$rules"; fi
  "$EXE" > "$EV/$name.stderr.log" 2>&1 &
  local sp=$!
  sleep 4
  py -3.13 -c "import socket;socket.create_connection(('127.0.0.1',5000),timeout=3).close()" \
    && echo "connect-ok" || echo "connect-FAIL"
  sleep 3
  taskkill //F //IM Server.exe >/dev/null 2>&1
  wait $lp 2>/dev/null
  sleep 1
}

run_case quiet  "__none__"
run_case verbose "xmu.*.debug=true"
run_case dbonly "xmu.db.debug=true"

# critical 可见性：占端口起第一实例，第二实例 listen 失败应输出 qCCritical（默认级别也可见）
py -3.13 "$WIN_EV/ods_listener.py" "$EV/critical.ods.log" 14 > "$EV/critical.listener.err" 2>&1 &
lp=$!
unset QT_LOGGING_RULES
"$EXE" > /dev/null 2>&1 &
sleep 3
"$EXE" > "$EV/critical.stderr.log" 2>&1 &
sleep 6
taskkill //F //IM Server.exe >/dev/null 2>&1
wait $lp 2>/dev/null

for n in quiet verbose dbonly critical; do
  echo "##### $n: ods_lines=$(wc -l < "$EV/$n.ods.log") stderr_lines=$(wc -l < "$EV/$n.stderr.log")"
done
echo DONE
