
#pragma once

#include "HarkenMath.h"
#include <QString>

namespace harken::studio {

// Lightweight spatial-object data model consumed by the Inspector pages
// (emitter/listener property editing). Extracted from the retired 3D
// previewer widget tree; deliberately carries no rendering state.
enum class SceneObjectType
{
    Listener,
    Emitter
};

class SceneObject
{
public:
    SceneObject(SceneObjectType type, const QString& name = "Object")
        : type_(type), name_(name) {}
    virtual ~SceneObject() = default;

    SceneObjectType type() const { return type_; }
    const QString& name() const { return name_; }
    void setName(const QString& name) { name_ = name; }

    harken::studio::Vec3 position() const { return position_; }
    void setPosition(const harken::studio::Vec3& pos) { position_ = pos; }
    void setPosition(float x, float y, float z) { position_ = harken::studio::Vec3(x, y, z); }

    harken::studio::Vec3 forward() const { return forward_; }
    void setForward(const harken::studio::Vec3& fwd) { forward_ = fwd.normalized(); }

    harken::studio::Vec3 up() const { return up_; }
    void setUp(const harken::studio::Vec3& u) { up_ = u.normalized(); }

private:
    SceneObjectType type_;
    QString name_;

    harken::studio::Vec3 position_ = harken::studio::Vec3(0, 0, 0);
    harken::studio::Vec3 forward_ = harken::studio::Vec3(0, 0, -1);
    harken::studio::Vec3 up_ = harken::studio::Vec3(0, 1, 0);
};

class ListenerObject : public SceneObject
{
public:
    ListenerObject() : SceneObject(SceneObjectType::Listener, "Listener") {}
};

class EmitterObject : public SceneObject
{
public:
    EmitterObject(const QString& name = "Emitter")
        : SceneObject(SceneObjectType::Emitter, name) {}

    float minDistance() const { return minDistance_; }
    void setMinDistance(float d) { minDistance_ = d; }

    float maxDistance() const { return maxDistance_; }
    void setMaxDistance(float d) { maxDistance_ = d; }

private:
    float minDistance_ = 1.0f;
    float maxDistance_ = 50.0f;
};

}
