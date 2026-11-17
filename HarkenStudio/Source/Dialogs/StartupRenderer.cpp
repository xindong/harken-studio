#include "StartupRenderer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLPaintDevice>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QDebug>

#include <cmath>
#include <memory>
#include <utility>

namespace {

constexpr int kPixelSize = 1;

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

const char* kWaveFragmentShader = R"glsl(
#version 330 core
uniform vec2 uResolution;
uniform float uCell;
uniform float uTime;
uniform float uWaveSpeed;
uniform float uWaveFrequency;
uniform float uWaveAmplitude;
uniform float uScale;
out vec4 fragColor;

float latticeHeight(ivec2 cell)
{
    uvec2 coordinate = uvec2(cell);
    uint seed = coordinate.x * 503u + coordinate.y * 911u + 101u;
    seed = (seed ^ (seed >> 16u)) * 0x4841524bu;
    seed = (seed ^ (seed >> 13u)) * 0x53545545u;
    seed ^= seed >> 16u;
    return float(seed & 65535u) / 32767.5 - 1.0;
}

float surfaceHeight(vec2 position)
{
    ivec2 cell = ivec2(floor(position));
    vec2 blend = smoothstep(vec2(0.0), vec2(1.0), fract(position));
    float lower = mix(latticeHeight(cell), latticeHeight(cell + ivec2(1, 0)), blend.x);
    float upper = mix(latticeHeight(cell + ivec2(0, 1)),
                      latticeHeight(cell + ivec2(1, 1)), blend.x);
    return mix(lower, upper, blend.y);
}

vec2 flowOffset(vec2 position, float time)
{
    vec2 drift = vec2(time, 0.0);
    vec2 broad = vec2(surfaceHeight(position + drift),
                      surfaceHeight(position + vec2(19.3, -7.8) - drift.yx));
    vec2 detail = position * uWaveFrequency + vec2(5.2, 9.1);
    vec2 fine = vec2(surfaceHeight(detail + drift.yx),
                     surfaceHeight(detail + vec2(-8.4, 17.6) + drift));
    return broad + uWaveAmplitude * fine;
}

void main()
{
    vec2 pixelCenter = gl_FragCoord.xy * uCell;
    vec2 position = (pixelCenter - uResolution * 0.5) / (uResolution.y * uScale);
    position = position * 2.5 + vec2(11.7, 23.4);
    float flowTime = uTime * uWaveSpeed;
    position -= flowTime * vec2(1.0, 0.35);
    position += 0.75 * flowOffset(position, flowTime);

    vec4 heights;
    mat2 rotation = mat2(0.8, 0.6, -0.6, 0.8);
    for (int layer = 0; layer < 4; ++layer) {
        heights[layer] = abs(surfaceHeight(position));
        position = rotation * position * uWaveFrequency + vec2(5.2, 9.1);
    }
    vec4 weights = pow(vec4(uWaveAmplitude), vec4(0.0, 1.0, 2.0, 3.0));
    float brightness = smoothstep(0.12, 1.12, dot(heights, weights));
    brightness -= 0.14 * smoothstep(0.55, 1.0, brightness);
    fragColor = vec4(brightness, 0.0, 0.0, 1.0);
}
)glsl";

const char* kDitherFragmentShader = R"glsl(
#version 330 core
uniform sampler2D uField;
uniform float uCellDevice;
uniform vec4 uPalette[4];
uniform vec3 uBackground;
out vec4 fragColor;

float orderedThreshold(ivec2 pixel)
{
    uvec2 address = uvec2(pixel);
    uint rank = 0u;
    for (int level = 0; level < 3; ++level) {
        uvec2 bit = address & uvec2(1u);
        rank = (rank << 2u) | ((bit.x ^ bit.y) << 1u) | bit.y;
        address >>= 1u;
    }
    return (float(rank) + 0.5) / 64.0;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy / uCellDevice);
    float brightness = texelFetch(uField, pixel, 0).r;
    int index = clamp(int(floor(brightness * 3.0 + orderedThreshold(pixel))), 0, 3);
    fragColor = vec4(mix(uBackground, uPalette[index].rgb, uPalette[index].a), 1.0);
}
)glsl";

