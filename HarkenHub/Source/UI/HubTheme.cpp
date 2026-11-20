#include "UI/HubTheme.h"

#include "Settings/HubSettings.h"

#include <QApplication>
#include <QFile>
#include <QEvent>
#include <QPalette>
#include <QRegularExpression>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QWidget>

namespace VNH::HubTheme {

namespace {

class ThemeChangeFilter final : public QObject
{
public:
    ThemeChangeFilter(QWidget* widget, std::function<void()> callback)
        : QObject(widget), callback_(std::move(callback)) { widget->installEventFilter(this); }

    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::PaletteChange && !pending_) {
            pending_ = true;
            QTimer::singleShot(0, this, [this] { pending_ = false; callback_(); });
        }
        return false;
    }

private:
    std::function<void()> callback_;
    bool pending_ = false;
};

}

void observe(QWidget* widget, std::function<void()> callback)
{
    new ThemeChangeFilter(widget, std::move(callback));
}

QColor color(Tone tone)
{
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    switch (tone) {
    case Tone::Text: return QApplication::palette().color(QPalette::WindowText);
    case Tone::Muted: return QApplication::palette().color(QPalette::PlaceholderText);
    case Tone::Accent: return QApplication::palette().color(QPalette::Highlight);
    case Tone::Warning: return QColor(dark ? "#FFB454" : "#9B4B00");
    case Tone::Error: return QColor(dark ? "#FF6B6B" : "#B42332");
    case Tone::Success: return QColor(dark ? "#69D391" : "#16743D");
    case Tone::Info: return QColor(dark ? "#83B9FF" : "#225BB0");
    }
    return {};
}

void setTone(QWidget* widget, Tone tone)
{
    widget->setProperty("hubTone", static_cast<int>(tone));
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString html(QString text)
{
    static const QRegularExpression styles(QStringLiteral("<[^>]*?\\bstyle\\s*=\\s*(['\"])(.*?)\\1"), QRegularExpression::CaseInsensitiveOption);
    auto matches = styles.globalMatch(text);
    QVector<QRegularExpressionMatch> found;
    while (matches.hasNext()) found.push_back(matches.next());
    for (auto it = found.crbegin(); it != found.crend(); ++it) {
        QString style = it->captured(2);
        for (const auto& entry : {std::pair{"#F0C36A", Tone::Warning}, {"#FFB454", Tone::Warning},
                {"#E3B341", Tone::Warning}, {"#FF6B6B", Tone::Error}, {"#FF8B8B", Tone::Error},
                {"#69D391", Tone::Success}, {"#83B9FF", Tone::Info}, {"#9AA6B8", Tone::Muted},
                {"#9AA4B5", Tone::Muted}, {"#00F0FF", Tone::Accent}, {"#00D9EF", Tone::Accent}})
            style.replace(QLatin1String(entry.first), color(entry.second).name(), Qt::CaseInsensitive);
        text.replace(it->capturedStart(2), it->capturedLength(2), style);
    }
    return text;
}

void applyPreference()
{
    const QString preference = HubSettings::themePreference();
    auto* hints = QApplication::styleHints();
    hints->setColorScheme(preference == QStringLiteral("light") ? Qt::ColorScheme::Light
        : preference == QStringLiteral("dark") ? Qt::ColorScheme::Dark : Qt::ColorScheme::Unknown);
    const bool dark = preference == QStringLiteral("dark")
        || (preference == QStringLiteral("system") && hints->colorScheme() == Qt::ColorScheme::Dark);
    QPalette palette;
    const auto set = [&](QPalette::ColorRole role, const char* darkColor, const char* lightColor) {
        palette.setColor(role, QColor(dark ? darkColor : lightColor));
    };
    set(QPalette::Window, "#0D111A", "#F5F7FA");
    set(QPalette::Base, "#080C14", "#FFFFFF");
    set(QPalette::AlternateBase, "#171C28", "#E9EDF3");
    set(QPalette::Button, "#2A3140", "#E1E7EF");
    set(QPalette::Light, "#333B4D", "#D3DCE7");
    set(QPalette::Midlight, "#222836", "#E5EBF2");
    set(QPalette::Mid, "#3D4554", "#AAB5C4");
    set(QPalette::Dark, "#06080D", "#EDF1F6");
    set(QPalette::Shadow, "#1A2030", "#D7DFE9");
    set(QPalette::Text, "#E0E0E0", "#202936");
    set(QPalette::WindowText, "#E0E0E0", "#202936");
    set(QPalette::ButtonText, "#E0E0E0", "#202936");
    set(QPalette::BrightText, "#FFFFFF", "#111827");
    set(QPalette::Highlight, "#00F0FF", "#007C8A");
    set(QPalette::HighlightedText, "#000000", "#FFFFFF");
    set(QPalette::Link, "#00D9EF", "#006B82");
    set(QPalette::LinkVisited, "#8BB8FF", "#3957A8");
    set(QPalette::PlaceholderText, "#AAB3C2", "#566375");
    set(QPalette::ToolTipBase, "#1D2330", "#FFFFFF");
    set(QPalette::ToolTipText, "#E0E0E0", "#202936");
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(dark ? "#747D8C" : "#718092"));
    QApplication::setPalette(palette);
    QFile file(QStringLiteral(":/Resources/HubStyle.qss"));
    if (!file.open(QIODevice::ReadOnly)) return;
    QString sheet = QString::fromUtf8(file.readAll());
    sheet.replace(QStringLiteral("palette(placeholder-text)"), color(Tone::Muted).name());
    for (auto tone : {Tone::Text, Tone::Muted, Tone::Accent, Tone::Warning, Tone::Error, Tone::Success, Tone::Info})
        sheet += QStringLiteral("\n*[hubTone=\"%1\"] { color: %2; }").arg(static_cast<int>(tone)).arg(color(tone).name());
    qApp->setStyleSheet(sheet);
}

void initialize()
{
    QObject::connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
        QTimer::singleShot(0, qApp, &applyPreference);
    });
    applyPreference();
}

}
