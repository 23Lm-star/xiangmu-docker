# REQ-EXT-RECLAIM-001 周期清理调度器 端到端脚本（证据归档，仿 FILE-002 fastupload_e2e.py 模式）。
# 前置：DB 已执行 migrate_ext_reclaim_001.sql（版本 2026100701）。
# 断言走两处证据面：DB（file_object/user_file/upload_session 行状态）+ 文件系统（dist/filesys）。
# 本套件不驱动客户端协议——调度器不由客户端触发，它挂在服务端主线程 QTimer 上。
#
# ---- 统一 e2e 运行形态（见 project-docs/evidence/E2E-RUNFORM.md）----
# 运行：py -3.13 project-docs/evidence/REQ-EXT-RECLAIM-001/reclaim_e2e.py
#   cwd     : 任意（脚本自管理，Server 以 <repo>/dist 为 cwd 启动）
#   exe     : <repo>/dist/Server.exe（qmake DESTDIR=dist；connect.config 构建期经 qrc 内嵌）
#   config  : Server/connect.config -> filesPath=./filesys（相对 cwd）；未入库的机器本地文件
#  数据根  : <repo>/dist/filesys
# 脚本自管理 Server 生命周期（server_ctl 按 PID 结束自己启动的进程，绝不 taskkill /IM）。
#
# 本套件会自己写好 <repo>/dist/reclaim.cfg（调度器读它）；收尾时还原现场。
# 覆盖两组契约：
#   phase 1  dry_run=1 —— 只报候选、不做任何写（库与盘都必须分毫未变）
#   phase 2  dry_run=0 —— 真实回收：对象线 5 例 + 暂存线 4 例

import os
import subprocess
import sys
import time

