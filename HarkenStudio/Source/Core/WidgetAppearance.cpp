#include "WidgetAppearance.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QMetaEnum>
#include <QMetaProperty>
#include <QPointer>
#include <QRegularExpression>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

namespace VNX {
namespace {

enum Edge { Left, Top, Right, Bottom };

struct VisualPatch
{
    ControlVisual values;
    std::array<std::optional<int>, 4> borders;
    std::array<std::optional<QSizeF>, 4> radii;
    std::array<std::optional<int>, 4> padding;
    std::array<std::optional<int>, 4> margin;
    std::array<std::optional<int>, 2> size;
    std::array<std::optional<int>, 2> minimum;
    std::array<std::optional<int>, 2> maximum;

    void apply(ControlVisual& result) const
    {
        const auto color = [](QColor& target, const QColor& source) {
            if (source.isValid()) target = source;
        };
        color(result.background, values.background);
        color(result.foreground, values.foreground);
        color(result.alternateBackground, values.alternateBackground);
        color(result.selectionBackground, values.selectionBackground);
        color(result.selectionForeground, values.selectionForeground);
        color(result.gridline, values.gridline);
        if (values.font.resolveMask()) {
            const auto mask = result.font.resolveMask() | values.font.resolveMask();
            result.font = values.font.resolve(result.font);
            result.font.setResolveMask(mask);
        }
        result.borderSpecified |= values.borderSpecified;
        const auto margins = [](QMargins& target, const auto& source) {
            if (source[Left]) target.setLeft(*source[Left]);
            if (source[Top]) target.setTop(*source[Top]);
            if (source[Right]) target.setRight(*source[Right]);
            if (source[Bottom]) target.setBottom(*source[Bottom]);
        };
        margins(result.borders, borders);
        margins(result.padding, padding);
        margins(result.margin, margin);
        for (int index = 0; index < 4; ++index) {
            color(result.borderColors[index], values.borderColors[index]);
            if (radii[index]) result.radii[index] = *radii[index];
        }
        if (!values.image.isNull()) result.image = values.image;
        if (values.textAlignment != Qt::Alignment{}) result.textAlignment = values.textAlignment;
        if (size[0]) result.size.setWidth(*size[0]);
        if (size[1]) result.size.setHeight(*size[1]);
        if (minimum[0]) result.minimum.setWidth(*minimum[0]);
        if (minimum[1]) result.minimum.setHeight(*minimum[1]);
        if (maximum[0]) result.maximum.setWidth(*maximum[0]);
        if (maximum[1]) result.maximum.setHeight(*maximum[1]);
    }
};

struct StatePredicate
{
    QString name;
    bool negated = false;
};

struct SelectorTerm
{
    QString type;
    QString exactClass;
    QString id;
    QVector<QPair<QByteArray, QString>> attributes;
    QVector<StatePredicate> states;
    bool directParent = false;
};

struct Rule
{
    QVector<SelectorTerm> terms;
    QString part;
    VisualPatch patch;
    int ids = 0;
    int attributes = 0;
    int types = 0;
    int order = 0;
};

struct StyleBlock
{
    QString selectors;
    QString body;
    QString nativeSelectors;
    QString nativeBoxSelectors;
    QString nativeFontSelectors;
    QString native;
    VisualPatch patch;
    QVector<int> rules;
};

QString unquote(QString value)
{
    if (value.size() >= 2 && ((value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
        || (value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\''))))
        return value.mid(1, value.size() - 2);
    return value;
}

QColor parseColor(QString value)
{
    value = value.trimmed();
    if (value == QLatin1String("none") || value == QLatin1String("transparent"))
        return QColor(Qt::transparent);
    if (value.startsWith(QLatin1String("rgb"))) {
        const int open = value.indexOf(QLatin1Char('('));
        const int close = value.lastIndexOf(QLatin1Char(')'));
        if (open < 0 || close <= open) return {};
        const auto parts = value.mid(open + 1, close - open - 1).split(QLatin1Char(','));
        if (parts.size() < 3 || parts.size() > 4) return {};
        QColor result(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(), parts[2].trimmed().toInt());
        if (parts.size() == 4) {
            const auto alpha = parts[3].trimmed();
            const qreal value = alpha.endsWith(QLatin1Char('%'))
                ? alpha.chopped(1).toDouble() * 2.55 : alpha.toDouble();
            result.setAlpha(std::clamp(value <= 1.0 ? static_cast<int>(value * 255.0) : qRound(value), 0, 255));
        }
        return result;
    }
    return QColor::fromString(value);
}

qreal length(QString value)
{
    value = value.trimmed();
    if (value.endsWith(QLatin1String("px"))) value.chop(2);
    return value.toDouble();
}

QStringList words(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("\\s+(?![^()]*\\))"));
    return value.split(expression, Qt::SkipEmptyParts);
}

QSizeF radiusValue(const QString& value)
{
    const auto parts = words(value);
    const qreal width = length(parts.value(0));
    return QSizeF(width, parts.size() > 1 ? length(parts[1]) : width);
}

std::array<qreal, 4> boxValues(const QString& value)
{
    const auto parts = words(value);
    if (parts.isEmpty()) return {};
    const qreal top = length(parts[0]);
    const qreal right = parts.size() > 1 ? length(parts[1]) : top;
    const qreal bottom = parts.size() > 2 ? length(parts[2]) : top;
    const qreal left = parts.size() > 3 ? length(parts[3]) : right;
    return {left, top, right, bottom};
}

int edgeFor(QStringView name)
{
    if (name == u"left") return Left;
    if (name == u"top") return Top;
    if (name == u"right") return Right;
    if (name == u"bottom") return Bottom;
    return -1;
}

void setBorder(VisualPatch& patch, int edge, const QString& value)
{
    patch.values.borderSpecified = true;
    int width = 0;
    QColor color;
    for (const auto& word : words(value)) {
        if (word == QLatin1String("none")) {
            width = 0;
            color = Qt::transparent;
        } else if (word.endsWith(QLatin1String("px")) || word == QLatin1String("0")) {
            width = qRound(length(word));
        } else if (const auto parsed = parseColor(word); parsed.isValid()) {
            color = parsed;
        }
    }
    if (edge >= 0) {
        patch.borders[edge] = width;
        patch.values.borderColors[edge] = color;
    } else {
        patch.borders.fill(width);
        patch.values.borderColors.fill(color);
    }
}

bool parseDeclaration(VisualPatch& patch, const QString& property, const QString& value,
                      QStringList& native, bool& borderGeometry)
{
    auto& result = patch.values;
    if (property == QLatin1String("background") || property == QLatin1String("background-color"))
        result.background = parseColor(value);
    else if (property == QLatin1String("color")) result.foreground = parseColor(value);
    else if (property == QLatin1String("alternate-background-color")) result.alternateBackground = parseColor(value);
    else if (property == QLatin1String("selection-background-color")) result.selectionBackground = parseColor(value);
    else if (property == QLatin1String("selection-color")) result.selectionForeground = parseColor(value);
    else if (property == QLatin1String("gridline-color")) result.gridline = parseColor(value);
    else if (property == QLatin1String("image")) {
        result.image = QStringLiteral("");
        if (value.startsWith(QLatin1String("url(")) && value.endsWith(QLatin1Char(')')))
            result.image = unquote(value.mid(4, value.size() - 5).trimmed());
    } else if (property == QLatin1String("font-size")) {
        if (value.endsWith(QLatin1String("pt"))) result.font.setPointSizeF(value.chopped(2).toDouble());
        else result.font.setPixelSize(qRound(length(value)));
        return false;
    } else if (property == QLatin1String("font-weight")) {
        result.font.setWeight(value == QLatin1String("normal") ? QFont::Normal
            : value == QLatin1String("bold") ? QFont::Bold : static_cast<QFont::Weight>(value.toInt()));
        return false;
    } else if (property == QLatin1String("font-family")) {
        QStringList families;
        for (const auto& family : value.split(QLatin1Char(','))) families.append(unquote(family.trimmed()));
        result.font.setFamilies(families);
        return false;
    } else if (property == QLatin1String("letter-spacing")) {
        result.font.setLetterSpacing(QFont::AbsoluteSpacing, length(value));
        return false;
    } else if (property == QLatin1String("text-transform")) {
        result.font.setCapitalization(value == QLatin1String("uppercase") ? QFont::AllUppercase
            : value == QLatin1String("lowercase") ? QFont::AllLowercase
            : value == QLatin1String("capitalize") ? QFont::Capitalize : QFont::MixedCase);
        return false;
    } else if (property == QLatin1String("text-align")) {
        result.textAlignment = Qt::AlignVCenter;
        for (const auto& word : words(value)) {
            if (word == QLatin1String("left")) result.textAlignment |= Qt::AlignLeft;
            else if (word == QLatin1String("right")) result.textAlignment |= Qt::AlignRight;
            else if (word == QLatin1String("center")) result.textAlignment |= Qt::AlignHCenter;
            else if (word == QLatin1String("top")) result.textAlignment = (result.textAlignment & ~Qt::AlignVertical_Mask) | Qt::AlignTop;
            else if (word == QLatin1String("bottom")) result.textAlignment = (result.textAlignment & ~Qt::AlignVertical_Mask) | Qt::AlignBottom;
        }
    } else if (property == QLatin1String("border-radius")) {
        patch.radii.fill(radiusValue(value));
    } else if (property.startsWith(QLatin1String("border-")) && property.endsWith(QLatin1String("-radius"))) {
        const auto corner = property.mid(7, property.size() - 14);
        const int index = corner == QLatin1String("top-left") ? 0 : corner == QLatin1String("top-right") ? 1
            : corner == QLatin1String("bottom-right") ? 2 : 3;
        patch.radii[index] = radiusValue(value);
    } else if (property == QLatin1String("border")) {
        setBorder(patch, -1, value);
        native.append(QStringLiteral("border-width: %1px").arg(*patch.borders[Left]));
        borderGeometry = true;
    } else if (property.startsWith(QLatin1String("border-"))) {
        const auto suffix = property.mid(7);
        const int edge = edgeFor(suffix);
        if (edge >= 0) {
            setBorder(patch, edge, value);
            native.append(QStringLiteral("border-%1-width: %2px").arg(suffix).arg(*patch.borders[edge]));
            borderGeometry = true;
        } else if (suffix == QLatin1String("color")) {
            const auto values = words(value);
            if (!values.isEmpty()) {
                const QColor top = parseColor(values[0]);
                const QColor right = values.size() > 1 ? parseColor(values[1]) : top;
                const QColor bottom = values.size() > 2 ? parseColor(values[2]) : top;
                const QColor left = values.size() > 3 ? parseColor(values[3]) : right;
                result.borderColors = {left, top, right, bottom};
            }
        } else if (suffix.endsWith(QLatin1String("-color"))) {
            const int colorEdge = edgeFor(QStringView(suffix).chopped(6));
            if (colorEdge >= 0) result.borderColors[colorEdge] = parseColor(value);
        } else if (suffix == QLatin1String("width")) {
            result.borderSpecified = true;
            const auto values = boxValues(value);
            for (int index = 0; index < 4; ++index) patch.borders[index] = qRound(values[index]);
            native.append(property + QStringLiteral(": ") + value);
            borderGeometry = true;
        } else if (suffix.endsWith(QLatin1String("-width"))) {
            result.borderSpecified = true;
            const int widthEdge = edgeFor(QStringView(suffix).chopped(6));
            if (widthEdge >= 0) patch.borders[widthEdge] = qRound(length(value));
            native.append(property + QStringLiteral(": ") + value);
            borderGeometry = true;
        } else if (suffix == QLatin1String("style") || suffix.endsWith(QLatin1String("-style"))) {
            result.borderSpecified = true;
            borderGeometry = true;
            if (value == QLatin1String("none")) {
                const int styleEdge = suffix == QLatin1String("style") ? -1 : edgeFor(QStringView(suffix).chopped(6));
                setBorder(patch, styleEdge, value);
                native.append(styleEdge < 0 ? QStringLiteral("border-width: 0px")
                    : QStringLiteral("border-%1-width: 0px").arg(suffix.chopped(6)));
            }
        } else {
            return false;
        }
    } else if (property == QLatin1String("padding") || property == QLatin1String("margin")) {
        auto& target = property == QLatin1String("padding") ? patch.padding : patch.margin;
        const auto values = boxValues(value);
        for (int index = 0; index < 4; ++index) target[index] = qRound(values[index]);
        return false;
    } else if (property.startsWith(QLatin1String("padding-")) || property.startsWith(QLatin1String("margin-"))) {
        const bool padding = property.startsWith(QLatin1String("padding-"));
        const int edge = edgeFor(QStringView(property).sliced(padding ? 8 : 7));
        if (edge >= 0) (padding ? patch.padding : patch.margin)[edge] = qRound(length(value));
        return false;
    } else if (property == QLatin1String("min-width") || property == QLatin1String("min-height")) {
        patch.minimum[property.endsWith(QLatin1String("width")) ? 0 : 1] = qRound(length(value));
        return false;
    } else if (property == QLatin1String("max-width") || property == QLatin1String("max-height")) {
        patch.maximum[property.endsWith(QLatin1String("width")) ? 0 : 1] = qRound(length(value));
        return false;
    } else if (property == QLatin1String("width") || property == QLatin1String("height")) {
        const int dimension = property == QLatin1String("width") ? 0 : 1;
        patch.size[dimension] = qRound(length(value));
        return false;
    } else if (property != QLatin1String("box-shadow") && property != QLatin1String("-qt-style-features")) {
        return false;
    }
    return true;
}

Rule parseSelector(const QString& selector, int order)
{
    Rule rule;
    rule.order = order;
    QStringList fragments;
    QVector<bool> directParents;
    int start = 0;
    int brackets = 0;
    QChar quote;
    bool direct = false;
    const auto append = [&](int end) {
        const auto fragment = selector.mid(start, end - start).trimmed();
        if (!fragment.isEmpty()) {
            fragments.append(fragment);
            directParents.append(direct);
            direct = false;
        }
    };
    for (int index = 0; index < selector.size(); ++index) {
        const auto ch = selector[index];
        if (!quote.isNull()) {
            if (ch == quote) quote = {};
        } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) quote = ch;
        else if (ch == QLatin1Char('[')) ++brackets;
        else if (ch == QLatin1Char(']')) --brackets;
        else if (brackets == 0 && (ch.isSpace() || ch == QLatin1Char('>'))) {
            append(index);
            start = index + 1;
            if (ch == QLatin1Char('>')) direct = true;
        }
    }
    append(selector.size());
    for (int fragmentIndex = 0; fragmentIndex < fragments.size(); ++fragmentIndex) {
        const auto& fragment = fragments[fragmentIndex];
        SelectorTerm term;
        term.directParent = directParents[fragmentIndex];
        int index = 0;
        const auto name = [&]() {
            const int begin = index;
            while (index < fragment.size()
                && (fragment[index].isLetterOrNumber() || fragment[index] == QLatin1Char('_')
                    || fragment[index] == QLatin1Char('-'))) ++index;
            return fragment.mid(begin, index - begin);
        };
        if (fragment.startsWith(QLatin1Char('*'))) ++index;
        else if (!fragment.isEmpty() && (fragment[0].isLetter() || fragment[0] == QLatin1Char('_'))) {
            term.type = name().replace(QStringLiteral("--"), QStringLiteral("::"));
            ++rule.types;
        }
        while (index < fragment.size()) {
            const auto ch = fragment[index++];
            if (ch == QLatin1Char('#')) {
                term.id = name();
                ++rule.ids;
            } else if (ch == QLatin1Char('.')) {
                term.exactClass = name().replace(QStringLiteral("--"), QStringLiteral("::"));
                ++rule.attributes;
            } else if (ch == QLatin1Char('[')) {
                const int end = fragment.indexOf(QLatin1Char(']'), index);
                if (end < 0) break;
                const auto expression = fragment.mid(index, end - index);
                const int equals = expression.indexOf(QLatin1Char('='));
                if (equals >= 0)
                    term.attributes.append({expression.left(equals).trimmed().toUtf8(),
                        unquote(expression.mid(equals + 1).trimmed())});
                else
                    term.attributes.append({expression.trimmed().toUtf8(), QString()});
                ++rule.attributes;
                index = end + 1;
            } else if (ch == QLatin1Char(':')) {
                if (index < fragment.size() && fragment[index] == QLatin1Char(':')) {
                    ++index;
                    rule.part = name();
                } else {
                    const bool negated = index < fragment.size() && fragment[index] == QLatin1Char('!');
                    if (negated) ++index;
                    term.states.append({name(), negated});
                    ++rule.attributes;
                }
            }
        }
        rule.terms.append(std::move(term));
    }
    return rule;
}

