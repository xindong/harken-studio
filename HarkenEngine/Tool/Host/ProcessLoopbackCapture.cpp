#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <objidl.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint16_t kChannels = 2;

struct Options {
    std::filesystem::path output;
    std::filesystem::path captureReadyGate;
    DWORD processId = 0;
    double seconds = 5.0;
};

bool ParseOptions(int argc, char** argv, Options* out) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--out" && index + 1 < argc) {
            out->output = std::filesystem::path(argv[++index]);
        } else if (argument == "--capture-ready-gate" && index + 1 < argc) {
            out->captureReadyGate = std::filesystem::path(argv[++index]);
        } else if (argument == "--pid" && index + 1 < argc) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(argv[++index], &end, 10);
            if (end == argv[index] || *end != '\0' || value == 0) {
                return false;
            }
            out->processId = static_cast<DWORD>(value);
        } else if (argument == "--seconds" && index + 1 < argc) {
            char* end = nullptr;
            const double value = std::strtod(argv[++index], &end);
            if (end == argv[index] || *end != '\0' || !(value > 0.0) ||
                !std::isfinite(value) || value > 300.0) {
                return false;
            }
            out->seconds = value;
        } else {
            return false;
        }
    }
    return !out->output.empty() && !out->captureReadyGate.empty() &&
           out->processId != 0;
}

bool PublishCaptureReadyGate(const std::filesystem::path& path,
                             DWORD processId) {
    std::filesystem::path temporary = path;
    temporary += L".publishing." + std::to_wstring(GetCurrentProcessId());
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
                                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::string marker =
        "process_id=" + std::to_string(static_cast<unsigned long>(processId)) +
        "\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, marker.data(),
                              static_cast<DWORD>(marker.size()), &written,
                              nullptr) != FALSE &&
                    written == marker.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

class ActivationHandler final
    : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivationHandler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId,
                                             void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(interfaceId, __uuidof(IUnknown)) ||
            IsEqualIID(interfaceId,
                       __uuidof(IActivateAudioInterfaceCompletionHandler)) ||
            IsEqualIID(interfaceId, __uuidof(IAgileObject))) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        IUnknown* activated = nullptr;
        HRESULT activationResult = E_FAIL;
        const HRESULT getResult =
            operation->GetActivateResult(&activationResult, &activated);
        result_ = FAILED(getResult) ? getResult : activationResult;
        if (SUCCEEDED(result_) && activated != nullptr) {
            result_ = activated->QueryInterface(__uuidof(IAudioClient),
                                                reinterpret_cast<void**>(&client_));
        }
        if (activated != nullptr) {
            activated->Release();
        }
        SetEvent(event_);
        return S_OK;
    }

    bool Wait(DWORD timeoutMs) const {
        return event_ != nullptr &&
               WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
    }

    HRESULT Result() const { return result_; }

    IAudioClient* AcquireClient() const {
        if (client_ != nullptr) {
            client_->AddRef();
        }
        return client_;
    }

private:
    ~ActivationHandler() {
        if (client_ != nullptr) {
            client_->Release();
        }
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
    }

    std::atomic<ULONG> references_{1};
    HANDLE event_ = nullptr;
    HRESULT result_ = E_PENDING;
    IAudioClient* client_ = nullptr;
};

