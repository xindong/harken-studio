#include "StartupRenderer.h"

#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLPaintDevice>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QThread>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QDebug>

#include <array>
#include <cmath>

namespace {

constexpr qreal kFieldSeed = 7.381;
constexpr std::array<std::array<int, 4>, 4> kControlIndices{{
    {{1, -2, 3, -4}},
    {{9, -8, 7, -6}},
    {{5, 2, 5, -5}},
    {{-1, -3, 8, 9}},
}};

const char* kVertexShader = R"glsl(
#version 330 core
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);
void main()
{
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 330 core
uniform vec2 iResolution;
uniform float iTime;
uniform float uMorphAmount;
uniform float uBands;
uniform float uThickness;
uniform float uScale;
uniform float uGlow;
uniform float uContrast;
uniform float uBrightness;
uniform float uGrainIntensity;
uniform vec3 uLow;
uniform vec3 uLowMid;
uniform vec3 uMid;
uniform vec3 uHighMid;
uniform vec3 uHigh;
uniform vec4 uCtrlA;
uniform vec4 uCtrlB;
uniform vec4 uCtrlC;
uniform vec4 uCtrlD;
out vec4 fragColor;

float bez(float t, vec4 c)
{
    float w = 6.2831853 * t;
    return 0.5 * (c.x * sin(w) + c.y * cos(w) + c.z * sin(2.0 * w) + c.w * cos(2.0 * w));
}

float field(vec2 uv)
{
    vec2 a = vec2(bez(uv.x, uCtrlA), bez(uv.x, uCtrlB));
    vec2 b = vec2(bez(uv.y, uCtrlC), bez(uv.y, uCtrlD));
    return distance(a, b);
}

vec3 elevationColor(float elevation)
{
    vec3 color = mix(uLow, uLowMid, smoothstep(0.0, 0.28, elevation));
    color = mix(color, uMid, smoothstep(0.22, 0.52, elevation));
    color = mix(color, uHighMid, smoothstep(0.46, 0.76, elevation));
    return mix(color, uHigh, smoothstep(0.70, 1.0, elevation));
}

void main()
{
    vec2 uv = gl_FragCoord.xy / iResolution;
    vec2 sampleUv = (uv - 0.5) / max(uScale, 0.001) + 0.5;
    float fieldValue = field(sampleUv);
    float bandValue = fieldValue * uBands;
    float fraction = fract(bandValue);
    float lineDistance = min(fraction, 1.0 - fraction);
    float aa = fwidth(bandValue) + 0.0001;
    float mask = 1.0 - smoothstep(uThickness - aa, uThickness + aa, lineDistance);
    float glowRadius = uThickness + uGlow * 0.5 + aa;
    float glow = 1.0 - smoothstep(uThickness, glowRadius, lineDistance);

    float elevation = clamp(fieldValue / (uMorphAmount * 2.5 + 0.001), 0.0, 1.0);
    vec3 lineColor = elevationColor(elevation);
    float coverage = pow(clamp(mask + glow * 0.55, 0.0, 1.0), max(uContrast, 0.001));

    float grain = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)) + iTime) * 43758.5453);
    coverage += (grain - 0.5) * uGrainIntensity;
    coverage = clamp(coverage, 0.0, 1.0);
    lineColor = clamp(lineColor * uBrightness, 0.0, 1.0);

    vec3 background = mix(vec3(0.051, 0.067, 0.102), vec3(0.031, 0.039, 0.059), uv.y);
    vec3 color = background + lineColor * coverage * 0.82;
    fragColor = vec4(color, 1.0);
}
)glsl";

