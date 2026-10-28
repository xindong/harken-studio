#include "Download/SourceProbe.h"

#include "Download/ContentRange.h"
#include "Update/ServerEndpoints.h"

#include <QDebug>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

namespace VNH {

namespace {

constexpr qint64 kSampleBytes = 1024 * 1024;
constexpr int kObservationMs = 300;
constexpr int kDeadlineMs = 2500;

}

SourceProbe::SourceProbe(QObject* parent) : QObject(parent)
{
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this]() { finish(); });
    decision_.setSingleShot(true);
    connect(&decision_, &QTimer::timeout, this, [this]() {
        if (std::any_of(samples_.cbegin(), samples_.cend(),
                        [](const Sample& sample) { return sample.done && sample.rate > 0; }))
            finish();
    });
}

SourceProbe::~SourceProbe()
{
    cancel();
}

void SourceProbe::start(const QList<QUrl>& sources, qint64 expectedSize)
{
    cancel();
    expectedSize_ = expectedSize;
    sampleSize_ = expectedSize > 0 ? std::min(expectedSize, kSampleBytes) : kSampleBytes;
    active_ = true;
    elapsed_.start();
    for (const auto& source : sources) {
        Sample sample;
        sample.source = source;
        samples_.append(sample);
    }
    deadline_.start(kDeadlineMs);
    decision_.start(kObservationMs);
    for (int index = 0; index < samples_.size(); ++index) {
        samples_[index].elapsed.start();
        request(index, samples_[index].source);
    }
}

void SourceProbe::stopReply(Sample& sample)
{
    if (!sample.reply)
        return;
    disconnect(sample.reply, nullptr, this, nullptr);
    sample.reply->abort();
    sample.reply->deleteLater();
    sample.reply = nullptr;
}

void SourceProbe::cancel()
{
    active_ = false;
    deadline_.stop();
    decision_.stop();
    for (auto& sample : samples_)
        stopReply(sample);
    samples_.clear();
}

void SourceProbe::request(int index, const QUrl& url)
{
    QNetworkRequest netRequest(url);
    netRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    netRequest.setRawHeader("Accept-Encoding", "identity");
    netRequest.setRawHeader("Range", "bytes=0-" + QByteArray::number(sampleSize_ - 1));
    auto* reply = manager_.get(netRequest);
    reply->setReadBufferSize(64 * 1024);
    samples_[index].reply = reply;
    connect(reply, &QIODevice::readyRead, this, [this, index]() { read(index); });
    connect(reply, &QNetworkReply::finished, this, [this, index]() {
        auto& sample = samples_[index];
        auto* reply = sample.reply;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            const QUrl next = reply->url().resolved(
                reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            if (!ServerEndpoints::isApprovedGithubRedirect(sample.source) ||
                !ServerEndpoints::isApprovedGithubRedirect(next) || ++sample.redirects > 5) {
                finish(QStringLiteral("Download redirect is not approved"));
                return;
            }
            stopReply(sample);
            request(index, next);
            return;
        }
        const bool usable = reply->error() == QNetworkReply::NoError;
        read(index);
        if (active_ && index < samples_.size() && samples_[index].reply == reply)
            completeSample(index, usable);
    });
}

void SourceProbe::read(int index)
{
    auto& sample = samples_[index];
    auto* reply = sample.reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        sample.received += reply->read(sampleSize_ - sample.received).size();
        if (sample.received >= sampleSize_)
            finish(QStringLiteral("Download redirect exceeded the sample limit"));
        return;
    }
    const auto encoding = reply->rawHeader("Content-Encoding").toLower();
    bool valid = encoding.isEmpty() || encoding == "identity";
    if (status == 206) {
        const auto range = ContentRange::parse(reply->rawHeader("Content-Range"));
        valid = valid && range && range->start == 0 && range->end < sampleSize_ &&
                (expectedSize_ < 0 || range->total == expectedSize_);
    } else if (status == 200) {
        bool parsed = false;
        const auto size = reply->rawHeader("Content-Length").toLongLong(&parsed);
        valid = valid && (expectedSize_ < 0 || (parsed && size == expectedSize_));
    } else {
        valid = false;
    }
    if (!valid) {
        completeSample(index, false);
        return;
    }
    const auto count = reply->read(sampleSize_ - sample.received).size();
    sample.received += count;
    sample.bytes += count;
    if (sample.received >= sampleSize_)
        completeSample(index, true);
}

void SourceProbe::completeSample(int index, bool usable)
{
    auto& sample = samples_[index];
    sample.rate = usable ? double(sample.bytes) * 1000 / std::max<qint64>(sample.elapsed.elapsed(), 1) : 0;
    sample.done = true;
    stopReply(sample);
    if ((sample.rate > 0 && elapsed_.elapsed() >= kObservationMs) ||
        std::all_of(samples_.cbegin(), samples_.cend(), [](const Sample& item) { return item.done; }))
        finish();
}

void SourceProbe::finish(const QString& error)
{
    if (!active_)
        return;
    deadline_.stop();
    decision_.stop();
    for (auto& sample : samples_) {
        if (!sample.done)
            sample.rate = double(sample.bytes) * 1000 / std::max<qint64>(sample.elapsed.elapsed(), 1);
        stopReply(sample);
        qInfo() << "Download source sample:" << sample.source.host()
                << "bytes:" << sample.bytes << "bytes/sec:" << sample.rate;
    }
    std::stable_sort(samples_.begin(), samples_.end(),
                     [](const Sample& left, const Sample& right) { return left.rate > right.rate; });
    QList<QUrl> sources;
    for (const auto& sample : samples_)
        sources.append(sample.source);
    active_ = false;
    emit finished(sources, error);
}

}
