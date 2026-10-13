// REQ-EXT-GROUP-004：读游标状态机断言探针。
//
// 为什么需要它：本项目的回归套件全是 socket 级 python，驱动不了 Qt 客户端内存里的游标。
// 客户端游标逻辑因此被抽成不依赖 QObject/Qt 库的纯头文件（Client/groupchatstate.h），
// 由本探针**直接编译产品源码**来断言，而不是复制一份等价实现——复制出来的实现通过测试
// 不能证明产品代码正确。
//
// 编译（verify.py 负责调用，工具链与项目一致）：
//   g++ -std=gnu++11 -I <repo> -I <repo>/Client
//       -I C:/Qt/5.14.2/mingw73_64/include -I C:/Qt/5.14.2/mingw73_64/include/QtCore
//       cursor_probe.cpp -o cursor_probe.exe
// 只依赖 QtGlobal 的类型别名，无需链接 Qt5Core，也无需在运行期部署 Qt DLL。
//
// 输出：[PASS]/[FAIL] <id> <描述>，末行 RESULT n/m PASS；全通过时退出码为 0。

#include "Client/groupchatstate.h"

#include <cstdio>
#include <string>

static int g_total = 0;
static int g_passed = 0;

static void check(const char *id, bool ok, const std::string &detail)
{
    ++g_total;
    if (ok) ++g_passed;
    std::printf("[%s] %s :: %s\n", ok ? "PASS" : "FAIL", id, detail.c_str());
}

static std::string u(quint64 v)
{
    return std::to_string(static_cast<unsigned long long>(v));
}