void parseBlock(StyleBlock& block)
{
    block.patch = {};
    QStringList geometry;
    QStringList boxes;
    QStringList fonts;
    bool borderGeometry = false;
    for (const auto& declaration : block.body.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int colon = declaration.indexOf(QLatin1Char(':'));
        if (colon < 0) continue;
        const auto property = declaration.left(colon).trimmed();
        const auto value = declaration.mid(colon + 1).trimmed();
        const bool box = property == QLatin1String("border") || property.startsWith(QLatin1String("border-"))
            || property == QLatin1String("padding") || property.startsWith(QLatin1String("padding-"))
            || property == QLatin1String("margin") || property.startsWith(QLatin1String("margin-"))
            || property == QLatin1String("width") || property == QLatin1String("height")
            || property == QLatin1String("min-width") || property == QLatin1String("min-height")
            || property == QLatin1String("max-width") || property == QLatin1String("max-height");
        auto& native = box && block.nativeBoxSelectors != block.nativeSelectors ? boxes : geometry;
        if (!parseDeclaration(block.patch, property, value, native, borderGeometry)) {
            const bool font = property == QLatin1String("font-size") || property == QLatin1String("font-weight")
                || property == QLatin1String("font-family") || property == QLatin1String("letter-spacing")
                || property == QLatin1String("text-transform");
            (font && block.nativeFontSelectors != block.nativeSelectors ? fonts : native)
                .append(property + QStringLiteral(": ") + value);
        }
    }
    if (borderGeometry)
        (block.nativeBoxSelectors != block.nativeSelectors ? boxes : geometry)
            .append(QStringLiteral("border-style: native"));
    block.native = block.nativeSelectors.isEmpty() || geometry.isEmpty() ? QString()
        : block.nativeSelectors + QStringLiteral(" {\n    ")
            + geometry.join(QStringLiteral(";\n    ")) + QStringLiteral(";\n}\n");
    if (!block.nativeBoxSelectors.isEmpty() && !boxes.isEmpty())
        block.native += block.nativeBoxSelectors + QStringLiteral(" {\n    ")
            + boxes.join(QStringLiteral(";\n    ")) + QStringLiteral(";\n}\n");
    if (!block.nativeFontSelectors.isEmpty() && !fonts.isEmpty())
        block.native += block.nativeFontSelectors + QStringLiteral(" {\n    ")
            + fonts.join(QStringLiteral(";\n    ")) + QStringLiteral(";\n}\n");
}