void drawLogo(QOpenGLPaintDevice& device, const QImage& logo, qreal time,
              const QColor& background, const std::array<QColor, 3>& palette)
{
    QPainter painter(&device);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF titleBox((device.width() / device.devicePixelRatioF() - 316.0) * 0.5,
                          (device.height() / device.devicePixelRatioF() - 88.0) * 0.5,
                          316.0, 88.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRect(titleBox);

    const QSizeF logoSize = logo.deviceIndependentSize();
    const qreal shineProgress = std::fmod(time, 4.0) / 4.0;
    const qreal shineCenter = logoSize.width() * (-0.5 + shineProgress * 2.0);
    QLinearGradient shineGradient(QPointF(shineCenter - 145.0, logoSize.height()),
                                  QPointF(shineCenter + 145.0, 0.0));
    shineGradient.setColorAt(0.0, palette[0]);
    shineGradient.setColorAt(0.22, palette[0]);
    shineGradient.setColorAt(0.38, palette[1]);
    shineGradient.setColorAt(0.5, palette[2]);
    shineGradient.setColorAt(0.62, palette[1]);
    shineGradient.setColorAt(0.78, palette[0]);
    shineGradient.setColorAt(1.0, palette[0]);

    QImage shinyLogo(logo.size(), QImage::Format_ARGB32_Premultiplied);
    shinyLogo.setDevicePixelRatio(logo.devicePixelRatio());
    shinyLogo.fill(Qt::transparent);
    {
        QPainter logoPainter(&shinyLogo);
        logoPainter.fillRect(QRectF(QPointF(), logoSize), shineGradient);
        logoPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        logoPainter.drawImage(QPointF(), logo);
    }
    painter.drawImage(QPointF(titleBox.center().x() - logoSize.width() * 0.5,
                              titleBox.center().y() - logoSize.height() * 0.5), shinyLogo);
}

}

