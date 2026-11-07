
#include "PropertyEditSession.h"
#include "ProjectManager.h"
#include "ActionManager.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "FilterAuthoring.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <vector>

namespace VNX {

namespace {

using harkenplayableproperties::PlayablePropertyIdV1;

std::vector<PropertyEditSession*>& sessionRegistry()
{
    static std::vector<PropertyEditSession*> registry;
    return registry;
}

bool isBusOutputVolumeField(const ProjectItem* item,
                            const PlayablePropertyAuthoringV1::Field* field)
{
    return item && item->type == ProjectItem::Type::Bus && field
        && field->id == static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume);
}

PlayablePropertyStateV1 overlaySlotState(
    ProjectItem& owner,
    const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId,
    ProjectPlatformId platform)
{
    return PlayablePropertyAuthoringV1::readPlatformOverlay(
               owner, locator, propertyId, platform)
        .value_or(PlayablePropertyStateV1{false, {}});
}

} // namespace

PropertyEditSession::PropertyEditSession(ProjectItem* item, QString propertyName, QVariant startValue,
                                         const QObject* originator,
                                         std::optional<QVector<ProjectPlatformId>> platforms)
    : item_(item)
    , propertyName_(std::move(propertyName))
    , startValue_(std::move(startValue))
    , originator_(originator)
    , platforms_(std::move(platforms))
{
    currentValue_ = startValue_;
    if (item_ && FilterAuthoring::isField(propertyName_)) captureFilterStart();
    if (item_) {
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName_);
        if (isBusOutputVolumeField(item_, field)) {
            propertyName_ = QString::fromLatin1(field->fieldName);
        } else {
            const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item_);
            if (field && locator) {
                playableLocator_ = *locator;
                playablePropertyId_ = field->id;
                capturePlayableStart();
                if (playableStartState_)
                    propertyName_ = QString::fromLatin1(field->fieldName);
                else {
                    playableLocator_.reset();
                    playablePropertyId_ = 0;
                }
            }
        }
    }
    sessionRegistry().push_back(this);
}

PropertyEditSession::PropertyEditSession(ProjectItem* owner,
                                         PlayablePropertyLocatorV1 locator,
                                         uint8_t propertyId,
                                         const QObject* originator,
                                         std::optional<QVector<ProjectPlatformId>> platforms)
    : item_(owner)
    , originator_(originator)
    , playableLocator_(std::move(locator))
    , platforms_(std::move(platforms))
    , playablePropertyId_(propertyId)
{
    if (playableLocator_->kind != PlayablePropertyTargetKindV1::Definition) {
        state_ = State::Cancelled;
        return;
    }
    const auto* field = PlayablePropertyAuthoringV1::fieldById(propertyId);
    if (field) propertyName_ = QString::fromLatin1(field->fieldName);
    if (isBusOutputVolumeField(item_, field)) {
        playableLocator_.reset();
        playablePropertyId_ = 0;
        if (const auto* mix = item_->comp<VMixNode>()) {
            startValue_ = mix->outputBusVolume;
            currentValue_ = startValue_;
        }
    } else if (item_ && field) {
        capturePlayableStart();
    }
    sessionRegistry().push_back(this);
}

PropertyEditSession::~PropertyEditSession()
{
    if (state_ == State::Active)
        cancel();
    auto& registry = sessionRegistry();
    registry.erase(std::remove(registry.begin(), registry.end(), this), registry.end());
}

void PropertyEditSession::finishAllActive(bool commitChanges)
{
    emit ProjectManager::instance().activePropertyEditsFinishing(commitChanges);
    const std::vector<PropertyEditSession*> snapshot = sessionRegistry();
    for (PropertyEditSession* session : snapshot) {
        auto& registry = sessionRegistry();
        if (std::find(registry.begin(), registry.end(), session) == registry.end())
            continue;
        if (!session->isActive())
            continue;
        if (commitChanges) session->commit();
        else               session->cancel();
    }
}

