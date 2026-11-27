#pragma once

#include "Core/ExternalEditorSelection.h"

#include <QVariantMap>
#include <QWidget>

class QScrollArea;
class QSplitter;

namespace harken::studio {

class BindingFieldEditor;
class CurveEditorWidget;
class SourceFilePropertiesView;
class SourceRangeEditor;
class FilterEditorWidget;

class ExternalEditorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit ExternalEditorPanel(QWidget* parent = nullptr);

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);
    ProjectItem* boundItem() const;
    const ExternalEditorSelection& selection() const { return selection_; }
    bool acceptsSelection(const ProjectItem* item) const;
    void bindItem(ProjectItem* item);
    void bindSelection(const ExternalEditorSelection& selection);
    void bindSelection(const CurveSelection& selection);
    void clearEditor();
    QStringList sourceTitlePath() const;
    void showBindingAddMenu();
    QVariantMap snapshotState() const;
    void restoreState(const QVariantMap& state);

signals:
    void panelFocused(int panelId);
    void pinToggled(bool pinned);
    void titleChanged();
    void navigateRequested(const QUuid& guid);
    void externalEditorRequested(const ExternalEditorSelection& selection);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    void ensureCurveEditor();
    void ensureBindingEditor();
    void ensureSourceEditor();
    void ensureFilterEditor();
    void installFocusFilterRecursively(QObject* root);
    void showContent(QWidget* content);
    void refreshSource();
    void refreshAttenuationReference();
    void commitSource(const SoundSourceDef& replacement);
    const SoundSourceDef* currentSource() const;

    int panelId_ = 0;
    bool pinned_ = false;
    ExternalEditorSelection selection_;
    QScrollArea* curvesScroll_ = nullptr;
    BindingFieldEditor* bindings_ = nullptr;
    CurveEditorWidget* curve_ = nullptr;
    QSplitter* sourceView_ = nullptr;
    SourceRangeEditor* source_ = nullptr;
    SourceFilePropertiesView* sourceProperties_ = nullptr;
    FilterEditorWidget* filter_ = nullptr;
};

}