bool matchesStructure(const QWidget* widget, const SelectorTerm& term)
{
    if (!widget) return false;
    if (!term.type.isEmpty() && !widget->inherits(term.type.toUtf8().constData())) return false;
    if (!term.exactClass.isEmpty() && term.exactClass != QLatin1String(widget->metaObject()->className())) return false;
    if (!term.id.isEmpty() && term.id != widget->objectName()) return false;
    for (const auto& [name, expected] : term.attributes) {
        const auto value = widget->property(name.constData());
        if (!value.isValid()) return false;
        if (expected.isNull()) continue;
        if (value.toString() == expected) continue;
        const int propertyIndex = widget->metaObject()->indexOfProperty(name.constData());
        if (propertyIndex < 0) return false;
        const auto property = widget->metaObject()->property(propertyIndex);
        if (!property.isEnumType() || expected != QLatin1String(property.enumerator().valueToKey(value.toInt())))
            return false;
    }
    return true;
}

QStyle::State widgetState(const QWidget* widget)
{
    QStyle::State result;
    if (widget->isEnabled()) result |= QStyle::State_Enabled;
    if (widget->hasFocus()) result |= QStyle::State_HasFocus;
    if (widget->underMouse()) result |= QStyle::State_MouseOver;
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget)) {
        if (button->isDown()) result |= QStyle::State_Sunken;
        if (button->isChecked()) result |= QStyle::State_On;
    }
    return result;
}

