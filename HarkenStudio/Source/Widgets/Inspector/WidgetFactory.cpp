
#include "WidgetFactory.h"
#include "ObjectReferencePicker.h"
#include "Core/QtControlSkin.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/SegmentedControl.h"
#include "Widgets/Common/ValueSegmentedControl.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/NumericToolTip.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/CollapsibleSection.h"
#include <QColor>
#include <QColorDialog>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QPushButton>
#include <QDebug>

namespace VNX {

WidgetFactory& WidgetFactory::instance()
{
    static WidgetFactory s;
    return s;
}

WidgetFactory::WidgetFactory()
{
    registerBuiltins();
}

void WidgetFactory::registerType(const QString& typeName, WidgetCreator creator)
{
    creators_[typeName] = std::move(creator);
}

QWidget* WidgetFactory::create(const QString& typeName, const QJsonObject& config, QWidget* parent) const
{
    auto it = creators_.find(typeName);
    if (it == creators_.end())
        return nullptr;
    return it.value()(config, parent);
}

QStringList WidgetFactory::registeredTypes() const
{
    return creators_.keys();
}

bool WidgetFactory::hasType(const QString& typeName) const
{
    return creators_.contains(typeName);
}

void WidgetFactory::registerCustomWidget(const QString& name, CustomWidgetCreator creator)
{
    customWidgets_[name] = std::move(creator);
}

bool WidgetFactory::hasCustomWidget(const QString& name) const
{
    return customWidgets_.contains(name);
}

void WidgetFactory::registerBuiltins()
{
    creators_["slider"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto label = cfg["label"].toString("Parameter");
        auto* slider = new ParameterSlider(label, parent);
        double minVal = cfg["min"].toDouble(0.0);
        double maxVal = cfg["max"].toDouble(100.0);
        int decimals = cfg["decimals"].toInt(2);
        slider->binding()->setDisplayScale(cfg["displayScale"].toDouble(1.0));
        slider->setRange(minVal, maxVal, decimals);
        if (cfg.contains("suffix"))
            slider->setSuffix(cfg["suffix"].toString());
        if (cfg.contains("default"))
            slider->setValue(cfg["default"].toDouble());
        if (cfg.contains("objectName"))
            slider->setObjectName(cfg["objectName"].toString());
        return slider;
    };

    creators_["int_spinbox"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* container = new QWidget(parent);
        DebugTint::tintContainer(container);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);

        auto labelText = cfg["label"].toString();
        if (!labelText.isEmpty()) {
            auto* label = new QLabel(labelText, container);
            label->setMinimumWidth(80);
            layout->addWidget(label);
        }

        auto* spin = new QSpinBox(container);
        spin->setKeyboardTracking(false);
        spin->setMinimum(cfg["min"].toInt(0));
        spin->setMaximum(cfg["max"].toInt(100));
        spin->setValue(cfg["default"].toInt(0));
        if (cfg.contains("suffix"))
            new NumericToolTip(spin, cfg["suffix"].toString());
        if (cfg.contains("specialValueText"))
            spin->setSpecialValueText(cfg["specialValueText"].toString());
        if (cfg.contains("objectName"))
            spin->setObjectName(cfg["objectName"].toString());
        if (cfg.contains("objectName"))
            container->setObjectName(cfg["objectName"].toString() + "_container");
        layout->addWidget(spin, 1);
        return container;
    };

    creators_["combo"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* container = new QWidget(parent);
        DebugTint::tintContainer(container);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);

        auto labelText = cfg["label"].toString();
        if (!labelText.isEmpty()) {
            auto* label = new QLabel(labelText, container);
            label->setMinimumWidth(80);
            layout->addWidget(label);
        }

