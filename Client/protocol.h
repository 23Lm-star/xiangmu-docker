#ifndef PROTOCOL_H
#define PROTOCOL_H
//PDU协议类型定义

#include <QByteArray>
#include <QtGlobal>

typedef unsigned int uint;

static const uint UPLOAD_RESUME_MAGIC = 0x5253554d; // "RSUM"
static const int UPLOAD_FILE_SIZE_OFFSET = 32;
static const int UPLOAD_RESUME_OFFSET = 32;
static const int UPLOAD_RESUME_MAGIC_OFFSET = 40;
static const int UPLOAD_CHUNK_OFFSET = 0;
static const int UPLOAD_ACK_NEXT_OFFSET = 8;
static const int UPLOAD_ACK_COMPLETE_OFFSET = 16;

//消息类型的枚举值
enum ENUM_MSG_TYPE {
    ENUM_MSG_TYPE_MIN=0,
    ENUM_MSG_TYPE_REGIST_REQUEST,//注册请求 1
    ENUM_MSG_TYPE_REGIST_RESPOND,//注册响应 2
    ENUM_MSG_TYPE_LOGIN_REQUEST,//登录请求 3
    ENUM_MSG_TYPE_LOGIN_RESPOND,//登录响应4
    ENUM_MSG_TYPE_findUser_REQUEST,//查找指定用户请求 5
    ENUM_MSG_TYPE_findUser_RESPOND,//查找指定用户响应 6
    ENUM_MSG_TYPE_ONLINEUSER_REQUEST,//查找所有用户请求 7
    ENUM_MSG_TYPE_ONLINEUSER_RESPOND,//查找所有用户响应 8
    ENUM_MSG_TYPE_ADDFRIEND_REQUEST,//添加好友请求 9
    ENUM_MSG_TYPE_ADDFRIEND_RESPOND,//添加好友响应 10
    ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST,//刷新好友请求 11
    ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND,//刷新好友响应 12
    ENUM_MSG_TYPE_DELETEFRIEND_REQUEST,//删除好友请求 13
    ENUM_MSG_TYPE_DELETEFRIEND_RESPOND,//删除好友响应 14
    ENUM_MSG_TYPE_CHAT_REQUEST,//聊天 15
    ENUM_MSG_TYPE_CHAT_RESPOND,//聊天 16
    ENUM_MSG_TYPE_GETCHATMSG_REQUEST,//获取聊天消息 17
    ENUM_MSG_TYPE_GETCHATMSG_RESPOND,//获取聊天消息 18
    ENUM_MSG_TYPE_MKDIR_REQUEST,//创建目录 19
    ENUM_MSG_TYPE_MKDIR_RESPOND,//创建目录 20
    ENUM_MSG_TYPE_CHANGEDIR_REQUEST,//跳转目录 21
    ENUM_MSG_TYPE_CHANGEDIR_RESPOND,//跳转目录 22
    ENUM_MSG_TYPE_FLUSHFILE_REQUEST,//刷新文件 23
    ENUM_MSG_TYPE_FLUSHFILE_RESPOND,//刷新文件 24
    ENUM_MSG_TYPE_DELFILE_REQUEST,//删除文件 25
    ENUM_MSG_TYPE_DELFILE_RESPOND,//删除文件 26
    ENUM_MSG_TYPE_RENAMEFILE_REQUEST,//重命名文件 27
    ENUM_MSG_TYPE_RENAMEFILE_RESPOND,//重命名文件 28
    ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST,//上传文件初始化 29
    ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND,//上传文件初始化 30
    ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST,//上传文件数据 31
    ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND,//上传文件数据 32
    ENUM_MSG_TYPE_SHAREFILE_REQUEST,//分享文件请求 33
    ENUM_MSG_TYPE_SHAREFILE_RESPOND,//分享文件回应 34
    ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST,//分享文件同意请求 35
    ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND,//分享文件同意回应 36
    ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND,//分享文件完成 37
    ENUM_MSG_TYPE_MOVEFILE_REQUEST,//移动文件请求 38
    ENUM_MSG_TYPE_MOVEFILE_RESPOND,//移动文件回应 39
    ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST = 40,     //下载文件请求
    ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND = 41,     //下载文件响应
    ENUM_MSG_TYPE_DOWNLOADFILE_DATA = 42,        //下载文件数据块
    ENUM_MSG_TYPE_DOWNLOADFILE_FINISH = 43,      //下载文件完成
    ENUM_MSG_TYPE_CAPTCHA_REQUEST = 44,          //验证码请求
    ENUM_MSG_TYPE_CAPTCHA_RESPOND = 45,          //验证码响应
    ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST = 46,   // 获取用户信息请求
    ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND = 47,   // 获取用户信息响应
    ENUM_MSG_TYPE_LOGOFF_REQUEST = 48,               // 注销(删除账号)请求
    ENUM_MSG_TYPE_LOGOFF_RESPOND = 49,               // 注销(删除账号)响应
    ENUM_MSG_TYPE_HEARTBEAT_REQUEST = 50,            // 心跳请求
    ENUM_MSG_TYPE_HEARTBEAT_RESPOND = 51,            // 心跳响应
    ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST = 52,       // 创建外链分享请求
    ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND = 53,       // 创建外链分享响应