bool stateValue(const StatePredicate& predicate, const QWidget* widget,
                QStyle::State state, const QStringList& extraStates)
{
    const auto& name = predicate.name;
    bool value = false;
    if (name == QLatin1String("enabled")) value = state.testFlag(QStyle::State_Enabled);
    else if (name == QLatin1String("disabled")) value = !state.testFlag(QStyle::State_Enabled);
    else if (name == QLatin1String("hover")) value = state.testFlag(QStyle::State_Enabled)
        && state.testFlag(QStyle::State_MouseOver);
    else if (name == QLatin1String("focus")) value = state.testFlag(QStyle::State_HasFocus);
    else if (name == QLatin1String("pressed")) value = state.testFlag(QStyle::State_Sunken);
    else if (name == QLatin1String("checked") || name == QLatin1String("on")) value = state.testFlag(QStyle::State_On);
    else if (name == QLatin1String("indeterminate")) value = state.testFlag(QStyle::State_NoChange);
    else if (name == QLatin1String("selected")) value = state.testFlag(QStyle::State_Selected);
    else if (name == QLatin1String("horizontal") || name == QLatin1String("vertical")) {
        const bool horizontal = extraStates.contains(QStringLiteral("horizontal"));
        const bool vertical = extraStates.contains(QStringLiteral("vertical"));
        value = horizontal || vertical ? extraStates.contains(name)
            : (name == QLatin1String("horizontal")) == state.testFlag(QStyle::State_Horizontal);
    }
    else if (name == QLatin1String("editable")) value = widget->property("editable").toBool() || extraStates.contains(name);
    else value = extraStates.contains(name);
    return predicate.negated ? !value : value;
}

