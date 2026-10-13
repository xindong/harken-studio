
#pragma once

#include <QVector3D>
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

    QVector3D position() const { return position_; }
    void setPosition(const QVector3D& pos) { position_ = pos; }
    void setPosition(float x, float y, float z) { position_ = QVector3D(x, y, z); }

    QVector3D forward() const { return forward_; }
    void setForward(const QVector3D& fwd) { forward_ = fwd.normalized(); }

    QVector3D up() const { return up_; }
    void setUp(const QVector3D& u) { up_ = u.normalized(); }

private:
    SceneObjectType type_;
    QString name_;

    QVector3D position_ = QVector3D(0, 0, 0);
    QVector3D forward_ = QVector3D(0, 0, -1);
    QVector3D up_ = QVector3D(0, 1, 0);
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