void PropertyEditSession::cancelActiveFor(
    ProjectItem* owner, const PlayablePropertyLocatorV1& locator,
    uint8_t propertyId)
{
    const std::vector<PropertyEditSession*> snapshot = sessionRegistry();
    for (PropertyEditSession* session : snapshot) {
        if (!session || !session->isActive() || session->item_ != owner
            || session->playablePropertyId_ != propertyId
            || !session->playableLocator_
            || *session->playableLocator_ != locator)
            continue;
        session->cancel();
    }
}

void PropertyEditSession::cancelActiveFor(ProjectItem* owner, const QString& field)
{
    const auto sessions = sessionRegistry();
    for (auto* session : sessions) {
        if (session->isActive() && session->item_ == owner && session->filterStartState_
            && (session->propertyName_ == field || field == QLatin1String("filter")
                || session->propertyName_ == QLatin1String("filter"))) session->cancel();
    }
}

void PropertyEditSession::captureFilterStart()
{
    const auto base = FilterAuthoring::readProperty(*item_, propertyName_);
    if (!base) return;
    PlayableStartSnapshot snapshot;
    snapshot.base = *base;
    snapshot.writesSharedBase = !platforms_;
    const auto targets = platforms_.value_or(occupiedTargetPlatforms(
        ProjectManager::instance().getTargetPlatforms()));
    for (const auto& platform : targets) {
        if (platform == ProjectPlatformId{}) continue;
        const auto state = FilterAuthoring::readProperty(*item_, propertyName_, platform, true);
        if (state) snapshot.overlays.append({platform, *state});
    }
    filterStartState_ = std::move(snapshot);
}

void PropertyEditSession::restoreFilterStart()
{
    for (const auto& [platform, state] : filterStartState_->overlays)
        FilterAuthoring::restoreProperty(*item_, propertyName_, state, platform);
    if (filterStartState_->writesSharedBase)
        FilterAuthoring::restoreProperty(*item_, propertyName_, filterStartState_->base, std::nullopt);
}

bool PropertyEditSession::previewFilter(const QJsonValue& value)
{
    if (filterStartState_->writesSharedBase) {
        if (!FilterAuthoring::setOverride(*item_, propertyName_, value, std::nullopt)) return false;
        for (const auto& [platform, state] : filterStartState_->overlays)
            FilterAuthoring::clearOverride(*item_, propertyName_, platform);
        return true;
    }
    for (const auto& [platform, state] : filterStartState_->overlays) {
        if (!FilterAuthoring::setOverride(*item_, propertyName_, value, platform)) return false;
    }
    return true;
}

bool PropertyEditSession::filterStateChanged() const
{
    if (filterStartState_->writesSharedBase
        && FilterAuthoring::readProperty(*item_, propertyName_) != filterStartState_->base) return true;
    for (const auto& [platform, state] : filterStartState_->overlays)
        if (FilterAuthoring::readProperty(*item_, propertyName_, platform, true) != state) return true;
    return false;
}

bool PropertyEditSession::isBusEdit() const
{
    if (!item_ || item_->comp<VBus>() == nullptr)
        return false;
    if (propertyName_ == QStringLiteral("pan"))
        return true;
    return isBusOutputVolumeField(
        item_, PlayablePropertyAuthoringV1::fieldByName(propertyName_));
}

bool PropertyEditSession::applyValue(const QVariant& value, bool live)
{
    if (item_->type == ProjectItem::Type::Variable
        && propertyName_ == QLatin1String("defaultValue")) {
        auto* variable = item_->comp<VVariable>();
        return variable && variable->setField(propertyName_, value);
    }
    auto& pm = ProjectManager::instance();
    if (live)
        return isBusEdit() ? pm.applyBusPropertyLive(item_, propertyName_, value)
                           : pm.applyItemPropertyLive(item_, propertyName_, value);
    return isBusEdit() ? pm.writeBusPropertySilent(item_, propertyName_, value)
                       : pm.writeAudioPropertySilent(item_, propertyName_, value);
}

