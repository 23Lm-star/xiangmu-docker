#ifndef FILEOBJECTSTORE_H
#define FILEOBJECTSTORE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>

// REQ-EXT-FILE-001 T2：内容寻址对象存储层。
// 物理对象唯一（sha256），逻辑条目（user_file）引用对象；
// 对象命中+条目创建+引用计数在同一个 DB 事务内完成；
// 引用归零仅标 pending_reclaim——本分支绝不物理删除对象文件（D3）。
class FileObjectStore : public QObject
{
    Q_OBJECT
public:
    struct CommitResult {
        bool ok = false;
        qint64 fileId = -1;      // 最终落定的 user_file.file_id
        qint64 objectId = -1;
        QString displayName;     // 服务端定名（含 (1) 后缀规则）
        bool dedupHit = false;   // 命中已有对象（秒传地基，本分支不开 PDU）
        QString error;
    };

    struct EntryInfo {
        qint64 fileId = -1;
        QString displayName;
        bool isDir = false;
        qint64 objectId = -1;    // 目录为 -1
        qint64 sizeBytes = 0;
        bool valid() const { return fileId >= 0; }
    };

    // REQ-EXT-FILE-002：秒传命中安置结果（复用 commitUpload 的同名安置语义）
    struct AttachResult {
        bool ok = false;
        qint64 fileId = -1;
        QString displayName;     // 服务端最终展示名（含 (1) 后缀规则）
        bool idempotent = false; // 同名同对象已存在，零写入返回既有条目
        QString error;
    };

    // REQ-EXT-GROUP-003：群文件引用安置结果（与对象/个人条目/群条目/引用计数同事务）
    struct GroupAttachResult {
        bool ok = false;
        qint64 groupFileId = -1;     // group_file.group_file_id
        qint64 fileId = -1;          // 上传者 user_file.file_id（个人根逻辑条目）
        QString groupDisplayName;    // 群目录最终展示名（含 (N) 去重）
        QString error;
    };

    static FileObjectStore& instance();

    // 服务端复算 staging 的 SHA-256 与大小（流式，1MiB 缓冲）
    bool hashStaging(const QString& stagingPath, QString& shaHex, qint64& sizeBytes) const;

    // REQ-EXT-FILE-002 §2 步骤4：摘要+大小去重命中判定；命中返回 object_id，否则 -1。
    qint64 findObjectBySha(const QString& shaHex, qint64 sizeBytes) const;

    // 事务内：find-or-create 对象 + 建/复活条目 + 引用计数。
    // expectedSize 为会话登记大小；复算不一致 = 失败（不转正，验收4）。
    // expectedSha 非空时另比对复算摘要（REQ-EXT-FILE-002 D2 复算对照，验收3）；
    // 为空 = 旧路径语义，与 FILE-001 逐字节等价。
    // 新对象：staging 原子 rename 到 objects/{sha前2}/{object_id}；命中已有对象：丢弃 staging。
    // copyOnly=true（T6 存量迁移）：源文件=旧路径实体——只复制不搬移，绝不删除源（验收5）。
    CommitResult commitUpload(const QString& owner, const QString& parentPath,
                              const QString& displayName, qint64 expectedSize,
                              const QString& stagingPath, bool copyOnly = false,
                              const QString& expectedSha = QString());

    // REQ-EXT-FILE-002 §2.1：指向既有对象的条目安置（复用 commitUpload 步骤 2/3/4，同事务）。
    // 不读客户端路径、不复制物理文件、不传输正文；pending_reclaim 对象复用即置回 ready。
    // expectedSize < 0 = 不校验对象大小。
    AttachResult attachExistingObject(const QString& owner, const QString& parentPath,
                                      const QString& wantedName, qint64 objectId,
                                      qint64 expectedSize = -1);

    // 删除条目（软删）并释放引用；对象引用归零 → pending_reclaim。
    bool releaseEntry(qint64 fileId, const QString& owner);

