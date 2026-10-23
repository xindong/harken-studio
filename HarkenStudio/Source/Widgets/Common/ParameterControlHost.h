#pragma once

#include "Core/ParameterPresenterRegistry.h"

#include <QString>
#include <QWidget>

#include <optional>

namespace VNX {

class ParameterBinding;
class ParameterPresenter;
class ThemeManager;

class ParameterControlHost : public QWidget
{
    Q_OBJECT

public:
    explicit ParameterControlHost(
        const QString& name,
        ParameterPresenterContext context =
            ParameterPresenterContext::InspectorFloat,
        QWidget* parent = nullptr,
        ThemeManager* themeManager = nullptr);
    ~ParameterControlHost() override;

    ParameterBinding* binding() const { return binding_; }
    ParameterPresenter* presenter() const { return presenter_; }
    ParameterPresenterContext presenterContext() const { return context_; }
    ParameterPresenterId presenterId() const;

    void setPresenterContext(ParameterPresenterContext context);
    void setPresenterOverride(std::optional<ParameterPresenterId> presenter);
    void setCompactValueColumn(int width);
    void setValueColumnStretch(int primaryStretch, int valueStretch);
    void rebuildPresenter();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    virtual void presenterInstalled() {}

private:
    enum class FocusRole { None, PrimaryEditor, ValueEditor };

    ParameterPresenterId selectedPresenterId() const;
    FocusRole currentFocusRole() const;
    void installPresenter(FocusRole focusRole);
    void syncToolTips();

    ParameterBinding* binding_;
    ThemeManager* themeManager_;
    ParameterPresenter* presenter_ = nullptr;
    ParameterPresenterContext context_;
    std::optional<ParameterPresenterId> presenterOverride_;
    int compactValueColumnWidth_ = 0;
    int primaryColumnStretch_ = 0;
    int valueColumnStretch_ = 0;
    bool resetClick_ = false;
};

}
