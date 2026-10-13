#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QtGlobal>

namespace {

// When platform-plugin init fails in a process without a console window
// (agent/CI pipes), QGuiApplication pops a native modal MessageBoxW that
// nothing ever dismisses — the test hangs silently. The box is not routed
// through Qt logging, so it cannot be suppressed; instead make init failure
// unreachable by appending known-good platform fallbacks to whatever was
// requested (Qt tries the ";"-separated list in order). Stderr routing keeps
// the remaining Qt diagnostics visible through pipes.
struct QtFatalToStderr {
    QtFatalToStderr()
    {
#ifdef Q_OS_WIN
        qputenv("QT_ASSUME_STDERR_HAS_CONSOLE", QByteArrayView{"1"});
        QByteArray chain = qgetenv("QT_QPA_PLATFORM");
        const QList<QByteArray> requested = chain.split(';');
        for (const QByteArray& fallback :
             {QByteArrayLiteral("windows"), QByteArrayLiteral("offscreen")}) {
            if (requested.contains(fallback)) continue;
            if (!chain.isEmpty()) chain += ';';
            chain += fallback;
        }
        qputenv("QT_QPA_PLATFORM", chain);
#endif
    }
};
const QtFatalToStderr guard;

}
