
#pragma once

#include "Core/BindingContract.h"
#include "Core/ContextInfo.h"
#include "Core/PropertyDescriptor.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "InspectorCustomization.h"
#include "InspectorCustomizationRegistry.h"
#include "FieldGroupInspectorView.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

class QStackedWidget;
class QCheckBox;

namespace VNX {

struct ProjectItem;
class IComponent;
class ParameterBinding;
class PropertyEditSession;

class PropertyEditorEngine : public QObject
{
    Q_OBJECT

public:
    explicit PropertyEditorEngine(QObject* parent = nullptr);
    ~PropertyEditorEngine() override;

    void setSchema(const PropertySchema& schema, ComponentType componentType);

    void setItemType(ProjectItem::Type type) { itemType_ = type; }

    void setCustomization(const InspectorCustomization* custom) { custom_ = custom; }

    void setBindingDecoratorFactory(InspectorCustomizationRegistry::DecoratorFactory factory)
        { perPageBinding_ = std::move(factory); }

    QWidget* buildPage(QWidget* parent = nullptr);

    void releasePage();

    void bindItem(ProjectItem* item);

    QJsonObject buildSetItemPropertyArgs(
        const QString& field,
        const QVariant& value,
        std::optional<QVector<ProjectPlatformId>> platforms = std::nullopt) const;

    bool refreshField(const QString& field);

    // Re-resolves every descriptor that declares a FieldDomain. A field whose
    // editing domain is a function of its siblings (a Variable's default value
    // reads the declared range and kind) has no value change of its own to ride
    // on, so the domain is re-pushed on bind, on refresh and after each commit.
    void refreshDomains();

    bool playablePropertyEventRelated(ProjectItem* owner,
                                      const QString& field) const;

    ComponentType componentType() const { return componentType_; }
    ProjectItem* currentItem() const { return currentItem_; }

    static IComponent* resolveComponentTyped(ProjectItem* item, ComponentType componentType);

signals:
    void fieldChanged(const QString& field, const QVariant& oldValue, const QVariant& newValue);

    void launchRequested(const QString& slot);

    void fieldGroupChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static QString widgetTypeFor(const PropertyDescriptor& d);

    static QJsonObject synthConfig(const PropertyDescriptor& d);

    QWidget* buildField(const PropertyDescriptor& d, QWidget* parent);

    struct FieldBinding {
        QString field;
        QString fieldLabel;
        std::optional<ComponentType> ownerComponent;
        QString widgetType;
        PropType type = PropType::Float;
        QPointer<QWidget> widget;
        QPointer<QWidget> topicHost;
        QPointer<ParameterBinding> parameterBinding;
        std::unique_ptr<PropertyEditSession> editSession;
        IFieldDrawer* drawer = nullptr;
        IFieldDecorator* decorator = nullptr;
        std::vector<IFieldDecorator*> decorators;
        std::optional<BindingAddress> bindingAddress;
        QString editCondition;
        std::function<std::optional<FieldDomain>(const IComponent*)> domain;
        QString mixedTooltip;
        bool readOnly = false;
        std::optional<QJsonObject> groupEditArgs;
        quint64 groupEditRevision = 0;
        bool groupPreviewActive = false;
    };

    struct SectionBinding {
        ISectionProvider* provider = nullptr;
        QPointer<QWidget> widget;
        PropertySection section = PropertySection::None;
        QPointer<QWidget> header;
    };

    void connectWidgetSignal(QWidget* widget, const QString& widgetType, const QString& field);
    void setWidgetValue(QWidget* widget, const QString& widgetType, const QVariant& value);
    bool applyPlayableFieldView(const FieldBinding& binding);
    QWidget* innerWidget(QWidget* container, const QString& widgetType) const;

    QVariant readField(const QString& field) const;
    IComponent* resolveComponent(ProjectItem* item) const;
    std::optional<PlayablePropertyLocatorV1> editablePlayableLocator(
        const QString& field, ProjectItem** owner = nullptr) const;

    void evalEditConditions();

    struct GroupBinding {
        FieldGroupInspectorView view;
        std::vector<QPointer<QCheckBox>> inheritControls;
        QPointer<QWidget> section;
        QString error;
    };

    std::optional<FieldGroupId> fieldGroup(const QString& field) const;
    void buildGroupHeader(FieldGroupId group, class CollapsibleSection* section, bool showRangeName);
    void refreshFieldGroups();
    void refreshFieldGroupPreview(ProjectItem* provider, const DefinitionFieldGroups& groups);
    void cancelGroupEdits(bool preserveListRows = false);
    void previewGroupField(FieldBinding& binding, double value);
    void previewGroupSends(FieldBinding& binding, const QVariant& value);
    void previewBusSends(FieldBinding& binding, const QVariant& value);
    void finishGroupPreview(FieldBinding& binding);
    bool applyGroupFieldView(FieldBinding& binding);
    void setGroupFieldWidgetValue(const FieldBinding& binding, const QVariant& value);
    QVariant groupFieldValue(const QString& field, const FieldGroupInspectorView* preview = nullptr) const;
    bool groupReferenceSelectable(const QString& field, uint32_t candidate) const;
    bool canEditField(const FieldBinding& binding) const;
    bool groupContextMatches(const FieldGroupInspectorView& view, quint64 revision) const;
    void commitFieldChange(const QString& field, const QVariant& value,
                           const std::optional<QJsonObject>& groupArgs = {});
    void executeGroupAction(FieldGroupId group, const QString& action, QJsonObject args);
    void showGroupMenu(FieldGroupId group, const QPoint& globalPosition);

    static bool evalCondition(const QString& expr, const std::function<QVariant(const QString&)>& get);

    void tagFieldTopic(const FieldBinding& binding, ProjectItem* owner);
    void tagCustomFields(ProjectItem* owner);

    PropertySchema schema_;
    ComponentType componentType_ = ComponentType::_Count;
    ProjectItem::Type itemType_ = ProjectItem::Type::None;
    const InspectorCustomization* custom_ = nullptr;
    InspectorCustomizationRegistry::DecoratorFactory perPageBinding_;
    std::vector<FieldBinding> bindings_;
    std::vector<SectionBinding> sectionBindings_;
    std::map<FieldGroupId, GroupBinding> groupBindings_;
    std::map<QUuid, DefinitionFieldGroups> groupPreviews_;
    quint64 groupEditRevision_ = 0;
    ProjectItem* currentItem_ = nullptr;
    bool updating_ = false;
};

}