int main()
{
    // C1 洞存在：超前到达只缓冲，游标必须停在 10（上报锚点绝不是"最大收到"）
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        const ReadCursorVerdict v = s.onArrived(90);
        check("C1_hole_buffers_no_advance",
              v == ReadCursorVerdict::Buffered && s.readCursor() == 10 && s.unread() == 90
                  && s.outOfOrderContains(90),
              "cursor=" + u(s.readCursor()) + " unread=" + u(s.unread()) + " buf="
                  + u(quint64(s.outOfOrderCount())) + " (期望 cursor=10 unread=90 buf=1)");
    }

    // C2 洞靠拉取补齐：11..89 按 seq 升序喂回同一入口 → 游标一路推到 90，未读收敛为 10
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        s.onArrived(90);
        ReadCursorVerdict last = ReadCursorVerdict::Ignored;
        for (quint64 seq = 11; seq <= 89; ++seq) last = s.onArrived(seq);
        check("C2_pull_fills_hole_and_advances",
              s.readCursor() == 90 && s.unread() == 10 && s.outOfOrderCount() == 0
                  && last == ReadCursorVerdict::Advanced,
              "cursor=" + u(s.readCursor()) + " unread=" + u(s.unread()) + " buf="
                  + u(quint64(s.outOfOrderCount())) + " (期望 90/10/0)");
    }

    // C3 乱序接上：先缓冲 13，11 到达后 12 到达顺带消费缓冲区 → 一次跳到 13
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        const ReadCursorVerdict a = s.onArrived(13);
        const ReadCursorVerdict b = s.onArrived(11);
        const ReadCursorVerdict c = s.onArrived(12);
        check("C3_out_of_order_drains_on_connect",
              a == ReadCursorVerdict::Buffered && b == ReadCursorVerdict::Advanced
                  && c == ReadCursorVerdict::Advanced && s.readCursor() == 13
                  && s.outOfOrderCount() == 0,
              "cursor=" + u(s.readCursor()) + " buf=" + u(quint64(s.outOfOrderCount()))
                  + " (期望 13/0)");
    }

    // C4 重复/迟到：seq <= 游标一律丢弃，游标与水位的单调性都不受影响
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        s.onArrived(11);
        s.onArrived(12);
        const ReadCursorVerdict dup = s.onArrived(12);
        const ReadCursorVerdict late = s.onArrived(5);
        check("C4_duplicate_and_late_ignored",
              dup == ReadCursorVerdict::Ignored && late == ReadCursorVerdict::Ignored
                  && s.readCursor() == 12 && s.waterSeq() == 100,
              "cursor=" + u(s.readCursor()) + " water=" + u(s.waterSeq()) + " (期望 12/100)");
    }

    // C5 上翻历史（mode=1）整页都在游标之下：即使喂进推进入口也不推进、不上报
    {
        GroupChatState s;
        s.applyServerAnchor(100, 50);
        bool allIgnored = true;
        for (quint64 seq = 50; seq >= 41; --seq) {
            if (s.onArrived(seq) != ReadCursorVerdict::Ignored) allIgnored = false;
        }
        check("C5_backward_page_never_advances",
              allIgnored && s.readCursor() == 50 && s.unread() == 50 && !s.needsReport(),
              "cursor=" + u(s.readCursor()) + " unread=" + u(s.unread()) + " (期望 50/50 且无待上报)");
    }

    // C6 越界跳跃：只缓冲不推进——上报锚点由服务端钳制，游标先于数据推进才是真隐患
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        const ReadCursorVerdict v = s.onArrived(5000);
        check("C6_out_of_range_jump_only_buffers",
              v == ReadCursorVerdict::Buffered && s.readCursor() == 10 && s.outOfOrderContains(5000),
              "cursor=" + u(s.readCursor()) + " (期望 10)");
    }

    // C7 服务端锚点单调收敛：水位/游标都只增不减（迟到的旧摘要不得把状态拽回去）
    {
        GroupChatState s;
        s.applyServerAnchor(100, 50);
        s.applyServerAnchor(200, 30);
        check("C7_anchor_is_monotonic",
              s.waterSeq() == 200 && s.readCursor() == 50,
              "water=" + u(s.waterSeq()) + " cursor=" + u(s.readCursor()) + " (期望 200/50)");
    }

    // C8 锚点覆盖：服务端把游标抬到 15 后，缓冲里 13/15 都不再需要
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        s.onArrived(13);
        s.onArrived(15);
        s.applyServerAnchor(100, 15);
        check("C8_anchor_drops_buffered_below_cursor",
              s.readCursor() == 15 && s.outOfOrderCount() == 0 && s.unread() == 85,
              "cursor=" + u(s.readCursor()) + " buf=" + u(quint64(s.outOfOrderCount()))
                  + " (期望 15/0)");
    }

    // C9 上报判据：取锚点后不需要上报（服务端回带值即已回认）；真实推进才需要；回认只增不减
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        const bool afterAnchor = s.needsReport();
        for (quint64 seq = 11; seq <= 20; ++seq) s.onArrived(seq);
        const bool pending = s.needsReport();
        s.markConfirmed(20);
        const bool afterAck = s.needsReport();
        s.markConfirmed(15);   // 迟到的旧回认不得回退
        check("C9_report_gate_and_ack_monotonic",
              !afterAnchor && pending && !afterAck && s.confirmedSeq() == 20,
              "取锚点后=0 推进后=1 回认后=0 confirmed=" + u(s.confirmedSeq()));
    }

    // C10 换会话清空：clear 后一切归零，且未再取锚点
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        s.onArrived(90);
        s.clear();
        check("C10_clear_resets_session_state",
              s.readCursor() == 0 && s.waterSeq() == 0 && s.confirmedSeq() == 0
                  && !s.anchored() && s.outOfOrderCount() == 0 && s.unread() == 0,
              "cursor/water/confirmed/buf 全零");
    }

    // C11 水位与游标分离：收到的 seq 是水位证据，但没接上前缀就不算已读
    {
        GroupChatState s;
        const ReadCursorVerdict v = s.onArrived(50);
        check("C11_water_moves_cursor_does_not",
              v == ReadCursorVerdict::Buffered && s.waterSeq() == 50 && s.readCursor() == 0
                  && s.unread() == 50,
              "water=" + u(s.waterSeq()) + " cursor=" + u(s.readCursor()) + " (期望 50/0)");
    }

    // C12 推进到水位：未读归零，且这是"全部已读"的唯一合法路径
    {
        GroupChatState s;
        s.applyServerAnchor(100, 98);
        const bool beforeAdvance = s.needsReport();
        s.onArrived(99);
        s.onArrived(100);
        check("C12_reach_water_level",
              !beforeAdvance && s.readCursor() == 100 && s.unread() == 0 && s.needsReport(),
              "cursor=" + u(s.readCursor()) + " unread=" + u(s.unread()) + " (期望 100/0/待上报)");
    }

    // C13 seq=0 是"未落位"占位（乐观条目），不得当作真实序号
    {
        GroupChatState s;
        s.applyServerAnchor(100, 10);
        const ReadCursorVerdict v = s.onArrived(0);
        check("C13_seq_zero_ignored",
              v == ReadCursorVerdict::Ignored && s.readCursor() == 10 && s.waterSeq() == 100,
              "cursor=" + u(s.readCursor()) + " (期望 10)");
    }

    std::printf("RESULT %d/%d PASS\n", g_passed, g_total);
    return g_passed == g_total ? 0 : 1;
}