    // ===== 分享链接扩展（BUG-ARC-PUBLINK-001） =====
    ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST = 54,      // 解析分享链接（通过 token）
    ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND = 55,      // 解析结果
    ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST = 56,   // 按文件路径查询已有链接
    ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND = 57,   // 返回已有链接（或空）

    // ===== 移动文件：目录浏览（选择目标目录） =====
    ENUM_MSG_TYPE_LISTDIR_REQUEST = 58,              // 列出某相对目录下的子目录
    ENUM_MSG_TYPE_LISTDIR_RESPOND = 59,              // 返回子目录列表（FileInfo 数组，uiType=0）

    // ===== REQ-EXT-GROUP-001：群生命周期与成员关系（仅追加，旧编号不变） =====
    ENUM_MSG_TYPE_CREATE_GROUP_REQUEST = 60,
    ENUM_MSG_TYPE_CREATE_GROUP_RESPOND = 61,
    ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_REQUEST = 62,
    ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_RESPOND = 63,
    ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_REQUEST = 64,
    ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_RESPOND = 65,
    ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_REQUEST = 66,
    ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_RESPOND = 67,
    ENUM_MSG_TYPE_LEAVE_GROUP_REQUEST = 68,
    ENUM_MSG_TYPE_LEAVE_GROUP_RESPOND = 69,
    ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_REQUEST = 70,
    ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_RESPOND = 71,
    ENUM_MSG_TYPE_SET_GROUP_ROLE_REQUEST = 72,
    ENUM_MSG_TYPE_SET_GROUP_ROLE_RESPOND = 73,
    ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_REQUEST = 74,
    ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_RESPOND = 75,
    ENUM_MSG_TYPE_DISSOLVE_GROUP_REQUEST = 76,
    ENUM_MSG_TYPE_DISSOLVE_GROUP_RESPOND = 77,
    ENUM_MSG_TYPE_LIST_GROUPS_REQUEST = 78,
    ENUM_MSG_TYPE_LIST_GROUPS_RESPOND = 79,
    ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_REQUEST = 80,
    ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_RESPOND = 81,
    ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_REQUEST = 82,
    ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND = 83,
    ENUM_MSG_TYPE_GROUP_MEMBER_EVENT = 84,
    ENUM_MSG_TYPE_GROUP_INVITATION_EVENT = 85,
    ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_REQUEST = 86,
    ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_RESPOND = 87,

    // ===== REQ-EXT-GROUP-002：可靠群文本消息（仅追加，旧编号 1-87 不变） =====
    ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST = 88,
    ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND = 89,
    ENUM_MSG_TYPE_GROUP_MESSAGE_EVENT = 90,
    ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST = 91,
    ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND = 92,
    ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST = 93,
    ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND = 94,
    ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST = 95,
    ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND = 96,

    // ===== REQ-EXT-FILE-002：顺序上传秒传预检查（仅追加，旧编号 1-96 不变） =====
    ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST = 97,   // 秒传预检查请求
    ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND = 98,   // 秒传预检查响应

    // ===== REQ-EXT-GROUP-003：群文件目录与访问控制（仅追加，旧编号 1-98 不变） =====
    ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST = 99,          // 群文件列表请求
    ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND = 100,         // 群文件列表响应
    ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST = 101,  // 群文件上传初始化（含秒传预检查）
    ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND = 102,  // 命中/续传起点
    ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST = 103,       // 删除群文件引用请求
    ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND = 104,       // 删除群文件引用响应
    ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST = 105,     // 群文件下载请求
    ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND = 106,     // 群文件下载元数据响应（正文走既有 42/43）

    // ===== REQ-EXT-TRANSFER-001：多文件有限并发（仅追加，旧编号 1-106 不变） =====
    ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST = 107,   // 上传任务释放请求（暂停/取消共用）
    ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND = 108,   // 上传任务释放响应

    ENUM_MSG_TYPE_MAX = 99999,
};

// ===== REQ-EXT-FILE-002 秒传预检查：请求/响应布局常量（两端逐字节一致，只追加） =====
static const uint FASTCHECK_MAGIC = 0x4643484b;    // "FCHK"
static const uint FASTCHECK_VERSION = 1;
static const int FASTCHECK_MAX_MSG_LEN = 1024;     // 64(sha) + 210(名) + 450(路径) + 96(reqId) + 余量

