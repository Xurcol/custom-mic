#include <napi.h>
#include <windows.h>
#include <cstring>

struct ShmHeader {
    volatile LONG width;
    volatile LONG height;
    volatile LONG frameIndex;
    volatile LONG ready;
};

static HANDLE g_shmHandle = NULL;
static void* g_shmPtr = NULL;
static DWORD g_shmSize = 0;
static int g_width = 0;
static int g_height = 0;

static const WCHAR SHMEM_NAME[] = L"Local\\CustomMicVCam";

Napi::Value Open(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 2) {
        Napi::TypeError::New(env, "Expected (width, height)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int w = info[0].As<Napi::Number>().Int32Value();
    int h = info[1].As<Napi::Number>().Int32Value();

    DWORD dataSize = w * h * 4; // RGBA
    DWORD totalSize = sizeof(ShmHeader) + dataSize;

    if (g_shmHandle) {
        UnmapViewOfFile(g_shmPtr);
        CloseHandle(g_shmHandle);
        g_shmPtr = NULL;
        g_shmHandle = NULL;
    }

    g_shmHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, totalSize, SHMEM_NAME);
    if (!g_shmHandle) {
        Napi::Error::New(env, "Failed to create shared memory").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    g_shmPtr = MapViewOfFile(g_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, totalSize);
    if (!g_shmPtr) {
        CloseHandle(g_shmHandle);
        g_shmHandle = NULL;
        Napi::Error::New(env, "Failed to map shared memory").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    g_shmSize = totalSize;
    g_width = w;
    g_height = h;

    ShmHeader* hdr = (ShmHeader*)g_shmPtr;
    InterlockedExchange(&hdr->width, w);
    InterlockedExchange(&hdr->height, h);
    InterlockedExchange(&hdr->frameIndex, 0);
    InterlockedExchange(&hdr->ready, 0);

    return Napi::Boolean::New(env, true);
}

Napi::Value SendFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!g_shmPtr) {
        Napi::Error::New(env, "Shared memory not open").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() < 1 || !info[0].IsBuffer()) {
        Napi::TypeError::New(env, "Expected Buffer argument").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    DWORD expectedSize = g_width * g_height * 4;
    if (buf.Length() < expectedSize) {
        Napi::Error::New(env, "Buffer too small").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    ShmHeader* hdr = (ShmHeader*)g_shmPtr;
    uint8_t* dst = (uint8_t*)g_shmPtr + sizeof(ShmHeader);
    memcpy(dst, buf.Data(), expectedSize);
    InterlockedIncrement(&hdr->frameIndex);
    InterlockedExchange(&hdr->ready, 1);

    return env.Undefined();
}

Napi::Value Close(const Napi::CallbackInfo& info) {
    if (g_shmPtr) {
        ShmHeader* hdr = (ShmHeader*)g_shmPtr;
        InterlockedExchange(&hdr->ready, 0);
        UnmapViewOfFile(g_shmPtr);
        g_shmPtr = NULL;
    }
    if (g_shmHandle) {
        CloseHandle(g_shmHandle);
        g_shmHandle = NULL;
    }
    g_width = 0;
    g_height = 0;
    g_shmSize = 0;
    return info.Env().Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("open", Napi::Function::New(env, Open));
    exports.Set("sendFrame", Napi::Function::New(env, SendFrame));
    exports.Set("close", Napi::Function::New(env, Close));
    return exports;
}

NODE_API_MODULE(vcam_sender, Init)