void drawTitle(QOpenGLPaintDevice& device, qreal time)
{
    QPainter painter(&device);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF titleBox((device.width() / device.devicePixelRatioF() - 316.0) * 0.5,
                          (device.height() / device.devicePixelRatioF() - 88.0) * 0.5,
                          316.0, 88.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(16, 21, 31));
    painter.drawRect(titleBox);

    QFont titleFont;
    titleFont.setPixelSize(34);
    titleFont.setWeight(QFont::DemiBold);
    const QString titleText = QStringLiteral("Harken Studio");
    const QFontMetricsF metrics(titleFont);
    const QRectF textBounds = metrics.boundingRect(titleText);
    const QSize maskSize(qCeil(textBounds.width()) + 8, qCeil(textBounds.height()) + 8);
    QImage textMask(maskSize, QImage::Format_ARGB32_Premultiplied);
    textMask.fill(Qt::transparent);
    {
        QPainter maskPainter(&textMask);
        maskPainter.setRenderHint(QPainter::TextAntialiasing);
        maskPainter.setFont(titleFont);
        maskPainter.setPen(Qt::white);
        maskPainter.drawText(QPointF(4.0 - textBounds.left(), 4.0 - textBounds.top()), titleText);
    }

    const qreal shineProgress = std::fmod(time, 4.0) / 4.0;
    const qreal shineCenter = maskSize.width() * (-0.5 + shineProgress * 2.0);
    QLinearGradient shineGradient(QPointF(shineCenter - 145.0, maskSize.height()),
                                  QPointF(shineCenter + 145.0, 0.0));
    shineGradient.setColorAt(0.0, QColor(181, 181, 181));
    shineGradient.setColorAt(0.22, QColor(181, 181, 181));
    shineGradient.setColorAt(0.38, QColor(211, 211, 213));
    shineGradient.setColorAt(0.5, QColor(255, 255, 255));
    shineGradient.setColorAt(0.62, QColor(211, 211, 213));
    shineGradient.setColorAt(0.78, QColor(181, 181, 181));
    shineGradient.setColorAt(1.0, QColor(181, 181, 181));

    QImage shinyText(maskSize, QImage::Format_ARGB32_Premultiplied);
    shinyText.fill(Qt::transparent);
    {
        QPainter textPainter(&shinyText);
        textPainter.fillRect(shinyText.rect(), shineGradient);
        textPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        textPainter.drawImage(QPointF(0.0, 0.0), textMask);
    }
    painter.drawImage(QPointF(titleBox.center().x() - maskSize.width() * 0.5,
                              titleBox.center().y() - maskSize.height() * 0.5), shinyText);
}

}

namespace VNX {

StartupRenderer::StartupRenderer()
{
    format_.setRenderableType(QSurfaceFormat::OpenGL);
    format_.setVersion(3, 3);
    format_.setProfile(QSurfaceFormat::CoreProfile);
    format_.setDepthBufferSize(0);
    format_.setStencilBufferSize(0);
    format_.setSamples(0);

    window_ = new QWindow;
    window_->setSurfaceType(QSurface::OpenGLSurface);
    window_->setFormat(format_);
    window_->setFlags(Qt::FramelessWindowHint);
    window_->setTitle(QStringLiteral("Harken Studio"));
}

StartupRenderer::~StartupRenderer()
{
    stop();
}

void StartupRenderer::start()
{
    if (thread_)
        return;
    running_.store(true, std::memory_order_release);
    thread_ = QThread::create([this]() { renderLoop(); });
    thread_->start();
}

void StartupRenderer::stop()
{
    running_.store(false, std::memory_order_release);
    if (thread_) {
        thread_->wait();
        delete thread_;
        thread_ = nullptr;
    }
    threadId_.store(0, std::memory_order_release);
}

void StartupRenderer::setViewport(const QSize& size, qreal devicePixelRatio)
{
    const QSize bounded = size.expandedTo(QSize(1, 1));
    pixelWidth_.store(qMax(1, qRound(bounded.width() * devicePixelRatio)),
                      std::memory_order_release);
    pixelHeight_.store(qMax(1, qRound(bounded.height() * devicePixelRatio)),
                       std::memory_order_release);
    devicePixelRatio_.store(qMax<qreal>(1.0, devicePixelRatio),
                            std::memory_order_release);
}

void StartupRenderer::setExposed(bool exposed)
{
    exposed_.store(exposed, std::memory_order_release);
}

void StartupRenderer::renderLoop()
{
    threadId_.store(reinterpret_cast<quintptr>(QThread::currentThreadId()),
                    std::memory_order_release);

    QOpenGLContext context;
    context.setFormat(format_);
    if (!context.create() || !context.makeCurrent(window_)) {
        qWarning() << "Startup renderer could not create an OpenGL context";
        running_.store(false, std::memory_order_release);
        threadId_.store(0, std::memory_order_release);
        emit initializationFailed();
        return;
    }

    QOpenGLFunctions* functions = context.functions();
    functions->initializeOpenGLFunctions();
    functions->glDisable(GL_DEPTH_TEST);
    functions->glDisable(GL_BLEND);

    QOpenGLShaderProgram program;
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        || !program.link()) {
        qWarning().noquote() << "Startup renderer shader initialization failed"
                             << program.log();
        context.doneCurrent();
        running_.store(false, std::memory_order_release);
        threadId_.store(0, std::memory_order_release);
        emit initializationFailed();
        return;
    }
    QOpenGLVertexArrayObject vao;
    vao.create();
    QElapsedTimer clock;
    clock.start();

