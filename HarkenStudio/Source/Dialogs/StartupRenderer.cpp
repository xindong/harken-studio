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
#include <QVector2D>
#include <QDebug>

#include <cmath>
#include <memory>

namespace {

constexpr int kPixelSize = 2;
constexpr int kLogoWidth = 268;

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

vec4 mod289(vec4 x)
{
    return x - floor(x * (1.0 / 289.0)) * 289.0;
}

vec4 permute(vec4 x)
{
    return mod289(((x * 34.0) + 1.0) * x);
}

vec4 taylorInvSqrt(vec4 r)
{
    return 1.79284291400159 - 0.85373472095314 * r;
}

vec2 fade(vec2 t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float cnoise(vec2 p)
{
    vec4 pi = floor(p.xyxy) + vec4(0.0, 0.0, 1.0, 1.0);
    vec4 pf = fract(p.xyxy) - vec4(0.0, 0.0, 1.0, 1.0);
    pi = mod289(pi);
    vec4 ix = pi.xzxz;
    vec4 iy = pi.yyww;
    vec4 fx = pf.xzxz;
    vec4 fy = pf.yyww;
    vec4 i = permute(permute(ix) + iy);
    vec4 gx = fract(i * (1.0 / 41.0)) * 2.0 - 1.0;
    vec4 gy = abs(gx) - 0.5;
    vec4 tx = floor(gx + 0.5);
    gx = gx - tx;
    vec2 g00 = vec2(gx.x, gy.x);
    vec2 g10 = vec2(gx.y, gy.y);
    vec2 g01 = vec2(gx.z, gy.z);
    vec2 g11 = vec2(gx.w, gy.w);
    vec4 norm = taylorInvSqrt(vec4(dot(g00, g00), dot(g01, g01), dot(g10, g10), dot(g11, g11)));
    g00 *= norm.x;
    g01 *= norm.y;
    g10 *= norm.z;
    g11 *= norm.w;
    float n00 = dot(g00, vec2(fx.x, fy.x));
    float n10 = dot(g10, vec2(fx.y, fy.y));
    float n01 = dot(g01, vec2(fx.z, fy.z));
    float n11 = dot(g11, vec2(fx.w, fy.w));
    vec2 fadeXY = fade(pf.xy);
    vec2 nx = mix(vec2(n00, n01), vec2(n10, n11), fadeXY.x);
    return 2.3 * mix(nx.x, nx.y, fadeXY.y);
}

float fbm(vec2 p)
{
    float value = 0.0;
    float amplitude = 1.0;
    for (int i = 0; i < 4; ++i) {
        value += amplitude * abs(cnoise(p));
        p *= uWaveFrequency;
        amplitude *= uWaveAmplitude;
    }
    return value;
}

float pattern(vec2 p)
{
    vec2 p2 = p - uTime * uWaveSpeed;
    return fbm(p + fbm(p2));
}

void main()
{
    vec2 corner = (gl_FragCoord.xy - 0.5) * uCell;
    vec2 uv = corner / uResolution - 0.5;
    uv.x *= uResolution.x / uResolution.y;
    float value = clamp(pattern(uv / uScale), 0.0, 1.0);
    fragColor = vec4(floor(value * 255.0) / 255.0, fract(value * 255.0), 0.0, 1.0);
}
)glsl";

const char* kDitherFragmentShader = R"glsl(
#version 330 core
uniform sampler2D uField;
uniform float uCellDevice;
out vec4 fragColor;

const float bayer8[64] = float[64](
    0.0 / 64.0, 48.0 / 64.0, 12.0 / 64.0, 60.0 / 64.0, 3.0 / 64.0, 51.0 / 64.0, 15.0 / 64.0, 63.0 / 64.0,
    32.0 / 64.0, 16.0 / 64.0, 44.0 / 64.0, 28.0 / 64.0, 35.0 / 64.0, 19.0 / 64.0, 47.0 / 64.0, 31.0 / 64.0,
    8.0 / 64.0, 56.0 / 64.0, 4.0 / 64.0, 52.0 / 64.0, 11.0 / 64.0, 59.0 / 64.0, 7.0 / 64.0, 55.0 / 64.0,
    40.0 / 64.0, 24.0 / 64.0, 36.0 / 64.0, 20.0 / 64.0, 43.0 / 64.0, 27.0 / 64.0, 39.0 / 64.0, 23.0 / 64.0,
    2.0 / 64.0, 50.0 / 64.0, 14.0 / 64.0, 62.0 / 64.0, 1.0 / 64.0, 49.0 / 64.0, 13.0 / 64.0, 61.0 / 64.0,
    34.0 / 64.0, 18.0 / 64.0, 46.0 / 64.0, 30.0 / 64.0, 33.0 / 64.0, 17.0 / 64.0, 45.0 / 64.0, 29.0 / 64.0,
    10.0 / 64.0, 58.0 / 64.0, 6.0 / 64.0, 54.0 / 64.0, 9.0 / 64.0, 57.0 / 64.0, 5.0 / 64.0, 53.0 / 64.0,
    42.0 / 64.0, 26.0 / 64.0, 38.0 / 64.0, 22.0 / 64.0, 41.0 / 64.0, 25.0 / 64.0, 37.0 / 64.0, 21.0 / 64.0
);

const vec3 palette[4] = vec3[4](
    vec3(6.0, 18.0, 31.0) / 255.0,
    vec3(23.0, 61.0, 92.0) / 255.0,
    vec3(95.0, 158.0, 199.0) / 255.0,
    vec3(228.0, 243.0, 255.0) / 255.0
);

void main()
{
    vec2 cell = floor(gl_FragCoord.xy / uCellDevice);
    vec4 sampleValue = texelFetch(uField, clamp(ivec2(cell), ivec2(0), textureSize(uField, 0) - 1), 0);
    float field = sampleValue.r + sampleValue.g / 255.0;
    ivec2 c = ivec2(mod(cell, 8.0));
    float threshold = bayer8[c.y * 8 + c.x] - 0.25;
    float value = field + threshold / 3.0;
    value = clamp(value - mix(0.2, 0.0, smoothstep(0.45, 0.8, value)), 0.0, 1.0);
    int index = int(floor(value * 3.0 + 0.5));
    fragColor = vec4(palette[index], 1.0);
}
)glsl";