    // REQ-EXT-GROUP-003：群文件引用安置（对象存在性 + 个人根条目 + 群目录条目 + 引用计数，
    // 同一事务）。createUserEntry=true：在 owner 个人根建/复活条目（秒传命中路径）；
    // false：复用 existingFileId（分块上传完成路径，条目已由 commitUpload 建好）。
    // 群内同名同对象 active → 幂等返回既有 group_file_id，不重复计数；同名不同对象走 (N) 递推。
    GroupAttachResult attachGroupFile(const QString& owner, quint64 groupId,
                                      const QString& wantedName, qint64 objectId,
                                      qint64 sizeBytes, bool createUserEntry,
                                      qint64 existingFileId = -1);

    // REQ-EXT-GROUP-003：软删群文件引用并减引用计数（归零 → pending_reclaim，绝不物理删）。
    // refcount 语义已含 user_file(active) ∪ group_file(active)，故 refcount<=0 即两表均无活跃引用。
    // actorIsOwner=true 允许删除他人上传；否则仅允许 uploader_name==actor（普通成员不能删他人文件）。
    bool releaseGroupFile(qint64 groupFileId, quint64 groupId,
                          const QString& actor, bool actorIsOwner, QString& err);

    // 新建引用（分享=同对象新条目，+1 引用，物理零复制）。
    bool refObject(qint64 objectId);

    // 条目 → 对象绝对路径（下载/迁移用）；对象不存在或已标回收失败返回空。
    QString objectAbsPath(qint64 objectId) const;

    // 列出 parent 下 active 条目（flushfile/listdir 渲染）；无记录返回空表。
    QList<EntryInfo> listEntries(const QString& owner, const QString& parentPath) const;

    // 查询单条目（含目录）；不存在返回 invalid。
    EntryInfo entryByPath(const QString& owner, const QString& parentPath,
                          const QString& displayName) const;

    // 目录软删：递归释放 parent 及子树全部文件条目（同事务）。
    bool releaseSubtree(qint64 dirFileId, const QString& owner);

    // 条目改名/移动：只动逻辑键，引用不变；目录行同事务级联子树 parent_path。
    bool relocateEntry(qint64 fileId, const QString& owner,
                       const QString& newParentPath, const QString& newDisplayName,
                       QString& err);

    // REQ-EXT-FILE-001 D5/T3：目录条目（is_dir=1，object_id NULL）。
    // 同名 active 目录条目=幂等成功；同名 active 文件=失败。
    bool insertDirEntry(const QString& owner, const QString& parentPath,
                        const QString& displayName, QString& err);

    // REQ-EXT-FILE-001 T5：给 owner 新建指向既有对象的条目（分享=物理零复制，+1 引用）。
    // 同名同对象幂等返回既有行不重复计数；同名不同对象走 (1) 递推。返回 fileId，失败 -1。
    qint64 attachEntry(const QString& owner, const QString& parentPath,
                       const QString& wantedName, qint64 objectId, QString& err);

    // 旧目录物理改名/移动后的条目级联（parent_path 前缀平移；无条目则空操作）。
    bool renameEntriesDir(const QString& owner, const QString& oldDirPath,
                          const QString& newDirPath, QString& err);

    // 旧目录物理删除后的子树条目软删+减引用（无条目则空操作，成功返回）。
    bool releaseSubtreeByPath(const QString& owner, const QString& dirPath, QString& err);

    // REQ-EXT-FILE-001 F-3/D5 修复：owner 相对规范逻辑键。
    // 接受 PDU 相对路径、本树绝对路径、既有逻辑键（幂等）三形态，归一为
    // "用户根内、'/' 结尾"的机器无关键（design §2/§6），保证同一逻辑请求
    // 在任何工作树得到同一 user_file.parent_path 字面值。
    // 无法映射（路径不含 <filesys根名>/<owner> 段）→ 返回空 = 无效输入。
    // 物理 IO 一律使用原始路径，不经过本函数。
    static QString normalizeParent(const QString& owner, const QString& dirPath);

private:
    explicit FileObjectStore(QObject* parent = nullptr);
    FileObjectStore(const FileObjectStore&) = delete;
    FileObjectStore& operator=(const FileObjectStore&) = delete;
};

#endif // FILEOBJECTSTORE_H
