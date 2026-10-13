// REQ-LOG-007 运行时机制取证 harness（非生产代码，仅证据目录）。
// 复制生产代码的守卫逻辑与过滤规则串，在同一 Qt 5.14.2 下验证：
//  quiet / verbose(env) / 单类(env) / 优先级裁决(FORCE_CODE=1: env 与 setFilterRules 同时存在时谁生效)
#include <QCoreApplication>
#include <QLoggingCategory>
#include <cstdio>

Q_LOGGING_CATEGORY(probeNet, "xmu.net")
Q_LOGGING_CATEGORY(probeDb,  "xmu.db")

static FILE *g_out = nullptr;
static void probeHandler(QtMsgType, const QMessageLogContext &ctx, const QString &msg) {
    if (g_out) { fprintf(g_out, "%s | %s\n", ctx.category ? ctx.category : "?", qPrintable(msg)); fflush(g_out); }
}

int main(int argc, char **argv) {
    QCoreApplication a(argc, argv);
    if (argc > 1) g_out = fopen(argv[1], "w");
    qInstallMessageHandler(probeHandler);

    const bool guard = qEnvironmentVariableIsEmpty("QT_LOGGING_RULES");
    if (guard || qEnvironmentVariableIsSet("FORCE_CODE")) {
        QLoggingCategory::setFilterRules(QStringLiteral("xmu.*.debug=false"));
        fprintf(stderr, "[probe] setFilterRules applied (guard=%d force=%d)\n", guard, qEnvironmentVariableIsSet("FORCE_CODE"));
    }
    qCDebug(probeDb)   << "DB-DETAIL";
    qCDebug(probeNet)  << "NET-DETAIL";
    qCWarning(probeDb) << "DB-WARNING";
    qCCritical(probeNet) << "NET-CRITICAL";
    return 0;
}