        auto* combo = new QComboBox(container);
        auto options = cfg["options"].toArray();
        for (const auto& opt : options) {
            if (opt.isObject()) {
                auto obj = opt.toObject();
                combo->addItem(obj["text"].toString(), obj["value"].toVariant());
            } else if (opt.isString()) {
                combo->addItem(opt.toString(), opt.toString());
            } else {
                auto val = opt.toInt();
                combo->addItem(QString::number(val), val);
            }
        }
        if (cfg.contains("objectName"))
            combo->setObjectName(cfg["objectName"].toString());
        if (cfg.contains("objectName"))
            container->setObjectName(cfg["objectName"].toString() + "_container");
        layout->addWidget(combo, 1);
        return container;
    };

    creators_["segmented"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* control = new SegmentedControl(parent);
        const QString objectName = cfg.value(QStringLiteral("objectName")).toString();
        if (!objectName.isEmpty())
            control->setObjectName(objectName);
        QList<QPair<QString, QVariant>> optionList;
        const auto jsonOptions = cfg.value(QStringLiteral("options")).toArray();
        for (const auto& value : jsonOptions) {
            const QJsonObject option = value.toObject();
            optionList.append({option.value(QStringLiteral("text")).toString(),
                               option.value(QStringLiteral("value")).toVariant()});
        }
        control->setOptions(optionList, cfg.value(QStringLiteral("exclusive")).toBool(true));
        return control;
    };

    creators_["value_segmented"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* control = new ValueSegmentedControl(parent);
        const QString objectName = cfg.value(QStringLiteral("objectName")).toString();
        if (!objectName.isEmpty())
            control->setObjectName(objectName);
        std::vector<SegmentSpec> specs;
        for (const auto& value : cfg.value(QStringLiteral("segments")).toArray()) {
            const QJsonObject segment = value.toObject();
            SegmentSpec spec;
            spec.text = segment.value(QStringLiteral("text")).toString();
            spec.value = segment.value(QStringLiteral("value")).toVariant();
            spec.numeric = segment.value(QStringLiteral("numeric")).toBool();
            if (segment.contains(QStringLiteral("numericMin")))
                spec.numericMin = segment.value(QStringLiteral("numericMin")).toVariant();
            if (segment.contains(QStringLiteral("numericMax")))
                spec.numericMax = segment.value(QStringLiteral("numericMax")).toVariant();
            specs.push_back(spec);
        }
        control->setSegments(specs);
        return control;
    };

    creators_["bus_selector"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* container = new QWidget(parent);
        DebugTint::tintContainer(container);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);

        auto labelText = cfg["label"].toString();
        if (!labelText.isEmpty()) {
            auto* label = new QLabel(labelText, container);
            label->setMinimumWidth(80);
            layout->addWidget(label);
        }

        auto* combo = new QComboBox(container);
        if (cfg.contains("objectName"))
            combo->setObjectName(cfg["objectName"].toString());
        if (cfg.contains("objectName"))
            container->setObjectName(cfg["objectName"].toString() + "_container");
        layout->addWidget(combo, 1);
        return container;
    };

    creators_["checkbox"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* check = new QCheckBox(cfg["label"].toString(), parent);
        check->setChecked(cfg["default"].toBool(false));
        if (cfg.contains("objectName"))
            check->setObjectName(cfg["objectName"].toString());
        return check;
    };

    creators_["label"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* label = new QLabel(cfg["text"].toString(), parent);
        if (cfg.contains("objectName"))
            label->setObjectName(cfg["objectName"].toString());
        return label;
    };

    creators_["text"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* edit = new QLineEdit(parent);
        if (cfg.contains("objectName"))
            edit->setObjectName(cfg["objectName"].toString());
        return edit;
    };

    creators_["color_picker"] = [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        auto* container = new QWidget(parent);
        DebugTint::tintContainer(container);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);

        auto labelText = cfg["label"].toString();
        if (!labelText.isEmpty()) {
            auto* label = new QLabel(labelText, container);
            label->setMinimumWidth(80);
            layout->addWidget(label);
        }

        auto* swatch = new QPushButton(container);
        swatch->setFixedWidth(120);
        const QString objName = cfg["objectName"].toString();
        if (!objName.isEmpty()) {
            swatch->setObjectName(objName);
            container->setObjectName(objName + "_container");
        }

        auto applyVisual = [swatch](uint32_t argb) {
            const QColor c = QColor::fromRgba(argb);
            const QString hex = QStringLiteral("0x%1")
                .arg(argb, 8, 16, QChar('0')).toUpper();
            QtControlSkin::setSemanticProperty(swatch, "fieldRole",
                                              QStringLiteral("colorSwatch"));
            swatch->setStyleSheet(QtControlSkin::colorSwatchStyle(c));
            swatch->setText(hex);
            swatch->setProperty("harkenColor",
                                QVariant::fromValue<qint64>(static_cast<qint64>(argb)));
        };

        const uint32_t seedColor = cfg.contains("default")
            ? static_cast<uint32_t>(cfg["default"].toVariant().toLongLong())
            : 0xFF808080u;
        applyVisual(seedColor);

        QObject::connect(swatch, &QPushButton::clicked, swatch, [swatch, applyVisual]() {
            const qint64 cur = swatch->property("harkenColor").toLongLong();
            const QColor before = QColor::fromRgba(static_cast<uint32_t>(cur));
            const QColor picked = QColorDialog::getColor(
                before, swatch, QObject::tr("Pick Color"),
                QColorDialog::ShowAlphaChannel);
            if (!picked.isValid()) return;
            const uint32_t newARGB = picked.rgba();
            applyVisual(newARGB);
        });

        layout->addWidget(swatch, 1);
        return container;
    };

    creators_["custom_widget"] = [this](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
        const QString name = cfg["widget"].toString();
        auto it = customWidgets_.find(name);
        if (it == customWidgets_.end()) {
            qWarning() << "[WidgetFactory] Unknown custom_widget:" << name;
            return nullptr;
        }
        auto* w = it.value()(cfg, parent);
        if (w && cfg.contains("objectName"))
            w->setObjectName(cfg["objectName"].toString());
        return w;
    };

    registerCustomWidget("ObjectReferencePicker",
        [](const QJsonObject& cfg, QWidget* parent) -> QWidget* {
            auto* picker = new ObjectReferencePicker(parent);
            const QJsonObject conf = cfg.value(QStringLiteral("config")).toObject();
            const QString ftStr = conf.value(QStringLiteral("filterType")).toString();
            if (!ftStr.isEmpty())
                picker->setFilterType(ObjectReferencePicker::parseTypeString(ftStr));
            const QString fieldName = conf.value(QStringLiteral("field")).toString();
            if (!fieldName.isEmpty())
                picker->setActionFieldName(fieldName);
            const QString actionId = conf.value(QStringLiteral("action")).toString();
            if (!actionId.isEmpty())
                picker->setActionId(actionId);
            return picker;
        });
}

}