using MatchPath = QVector<QPointer<QWidget>>;

void collectPaths(const Rule& rule, int termIndex, const QWidget* widget,
                   MatchPath& path, QVector<MatchPath>& result)
{
    if (!matchesStructure(widget, rule.terms[termIndex])) return;
    path[termIndex] = const_cast<QWidget*>(widget);
    if (termIndex == 0) {
        result.append(path);
        return;
    }
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
        collectPaths(rule, termIndex - 1, parent, path, result);
        if (rule.terms[termIndex].directParent) break;
    }
}

bool matchesStates(const Rule& rule, const MatchPath& path,
                   QStyle::State state, const QStringList& extraStates)
{
    static const QStringList noExtraStates;
    for (int index = 0; index < rule.terms.size(); ++index) {
        const auto* widget = path[index].data();
        if (!widget) return false;
        const bool target = index == rule.terms.size() - 1;
        const auto currentState = target ? state : widgetState(widget);
        for (const auto& predicate : rule.terms[index].states)
            if (!stateValue(predicate, widget, currentState, target ? extraStates : noExtraStates)) return false;
    }
    return true;
}

}

class WidgetAppearance::Impl
{
public:
    struct Candidate
    {
        int rule = 0;
        QVector<MatchPath> paths;
    };

    struct WidgetRules
    {
        QPointer<QWidget> widget;
        const QMetaObject* metaObject = nullptr;
        QVector<Candidate> candidates;
        QVector<QPointer<QWidget>> localScopes;
    };

