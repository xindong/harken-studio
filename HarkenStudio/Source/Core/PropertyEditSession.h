
#pragma once

#include "PlayablePropertyAuthoringV1.h"

#include <QPair>
#include <QString>
#include <QVariant>
#include <QVector>

#include <optional>

class QObject;

namespace VNX {

struct ProjectItem;

struct PlayableStartSnapshot {
    PlayablePropertyStateV1 base;
    QVector<QPair<ProjectPlatformId, PlayablePropertyStateV1>> overlays;
    bool writesSharedBase = true;
};

class PropertyEditSession
{
public:
    PropertyEditSession(ProjectItem* item, QString propertyName, QVariant startValue,
                        const QObject* originator = nullptr,
                        std::optional<QVector<ProjectPlatformId>> platforms = std::nullopt);
    PropertyEditSession(ProjectItem* owner, PlayablePropertyLocatorV1 locator,
                        uint8_t propertyId, const QObject* originator = nullptr,
                        std::optional<QVector<ProjectPlatformId>> platforms = std::nullopt);
    ~PropertyEditSession();

    PropertyEditSession(const PropertyEditSession&) = delete;
    PropertyEditSession& operator=(const PropertyEditSession&) = delete;

    void preview(const QVariant& newValue);
    void commit();
    void cancel();

    static void finishAllActive(bool commitChanges);
    static void cancelActiveFor(ProjectItem* owner,
                                const PlayablePropertyLocatorV1& locator,
                                uint8_t propertyId);
    static void cancelActiveFor(ProjectItem* owner, const QString& field);

    bool isActive() const { return state_ == State::Active; }
    const QVariant& startValue() const { return startValue_; }
    const QVariant& currentValue() const { return currentValue_; }

private:
    bool isBusEdit() const;
    bool applyValue(const QVariant& value, bool live);
    QJsonObject playableActionArgs(const QString& operation,
                                   const QJsonValue& value) const;
    void capturePlayableStart();
    void restorePlayableStart();
    bool previewPlayable(const QJsonValue& json);
    bool playableStateChanged() const;
    void captureFilterStart();
    void restoreFilterStart();
    bool previewFilter(const QJsonValue& value);
    bool filterStateChanged() const;

    ProjectItem* item_;
    QString propertyName_;
    QVariant startValue_;
    QVariant currentValue_;
    const QObject* originator_ = nullptr;
    std::optional<PlayablePropertyLocatorV1> playableLocator_;
    std::optional<PlayableStartSnapshot> playableStartState_;
    std::optional<PlayableStartSnapshot> filterStartState_;
    std::optional<QVector<ProjectPlatformId>> platforms_;
    uint8_t playablePropertyId_ = 0;
    enum class State { Active, Committed, Cancelled } state_ = State::Active;
};

}
