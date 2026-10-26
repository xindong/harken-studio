#pragma once

#include "Core/ThemeSnapshot.h"

#include <QColor>
#include <QIcon>
#include <QIconEngine>

#include <memory>

class QPainter;

namespace VNX {

class ThemeAssetResolver;
struct ThemeImageResource;

class SvgIconEngine : public QIconEngine
{
public:
    explicit SvgIconEngine(ThemeAssetResolver* resolver,
                           ThemeSnapshotPtr snapshot,
                           QString onLogicalId,
                           QString offLogicalId,
                           QColor overrideColor,
                           std::shared_ptr<const ThemeImageResource> onResource,
                           std::shared_ptr<const ThemeImageResource> offResource = {});

    QIconEngine* clone() const override;
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override;
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override;
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override;
    QSize actualSize(const QSize& size, QIcon::Mode mode,
                     QIcon::State state) override;
    QString key() const override;

private:
    QColor resolveColor(QIcon::Mode mode) const;

    ThemeAssetResolver* resolver_ = nullptr;
    ThemeSnapshotPtr snapshot_;
    QString onLogicalId_;
    QString offLogicalId_;
    QColor overrideColor_;
    std::shared_ptr<const ThemeImageResource> onResource_;
    std::shared_ptr<const ThemeImageResource> offResource_;
};

}