    struct LocalRules
    {
        QPointer<QWidget> widget;
        QString source;
        std::shared_ptr<WidgetAppearance> appearance;
        bool managed = false;
    };

    struct WidgetGeometry
    {
        QPointer<QWidget> widget;
        QSize size;
        QSize minimum;
        QSize maximum;
        QMargins box;
        std::array<QSize, 3> dropdownSizes;
        QMargins dropdownBox;

        bool operator==(const WidgetGeometry&) const = default;
    };

    QString source;
    QString native;
    QVector<StyleBlock> blocks;
    QVector<Rule> rules;
    bool includeLocalScopes = true;
    mutable QHash<const QWidget*, WidgetRules> widgets;
    mutable QHash<const QWidget*, LocalRules> localRules;
    QHash<const QWidget*, WidgetGeometry> geometries;

    const WidgetRules& candidates(const QWidget* widget) const
    {
        auto found = widgets.find(widget);
        if (found != widgets.end() && found->widget == widget && found->metaObject == widget->metaObject())
            return *found;
        WidgetRules result;
        result.widget = const_cast<QWidget*>(widget);
        result.metaObject = widget->metaObject();
        if (includeLocalScopes)
            for (auto* owner = const_cast<QWidget*>(widget); owner; owner = owner->parentWidget()) {
                const auto local = localRules.constFind(owner);
                if (!owner->styleSheet().isEmpty() || (local != localRules.cend()
                    && local->widget == owner && local->managed)) result.localScopes.append(owner);
            }
        for (int index = 0; index < rules.size(); ++index) {
            const auto& rule = rules[index];
            if (rule.terms.isEmpty()) continue;
            Candidate candidate;
            candidate.rule = index;
            MatchPath path(rule.terms.size());
            collectPaths(rule, rule.terms.size() - 1, widget, path, candidate.paths);
            if (!candidate.paths.isEmpty()) result.candidates.append(std::move(candidate));
        }
        return *widgets.insert(widget, std::move(result));
    }

    void apply(ControlVisual& result, const QWidget* widget, QStyle::State state,
               QStringView part, const QStringList& extraStates) const
    {
        for (const auto& candidate : candidates(widget).candidates) {
            const auto& rule = rules[candidate.rule];
            if (rule.part != part) continue;
            if (std::any_of(candidate.paths.cbegin(), candidate.paths.cend(), [&](const MatchPath& path) {
                    return matchesStates(rule, path, state, extraStates);
                }))
                rule.patch.apply(result);
        }
    }
};

WidgetAppearance::WidgetAppearance() : impl_(std::make_unique<Impl>()) {}
WidgetAppearance::~WidgetAppearance() = default;