    while (running_.load(std::memory_order_acquire)) {
        if (exposed_.load(std::memory_order_acquire)) {
            if (!context.makeCurrent(window_)) {
                QThread::msleep(16);
                continue;
            }
            const int width = pixelWidth_.load(std::memory_order_acquire);
            const int height = pixelHeight_.load(std::memory_order_acquire);
            const qreal dpr = devicePixelRatio_.load(std::memory_order_acquire);
            functions->glViewport(0, 0, width, height);
            functions->glClearColor(0.031f, 0.039f, 0.059f, 1.0f);
            functions->glClear(GL_COLOR_BUFFER_BIT);

            const qreal time = static_cast<qreal>(clock.elapsed()) / 1000.0;
            std::array<QVector4D, 4> controls;
            for (int group = 0; group < 4; ++group) {
                std::array<float, 4> values{};
                for (int component = 0; component < 4; ++component) {
                    const int index = kControlIndices[static_cast<size_t>(group)]
                                                       [static_cast<size_t>(component)];
                    values[static_cast<size_t>(component)] = static_cast<float>(
                        2.0 * std::sin(time * 0.1 * std::sin((index + kFieldSeed) * 0.05)
                                       + index + kFieldSeed));
                }
                controls[static_cast<size_t>(group)] =
                    QVector4D(values[0], values[1], values[2], values[3]);
            }

            program.bind();
            program.setUniformValue("iResolution", QVector2D(width, height));
            program.setUniformValue("iTime", static_cast<float>(time));
            program.setUniformValue("uMorphAmount", 2.0f);
            program.setUniformValue("uBands", 3.0f);
            program.setUniformValue("uThickness", 0.2f);
            program.setUniformValue("uScale", 2.0f);
            program.setUniformValue("uGlow", 0.5f);
            program.setUniformValue("uContrast", 3.0f);
            program.setUniformValue("uBrightness", 1.0f);
            program.setUniformValue("uGrainIntensity", 0.05f);
            program.setUniformValue("uLow", QVector3D(0x52 / 255.0f, 0x27 / 255.0f, 0xff / 255.0f));
            program.setUniformValue("uLowMid", QVector3D(0x29 / 255.0f, 0x8c / 255.0f, 0xff / 255.0f));
            program.setUniformValue("uMid", QVector3D(0.0f, 0xf0 / 255.0f, 1.0f));
            program.setUniformValue("uHighMid", QVector3D(0x80 / 255.0f, 0xf8 / 255.0f, 1.0f));
            program.setUniformValue("uHigh", QVector3D(1.0f, 1.0f, 1.0f));
            program.setUniformValue("uCtrlA", controls[0]);
            program.setUniformValue("uCtrlB", controls[1]);
            program.setUniformValue("uCtrlC", controls[2]);
            program.setUniformValue("uCtrlD", controls[3]);
            vao.bind();
            functions->glDrawArrays(GL_TRIANGLES, 0, 3);
            vao.release();
            program.release();

            QOpenGLPaintDevice device(QSize(width, height));
            device.setDevicePixelRatio(dpr);
            drawTitle(device, time);
            context.swapBuffers(window_);
            if (!initialFramePresented_.exchange(true, std::memory_order_acq_rel))
                emit initialFramePresented();
        }
        QThread::msleep(16);
    }

    vao.destroy();
    context.doneCurrent();
    threadId_.store(0, std::memory_order_release);
}

}
