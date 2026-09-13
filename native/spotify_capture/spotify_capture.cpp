// spotify_capture.cpp - capture one application's audio for Custom Mic.
//
// Uses Windows' process loopback capture (Windows 10 2004 and later): an
// IAudioClient activated on VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK records only
// the audio a given process tree plays. That is what lets Spotify go into the
// mic without also recording a Discord call on the same speakers, which
// whole-system loopback would do.
//
// JS API:
//   findProcess(exeName)        -> pid of the root process with that image, or 0
//   start(pid, sampleRate, cb)  -> true; cb(type, payload) is called with
//                                  "audio"  + Float32Array of interleaved stereo
//                                  "status" + { state, error, hresult }
//   stop()
//   isRunning()                 -> boolean

#include <napi.h>

#include <windows.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <objidl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// ActivateAudioInterfaceAsync completes on a worker thread and requires an
// agile handler, so this answers IAgileObject as well.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done;
    HRESULT result = E_PENDING;
    IAudioClient* client = nullptr;

    ActivationHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG remaining = --refs_;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activated = E_FAIL;
        IUnknown* unknown = nullptr;
        HRESULT hr = op->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(hr) && SUCCEEDED(activated) && unknown) {
            result = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
        } else {
            result = FAILED(hr) ? hr : activated;
        }
        if (unknown) unknown->Release();
        SetEvent(done);
        return S_OK;
    }

private:
    ~ActivationHandler() {
        if (client) client->Release();
        if (done) CloseHandle(done);
    }
    std::atomic<ULONG> refs_{1};
};

struct Session {
    std::thread worker;
    HANDLE stopEvent = nullptr;
    std::atomic<bool> running{false};
    Napi::ThreadSafeFunction tsfn;
    bool hasTsfn = false;
};

Session g;

std::string Hex(HRESULT hr) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

struct StatusData {
    std::string state;
    std::string error;
    long hresult;
};

// Everything the capture thread sends uses non-blocking calls: stop() joins
// the thread from the JS thread, so a blocking call there would deadlock.
void EmitStatus(const std::string& state, const std::string& error = "", HRESULT hr = S_OK) {
    if (!g.hasTsfn) return;
    auto* data = new StatusData{state, error, static_cast<long>(hr)};
    auto rc = g.tsfn.NonBlockingCall(data, [](Napi::Env env, Napi::Function cb, StatusData* d) {
        if (env == nullptr || cb.IsEmpty()) { delete d; return; }
        Napi::Object payload = Napi::Object::New(env);
        payload.Set("state", d->state);
        payload.Set("error", d->error);
        payload.Set("hresult", Napi::Number::New(env, static_cast<double>(d->hresult)));
        delete d;
        cb.Call({Napi::String::New(env, "status"), payload});
    });
    if (rc != napi_ok) delete data;
}

void EmitAudio(std::vector<float>&& samples) {
    if (!g.hasTsfn || samples.empty()) return;
    auto* data = new std::vector<float>(std::move(samples));
    auto rc = g.tsfn.NonBlockingCall(data, [](Napi::Env env, Napi::Function cb, std::vector<float>* d) {
        if (env == nullptr || cb.IsEmpty()) { delete d; return; }
        Napi::Float32Array chunk = Napi::Float32Array::New(env, d->size());
        std::memcpy(chunk.Data(), d->data(), d->size() * sizeof(float));
        delete d;
        cb.Call({Napi::String::New(env, "audio"), chunk});
    });
    // Queue full means JS has fallen behind: drop this chunk rather than
    // stall the capture thread.
    if (rc != napi_ok) delete data;
}

// Spotify runs several processes with the same image name. The one whose
// parent is not also Spotify is the root; capturing its tree includes the
// child that actually plays audio.
DWORD FindRootProcess(const std::wstring& exe) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::pair<DWORD, DWORD>> matches;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exe.c_str()) == 0) {
                matches.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    for (const auto& m : matches) {
        bool parentIsMatch = false;
        for (const auto& other : matches) {
            if (other.first == m.second) { parentIsMatch = true; break; }
        }
        if (!parentIsMatch) return m.first;
    }
    return matches.empty() ? 0 : matches.front().first;
}

