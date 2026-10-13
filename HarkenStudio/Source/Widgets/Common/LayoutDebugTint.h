#pragma once

// Inspector column/row layout tinting scaffolding.
//
// A resident debugging tool, not a temporary patch: flip kEnabled to paint
// every layout container, cell and row in its role color while reviewing the
// inspector's three-column field layout or the Steps table alignment. With
// kEnabled false the whole thing compiles away — no stylesheet, no property,
// nothing left on a production widget.
//
// Single source of truth for those colors.
//
// Legend (role -> tint):
//   Row       green   content row background (field row, steps data row)
//   Label     blue    fixed label / index column (field label, steps '#')
//   Flex      yellow  flexible value column container (field value, steps 'Target')
//   Gutter    orange  link-slot / launch gutter
//   Section   purple  collapsible section content region (indent gutter)
//   Box       teal    list/table outer box
//   Head      amber   table header strip
//   Foot      amber-  table footer strip
//   ColType   indigo  steps 'Type' column
//   ColDelay  cyan    steps 'Delay' column
//   ColToggle pink    steps expand-toggle column
//   Accordion teal-   steps expanded sub-row
//
// Every call site is reachable through the DebugTint:: qualifier.

#include <QColor>
#include <QString>
#include <QStringLiteral>
#include <QWidget>

namespace VNX {
namespace DebugTint {

inline constexpr bool kEnabled = false;

enum class Role {
    Row,
    Label,
    Flex,
    Gutter,
    Section,
    Box,
    Head,
    Foot,
    ColType,
    ColDelay,
    ColToggle,
    Accordion,
};

inline const char* rgba(Role role)
{
    switch (role) {
        case Role::Row:       return "64,220,140,255";
        case Role::Label:     return "64,140,255,255";
        case Role::Flex:      return "240,220,70,255";
        case Role::Gutter:    return "255,140,0,255";
        case Role::Section:   return "150,110,230,255";
        case Role::Box:       return "0,190,190,255";
        case Role::Head:      return "255,180,40,255";
        case Role::Foot:      return "200,130,20,255";
        case Role::ColType:   return "120,100,255,255";
        case Role::ColDelay:  return "60,200,255,255";
        case Role::ColToggle: return "255,120,200,255";
        case Role::Accordion: return "120,210,210,255";
    }
    return "255,0,0,255";
}

// QColor view over the same palette, for painter-drawn rows (tree delegates)
// where stylesheets never apply. Alpha < 255 keeps glyphs/guides readable.
inline QColor color(Role role, int alpha = 255)
{
    const QStringList parts = QString::fromLatin1(rgba(role)).split(QLatin1Char(','));
    QColor c(parts.value(0).toInt(), parts.value(1).toInt(), parts.value(2).toInt());
    c.setAlpha(alpha);
    return c;
}

// ID-anchored tint, for a widget the surrounding code already names or is happy
// to have named. A generated name is assigned only when the widget has none, so
// widgets carrying a meaningful objectName (e.g. listRow) keep it and the rule
// wins on cascade order against the global QWidget{} background.
inline void tint(QWidget* w, Role role)
{
    if (!kEnabled || !w)
        return;
    QString name = w->objectName();
    if (name.isEmpty()) {
        static unsigned long long counter = 0;
        name = QStringLiteral("dbgTint_%1").arg(counter++);
        w->setObjectName(name);
    }
    w->setStyleSheet(w->styleSheet()
        + QStringLiteral("QWidget#%1{background-color:rgba(%2);}")
              .arg(name, QString::fromLatin1(rgba(role))));
}

// Selectorless declaration, for a widget whose stylesheet is itself a bare
// property list rather than a rule block, e.g. the inspector field label.
inline void tintBare(QWidget* w, Role role)
{
    if (!kEnabled || !w)
        return;
    w->setStyleSheet(w->styleSheet()
        + QStringLiteral("background-color:rgba(%1);")
              .arg(QString::fromLatin1(rgba(role))));
}

// Property-anchored tint, for the bare `new QWidget` containers that carry a
// layout instead of content. The marker property scopes the rule to the
// container itself: an exact-class rule would otherwise cascade into every
// nameless bare-QWidget descendant, and an ID-anchored one would be dropped by
// a later setObjectName on the container.
inline void tintContainer(QWidget* w, Role role = Role::Flex)
{
    if (!kEnabled || !w)
        return;
    w->setProperty("layoutContainer", true);
    w->setStyleSheet(w->styleSheet()
        + QStringLiteral(".QWidget[layoutContainer=\"true\"]{background-color:rgba(%1);}")
              .arg(QString::fromLatin1(rgba(role))));
}

}
}
