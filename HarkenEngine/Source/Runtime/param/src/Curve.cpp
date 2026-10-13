#include "harken/param/Curve.h"

#include <algorithm>
#include <cmath>

namespace harken {

namespace {

double BezierCoordinate(double p0, double p1, double p2, double p3,
                        double t) {
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * p1 +
           3.0 * u * t * t * p2 + t * t * t * p3;
}

} // namespace

double CurveValueToCoordinate(float value, CurveDomain domain) {
    if (domain == CurveDomain::AmplitudeDb)
        return std::pow(10.0, static_cast<double>(value) / 20.0);
    if (domain == CurveDomain::MirroredAmplitudeDb) {
        const double amplitude = std::pow(10.0, -std::abs(static_cast<double>(value)) / 20.0);
        return value <= 0.0f ? amplitude : 2.0 - amplitude;
    }
    return value;
}

float CurveValueFromCoordinate(double coordinate, CurveDomain domain) {
    if (domain == CurveDomain::AmplitudeDb)
        return static_cast<float>(20.0 * std::log10(coordinate));
    if (domain == CurveDomain::MirroredAmplitudeDb)
        return static_cast<float>(coordinate <= 1.0
            ? 20.0 * std::log10(coordinate)
            : -20.0 * std::log10(2.0 - coordinate));
    return static_cast<float>(coordinate);
}

bool Curve::SetPoints(const CurvePoint* pts,
                      const CurveHandlePair* segmentHandles,
                      std::uint32_t count) {
    if (pts == nullptr || count == 0 || count > kMaxCurvePoints) {
        return false;
    }
    if (count > 1 && segmentHandles == nullptr) {
        return false;
    }

    CurvePoint newPoints[kMaxCurvePoints] = {};
    CurveHandlePair newHandles[kMaxCurvePoints - 1] = {};
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!std::isfinite(pts[i].x) || !std::isfinite(pts[i].y) ||
            (i > 0 && !(pts[i].x > pts[i - 1].x))) {
            return false;
        }
        newPoints[i] = pts[i];
    }
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        const CurveHandlePair h = segmentHandles[i];
        if (!std::isfinite(h.h1x) || !std::isfinite(h.h1y) ||
            !std::isfinite(h.h2x) || !std::isfinite(h.h2y)) {
            return false;
        }
        const float xa = pts[i].x;
        const float xb = pts[i + 1].x;
        if (h.h1x < xa || h.h1x > xb || h.h2x < xa || h.h2x > xb) {
            return false;
        }
        newHandles[i] = h;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        points[i] = newPoints[i];
    }
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        handles[i] = newHandles[i];
    }
    pointCount = count;
    return true;
}

float Curve::Eval(float x, CurveDomain domain) const {
    if (pointCount == 0) {
        return 0.0f;
    }
    if (pointCount == 1 || x <= points[0].x) {
        return points[0].y;
    }
    if (x >= points[pointCount - 1].x) {
        return points[pointCount - 1].y;
    }

    std::uint32_t seg = 0;
    while (x >= points[seg + 1].x) {
        ++seg;
    }
    const CurvePoint& a = points[seg];
    const CurvePoint& b = points[seg + 1];
    const CurveHandlePair& h = handles[seg];
    if (x == a.x) {
        return a.y;
    }
    const double target = static_cast<double>(x);
    const double xa = static_cast<double>(a.x);
    const double xb = static_cast<double>(b.x);

    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 32; ++i) {
        const double mid = (lo + hi) * 0.5;
        const double bx = BezierCoordinate(
            xa, static_cast<double>(h.h1x), static_cast<double>(h.h2x), xb,
            mid);
        if (bx < target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const double t = (lo + hi) * 0.5;
    if (domain != CurveDomain::LinearValue) {
        if (a.y == b.y && a.y == h.h1y && a.y == h.h2y)
            return a.y;
        const double minimum = std::min({a.y, h.h1y, h.h2y, b.y});
        const double maximum = std::max({a.y, h.h1y, h.h2y, b.y});
        const bool mirrored = domain == CurveDomain::MirroredAmplitudeDb;
        if (mirrored && minimum < 0.0 && maximum > 0.0) {
            const double coordinate = BezierCoordinate(
                CurveValueToCoordinate(a.y, domain), CurveValueToCoordinate(h.h1y, domain),
                CurveValueToCoordinate(h.h2y, domain), CurveValueToCoordinate(b.y, domain), t);
            if (coordinate <= 1.0)
                return static_cast<float>(20.0 * std::log10(coordinate));
            const double distance = BezierCoordinate(
                CurveValueToCoordinate(-a.y, domain), CurveValueToCoordinate(-h.h1y, domain),
                CurveValueToCoordinate(-h.h2y, domain), CurveValueToCoordinate(-b.y, domain), t);
            return static_cast<float>(-20.0 * std::log10(distance));
        }
        const double direction = mirrored && minimum >= 0.0 ? -1.0 : 1.0;
        const double offset = direction < 0.0 ? minimum : maximum;
        const auto amplitude = [offset, direction](float db) {
            return std::pow(10.0, direction * (static_cast<double>(db) - offset) / 20.0);
        };
        const double value = BezierCoordinate(amplitude(a.y), amplitude(h.h1y),
                                              amplitude(h.h2y), amplitude(b.y), t);
        return static_cast<float>(offset + direction * 20.0 * std::log10(value));
    }
    const double y = BezierCoordinate(
        static_cast<double>(a.y), static_cast<double>(h.h1y),
        static_cast<double>(h.h2y), static_cast<double>(b.y), t);
    return static_cast<float>(y);
}

void CurveLUT::Bake(const Curve& curve) {
    if (curve.pointCount == 0) {
        xMin = xMax = 0.0f;
        for (std::uint32_t i = 0; i <= kCurveLutEntries; ++i) {
            values[i] = 0.0f;
        }
        return;
    }
    xMin = curve.points[0].x;
    xMax = curve.points[curve.pointCount - 1].x;
    const double span = static_cast<double>(xMax) - static_cast<double>(xMin);
    for (std::uint32_t i = 0; i <= kCurveLutEntries; ++i) {
        const double u = static_cast<double>(i) /
                         static_cast<double>(kCurveLutEntries);
        values[i] = curve.Eval(static_cast<float>(xMin + span * u));
    }
}

float CurveLUT::Eval(float x) const {
    if (!(xMax > xMin)) {
        return values[0];
    }
    if (x <= xMin) {
        return values[0];
    }
    if (x >= xMax) {
        return values[kCurveLutEntries];
    }
    const float u = (x - xMin) / (xMax - xMin) *
                    static_cast<float>(kCurveLutEntries);
    std::uint32_t index = static_cast<std::uint32_t>(u);
    if (index >= kCurveLutEntries) {
        index = kCurveLutEntries - 1;
    }
    const float fraction = u - static_cast<float>(index);
    return values[index] +
           (values[index + 1] - values[index]) * fraction;
}

} // namespace harken
