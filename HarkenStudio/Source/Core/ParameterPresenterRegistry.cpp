#include "ParameterPresenterRegistry.h"

namespace VNX {

QString parameterPresenterIdName(ParameterPresenterId id)
{
    switch (id)
    {
        case ParameterPresenterId::SliderWithValue:
            return QStringLiteral("slider-with-value");
        case ParameterPresenterId::HorizontalSlider:
            return QStringLiteral("horizontal-slider");
        case ParameterPresenterId::VerticalFader:
            return QStringLiteral("vertical-fader");
        case ParameterPresenterId::RotaryKnob:
            return QStringLiteral("rotary-knob");
    }
    return {};
}

QString parameterPresenterContextName(ParameterPresenterContext context)
{
    switch (context)
    {
        case ParameterPresenterContext::InspectorFloat:
            return QStringLiteral("inspector.float");
        case ParameterPresenterContext::EffectParameter:
            return QStringLiteral("effect.parameter");
    }
    return {};
}

std::optional<ParameterPresenterId> parseParameterPresenterId(const QString& id)
{
    if (id == QStringLiteral("slider-with-value"))
        return ParameterPresenterId::SliderWithValue;
    if (id == QStringLiteral("horizontal-slider"))
        return ParameterPresenterId::HorizontalSlider;
    if (id == QStringLiteral("vertical-fader"))
        return ParameterPresenterId::VerticalFader;
    if (id == QStringLiteral("rotary-knob"))
        return ParameterPresenterId::RotaryKnob;
    return std::nullopt;
}

std::optional<ParameterPresenterContext> parseParameterPresenterContext(
    const QString& context)
{
    if (context == QStringLiteral("inspector.float"))
        return ParameterPresenterContext::InspectorFloat;
    if (context == QStringLiteral("effect.parameter"))
        return ParameterPresenterContext::EffectParameter;
    return std::nullopt;
}

ParameterPresenterCapabilities parameterPresenterCapabilities(
    ParameterPresenterId id)
{
    using Capability = ParameterPresenterCapability;
    switch (id)
    {
        case ParameterPresenterId::SliderWithValue:
            return Capability::Horizontal | Capability::ValueEditor
                | Capability::Label;
        case ParameterPresenterId::HorizontalSlider:
            return Capability::Horizontal | Capability::Compact;
        case ParameterPresenterId::VerticalFader:
            return Capability::Vertical | Capability::ValueEditor
                | Capability::Label;
        case ParameterPresenterId::RotaryKnob:
            return Capability::Rotary | Capability::ValueEditor
                | Capability::Label | Capability::Compact;
    }
    return Capability::None;
}

ParameterPresenterId defaultParameterPresenter(
    ParameterPresenterContext context)
{
    switch (context)
    {
        case ParameterPresenterContext::InspectorFloat:
            return ParameterPresenterId::SliderWithValue;
        case ParameterPresenterContext::EffectParameter:
            return ParameterPresenterId::RotaryKnob;
    }
    return ParameterPresenterId::SliderWithValue;
}

bool parameterPresenterSupportsContext(ParameterPresenterId id,
                                       ParameterPresenterContext context)
{
    switch (context)
    {
        case ParameterPresenterContext::InspectorFloat:
            return id == ParameterPresenterId::SliderWithValue
                || id == ParameterPresenterId::HorizontalSlider
                || id == ParameterPresenterId::VerticalFader
                || id == ParameterPresenterId::RotaryKnob;
        case ParameterPresenterContext::EffectParameter:
            return id == ParameterPresenterId::RotaryKnob
                || id == ParameterPresenterId::SliderWithValue;
    }
    return false;
}

}