void PropertyEditSession::capturePlayableStart()
{
    if (!item_ || !playableLocator_)
        return;
    const bool sharedComponent = PlayablePropertyAuthoringV1::isComponentStored(playablePropertyId_);
    if (sharedComponent) platforms_.reset();
    const auto base = PlayablePropertyAuthoringV1::readProperty(
        *item_, *playableLocator_, playablePropertyId_, std::nullopt);
    if (!base)
        return;
    PlayableStartSnapshot snap;
    snap.base = *base;
    snap.writesSharedBase = !platforms_;
    const auto targets = sharedComponent ? QVector<ProjectPlatformId>{}
        : platforms_.value_or(occupiedTargetPlatforms(
            ProjectManager::instance().getTargetPlatforms()));
    {
        for (ProjectPlatformId platform : targets) {
            if (platform == ProjectPlatformId{})
                continue;
            snap.overlays.append(
                {platform, overlaySlotState(
                    *item_, *playableLocator_, playablePropertyId_, platform)});
        }
    }
    playableStartState_ = std::move(snap);
    startValue_ = playableStartState_->base.value.toVariant();
    currentValue_ = startValue_;
}

void PropertyEditSession::restorePlayableStart()
{
    if (!item_ || !playableLocator_ || !playableStartState_)
        return;
    {
        for (auto it = playableStartState_->overlays.rbegin();
             it != playableStartState_->overlays.rend(); ++it) {
            PlayablePropertyAuthoringV1::restoreProperty(
                *item_, *playableLocator_, playablePropertyId_, it->second,
                it->first);
        }
        if (!playableStartState_->writesSharedBase)
            return;
    }
    PlayablePropertyAuthoringV1::restoreProperty(
        *item_, *playableLocator_, playablePropertyId_, playableStartState_->base,
        std::nullopt);
}

bool PropertyEditSession::previewPlayable(const QJsonValue& json)
{
    if (!item_ || !playableLocator_)
        return false;
    if (playableStartState_->writesSharedBase) {
        if (!PlayablePropertyAuthoringV1::setOverride(
                *item_, *playableLocator_, playablePropertyId_, json, std::nullopt))
            return false;
        for (const auto& [platform, state] : playableStartState_->overlays) {
            if (!PlayablePropertyAuthoringV1::clearOverride(
                    *item_, *playableLocator_, playablePropertyId_, platform))
                return false;
        }
        return true;
    }
    for (ProjectPlatformId platform : *platforms_) {
        if (platform == ProjectPlatformId{})
            continue;
        if (!PlayablePropertyAuthoringV1::setOverride(
                *item_, *playableLocator_, playablePropertyId_, json, platform))
            return false;
    }
    return true;
}

bool PropertyEditSession::playableStateChanged() const
{
    if (!item_ || !playableLocator_ || !playableStartState_)
        return false;
    const auto base = PlayablePropertyAuthoringV1::readProperty(
        *item_, *playableLocator_, playablePropertyId_, std::nullopt);
    if (!base || *base != playableStartState_->base)
        return true;
    for (const auto& overlay : playableStartState_->overlays) {
        if (overlaySlotState(
                *item_, *playableLocator_, playablePropertyId_, overlay.first)
            != overlay.second)
            return true;
    }
    return false;
}

void PropertyEditSession::preview(const QVariant& newValue)
{
    if (state_ != State::Active || !item_) return;
    if (filterStartState_) {
        if (previewFilter(QJsonValue::fromVariant(newValue))) {
            currentValue_ = newValue;
            emit ProjectManager::instance().itemPropertyPreviewed(item_, propertyName_, originator_);
        }
        return;
    }
    if (playableLocator_ && playableStartState_) {
        const auto json = PlayablePropertyAuthoringV1::wireValueFromVariant(
            playablePropertyId_, newValue);
        if (json && previewPlayable(*json)) {
            currentValue_ = newValue;
            if (item_->type == ProjectItem::Type::Bus) {
                auto& pm = ProjectManager::instance();
                if (propertyName_ == QStringLiteral("sends"))
                    pm.applyBusRuntimeState(item_);
                else
                    pm.pushBusPropertyLive(item_, propertyName_);
            }
            emit ProjectManager::instance().itemPropertyPreviewed(item_, propertyName_, originator_);
        }
        return;
    }
    auto& pm = ProjectManager::instance();
    if (applyValue(newValue, true)) {
        currentValue_ = newValue;
        emit pm.itemPropertyPreviewed(item_, propertyName_, originator_);
    }
}

