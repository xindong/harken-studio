#pragma once

#include <QUuid>
#include <QWidget>

#include <array>
#include <cstdint>

class QLabel;

namespace VNX {

class SourceFilePropertiesView final : public QWidget
{
    Q_OBJECT

public:
    explicit SourceFilePropertiesView(QWidget* parent = nullptr);
    void setSource(const QUuid& soundGuid, const QUuid& entryId, uint32_t sourceId);

private:
    enum Property { RelativePath, AbsolutePath, Channels, SampleRate, Duration, Size, Format, PropertyCount };
    void refresh();

    QUuid soundGuid_;
    QUuid entryId_;
    uint32_t sourceId_ = 0;
    std::array<QLabel*, PropertyCount> values_{};
};

}
