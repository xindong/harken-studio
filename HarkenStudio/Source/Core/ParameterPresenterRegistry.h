#pragma once

#include <QFlags>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace VNX {

enum class ParameterPresenterId
{
    SliderWithValue,
    HorizontalSlider,
    VerticalFader
};

enum class ParameterPresenterContext
{
    InspectorFloat,
    EffectParameter
};

enum class ParameterPresenterCapability : unsigned int
{
    None = 0,
    Horizontal = 1U << 0U,
    Vertical = 1U << 1U,
    ValueEditor = 1U << 3U,
    Label = 1U << 4U,
    Compact = 1U << 5U
};
Q_DECLARE_FLAGS(ParameterPresenterCapabilities, ParameterPresenterCapability)

QString parameterPresenterIdName(ParameterPresenterId id);
QString parameterPresenterContextName(ParameterPresenterContext context);
std::optional<ParameterPresenterId> parseParameterPresenterId(const QString& id);
std::optional<ParameterPresenterContext> parseParameterPresenterContext(
    const QString& context);
ParameterPresenterCapabilities parameterPresenterCapabilities(
    ParameterPresenterId id);
ParameterPresenterId defaultParameterPresenter(
    ParameterPresenterContext context);
bool parameterPresenterSupportsContext(ParameterPresenterId id,
                                       ParameterPresenterContext context);

}

Q_DECLARE_OPERATORS_FOR_FLAGS(VNX::ParameterPresenterCapabilities)