// 请求 caData：定长字段与长度字段；可变长度一律进 caMsg
static const int FASTCHECK_VERSION_OFFSET    = 0;   // u32，服务端不等则拒
static const int FASTCHECK_MAGIC_OFFSET      = 4;   // u32
static const int FASTCHECK_FILE_SIZE_OFFSET  = 8;   // qint64
static const int FASTCHECK_SHA_LEN_OFFSET    = 16;  // u32，须为 64
static const int FASTCHECK_NAME_LEN_OFFSET   = 20;  // u32
static const int FASTCHECK_DIR_LEN_OFFSET    = 24;  // u32
static const int FASTCHECK_REQID_LEN_OFFSET  = 28;  // u32
static const int FASTCHECK_SHA_HEX_LEN       = 64;
static const int FASTCHECK_REQID_HEX_LEN     = 32;

// 响应 caData：ret/hit 后接定长字段；最终展示名进 caMsg（hit 时非空）
static const int FASTCHECK_RESP_RET_OFFSET        = 0;   // bool
static const int FASTCHECK_RESP_HIT_OFFSET        = 1;   // bool
static const int FASTCHECK_RESP_VERSION_OFFSET    = 2;   // u32
static const int FASTCHECK_RESP_FILE_ID_OFFSET    = 8;   // qint64
static const int FASTCHECK_RESP_SESSION_ID_OFFSET = 16;  // qint64
static const int FASTCHECK_RESP_NEXT_OFFSET       = 24;  // qint64，miss 时续传起点
static const int FASTCHECK_RESP_NAME_LEN_OFFSET   = 32;  // u32

// ===== REQ-EXT-GROUP-003：群文件 PDU 布局常量（两端逐字节一致，只追加） =====
// 全部载荷以 GROUP-001/002 的 20B 群信封开头（version|envelope_length|request_id|group_id），
// 其后为 code(u32) 与业务字段；偏移相对完整载荷（caMsg 起始）。
static const int GROUP_FILE_LIST_OFFSET_OFFSET   = 20;  // u32 分页起点（0 起）
static const int GROUP_FILE_LIST_LIMIT_OFFSET    = 24;  // u16 1-200
static const int GROUP_FILE_LIST_RESP_COUNT_OFFSET   = 24;  // u32 本页记录数
static const int GROUP_FILE_LIST_RESP_TOTAL_OFFSET   = 28;  // u32 群内 active 总数
static const int GROUP_FILE_LIST_RESP_RECORDS_OFFSET = 32;  // records[] 起始
static const int GROUP_FILE_INIT_SIZE_OFFSET     = 20;  // u64 文件大小
static const int GROUP_FILE_INIT_RESP_HIT_OFFSET       = 24;  // u8 1=秒传命中/0=需分块
static const int GROUP_FILE_INIT_RESP_FILE_ID_OFFSET   = 28;  // qint64 上传者 user_file.file_id
static const int GROUP_FILE_INIT_RESP_GFID_OFFSET      = 36;  // qint64 group_file_id（hit 非 0）
static const int GROUP_FILE_INIT_RESP_SESSION_OFFSET   = 44;  // qint64 upload_session.session_id
static const int GROUP_FILE_INIT_RESP_NEXT_OFFSET      = 52;  // qint64 续传起点（hit=file_size）
static const int GROUP_FILE_INIT_RESP_NAME_LEN_OFFSET  = 60;  // u32 服务端最终展示名长度
static const int GROUP_FILE_INIT_RESP_NAME_OFFSET      = 64;  // 最终展示名 UTF-8
static const int GROUP_FILE_DELETE_ID_OFFSET     = 20;  // u64 group_file_id
static const int GROUP_FILE_DELETE_RESP_ID_OFFSET    = 24;  // u64 回带 group_file_id
static const int GROUP_FILE_DOWNLOAD_ID_OFFSET   = 20;  // u64 group_file_id
static const int GROUP_FILE_DOWNLOAD_RESP_SIZE_OFFSET     = 24;  // u64 文件大小
static const int GROUP_FILE_DOWNLOAD_RESP_NAME_LEN_OFFSET = 32;  // u32 展示名长度
static const int GROUP_FILE_DOWNLOAD_RESP_NAME_OFFSET     = 36;  // 展示名 UTF-8
static const int GROUP_FILE_MAX_NAME_BYTES       = 70;  // group_file.display_name 列宽（UTF-8 字节）
static const int GROUP_FILE_MAX_LIST_LIMIT       = 200; // 单页上限

struct PDU{//协议数据单元
    uint uiTotalLen;    //总长度
    uint uiMsgLen;      //柔性数组的长度(消息长度)
    uint uiType;        //类型
    char caData[64];    //参数，数据长度小于参数长度就放到参数里
    char caMsg[];       //消息
};
//文件信息
struct FileInfo{
    char caName[32];
    uint uiType;
};
//消息
struct Msg{
    uint totalLen;
    uint msgLen;
    char Time[30];
    char msg[];
};

