
#pragma once

#include "HarkenMath.h"
#include <QString>

namespace VNX {

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

    HarkenEditor::Vec3 position() const { return position_; }
    void setPosition(const HarkenEditor::Vec3& pos) { position_ = pos; }
    void setPosition(float x, float y, float z) { position_ = HarkenEditor::Vec3(x, y, z); }

    HarkenEditor::Vec3 forward() const { return forward_; }
    void setForward(const HarkenEditor::Vec3& fwd) { forward_ = fwd.normalized(); }

    HarkenEditor::Vec3 up() const { return up_; }
    void setUp(const HarkenEditor::Vec3& u) { up_ = u.normalized(); }

private:
    SceneObjectType type_;
    QString name_;

    HarkenEditor::Vec3 position_ = HarkenEditor::Vec3(0, 0, 0);
    HarkenEditor::Vec3 forward_ = HarkenEditor::Vec3(0, 0, -1);
    HarkenEditor::Vec3 up_ = HarkenEditor::Vec3(0, 1, 0);
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