bool WidgetAppearance::updateStyleSheet(QString styleSheet)
{
    if (impl_->source == styleSheet) return false;
    impl_->source = std::move(styleSheet);
    QString clean = impl_->source;
    static const QRegularExpression comments(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption);
    clean.remove(comments);
    QVector<QPair<QString, QString>> sources;
    int position = 0;
    while (position < clean.size()) {
        const int open = clean.indexOf(QLatin1Char('{'), position);
        if (open < 0) break;
        const int close = clean.indexOf(QLatin1Char('}'), open + 1);
        if (close < 0) break;
        sources.append({clean.mid(position, open - position).trimmed(), clean.mid(open + 1, close - open - 1)});
        position = close + 1;
    }
    const bool sameSelectors = sources.size() == impl_->blocks.size()
        && std::equal(sources.cbegin(), sources.cend(), impl_->blocks.cbegin(),
                      [](const auto& source, const StyleBlock& block) { return source.first == block.selectors; });
    bool nativeChanged = !sameSelectors;
    if (sameSelectors) {
        for (int index = 0; index < sources.size(); ++index) {
            auto& block = impl_->blocks[index];
            if (block.body == sources[index].second) continue;
            block.body = std::move(sources[index].second);
            const auto previousNative = block.native;
            parseBlock(block);
            nativeChanged |= previousNative != block.native;
            for (const int rule : block.rules) impl_->rules[rule].patch = block.patch;
        }
    } else {
        impl_->blocks.clear();
        impl_->rules.clear();
        impl_->blocks.reserve(sources.size());
        for (int order = 0; order < sources.size(); ++order) {
            StyleBlock block;
            block.selectors = std::move(sources[order].first);
            block.body = std::move(sources[order].second);
            QStringList nativeSelectors;
            QStringList nativeBoxSelectors;
            QStringList nativeFontSelectors;
            for (const auto& selector : block.selectors.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                auto rule = parseSelector(selector.trimmed(), order);
                const auto type = rule.terms.isEmpty() ? QString() : rule.terms.last().type;
                const bool customTab = type == QLatin1String("QTabBar") && rule.part == QLatin1String("tab");
                const bool customCombo = type == QLatin1String("QComboBox")
                    && (rule.part == QLatin1String("drop-down") || rule.part == QLatin1String("down-arrow"));
                if (!customTab && !customCombo) {
                    nativeSelectors.append(selector.trimmed());
                    if (type != QLatin1String("QComboBox") || !rule.part.isEmpty())
                        nativeBoxSelectors.append(selector.trimmed());
                    if (type != QLatin1String("QHeaderView") || rule.part != QLatin1String("section"))
                        nativeFontSelectors.append(selector.trimmed());
                }
                impl_->rules.append(std::move(rule));
            }
            block.nativeSelectors = nativeSelectors.join(QStringLiteral(",\n"));
            block.nativeBoxSelectors = nativeBoxSelectors.join(QStringLiteral(",\n"));
            block.nativeFontSelectors = nativeFontSelectors.join(QStringLiteral(",\n"));
            parseBlock(block);
            impl_->blocks.append(std::move(block));
        }
        std::stable_sort(impl_->rules.begin(), impl_->rules.end(), [](const Rule& first, const Rule& second) {
            return std::tie(first.ids, first.attributes, first.types, first.order)
                < std::tie(second.ids, second.attributes, second.types, second.order);
        });
        for (int index = 0; index < impl_->rules.size(); ++index) {
            auto& rule = impl_->rules[index];
            auto& block = impl_->blocks[rule.order];
            block.rules.append(index);
            rule.patch = block.patch;
        }
        impl_->widgets.clear();
    }
    if (nativeChanged) {
        QString native;
        for (const auto& block : impl_->blocks) native += block.native;
        if (impl_->native != native) impl_->widgets.clear();
        impl_->native = std::move(native);
    }
    return true;
}

const QString& WidgetAppearance::nativeStyleSheet() const
{
    return impl_->native;
}

const QString& WidgetAppearance::updateWidgetStyle(QWidget* widget, QString styleSheet)
{
    auto found = impl_->localRules.find(widget);
    if (found == impl_->localRules.end() || found->widget != widget) {
        Impl::LocalRules local;
        local.widget = widget;
        local.appearance = std::make_shared<WidgetAppearance>();
        local.appearance->impl_->includeLocalScopes = false;
        found = impl_->localRules.insert(widget, std::move(local));
    }
    const bool addedScope = !found->managed;
    found->managed = true;
    if (found->source != styleSheet) {
        found->source = std::move(styleSheet);
        found->appearance->updateStyleSheet(found->source.contains(QLatin1Char('{'))
            ? found->source : QStringLiteral("* {") + found->source + QLatin1Char('}'));
    }
    const auto appearance = found->appearance;
    if (addedScope) invalidate(widget);
    return appearance->nativeStyleSheet();
}