void WriteU16(std::ofstream& output, std::uint16_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WriteU32(std::ofstream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool WriteWav(const std::filesystem::path& path,
              const std::vector<float>& samples) {
    if (samples.size() > UINT32_MAX / sizeof(float)) {
        return false;
    }
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(float));
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write("RIFF", 4);
    WriteU32(output, 36u + dataBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    WriteU32(output, 16);
    WriteU16(output, WAVE_FORMAT_IEEE_FLOAT);
    WriteU16(output, kChannels);
    WriteU32(output, kSampleRate);
    WriteU32(output, kSampleRate * kChannels * sizeof(float));
    WriteU16(output, kChannels * sizeof(float));
    WriteU16(output, 32);
    output.write("data", 4);
    WriteU32(output, dataBytes);
    output.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    return output.good();
}

void Release(IUnknown* object) {
    if (object != nullptr) {
        object->Release();
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        std::fprintf(stderr,
                     "usage: harken_process_loopback_capture --pid <id> "
                     "--out <wav> --seconds <n> "
                     "--capture-ready-gate <path>\n");
        return 2;
    }

    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        std::fprintf(stderr, "error: CoInitializeEx failed (0x%08lx)\n",
                     static_cast<unsigned long>(comResult));
        return 3;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = options.processId;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation;
    PropVariantInit(&activation);
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(parameters);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

    auto* handler = new (std::nothrow) ActivationHandler();
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT result = handler == nullptr
        ? E_OUTOFMEMORY
        : ActivateAudioInterfaceAsync(
              VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
              &activation, handler, &operation);
    if (SUCCEEDED(result) && !handler->Wait(10000)) {
        result = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    if (SUCCEEDED(result)) {
        result = handler->Result();
    }
    IAudioClient* client =
        SUCCEEDED(result) ? handler->AcquireClient() : nullptr;
    Release(operation);
    if (handler != nullptr) {
        handler->Release();
    }
    if (FAILED(result) || client == nullptr) {
        std::fprintf(stderr,
                     "error: process loopback activation failed (0x%08lx)\n",
                     static_cast<unsigned long>(result));
        Release(client);
        CoUninitialize();
        return 3;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = kChannels * sizeof(float);
    format.nAvgBytesPerSec = kSampleRate * format.nBlockAlign;
    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                              AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                100000, 0, &format, nullptr);
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "error: process loopback initialize failed (0x%08lx)\n",
                     static_cast<unsigned long>(result));
        Release(client);
        CoUninitialize();
        return 3;
    }

    IAudioCaptureClient* capture = nullptr;
    result = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "error: capture service activation failed (0x%08lx)\n",
                     static_cast<unsigned long>(result));
        Release(client);
        CoUninitialize();
        return 3;
    }
    result = client->Start();
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "error: process loopback start failed (0x%08lx)\n",
                     static_cast<unsigned long>(result));
        Release(capture);
        Release(client);
        CoUninitialize();
        return 3;
    }

    if (!PublishCaptureReadyGate(options.captureReadyGate, options.processId)) {
        std::fprintf(stderr,
                     "error: failed to publish fresh capture-ready gate '%s'\n",
                     options.captureReadyGate.string().c_str());
        client->Stop();
        Release(capture);
        Release(client);
        CoUninitialize();
        return 3;
    }

    std::printf("backend=WASAPI process_id=%lu rate=%u channels=%u\n",
                static_cast<unsigned long>(options.processId), kSampleRate,
                kChannels);
    std::fflush(stdout);
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(
        std::ceil(options.seconds * kSampleRate * kChannels)));
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(
            static_cast<std::uint64_t>(options.seconds * 1000.0));
    bool dataDiscontinuity = false;
    while (std::chrono::steady_clock::now() < deadline) {
        UINT32 packetFrames = 0;
        result = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(result)) {
            break;
        }
        if (packetFrames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(result)) {
            break;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
            dataDiscontinuity = true;
            result = capture->ReleaseBuffer(frames);
            break;
        }
        const std::size_t count =
            static_cast<std::size_t>(frames) * kChannels;
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            samples.insert(samples.end(), count, 0.0f);
        } else {
            const float* input = reinterpret_cast<const float*>(data);
            samples.insert(samples.end(), input, input + count);
        }
        result = capture->ReleaseBuffer(frames);
        if (FAILED(result)) {
            break;
        }
    }
    client->Stop();
    Release(capture);
    Release(client);
    CoUninitialize();
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "error: process loopback read failed (0x%08lx)\n",
                     static_cast<unsigned long>(result));
        return 3;
    }
    if (dataDiscontinuity) {
        std::fprintf(stderr,
                     "error: process loopback capture data discontinuity\n");
        return 4;
    }
    if (!WriteWav(options.output, samples)) {
        std::fprintf(stderr, "error: failed to write capture\n");
        return 3;
    }
    std::printf("captured_frames=%llu path=%s\n",
                static_cast<unsigned long long>(samples.size() / kChannels),
                options.output.string().c_str());
    return 0;
}