void PropertyEditSession::commit()
{
    if (state_ != State::Active) return;
    state_ = State::Committed;
    if (!item_) return;

    if (filterStartState_) {
        if (!filterStateChanged()) return;
        restoreFilterStart();
        ActionManager::instance().execute(QStringLiteral("set_item_property"),
            playableActionArgs(QStringLiteral("set_override"), QJsonValue::fromVariant(currentValue_)),
            ActionSource::Internal, originator_);
        return;
    }

    if (playableLocator_ && playableStartState_) {
        if (!playableStateChanged()) return;
        const auto json = PlayablePropertyAuthoringV1::wireValueFromVariant(
            playablePropertyId_, currentValue_);
        if (!json) return;
        restorePlayableStart();
        ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            playableActionArgs(QStringLiteral("set_override"), *json),
            ActionSource::Internal, originator_);
        return;
    }
    if (currentValue_ == startValue_) return;

    applyValue(startValue_, false);
    if (isBusEdit()) {
        ActionManager::instance().execute(QStringLiteral("set_bus_properties"),
            QJsonObject{
                {QStringLiteral("bus_id"), static_cast<qint64>(item_->objectID)},
                {propertyName_, QJsonValue::fromVariant(currentValue_)}},
            ActionSource::Internal, originator_);
    } else {
        ActionManager::instance().execute(QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"), static_cast<qint64>(item_->objectID)},
                {QStringLiteral("property"), propertyName_},
                {QStringLiteral("value"), QJsonValue::fromVariant(currentValue_)}},
            ActionSource::Internal, originator_);
    }
}

void PropertyEditSession::cancel()
{
    if (state_ != State::Active) return;
    state_ = State::Cancelled;
    if (!item_) return;
    if (filterStartState_) {
        if (!filterStateChanged()) return;
        restoreFilterStart();
        currentValue_ = startValue_;
        emit ProjectManager::instance().itemPropertyPreviewed(item_, propertyName_, originator_);
        return;
    }
    if (playableLocator_ && playableStartState_) {
        restorePlayableStart();
        currentValue_ = startValue_;
        if (item_->type == ProjectItem::Type::Bus) {
            auto& pm = ProjectManager::instance();
            if (propertyName_ == QStringLiteral("sends"))
                pm.applyBusRuntimeState(item_);
            else
                pm.pushBusPropertyLive(item_, propertyName_);
        }
        emit ProjectManager::instance().itemPropertyPreviewed(item_, propertyName_, originator_);
        return;
    }
    if (currentValue_ == startValue_) return;

    auto& pm = ProjectManager::instance();
    applyValue(startValue_, true);
    currentValue_ = startValue_;
    emit pm.itemPropertyPreviewed(item_, propertyName_, originator_);
}

QJsonObject PropertyEditSession::playableActionArgs(
    const QString& operation, const QJsonValue& value) const
{
    QJsonObject args{
        {QStringLiteral("authoring_handle"),
         item_ ? static_cast<qint64>(item_->objectID) : 0},
        {QStringLiteral("property"), propertyName_},
        {QStringLiteral("override_operation"), operation},
        {QStringLiteral("value"), value}};
    if (platforms_) {
        QJsonArray names;
        for (ProjectPlatformId platform : *platforms_)
            names.append(ProjectPlatformSemantics::idString(platform));
        args[QStringLiteral("platform_ids")] = names;
    }
    if (!playableLocator_ || playableLocator_->kind
            == PlayablePropertyTargetKindV1::Definition)
        return args;
    args[QStringLiteral("occurrence_kind")] = playableLocator_->kind
            == PlayablePropertyTargetKindV1::TrackClip
        ? QStringLiteral("track_clip") : QStringLiteral("container_child");
    args[QStringLiteral("occurrence_guid")] =
        playableLocator_->occurrenceGuid.toString(QUuid::WithoutBraces);
    args[QStringLiteral("occurrence_target_guid")] =
        playableLocator_->targetGuid.toString(QUuid::WithoutBraces);
    args[QStringLiteral("occurrence_ordinal")] = static_cast<qint64>(
        playableLocator_->stableOccurrenceOrdinal);
    return args;
}

}
