#include "Cli/HubCli.h"
#include "Localization/HubLocalization.h"
#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace VNH::Cli {

QString writeGuides(QString* error)
{
    const QString root = userDataDir() + QStringLiteral("/Documentation/CLI");
    if (!QDir().mkpath(root)) { *error = QStringLiteral("Could not create the guide directory: ") + root; return {}; }
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QSaveFile file(root + '/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            *error = file.errorString();
            return false;
        }
        return true;
    };
    const auto reference = commandReference();
    const bool chinese = HubLocalization::effectiveLanguage() == HubLocalization::simplifiedChinesePreference();
    QString html = QStringLiteral("<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Harken Hub CLI — AI reference</title><style>html{color-scheme:light dark}body{font:16px/1.65 system-ui,sans-serif;max-width:1000px;margin:40px auto;padding:0 24px}"
        "a{color:light-dark(#006b82,#5edce9)}pre{white-space:pre-wrap;overflow-wrap:anywhere;padding:18px;background:light-dark(#eef2f5,#1d2430);border-radius:8px}"
        "section{border-top:1px solid #8886;margin-top:30px}code{font-family:ui-monospace,Consolas,monospace}</style>"
        "<nav><a href='index.html'>User guide</a> · <a href='command-reference.json'>Machine-readable JSON</a></nav>"
        "<h1>Harken Hub CLI: AI command reference</h1><p>This offline reference is generated from the same command definitions as the parser. "
        "Run <code>HarkenHub --cli help</code> to discover the contract of the executable you are using.</p>"
        "<p>Pass each argument separately when launching a process. Quote paths in a shell. Do not interpolate user input into shell command text. "
        "All options take one string value. Boolean values are the literal strings <code>true</code> and <code>false</code>. "
        "Unknown commands, unknown/repeated options, missing values and invalid enum values are rejected before execution.</p>"
        "<p>Read the process exit code and the <code>ok</code> field. Exit 0 means the requested command succeeded; "
        "project creation/opening starts Studio, and a Hub update can be queued for the update bridge. Those results are not proof that the separate process has finished. "
        "Use the returned transaction id and <code>updates status</code> / <code>diagnostics transactions</code> to check completion.</p>"
        "<p>Exit 2: invalid arguments. Exit 3: operation failed. Exit 4: review or a content decision is required. Exit 5: consent is required. "
        "Errors may include structured <code>error</code> or detailed operation steps under <code>data</code>. Preserve the full response when reporting failures. "
        "Integration step status values are 0 pending, 1 running, 2 succeeded, 3 warning, 4 failed and 5 skipped.</p>"
        "<h2>Approval workflow</h2><p>For a command marked <code>reviewBeforeApply</code>, run without <code>--confirm</code> first. "
        "Present the returned plan to the user; submit its opaque token with the identical options only after approval. "
        "Do not automatically accept a replacement token: file or option changes invalidate prior approval. "
        "For <code>contentSelectionRequired</code>, obtain keep/update/remove choices for each existing component and request a new plan. "
        "Document acceptance is a separate user decision: show the complete current documents before submitting a legal acceptance token.</p>"
        "<p>Core integration is always installed. Optional documentation and samples default to <code>ask</code>: absent content is omitted; "
        "existing content requires a choice. <code>keep</code> retains existing content, <code>update</code> copies the selected source, "
        "and <code>remove</code> removes only managed content. User-owned files are preserved; overwritten/removed optional content is backed up.</p>"
        "<p><code>--backup</code> must name a new backup directory. Repeat the same path with the confirmation token. "
        "Restoration verifies the original official release; <code>--accept-conflicts true</code> separately permits backing up and replacing conflicting unowned paths. "
        "Mutating integration commands require the game editor to be closed. Machine-wide installation/repair/uninstall can display the operating system's administrator prompt.</p>");
    if (chinese) {
        QFile introduction(QStringLiteral(":/Resources/cli-ai-reference.zh_CN.html"));
        if (!introduction.open(QIODevice::ReadOnly)) { *error = introduction.errorString(); return {}; }
        html = QString::fromUtf8(introduction.readAll());
    }
    for (const auto& value : reference["commands"].toArray()) {
        const auto command = value.toObject();
        html += QStringLiteral("<section><h2><code>%1</code></h2><p>%2</p><pre>%3</pre></section>")
            .arg(command["command"].toString().toHtmlEscaped(), command["description"].toString().toHtmlEscaped(),
                 QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Indented)).toHtmlEscaped());
    }
    html += QStringLiteral("</html>");
    QFile guide(chinese ? QStringLiteral(":/Resources/cli-guide.zh_CN.html") : QStringLiteral(":/Resources/cli-guide.html"));
    if (!guide.open(QIODevice::ReadOnly)) { *error = guide.errorString(); return {}; }
    if (!write("index.html", guide.readAll()) || !write("ai-reference.html", html.toUtf8())
        || !write("command-reference.json", QJsonDocument(reference).toJson(QJsonDocument::Indented))) return {};
    return root + QStringLiteral("/index.html");
}

}