namespace VNX {

StartupRenderer::StartupRenderer(ThemeSnapshotPtr theme)
    : theme_(std::move(theme))
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
struct StartupRenderer::Frame
{
    QOpenGLContext context;
    QOpenGLShaderProgram waveProgram;
    QOpenGLShaderProgram ditherProgram;
    QOpenGLVertexArrayObject vao;
    std::unique_ptr<QOpenGLFramebufferObject> field;
    QOpenGLFramebufferObjectFormat fieldFormat;
    QImage logo;
    QElapsedTimer clock;
    QTimer timer;
    bool initialized = false;
    bool failed = false;
};

void StartupRenderer::start()
{
    if (running_.load(std::memory_order_acquire))
        return;
    initialFramePresented_.store(false, std::memory_order_release);
    if (!frame_) {
        frame_ = std::make_unique<Frame>();
        frame_->timer.setInterval(16);
        connect(&frame_->timer, &QTimer::timeout, this, [this]() { renderFrame(); });
    }
    threadId_.store(reinterpret_cast<quintptr>(QThread::currentThreadId()),
                    std::memory_order_release);
    running_.store(true, std::memory_order_release);
    // Present the first frame synchronously: startup work is queued on the next
    // event loop turn and would otherwise run before anything is on screen.
    if (exposed_.load(std::memory_order_acquire))
        renderFrame();
    if (running_.load(std::memory_order_acquire) && !frame_->timer.isActive())
        frame_->timer.start();
}

void StartupRenderer::stop()
{
    running_.store(false, std::memory_order_release);
    if (frame_) {
        frame_->timer.stop();
        if (frame_->initialized && frame_->context.isValid()) {
            frame_->context.makeCurrent(window_);
            frame_->field.reset();
            frame_->vao.destroy();
            frame_->ditherProgram.removeAllShaders();
            frame_->waveProgram.removeAllShaders();
            frame_->context.doneCurrent();
        }
        frame_.reset();
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

void StartupRenderer::renderFrame()
{
    if (!running_.load(std::memory_order_acquire))
        return;
    Frame& frame = *frame_;
    if (frame.failed)
        return;

    if (!frame.initialized) {
        frame.context.setFormat(format_);
        if (!frame.context.create() || !frame.context.makeCurrent(window_)) {
            qWarning() << "Startup renderer could not create an OpenGL context";
            frame.failed = true;
            running_.store(false, std::memory_order_release);
            threadId_.store(0, std::memory_order_release);
            emit initializationFailed();
            return;
        }

        QOpenGLFunctions* functions = frame.context.functions();
        functions->initializeOpenGLFunctions();
        functions->glDisable(GL_DEPTH_TEST);
        functions->glDisable(GL_BLEND);

        if (!frame.waveProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
            || !frame.waveProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kWaveFragmentShader)
            || !frame.waveProgram.link()
            || !frame.ditherProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
            || !frame.ditherProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kDitherFragmentShader)
            || !frame.ditherProgram.link()) {
            qWarning().noquote() << "Startup renderer shader initialization failed"
                                 << frame.waveProgram.log() << frame.ditherProgram.log();
            frame.context.doneCurrent();
            frame.failed = true;
            running_.store(false, std::memory_order_release);
            threadId_.store(0, std::memory_order_release);
            emit initializationFailed();
            return;
        }

        const auto& paletteColors = theme_->domains.startup.wavePalette;
        QVector4D palette[4];
        for (int i = 0; i < 4; ++i)
            palette[i] = QVector4D(paletteColors[i].redF(), paletteColors[i].greenF(),
                                   paletteColors[i].blueF(), paletteColors[i].alphaF());
        const QColor& background = theme_->ui.colors.bgPrimary;
        frame.ditherProgram.bind();
        frame.ditherProgram.setUniformValueArray("uPalette", palette, 4);
        frame.ditherProgram.setUniformValue("uBackground",
            QVector3D(background.redF(), background.greenF(), background.blueF()));
        frame.ditherProgram.release();

        frame.vao.create();
        frame.fieldFormat.setInternalTextureFormat(GL_R16F);
        frame.clock.start();
        frame.initialized = true;
    } else if (!frame.context.makeCurrent(window_)) {
        return;
    }

    if (!exposed_.load(std::memory_order_acquire)) {
        frame.context.doneCurrent();
        return;
    }

    QOpenGLFunctions* functions = frame.context.functions();
    const int width = pixelWidth_.load(std::memory_order_acquire);
    const int height = pixelHeight_.load(std::memory_order_acquire);
    const qreal dpr = devicePixelRatio_.load(std::memory_order_acquire);
    static const QImage logoSource(QStringLiteral(":/Resources/branding/HarkenStudioWhite.png"));
    const int logoPixelWidth = qRound(theme_->ui.metrics.startup.logoWidth * dpr);
    if (frame.logo.width() != logoPixelWidth || frame.logo.devicePixelRatio() != dpr) {
        frame.logo = logoSource.scaledToWidth(logoPixelWidth, Qt::SmoothTransformation);
        frame.logo.setDevicePixelRatio(dpr);
    }
    const int cell = qMax(1, qRound(kPixelSize * dpr));
    const QSize fieldSize((width + cell - 1) / cell, (height + cell - 1) / cell);
    if (!frame.field || frame.field->size() != fieldSize) {
        frame.field = std::make_unique<QOpenGLFramebufferObject>(fieldSize, frame.fieldFormat);
        if (!frame.field->isValid()) {
            qWarning() << "Startup renderer could not create the wave framebuffer";
            frame.context.doneCurrent();
            frame.failed = true;
            running_.store(false, std::memory_order_release);
            emit initializationFailed();
            return;
        }
    }

    const qreal time = static_cast<qreal>(frame.clock.elapsed()) / 1000.0;
    functions->glDisable(GL_BLEND);
    functions->glDisable(GL_SCISSOR_TEST);
    frame.vao.bind();
    frame.field->bind();
    functions->glViewport(0, 0, fieldSize.width(), fieldSize.height());
    frame.waveProgram.bind();
    frame.waveProgram.setUniformValue("uResolution", QVector2D(width, height));
    frame.waveProgram.setUniformValue("uCell", static_cast<float>(cell));
    frame.waveProgram.setUniformValue("uTime", static_cast<float>(time));
    frame.waveProgram.setUniformValue("uWaveSpeed", 0.12f);
    frame.waveProgram.setUniformValue("uWaveFrequency", 3.0f);
    frame.waveProgram.setUniformValue("uWaveAmplitude", 0.3f);
    frame.waveProgram.setUniformValue("uScale", 0.9f);
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);
    frame.waveProgram.release();
    frame.field->release();

    functions->glViewport(0, 0, width, height);
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, frame.field->texture());
    frame.ditherProgram.bind();
    frame.ditherProgram.setUniformValue("uField", 0);
    frame.ditherProgram.setUniformValue("uCellDevice", static_cast<float>(cell));
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);
    frame.ditherProgram.release();
    functions->glBindTexture(GL_TEXTURE_2D, 0);
    frame.vao.release();

    QOpenGLPaintDevice device(QSize(width, height));
    device.setDevicePixelRatio(dpr);
    drawLogo(device, frame.logo, time, theme_->ui.colors.panelBackground,
             theme_->domains.startup.logoPalette);
    frame.context.swapBuffers(window_);
    if (!initialFramePresented_.exchange(true, std::memory_order_acq_rel))
        emit initialFramePresented();
}

}