void CaptureThread(DWORD pid, UINT32 sampleRate) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialised = SUCCEEDED(hr);

    ActivationHandler* handler = nullptr;
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE packetEvent = nullptr;
    bool started = false;
    bool errored = false;

    auto fail = [&](const char* what, HRESULT code) {
        errored = true;
        EmitStatus("error", std::string(what) + " failed (" + Hex(code) + ")", code);
    };

    do {
        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activation;
        PropVariantInit(&activation);
        activation.vt = VT_BLOB;
        activation.blob.cbSize = sizeof(params);
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        handler = new ActivationHandler();
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                         &activation, handler, &op);
        if (FAILED(hr)) { fail("Activating Spotify capture", hr); break; }

        HANDLE waits[2] = {g.stopEvent, handler->done};
        DWORD woke = WaitForMultipleObjects(2, waits, FALSE, 10000);
        if (woke == WAIT_OBJECT_0) break;  // stopped while activating
        if (woke != WAIT_OBJECT_0 + 1) { fail("Waiting for Spotify capture", HRESULT_FROM_WIN32(ERROR_TIMEOUT)); break; }
        if (FAILED(handler->result) || !handler->client) { fail("Opening Spotify capture", handler->result); break; }
        client = handler->client;
        handler->client = nullptr;

        // Process loopback has no mix format to query: request 16-bit PCM at
        // the app's rate and let Windows convert.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                200000, 0, &format, nullptr);
        if (FAILED(hr)) { fail("Initialising Spotify capture", hr); break; }

        packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!packetEvent) { fail("Creating the capture event", HRESULT_FROM_WIN32(GetLastError())); break; }
        hr = client->SetEventHandle(packetEvent);
        if (FAILED(hr)) { fail("Setting the capture event", hr); break; }
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
        if (FAILED(hr)) { fail("Getting the capture service", hr); break; }
        hr = client->Start();
        if (FAILED(hr)) { fail("Starting Spotify capture", hr); break; }
        started = true;
        EmitStatus("capturing");

        const size_t chunkSamples = static_cast<size_t>(sampleRate / 50) * 2;  // ~20 ms of stereo
        std::vector<float> pending;
        pending.reserve(chunkSamples * 2);
        HANDLE loopWaits[2] = {g.stopEvent, packetEvent};

        for (;;) {
            DWORD signalled = WaitForMultipleObjects(2, loopWaits, FALSE, 1000);
            if (signalled == WAIT_OBJECT_0) break;
            // A timeout only means Spotify is silent; keep waiting.
            bool lost = false;
            for (;;) {
                UINT32 packet = 0;
                hr = capture->GetNextPacketSize(&packet);
                if (FAILED(hr)) { lost = true; break; }
                if (packet == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) { lost = true; break; }
                const size_t at = pending.size();
                const size_t count = static_cast<size_t>(frames) * 2;
                pending.resize(at + count);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    std::fill(pending.begin() + at, pending.end(), 0.0f);
                } else {
                    const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
                    for (size_t i = 0; i < count; ++i) pending[at + i] = pcm[i] / 32768.0f;
                }
                capture->ReleaseBuffer(frames);
            }
            if (lost) { fail("Reading Spotify audio", hr); break; }
            if (pending.size() >= chunkSamples) {
                EmitAudio(std::move(pending));
                pending = std::vector<float>();
                pending.reserve(chunkSamples * 2);
            }
        }
    } while (false);

    if (client && started) client->Stop();
    if (capture) capture->Release();
    if (client) client->Release();
    if (op) op->Release();
    if (handler) handler->Release();
    if (packetEvent) CloseHandle(packetEvent);
    if (comInitialised) CoUninitialize();
    g.running = false;
    if (!errored) EmitStatus("stopped");
}

void StopCapture() {
    if (g.stopEvent) SetEvent(g.stopEvent);
    if (g.worker.joinable()) g.worker.join();
    if (g.stopEvent) {
        CloseHandle(g.stopEvent);
        g.stopEvent = nullptr;
    }
    if (g.hasTsfn) {
        g.tsfn.Release();
        g.hasTsfn = false;
    }
    g.running = false;
}

Napi::Value FindProcess(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "findProcess(exeName) expects a string").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::u16string name16 = info[0].As<Napi::String>().Utf16Value();
    std::wstring name(name16.begin(), name16.end());
    return Napi::Number::New(env, static_cast<double>(FindRootProcess(name)));
}

Napi::Value Start(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "start(pid, sampleRate, callback)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    const DWORD pid = info[0].As<Napi::Number>().Uint32Value();
    const UINT32 rate = info[1].As<Napi::Number>().Uint32Value();
    if (pid == 0 || rate < 8000 || rate > 384000) {
        Napi::RangeError::New(env, "invalid pid or sample rate").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    StopCapture();  // one capture at a time

    g.tsfn = Napi::ThreadSafeFunction::New(env, info[2].As<Napi::Function>(), "SpotifyCapture", 256, 1);
    g.tsfn.Unref(env);  // never keep the process alive just for this
    g.hasTsfn = true;
    g.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.running = true;
    g.worker = std::thread(CaptureThread, pid, rate);
    return Napi::Boolean::New(env, true);
}

Napi::Value Stop(const Napi::CallbackInfo& info) {
    StopCapture();
    return info.Env().Undefined();
}

Napi::Value IsRunning(const Napi::CallbackInfo& info) {
    return Napi::Boolean::New(info.Env(), g.running.load());
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("findProcess", Napi::Function::New(env, FindProcess));
    exports.Set("start", Napi::Function::New(env, Start));
    exports.Set("stop", Napi::Function::New(env, Stop));
    exports.Set("isRunning", Napi::Function::New(env, IsRunning));
    env.AddCleanupHook([] { StopCapture(); });
    return exports;
}

NODE_API_MODULE(spotify_capture, Init)