void drawLogo(QOpenGLPaintDevice& device, const QImage& logo, qreal time)
{
    QPainter painter(&device);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF titleBox((device.width() / device.devicePixelRatioF() - 316.0) * 0.5,
                          (device.height() / device.devicePixelRatioF() - 88.0) * 0.5,
                          316.0, 88.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(16, 21, 31));
    painter.drawRect(titleBox);

    const QSizeF logoSize = logo.deviceIndependentSize();
    const qreal shineProgress = std::fmod(time, 4.0) / 4.0;
    const qreal shineCenter = logoSize.width() * (-0.5 + shineProgress * 2.0);
    QLinearGradient shineGradient(QPointF(shineCenter - 145.0, logoSize.height()),
                                  QPointF(shineCenter + 145.0, 0.0));
    shineGradient.setColorAt(0.0, QColor(181, 181, 181));
    shineGradient.setColorAt(0.22, QColor(181, 181, 181));
    shineGradient.setColorAt(0.38, QColor(211, 211, 213));
    shineGradient.setColorAt(0.5, QColor(255, 255, 255));
    shineGradient.setColorAt(0.62, QColor(211, 211, 213));
    shineGradient.setColorAt(0.78, QColor(181, 181, 181));
    shineGradient.setColorAt(1.0, QColor(181, 181, 181));

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

    QOpenGLShaderProgram waveProgram;
    QOpenGLShaderProgram ditherProgram;
    if (!waveProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !waveProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kWaveFragmentShader)
        || !waveProgram.link()
        || !ditherProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !ditherProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kDitherFragmentShader)
        || !ditherProgram.link()) {
        qWarning().noquote() << "Startup renderer shader initialization failed"
                             << waveProgram.log() << ditherProgram.log();
        context.doneCurrent();
        running_.store(false, std::memory_order_release);
        threadId_.store(0, std::memory_order_release);
        emit initializationFailed();
        return;
    }
    QOpenGLVertexArrayObject vao;
    vao.create();
    QOpenGLFramebufferObjectFormat fieldFormat;
    fieldFormat.setInternalTextureFormat(GL_RGBA8);
    std::unique_ptr<QOpenGLFramebufferObject> field;
    const QImage logoSource(QStringLiteral(":/Resources/branding/HarkenStudioWhite.png"));
    QImage logo;
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
            const int logoPixelWidth = qRound(kLogoWidth * dpr);
            if (logo.width() != logoPixelWidth || logo.devicePixelRatio() != dpr) {
                logo = logoSource.scaledToWidth(logoPixelWidth, Qt::SmoothTransformation);
                logo.setDevicePixelRatio(dpr);
            }
            const int cell = qMax(1, qRound(kPixelSize * dpr));
            const QSize fieldSize((width + cell - 1) / cell, (height + cell - 1) / cell);
            if (!field || field->size() != fieldSize) {
                field = std::make_unique<QOpenGLFramebufferObject>(fieldSize, fieldFormat);
                if (!field->isValid()) {
                    qWarning() << "Startup renderer could not create the wave framebuffer";
                    running_.store(false, std::memory_order_release);
                    emit initializationFailed();
                    break;
                }
            }

            const qreal time = static_cast<qreal>(clock.elapsed()) / 1000.0;
            functions->glDisable(GL_BLEND);
            functions->glDisable(GL_SCISSOR_TEST);
            vao.bind();
            field->bind();
            functions->glViewport(0, 0, fieldSize.width(), fieldSize.height());
            waveProgram.bind();
            waveProgram.setUniformValue("uResolution", QVector2D(width, height));
            waveProgram.setUniformValue("uCell", static_cast<float>(cell));
            waveProgram.setUniformValue("uTime", static_cast<float>(time));
            waveProgram.setUniformValue("uWaveSpeed", 0.05f);
            waveProgram.setUniformValue("uWaveFrequency", 3.0f);
            waveProgram.setUniformValue("uWaveAmplitude", 0.3f);
            waveProgram.setUniformValue("uScale", 0.6f);
            functions->glDrawArrays(GL_TRIANGLES, 0, 3);
            waveProgram.release();
            field->release();

            functions->glViewport(0, 0, width, height);
            functions->glActiveTexture(GL_TEXTURE0);
            functions->glBindTexture(GL_TEXTURE_2D, field->texture());
            ditherProgram.bind();
            ditherProgram.setUniformValue("uField", 0);
            ditherProgram.setUniformValue("uCellDevice", static_cast<float>(cell));
            functions->glDrawArrays(GL_TRIANGLES, 0, 3);
            ditherProgram.release();
            functions->glBindTexture(GL_TEXTURE_2D, 0);
            vao.release();

            QOpenGLPaintDevice device(QSize(width, height));
            device.setDevicePixelRatio(dpr);
            drawLogo(device, logo, time);
            context.swapBuffers(window_);
            if (!initialFramePresented_.exchange(true, std::memory_order_acq_rel))
                emit initialFramePresented();
        }
        QThread::msleep(16);
    }

    field.reset();
    vao.destroy();
    context.doneCurrent();
    threadId_.store(0, std::memory_order_release);
}

}
