#ifndef GROUPCHATSTATE_H
#define GROUPCHATSTATE_H

#include <QtGlobal>
#include <set>

// REQ-EXT-GROUP-004：群读游标状态机（纯逻辑：无 UI、无网络、无 QObject）。
//
// 语义：实时投递尽力而为，服务端每连接出站队列满即丢弃该连接的实时事件
// （Server/mytcpsocket.cpp），所以"收到的最大 seq"与"已连续读到的 seq"不是一回事。
// 上报锚点因此恒为「已连续拿到的最长前缀的末尾」，绝不是收到的最大 seq。
//
// 两个数分开：
//   waterSeq   群水位 last_seq："我知道群里最新到哪"——收到任意 seq 即是证据，乐观推进
//   readCursor 连续前缀末尾："我读到哪"（上报锚点）——只在可见且前缀接上时推进，悲观推进
// 把两者混成一个数，就是"报最大收到"导致洞内消息被永久判为已读的成因。
//
// 容器用 std::set 而非 QSet：本头文件要能在项目工具链下被独立编译成断言探针
// （project-docs/evidence/REQ-EXT-GROUP-004/cursor_probe.cpp），只依赖 QtGlobal 的类型别名，
// 不需要链接 Qt5Core，也不需要在运行期部署 Qt DLL。
enum class ReadCursorVerdict {
    Advanced,   // A：正好接上前缀，游标推进（可能顺带消费缓冲区，一次跳多格）
    Buffered,   // B：超前到达（中间有洞），消息照渲染，游标绝不动
    Ignored     // C：重复/迟到（seq <= 游标），丢弃
};

class GroupChatState
{
public:
    quint64 waterSeq() const { return m_waterSeq; }
    quint64 readCursor() const { return m_readCursor; }
    quint64 confirmedSeq() const { return m_confirmedSeq; }
    bool anchored() const { return m_anchored; }        // 是否已从服务端取到过锚点
    int outOfOrderCount() const { return int(m_outOfOrderBuffer.size()); }
    bool outOfOrderContains(quint64 seq) const { return m_outOfOrderBuffer.count(seq) != 0; }

    // 显示口径：与服务器 COUNT(seq in (last_read_seq, last_seq]) 等价（序号稠密，见需求 §4.5）
    quint64 unread() const { return m_waterSeq > m_readCursor ? m_waterSeq - m_readCursor : 0; }

    // 服务端权威锚点（95/96 摘要、92 拉取回带）：水位与游标都只增不减（单调收敛）。
    void applyServerAnchor(quint64 waterSeq, quint64 readCursor);
    // 收到任意 seq 即为水位证据（不推进游标）
    void noteWater(quint64 seq);
    // 唯一推进入口
    ReadCursorVerdict onArrived(quint64 seq);
    // 94 已读确认回认（服务端钳制后的有效值）
    void markConfirmed(quint64 effectiveSeq);
    // 是否需要上报（游标已超过服务端已回认值）
    bool needsReport() const { return m_readCursor > m_confirmedSeq; }
    // 换会话/换账号时整表清空
    void clear();

private:
    quint64 m_waterSeq = 0;
    quint64 m_readCursor = 0;
    quint64 m_confirmedSeq = 0;
    bool m_anchored = false;
    std::set<quint64> m_outOfOrderBuffer;
};

inline void GroupChatState::applyServerAnchor(quint64 waterSeq, quint64 readCursor)
{
    m_anchored = true;
    if (waterSeq > m_waterSeq) m_waterSeq = waterSeq;
    if (readCursor > m_readCursor) {
        m_readCursor = readCursor;
        // 已被服务端锚点覆盖的缓冲项不再需要（复制后遍历，避免边遍历边删）
        const std::set<quint64> stale = m_outOfOrderBuffer;
        for (quint64 v : stale) {
            if (v <= m_readCursor) m_outOfOrderBuffer.erase(v);
        }
    }
    // 服务端回带的 last_read_seq 本身就是"服务端已记下的游标"，即已回认值：
    // 不把它算进 confirmed，窗口一打开就会为同一个值白发一次确认。
    if (m_readCursor > m_confirmedSeq) m_confirmedSeq = m_readCursor;
}

inline void GroupChatState::noteWater(quint64 seq)
{
    if (seq > m_waterSeq) m_waterSeq = seq;
}

inline ReadCursorVerdict GroupChatState::onArrived(quint64 seq)
{
    if (seq == 0) return ReadCursorVerdict::Ignored;
    noteWater(seq);                                          // 任何一段 seq 都是水位证据
    if (seq <= m_readCursor) return ReadCursorVerdict::Ignored;   // C 重复/迟到
    if (seq > m_readCursor + 1) {                            // B 有洞：先存着，游标绝不动
        m_outOfOrderBuffer.insert(seq);
        return ReadCursorVerdict::Buffered;
    }
    m_readCursor = seq;                                      // A 接上：前缀延长
    while (m_outOfOrderBuffer.count(m_readCursor + 1) != 0) { // 顺带消费缓冲区
        ++m_readCursor;
        m_outOfOrderBuffer.erase(m_readCursor);
    }
    return ReadCursorVerdict::Advanced;
}

inline void GroupChatState::markConfirmed(quint64 effectiveSeq)
{
    if (effectiveSeq > m_confirmedSeq) m_confirmedSeq = effectiveSeq;
}

inline void GroupChatState::clear()
{
    m_waterSeq = 0;
    m_readCursor = 0;
    m_confirmedSeq = 0;
    m_anchored = false;
    m_outOfOrderBuffer.clear();
}

#endif // GROUPCHATSTATE_H