MYSQL = os.environ.get("RECLAIM_MYSQL", r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe")
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl

FS = os.path.join(DIST, "filesys")
STG = os.path.join(FS, ".staging")
OBJ = os.path.join(FS, "objects", "zz")          # zz 不是真实 sha 前缀，只为本套件自造
CFG = os.path.join(DIST, "reclaim.cfg")

OWNER = "re2euser"
OIDS = [999901, 999902, 999903, 999904, 999905]
# (id, status, reclaim_at 偏移小时, 是否有实体文件, 是否有 active user_file 引用)
# 宽限期取 grace_hours=6（见 write_cfg）：40 小时已过宽限，1 小时未过（C 例的判据）。
# E 必须用 1 小时：崩溃残留的处理顺序是"先退回 pending_reclaim、同一轮再取候选"，
# 若它已过宽限，退回后当轮就会被正常收走（这是对的），E1/E2 的"保留"就不成立了。
CASES = [
    (999901, "pending_reclaim", 40, True, False),   # A 到期+无引用 → 行与文件都删
    (999902, "pending_reclaim", 40, True, True),    # B 到期+有活跃引用 → 一条都不许动
    (999903, "pending_reclaim", 1, True, False),    # C 未到宽限期 → 不许动
    (999904, "reclaiming", 40, False, False),       # D 崩溃残留(文件已无) → 补完删记录
    (999905, "reclaiming", 1, True, False),         # E 崩溃残留(文件仍在) → 退回待回收
]
STG_NEW, STG_OLD, STG_ACTIVE = "re2enew.part", "re2eold.part", "re2eactive.part"

PASS = FAIL = 0


def sql(q, db="mydb"):
    r = subprocess.run([MYSQL, "-h", "localhost", "-u", "root", "-p123456", db, "-N", "-B", "-e", q],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError("SQL failed: %s\n%s" % (q, r.stderr.strip()))
    return [ln.split("\t") for ln in r.stdout.splitlines()]


def scalar(q, default=None):
    rows = sql(q)
    return rows[0][0] if rows and rows[0] else default


def check(cid, desc, got, want):
    global PASS, FAIL
    if str(got) == str(want):
        print("  PASS  %s %s = %s" % (cid, desc, got))
        PASS += 1
    else:
        print("  FAIL  %s %s = %s (期望 %s)" % (cid, desc, got, want))
        FAIL += 1


def write_cfg(dry_run):
    with open(CFG, "w", encoding="utf-8", newline="\n") as f:
        f.write("# REQ-EXT-RECLAIM-001 由 reclaim_e2e.py 临时写入；收尾会还原现场\n")
        f.write("enabled=1\ndry_run=%d\n" % (1 if dry_run else 0))
        f.write("tick_ms=1000\ninterval_minutes=60\nper_tick=20\nper_round_limit=500\n")
        f.write("staging_ttl_hours=6\ngrace_hours=6\n")


def cleanup():
    """先摘引用再删对象：file_object 上有 fk_uf_obj/fk_gf_obj，顺序反了就是 1451。"""
    sql("DELETE FROM user_file WHERE owner_name='%s'" % OWNER)
    sql("DELETE FROM upload_session WHERE owner_name='%s'" % OWNER)
    sql("DELETE FROM file_object WHERE object_id BETWEEN 999901 AND 999906")
    for name in (STG_NEW, STG_OLD, STG_ACTIVE):
        try:
            os.remove(os.path.join(STG, name))
        except OSError:
            pass
    for oid in OIDS + [999906]:
        try:
            os.remove(os.path.join(OBJ, str(oid)))
        except OSError:
            pass


def build_fixture():
    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(STG, exist_ok=True)
    cleanup()

    rows = ",".join(
        "(%d,REPEAT('%s',64),5,'objects/zz/%d',0,'%s',NOW(),DATE_SUB(NOW(),INTERVAL %d HOUR))"
        % (oid, "abcdefgh"[i], oid, status, hours)
        for i, (oid, status, hours, _f, _r) in enumerate(CASES))
    sql("INSERT INTO file_object (object_id,sha256,size_bytes,storage_path,refcount,status,"
        "created_at,reclaim_at) VALUES " + rows)
    sql("INSERT INTO user_file (owner_name,parent_path,display_name,object_id,status) "
        "VALUES ('%s','/','re2ehold.bin',999902,'active')" % OWNER)
    # staging_path 必须是 QDir::absoluteFilePath 形态（Windows 反斜杠/正斜杠皆可，
    # 但不能是 MSYS 的 /e/... 形态，否则 normKey 比对静默失配、在用文件会被误删）
    win_stg = os.path.join(STG, STG_ACTIVE).replace("\\", "/")
    sql("INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,"
        "staging_path,status,expires_at) VALUES "
        "('%s','/','re2eactive.bin',5,'%s','uploading',DATE_ADD(NOW(),INTERVAL 1 HOUR))"
        % (OWNER, win_stg))

    for oid, _s, _h, has_file, _r in CASES:
        p = os.path.join(OBJ, str(oid))
        if has_file:
            with open(p, "wb") as f:
                f.write(b"PAYLOAD-%d" % oid)
        elif os.path.exists(p):
            os.remove(p)

    # TTL 是 staging_ttl_hours=6：10 小时=过期两例，1 小时=未过期一例。
    now = time.time()
    for name, age_hours in ((STG_OLD, 10), (STG_ACTIVE, 10), (STG_NEW, 1)):
        p = os.path.join(STG, name)
        with open(p, "wb") as f:
            f.write(b"X")
        os.utime(p, (now - age_hours * 3600, now - age_hours * 3600))

    got = int(scalar("SELECT COUNT(*) FROM file_object WHERE object_id BETWEEN 999901 AND 999906"))
    if got != 5:
        raise RuntimeError("夹具建失败：期望 5 行，实到 %d" % got)


def prior_parts():
    """夹具之外既有的 .part 数量。断言 I 比较同一次运行的前后，证明"本轮只动了该动的"；
    它测不到"本轮开始前就已被别的运行收走"——那不是它的职责（见 INDEX.md）。"""
    return len([n for n in os.listdir(STG)
                if n.endswith(".part") and not n.startswith("re2e")]) if os.path.isdir(STG) else 0


def run_server_once(dry_run, wait_s=4.0):
    write_cfg(dry_run)
    ctl = server_ctl.ServerCtl(EXE, DIST, "127.0.0.1", 5000)
    if not ctl.start():
        raise SystemExit("FATAL server did not start: %s" % ctl.last_error)
    try:
        # 心跳 1000ms：第一次心跳就跑满一轮（m_lastRoundStartMs 初值 0）。留足余量。
        time.sleep(wait_s)
    finally:
        ctl.stop()


def exists(p):
    return os.path.exists(p)


def main():
    global PASS, FAIL
    if not os.path.isfile(EXE):
        raise SystemExit("FATAL 未找到 %s（先跑 pipeline/build.ps1）" % EXE)

    cfg_backup = None
    if os.path.isfile(CFG):
        with open(CFG, "rb") as f:
            cfg_backup = f.read()

    try:
        # ---------------- phase 1: dry_run=1 ----------------
        print("=== phase 1  dry_run=1（只报候选，不做任何写）===")
        build_fixture()
        run_server_once(True)

        check("P1a", "夹具 5 行完好", scalar("SELECT COUNT(*) FROM file_object WHERE object_id BETWEEN 999901 AND 999906"), 5)
        check("P1b", "999904 仍 reclaiming（崩溃残留未被写）",
              scalar("SELECT status FROM file_object WHERE object_id=999904"), "reclaiming")
        check("P1c", "999901 实体文件仍在", "yes" if exists(os.path.join(OBJ, "999901")) else "no", "yes")
        check("P1d", "过期暂存 re2eold.part 仍在", "yes" if exists(os.path.join(STG, STG_OLD)) else "no", "yes")

        # ---------------- phase 2: dry_run=0 ----------------
        print("=== phase 2  dry_run=0（真实回收）===")
        build_fixture()
        base = prior_parts()
        run_server_once(False)

        print("-- 对象线")
        check("A1", "999901 行已删（到期+无引用）",
              scalar("SELECT COUNT(*) FROM file_object WHERE object_id=999901"), 0)
        check("A2", "999901 实体文件已删",
              "yes" if exists(os.path.join(OBJ, "999901")) else "no", "no")
        check("B1", "999902 行保留（有 active 引用）",
              scalar("SELECT status FROM file_object WHERE object_id=999902"), "pending_reclaim")
        check("B2", "999902 实体文件保留",
              "yes" if exists(os.path.join(OBJ, "999902")) else "no", "yes")
        check("B3", "999902 的 user_file 引用未被摘",
              scalar("SELECT object_id FROM user_file WHERE owner_name='%s'" % OWNER), 999902)
        check("C1", "999903 行保留（未到宽限期）",
              scalar("SELECT status FROM file_object WHERE object_id=999903"), "pending_reclaim")
        check("C2", "999903 实体文件保留",
              "yes" if exists(os.path.join(OBJ, "999903")) else "no", "yes")
        check("D1", "999904 行已删（崩溃残留·文件已无→补删记录）",
              scalar("SELECT COUNT(*) FROM file_object WHERE object_id=999904"), 0)
        check("E1", "999905 退回 pending_reclaim（崩溃残留·文件仍在）",
              scalar("SELECT status FROM file_object WHERE object_id=999905"), "pending_reclaim")
        check("E2", "999905 实体文件保留",
              "yes" if exists(os.path.join(OBJ, "999905")) else "no", "yes")

        print("-- 暂存线")
        check("F", "过期且无会话引用 → 删除",
              "yes" if exists(os.path.join(STG, STG_OLD)) else "no", "no")
        check("G", "未过期 → 保留",
              "yes" if exists(os.path.join(STG, STG_NEW)) else "no", "yes")
        check("H", "过期但 uploading 会话在用 → 保留",
              "yes" if exists(os.path.join(STG, STG_ACTIVE)) else "no", "yes")
        check("I", "夹具之外的既有 .part 同一次运行前后不变", prior_parts(), base)
    finally:
        try:
            cleanup()
        finally:
            if cfg_backup is None:
                try:
                    os.remove(CFG)
                except OSError:
                    pass
            else:
                with open(CFG, "wb") as f:
                    f.write(cfg_backup)

    print("-------------------------------------------")
    print("PASS=%d FAIL=%d" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