void WidgetAppearance::updateWidgetGeometry(QWidget* widget, const ControlVisual& visual)
{
    const auto dropdown = this->visual(widget, QStyle::State_Enabled, u"drop-down");
    const Impl::WidgetGeometry next{widget, visual.size, visual.minimum, visual.maximum,
        visual.margin + visual.borders + visual.padding,
        {dropdown.size, dropdown.minimum, dropdown.maximum},
        dropdown.margin + dropdown.borders + dropdown.padding};
    const auto previous = impl_->geometries.value(widget);
    if (previous == next) return;
    const bool managed = previous.widget == widget;
    impl_->geometries.insert(widget, next);
    const int horizontal = next.box.left() + next.box.right();
    const int vertical = next.box.top() + next.box.bottom();
    if (next.minimum.width() >= 0)
        widget->setMinimumWidth(std::max(next.size.width(), next.minimum.width()) + horizontal);
    else if (managed && previous.minimum.width() >= 0)
        widget->setMinimumWidth(0);
    if (next.minimum.height() >= 0)
        widget->setMinimumHeight(std::max(next.size.height(), next.minimum.height()) + vertical);
    else if (managed && previous.minimum.height() >= 0)
        widget->setMinimumHeight(0);
    if (next.maximum.width() >= 0)
        widget->setMaximumWidth(std::min(next.size.width() >= 0 ? next.size.width() : QWIDGETSIZE_MAX,
                                        next.maximum.width()) + horizontal);
    else if (managed && previous.maximum.width() >= 0)
        widget->setMaximumWidth(QWIDGETSIZE_MAX);
    if (next.maximum.height() >= 0)
        widget->setMaximumHeight(std::min(next.size.height() >= 0 ? next.size.height() : QWIDGETSIZE_MAX,
                                         next.maximum.height()) + vertical);
    else if (managed && previous.maximum.height() >= 0)
        widget->setMaximumHeight(QWIDGETSIZE_MAX);
    QEvent changed(QEvent::StyleChange);
    QCoreApplication::sendEvent(widget, &changed);
    widget->updateGeometry();
}

ControlVisual WidgetAppearance::visual(const QWidget* widget, QStyle::State state,
                                        QStringView part, QStringList extraStates) const
{
    ControlVisual result;
    if (!widget) return result;
    impl_->apply(result, widget, state, part, extraStates);
    const auto& scopes = impl_->candidates(widget).localScopes;
    for (auto owner = scopes.crbegin(); owner != scopes.crend(); ++owner) {
        if (!*owner) continue;
        auto found = impl_->localRules.find(*owner);
        if (found == impl_->localRules.end() || found->widget != *owner) {
            Impl::LocalRules local;
            local.widget = *owner;
            local.appearance = std::make_shared<WidgetAppearance>();
            local.appearance->impl_->includeLocalScopes = false;
            found = impl_->localRules.insert(*owner, std::move(local));
        }
        if (!found->managed) {
            const auto source = (*owner)->styleSheet();
            if (source.isEmpty()) continue;
            if (found->source != source) {
                found->source = source;
                found->appearance->updateStyleSheet(source.contains(QLatin1Char('{'))
                    ? source : QStringLiteral("* {") + source + QLatin1Char('}'));
            }
        }
        found->appearance->impl_->apply(result, widget, state, part, extraStates);
    }
    return result;
}

void WidgetAppearance::invalidate(const QWidget* widget)
{
    for (auto it = impl_->geometries.begin(); it != impl_->geometries.end();) {
        if (it->widget.isNull()) it = impl_->geometries.erase(it);
        else ++it;
    }
    for (auto it = impl_->widgets.begin(); it != impl_->widgets.end();) {
        if (it->widget.isNull() || it.key() == widget || (widget && widget->isAncestorOf(it->widget)))
            it = impl_->widgets.erase(it);
        else
            ++it;
    }
    for (auto it = impl_->localRules.begin(); it != impl_->localRules.end();) {
        if (it->widget.isNull()) it = impl_->localRules.erase(it);
        else {
            it->appearance->invalidate(widget);
            ++it;
        }
    }
}

void WidgetAppearance::clearWidgetCache()
{
    impl_->widgets.clear();
    for (auto& local : impl_->localRules) local.appearance->clearWidgetCache();
}

}