//构建pdu协议对象的函数
//返回pdu对象的地址
PDU* mkPDU(uint uiMsgLen=0);

// 群协议公共信封长度：版本(2)+长度(2)+request_id(8)+group_id(8)。
static const quint16 GROUP_PROTOCOL_VERSION = 1;
static const quint16 GROUP_ENVELOPE_LENGTH = 20;

inline void groupAppendU16(QByteArray &out, quint16 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
}
inline void groupAppendU32(QByteArray &out, quint32 value)
{
    for (int i = 0; i < 4; ++i) out.append(char((value >> (8 * i)) & 0xff));
}
inline void groupAppendU64(QByteArray &out, quint64 value)
{
    for (int i = 0; i < 8; ++i) out.append(char((value >> (8 * i)) & 0xff));
}
inline bool groupReadU16(const QByteArray &in, int &offset, quint16 &value)
{
    if (offset < 0 || offset + 2 > in.size()) return false;
    value = quint16(quint8(in.at(offset))) | quint16(quint8(in.at(offset + 1)) << 8);
    offset += 2;
    return true;
}
inline bool groupReadU32(const QByteArray &in, int &offset, quint32 &value)
{
    if (offset < 0 || offset + 4 > in.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) value |= quint32(quint8(in.at(offset + i))) << (8 * i);
    offset += 4;
    return true;
}
inline bool groupReadU64(const QByteArray &in, int &offset, quint64 &value)
{
    if (offset < 0 || offset + 8 > in.size()) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) value |= quint64(quint8(in.at(offset + i))) << (8 * i);
    offset += 8;
    return true;
}
inline void groupAppendString(QByteArray &out, const QByteArray &value)
{
    groupAppendU32(out, quint32(value.size()));
    out.append(value);
}
inline bool groupReadString(const QByteArray &in, int &offset, QByteArray &value)
{
    quint32 length = 0;
    if (!groupReadU32(in, offset, length) || length > quint32(in.size() - offset)) return false;
    value = in.mid(offset, int(length));
    offset += int(length);
    return true;
}

// ===== REQ-EXT-TRANSFER-001：transfer_id 落位（两端逐字节一致，仅追加；0 = 旧单流路径） =====
static const int TRANSFER_ID_OFFSET = 48;                 // u64；29/30/31/32/40/42/43/101/102/105/106/107/108
static const int DOWNLOAD_RESP_TRANSFER_ID_OFFSET = 16;   // u64；仅 41（其 [32..62] 已被文件名字段占用）
static const quint64 TRANSFER_ID_LEGACY = 0;              // 保留值：旧协议单流路径
static const quint32 MAX_PARALLEL_UPLOADS = 3;            // 单连接并发上传上限（个人+群共享；客户端排队 + 服务端硬兜底）
static const quint32 MAX_PARALLEL_DOWNLOADS = 3;          // 单连接并发下载上限（个人+群共享；客户端排队 + 服务端硬兜底）

// ===== REQ-EXT-TRANSFER-002：下载续传起点（两端逐字节一致，仅追加；0 = 从头） =====
static const int DOWNLOAD_RESUME_OFFSET = 32;             // u64；40/105 请求：客户端声明的续传起点
static const int DOWNLOAD_ACCEPTED_OFFSET = 24;           // u64；41/106 响应：服务端实际采纳的起点

// ===== REQ-EXT-SHARE-001：外链分享生命周期（两端逐字节一致，仅追加） =====
// 53/57 响应 caData[PUBLINK_STATE_OFFSET]：u32 状态字。旧服务端不写此区间、
// mkPDU 全零初始化 → 旧客户端读到 0 = 有效，语义与旧版一致。
static const int PUBLINK_STATE_OFFSET = 8;
static const quint32 PUBLINK_STATE_VALID = 0;
static const quint32 PUBLINK_STATE_EXPIRED = 1;
static const quint32 PUBLINK_STATE_REVOKED = 2;
// 52 请求 caMsg 的附加动作区：<relativePath>\0[<action>\0[<seconds>]]。action 缺省 = 旧语义
// （有记录则复用、无则新建）。旧服务端用 fixedString 读到首个 NUL 即止 → 附加区被忽略，向后兼容。
static const char PUBLINK_ACTION_NEW[] = "new";       // 作废旧行并重新生成
static const char PUBLINK_ACTION_RENEW[] = "renew";   // 续期：延长既有行有效期
static const int PUBLINK_RENEW_DEFAULT_SECONDS = 30 * 24 * 3600;   // 续期缺省 30 天
#endif // PROTOCOL_H
