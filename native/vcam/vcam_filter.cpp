#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <olectl.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <cstring>

#include "vcam_guids.h"
#include "vcam_filter.h"

// Globals
LONG g_cServerLocks = 0;
HMODULE g_hModule = NULL;

// Helper: deep copy AM_MEDIA_TYPE
static HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
    *dst = *src;
    if (src->cbFormat > 0 && src->pbFormat) {
        dst->pbFormat = (BYTE*)CoTaskMemAlloc(src->cbFormat);
        if (!dst->pbFormat) return E_OUTOFMEMORY;
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    }
    if (dst->pUnk) dst->pUnk->AddRef();
    return S_OK;
}

static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat > 0 && mt.pbFormat) { CoTaskMemFree(mt.pbFormat); mt.pbFormat = NULL; mt.cbFormat = 0; }
    if (mt.pUnk) { mt.pUnk->Release(); mt.pUnk = NULL; }
}

static AM_MEDIA_TYPE* AllocMediaType(const AM_MEDIA_TYPE* src) {
    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!pmt) return NULL;
    if (FAILED(CopyMediaType(pmt, src))) { CoTaskMemFree(pmt); return NULL; }
    return pmt;
}

static void DeleteMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return;
    FreeMediaType(*pmt);
    CoTaskMemFree(pmt);
}

// ══════════════════════════════════════════════
//  CEnumPins
// ══════════════════════════════════════════════

CEnumPins::CEnumPins(CVCamFilter* f, int pos) : m_ref(1), m_filter(f), m_pos(pos) { if (m_filter) m_filter->AddRef(); }

STDMETHODIMP CEnumPins::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IEnumPins) { *ppv = static_cast<IEnumPins*>(this); AddRef(); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CEnumPins::AddRef() { return InterlockedIncrement(&m_ref); }
STDMETHODIMP_(ULONG) CEnumPins::Release() { LONG r = InterlockedDecrement(&m_ref); if (r == 0) { if (m_filter) m_filter->Release(); delete this; } return r; }

STDMETHODIMP CEnumPins::Next(ULONG c, IPin** pp, ULONG* fetched) {
    ULONG n = 0;
    while (n < c && m_pos == 0) {
        pp[n] = m_filter->GetPin();
        pp[n]->AddRef();
        m_pos++;
        n++;
    }
    if (fetched) *fetched = n;
    return (n == c) ? S_OK : S_FALSE;
}
STDMETHODIMP CEnumPins::Skip(ULONG c) { m_pos += (int)c; return (m_pos <= 1) ? S_OK : S_FALSE; }
STDMETHODIMP CEnumPins::Reset() { m_pos = 0; return S_OK; }
STDMETHODIMP CEnumPins::Clone(IEnumPins** pp) { *pp = new CEnumPins(m_filter, m_pos); return S_OK; }

// ══════════════════════════════════════════════
//  CEnumMediaTypes
// ══════════════════════════════════════════════

CEnumMediaTypes::CEnumMediaTypes(int pos) : m_ref(1), m_pos(pos) {}

STDMETHODIMP CEnumMediaTypes::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) { *ppv = static_cast<IEnumMediaTypes*>(this); AddRef(); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CEnumMediaTypes::AddRef() { return InterlockedIncrement(&m_ref); }
STDMETHODIMP_(ULONG) CEnumMediaTypes::Release() { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }

STDMETHODIMP CEnumMediaTypes::Next(ULONG c, AM_MEDIA_TYPE** pp, ULONG* fetched) {
    ULONG n = 0;
    while (n < c && m_pos == 0) {
        AM_MEDIA_TYPE mt;
        ZeroMemory(&mt, sizeof(mt));
        // Build RGB24 media type
        mt.majortype = MEDIATYPE_Video;
        mt.subtype = MEDIASUBTYPE_RGB24;
        mt.bFixedSizeSamples = TRUE;
        mt.bTemporalCompression = FALSE;
        mt.formattype = FORMAT_VideoInfo;
        mt.cbFormat = sizeof(VIDEOINFOHEADER);
        mt.pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
        ZeroMemory(mt.pbFormat, sizeof(VIDEOINFOHEADER));
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)mt.pbFormat;
        vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        vih->bmiHeader.biWidth = FRAME_WIDTH;
        vih->bmiHeader.biHeight = FRAME_HEIGHT;
        vih->bmiHeader.biPlanes = 1;
        vih->bmiHeader.biBitCount = 24;
        vih->bmiHeader.biCompression = BI_RGB;
        vih->bmiHeader.biSizeImage = FRAME_WIDTH * FRAME_HEIGHT * 3;
        vih->AvgTimePerFrame = 10000000LL / FRAME_RATE;
        mt.lSampleSize = vih->bmiHeader.biSizeImage;

        pp[n] = AllocMediaType(&mt);
        FreeMediaType(mt);
        m_pos++;
        n++;
    }
    if (fetched) *fetched = n;
    return (n == c) ? S_OK : S_FALSE;
}
STDMETHODIMP CEnumMediaTypes::Skip(ULONG c) { m_pos += (int)c; return (m_pos <= 1) ? S_OK : S_FALSE; }
STDMETHODIMP CEnumMediaTypes::Reset() { m_pos = 0; return S_OK; }
STDMETHODIMP CEnumMediaTypes::Clone(IEnumMediaTypes** pp) { *pp = new CEnumMediaTypes(m_pos); return S_OK; }

// ══════════════════════════════════════════════
//  CVCamPin
// ══════════════════════════════════════════════

CVCamPin::CVCamPin(CVCamFilter* f)
    : m_ref(1), m_filter(f), m_connected(NULL), m_thread(NULL), m_stopEvent(NULL),
      m_flushing(false), m_shmHandle(NULL), m_shmPtr(NULL), m_lastFrameIndex(-1) {
    InitializeCriticalSection(&m_lock);
    ZeroMemory(&m_mt, sizeof(m_mt));
    FillMediaType(&m_mt);
    m_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

CVCamPin::~CVCamPin() {
    CloseSharedMemory();
    FreeMediaType(m_mt);
    if (m_connected) m_connected->Release();
    if (m_stopEvent) CloseHandle(m_stopEvent);
    DeleteCriticalSection(&m_lock);
}

// The pin publishes exactly one format and the push loop writes exactly one
// frame size. Accepting anything else - a different resolution in particular -
// leaves the downstream allocator sized for the negotiated type while we keep
// writing FRAME_WIDTH*FRAME_HEIGHT*3 bytes into it, which corrupts the heap of
// whichever process hosts the graph.
static bool IsOurFormat(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return false;
    if (pmt->majortype != MEDIATYPE_Video) return false;
    if (pmt->subtype != MEDIASUBTYPE_RGB24) return false;
    if (pmt->formattype != FORMAT_VideoInfo) return false;
    if (!pmt->pbFormat || pmt->cbFormat < sizeof(VIDEOINFOHEADER)) return false;
    const VIDEOINFOHEADER* vih = (const VIDEOINFOHEADER*)pmt->pbFormat;
    if (vih->bmiHeader.biWidth != FRAME_WIDTH) return false;
    if (abs(vih->bmiHeader.biHeight) != FRAME_HEIGHT) return false;
    if (vih->bmiHeader.biBitCount != 24) return false;
    if (vih->bmiHeader.biCompression != BI_RGB) return false;
    return true;
}

void CVCamPin::FillMediaType(AM_MEDIA_TYPE* pmt) {
    ZeroMemory(pmt, sizeof(*pmt));
    pmt->majortype = MEDIATYPE_Video;
    pmt->subtype = MEDIASUBTYPE_RGB24;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    ZeroMemory(pmt->pbFormat, sizeof(VIDEOINFOHEADER));
    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = FRAME_WIDTH;
    vih->bmiHeader.biHeight = FRAME_HEIGHT;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = 24;
    vih->bmiHeader.biCompression = BI_RGB;
    vih->bmiHeader.biSizeImage = FRAME_WIDTH * FRAME_HEIGHT * 3;
    vih->AvgTimePerFrame = 10000000LL / FRAME_RATE;
    pmt->lSampleSize = vih->bmiHeader.biSizeImage;
}

STDMETHODIMP CVCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown)           { *ppv = static_cast<IPin*>(this); AddRef(); return S_OK; }
    if (riid == IID_IPin)               { *ppv = static_cast<IPin*>(this); AddRef(); return S_OK; }
    if (riid == IID_IKsPropertySet)     { *ppv = static_cast<IKsPropertySet*>(this); AddRef(); return S_OK; }
    if (riid == IID_IAMStreamConfig)    { *ppv = static_cast<IAMStreamConfig*>(this); AddRef(); return S_OK; }
    if (riid == IID_IQualityControl)    { *ppv = static_cast<IQualityControl*>(this); AddRef(); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CVCamPin::AddRef() { return InterlockedIncrement(&m_ref); }
STDMETHODIMP_(ULONG) CVCamPin::Release() { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }

// IPin
STDMETHODIMP CVCamPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    if (m_connected) return VFW_E_ALREADY_CONNECTED;

    // A partial or mismatched type is refused rather than negotiated: we have
    // one format, and pretending otherwise is what corrupts the client's heap.
    if (pmt && !IsOurFormat(pmt)) return VFW_E_TYPE_NOT_ACCEPTED;

    AM_MEDIA_TYPE proposed;
    FillMediaType(&proposed);

    HRESULT hr = pReceivePin->ReceiveConnection(static_cast<IPin*>(this), &proposed);
    if (SUCCEEDED(hr)) {
        EnterCriticalSection(&m_lock);
        FreeMediaType(m_mt);
        CopyMediaType(&m_mt, &proposed);
        m_connected = pReceivePin;
        m_connected->AddRef();
        LeaveCriticalSection(&m_lock);
    }
    FreeMediaType(proposed);
    return hr;
}

STDMETHODIMP CVCamPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }

STDMETHODIMP CVCamPin::Disconnect() {
    EnterCriticalSection(&m_lock);
    IPin* old = m_connected;
    m_connected = NULL;
    LeaveCriticalSection(&m_lock);
    if (!old) return S_FALSE;
    old->Release();
    return S_OK;
}

STDMETHODIMP CVCamPin::ConnectedTo(IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    if (!m_connected) { *ppPin = NULL; return VFW_E_NOT_CONNECTED; }
    *ppPin = m_connected;
    m_connected->AddRef();
    return S_OK;
}

STDMETHODIMP CVCamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (!m_connected) return VFW_E_NOT_CONNECTED;
    return CopyMediaType(pmt, &m_mt);
}

STDMETHODIMP CVCamPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = m_filter;
    if (m_filter) m_filter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Output");
    return S_OK;
}

STDMETHODIMP CVCamPin::QueryDirection(PIN_DIRECTION* pDir) { if (!pDir) return E_POINTER; *pDir = PINDIR_OUTPUT; return S_OK; }
STDMETHODIMP CVCamPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const size_t chars = 7;                       // L"Output" plus terminator
    *Id = (LPWSTR)CoTaskMemAlloc(chars * sizeof(WCHAR));
    if (!*Id) return E_OUTOFMEMORY;
    wcscpy_s(*Id, chars, L"Output");
    return S_OK;
}

STDMETHODIMP CVCamPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    return IsOurFormat(pmt) ? S_OK : S_FALSE;
}

STDMETHODIMP CVCamPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumMediaTypes();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CVCamPin::QueryInternalConnections(IPin**, ULONG* nPin) { return E_NOTIMPL; }
STDMETHODIMP CVCamPin::EndOfStream() { return E_UNEXPECTED; }
STDMETHODIMP CVCamPin::BeginFlush() { m_flushing = true; return S_OK; }
STDMETHODIMP CVCamPin::EndFlush() { m_flushing = false; return S_OK; }
STDMETHODIMP CVCamPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

// IAMStreamConfig
STDMETHODIMP CVCamPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    // Callers routinely try to set their preferred resolution here. Accepting
    // one we do not actually produce is the bug that crashed the client.
    if (!IsOurFormat(pmt)) return VFW_E_INVALIDMEDIATYPE;
    EnterCriticalSection(&m_lock);
    FreeMediaType(m_mt);
    HRESULT hr = CopyMediaType(&m_mt, pmt);
    LeaveCriticalSection(&m_lock);
    return hr;
}

STDMETHODIMP CVCamPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = AllocMediaType(&m_mt);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CVCamPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = 1;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP CVCamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex != 0) return S_FALSE;

    AM_MEDIA_TYPE mt;
    FillMediaType(&mt);
    *ppmt = AllocMediaType(&mt);
    FreeMediaType(mt);
    if (!*ppmt) return E_OUTOFMEMORY;

    VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
    ZeroMemory(caps, sizeof(*caps));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = 0;
    caps->InputSize.cx = FRAME_WIDTH;
    caps->InputSize.cy = FRAME_HEIGHT;
    caps->MinCroppingSize.cx = FRAME_WIDTH;
    caps->MinCroppingSize.cy = FRAME_HEIGHT;
    caps->MaxCroppingSize.cx = FRAME_WIDTH;
    caps->MaxCroppingSize.cy = FRAME_HEIGHT;
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->MinOutputSize.cx = FRAME_WIDTH;
    caps->MinOutputSize.cy = FRAME_HEIGHT;
    caps->MaxOutputSize.cx = FRAME_WIDTH;
    caps->MaxOutputSize.cy = FRAME_HEIGHT;
    caps->OutputGranularityX = 1;
    caps->OutputGranularityY = 1;
    caps->MinFrameInterval = 10000000LL / FRAME_RATE;
    caps->MaxFrameInterval = 10000000LL / FRAME_RATE;
    caps->MinBitsPerSecond = FRAME_WIDTH * FRAME_HEIGHT * 24 * FRAME_RATE;
    caps->MaxBitsPerSecond = FRAME_WIDTH * FRAME_HEIGHT * 24 * FRAME_RATE;

    return S_OK;
}

// IKsPropertySet — required for DirectShow to recognize this as a capture device
STDMETHODIMP CVCamPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }

STDMETHODIMP CVCamPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID, DWORD, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
    *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
    if (pcbReturned) *pcbReturned = sizeof(GUID);
    return S_OK;
}

STDMETHODIMP CVCamPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// IQualityControl
STDMETHODIMP CVCamPin::Notify(IBaseFilter*, Quality) { return E_NOTIMPL; }
STDMETHODIMP CVCamPin::SetSink(IQualityControl*) { return S_OK; }

// Shared memory
void CVCamPin::OpenSharedMemory() {
    if (m_shmHandle) return;
    m_shmHandle = OpenFileMappingW(FILE_MAP_READ, FALSE, SHMEM_NAME);
    if (m_shmHandle) {
        m_shmPtr = MapViewOfFile(m_shmHandle, FILE_MAP_READ, 0, 0, SHMEM_SIZE);
    }
    m_lastFrameIndex = -1;
}

void CVCamPin::CloseSharedMemory() {
    if (m_shmPtr) { UnmapViewOfFile(m_shmPtr); m_shmPtr = NULL; }
    if (m_shmHandle) { CloseHandle(m_shmHandle); m_shmHandle = NULL; }
}

bool CVCamPin::ReadFrame(BYTE* rgb24buf, int w, int h) {
    if (!m_shmPtr) {
        // Try to open each time — sender may start later
        OpenSharedMemory();
        if (!m_shmPtr) return false;
    }

    ShmHeader* hdr = (ShmHeader*)m_shmPtr;
    LONG ready = InterlockedCompareExchange(&hdr->ready, 1, 1);
    if (!ready) return false;

    LONG fi = InterlockedCompareExchange(&hdr->frameIndex, 0, 0);
    if (fi == m_lastFrameIndex) return false;
    m_lastFrameIndex = fi;

    int srcW = (int)InterlockedCompareExchange(&hdr->width, 0, 0);
    int srcH = (int)InterlockedCompareExchange(&hdr->height, 0, 0);
    if (srcW != w || srcH != h) return false;

    const BYTE* rgba = (const BYTE*)m_shmPtr + sizeof(ShmHeader);

    // Convert RGBA to bottom-up RGB24 (DirectShow RGB24 is bottom-up)
    for (int y = 0; y < h; y++) {
        const BYTE* srcRow = rgba + (y * w * 4);
        BYTE* dstRow = rgb24buf + ((h - 1 - y) * w * 3); // flip vertically
        for (int x = 0; x < w; x++) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // B
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // R
        }
    }
    return true;
}

// Push thread
DWORD WINAPI CVCamPin::ThreadProc(LPVOID param) {
    ((CVCamPin*)param)->PushLoop();
    return 0;
}

void CVCamPin::PushLoop() {
    const DWORD frameMs = 1000 / FRAME_RATE;
    const DWORD frameSize = FRAME_WIDTH * FRAME_HEIGHT * 3;

    // Take our own reference under the lock: Disconnect() can run at any point
    // (the user toggling their camera off), and releasing the pin out from
    // under this thread is a use-after-free.
    EnterCriticalSection(&m_lock);
    IPin* connected = m_connected;
    if (connected) connected->AddRef();
    LeaveCriticalSection(&m_lock);
    if (!connected) return;

    IMemInputPin* pInput = NULL;
    HRESULT hrQI = connected->QueryInterface(IID_IMemInputPin, (void**)&pInput);
    connected->Release();
    if (FAILED(hrQI) || !pInput) return;

    // Get or create allocator
    IMemAllocator* pAlloc = NULL;
    HRESULT hr = pInput->GetAllocator(&pAlloc);
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, NULL, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)&pAlloc);
        if (FAILED(hr)) { pInput->Release(); return; }
    }

    ALLOCATOR_PROPERTIES props, actual;
    props.cBuffers = 1;
    props.cbBuffer = frameSize;
    props.cbAlign = 1;
    props.cbPrefix = 0;
    ZeroMemory(&actual, sizeof(actual));
    hr = pAlloc->SetProperties(&props, &actual);
    if (FAILED(hr)) { pAlloc->Release(); pInput->Release(); return; }
    // SetProperties reports what was actually granted, which can be smaller
    // than what we asked for. Writing a full frame into a short buffer is the
    // heap overflow that took the client process down with it.
    if ((DWORD)actual.cbBuffer < frameSize) { pAlloc->Release(); pInput->Release(); return; }

    hr = pInput->NotifyAllocator(pAlloc, FALSE);
    if (FAILED(hr)) { pAlloc->Release(); pInput->Release(); return; }

    hr = pAlloc->Commit();
    if (FAILED(hr)) { pAlloc->Release(); pInput->Release(); return; }

    OpenSharedMemory();

    REFERENCE_TIME rtStart = 0;
    REFERENCE_TIME rtFrameDur = 10000000LL / FRAME_RATE;

    while (WaitForSingleObject(m_stopEvent, frameMs) == WAIT_TIMEOUT) {
        if (m_flushing) continue;

        IMediaSample* pSample = NULL;
        hr = pAlloc->GetBuffer(&pSample, NULL, NULL, 0);
        if (FAILED(hr)) continue;

        BYTE* pData = NULL;
        if (FAILED(pSample->GetPointer(&pData)) || !pData) { pSample->Release(); continue; }
        // Re-check per sample: the allocator can hand back a buffer smaller
        // than negotiated, and this write is unbounded otherwise.
        if ((DWORD)pSample->GetSize() < frameSize) { pSample->Release(); continue; }
        pSample->SetActualDataLength(frameSize);

        if (!ReadFrame(pData, FRAME_WIDTH, FRAME_HEIGHT)) {
            // No new frame — send black
            ZeroMemory(pData, frameSize);
        }

        pSample->SetTime(&rtStart, NULL);
        REFERENCE_TIME rtEnd = rtStart + rtFrameDur;
        pSample->SetTime(&rtStart, &rtEnd);
        pSample->SetSyncPoint(TRUE);
        rtStart = rtEnd;

        hr = pInput->Receive(pSample);
        pSample->Release();
        if (FAILED(hr)) break;
    }

    pAlloc->Decommit();
    pAlloc->Release();
    pInput->Release();
    CloseSharedMemory();
}

// ══════════════════════════════════════════════
//  CVCamFilter
// ══════════════════════════════════════════════

CVCamFilter::CVCamFilter() : m_ref(1), m_state(State_Stopped), m_graph(NULL), m_clock(NULL) {
    m_pin = new CVCamPin(this);
    m_name[0] = L'\0';
}

CVCamFilter::~CVCamFilter() {
    if (m_pin) m_pin->Release();
    if (m_clock) m_clock->Release();
}

STDMETHODIMP CVCamFilter::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown)           { *ppv = static_cast<IBaseFilter*>(this); AddRef(); return S_OK; }
    if (riid == IID_IPersist)           { *ppv = static_cast<IPersist*>(this); AddRef(); return S_OK; }
    if (riid == IID_IMediaFilter)       { *ppv = static_cast<IMediaFilter*>(this); AddRef(); return S_OK; }
    if (riid == IID_IBaseFilter)        { *ppv = static_cast<IBaseFilter*>(this); AddRef(); return S_OK; }
    if (riid == IID_IAMFilterMiscFlags) { *ppv = static_cast<IAMFilterMiscFlags*>(this); AddRef(); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CVCamFilter::AddRef() { return InterlockedIncrement(&m_ref); }
STDMETHODIMP_(ULONG) CVCamFilter::Release() { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }

STDMETHODIMP CVCamFilter::GetClassID(CLSID* pClsID) { *pClsID = CLSID_CustomMicVCam; return S_OK; }

STDMETHODIMP CVCamFilter::Stop() {
    if (m_pin->m_thread) {
        SetEvent(m_pin->m_stopEvent);
        WaitForSingleObject(m_pin->m_thread, 3000);
        CloseHandle(m_pin->m_thread);
        m_pin->m_thread = NULL;
        ResetEvent(m_pin->m_stopEvent);
    }
    m_state = State_Stopped;
    return S_OK;
}

STDMETHODIMP CVCamFilter::Pause() {
    if (m_state == State_Stopped) {
        // Start push thread
        ResetEvent(m_pin->m_stopEvent);
        m_pin->m_thread = CreateThread(NULL, 0, CVCamPin::ThreadProc, m_pin, 0, NULL);
    }
    m_state = State_Paused;
    return S_OK;
}

STDMETHODIMP CVCamFilter::Run(REFERENCE_TIME) {
    if (m_state == State_Stopped) Pause();
    m_state = State_Running;
    return S_OK;
}

STDMETHODIMP CVCamFilter::GetState(DWORD, FILTER_STATE* pState) { *pState = m_state; return S_OK; }

STDMETHODIMP CVCamFilter::SetSyncSource(IReferenceClock* pClock) {
    if (m_clock) m_clock->Release();
    m_clock = pClock;
    if (m_clock) m_clock->AddRef();
    return S_OK;
}

STDMETHODIMP CVCamFilter::GetSyncSource(IReferenceClock** ppClock) {
    *ppClock = m_clock;
    if (m_clock) m_clock->AddRef();
    return S_OK;
}

STDMETHODIMP CVCamFilter::EnumPins(IEnumPins** ppEnum) { *ppEnum = new CEnumPins(this); return S_OK; }

STDMETHODIMP CVCamFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (wcscmp(Id, L"Output") == 0) { *ppPin = m_pin; m_pin->AddRef(); return S_OK; }
    *ppPin = NULL; return VFW_E_NOT_FOUND;
}

STDMETHODIMP CVCamFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    wcscpy_s(pInfo->achName, FILTER_NAME);
    pInfo->pGraph = m_graph;
    if (m_graph) m_graph->AddRef();
    return S_OK;
}

STDMETHODIMP CVCamFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    m_graph = pGraph; // weak ref, no AddRef
    if (pName) wcscpy_s(m_name, pName);
    else m_name[0] = L'\0';
    return S_OK;
}

STDMETHODIMP CVCamFilter::QueryVendorInfo(LPWSTR*) { return E_NOTIMPL; }

STDMETHODIMP_(ULONG) CVCamFilter::GetMiscFlags() { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

// ══════════════════════════════════════════════
//  CVCamClassFactory
// ══════════════════════════════════════════════

CVCamClassFactory::CVCamClassFactory() : m_ref(1) {}

STDMETHODIMP CVCamClassFactory::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CVCamClassFactory::AddRef() { return InterlockedIncrement(&m_ref); }
STDMETHODIMP_(ULONG) CVCamClassFactory::Release() { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }

STDMETHODIMP CVCamClassFactory::CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) {
    if (pOuter) return CLASS_E_NOAGGREGATION;
    CVCamFilter* pFilter = new CVCamFilter();
    HRESULT hr = pFilter->QueryInterface(riid, ppv);
    pFilter->Release();
    return hr;
}

STDMETHODIMP CVCamClassFactory::LockServer(BOOL fLock) {
    if (fLock) InterlockedIncrement(&g_cServerLocks);
    else InterlockedDecrement(&g_cServerLocks);
    return S_OK;
}

// ══════════════════════════════════════════════
//  DLL Entry Points
// ══════════════════════════════════════════════

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (rclsid != CLSID_CustomMicVCam) return CLASS_E_CLASSNOTAVAILABLE;
    CVCamClassFactory* pFactory = new CVCamClassFactory();
    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_cServerLocks == 0) ? S_OK : S_FALSE;
}

// Helper to convert CLSID to string
static void GuidToString(REFGUID guid, WCHAR* buf, int bufLen) {
    StringFromGUID2(guid, buf, bufLen);
}

STDAPI DllRegisterServer() {
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);

    WCHAR clsidStr[64];
    GuidToString(CLSID_CustomMicVCam, clsidStr, 64);

    HKEY hKey;
    WCHAR keyPath[256];
    DWORD disp;

    // Register CLSID
    wsprintfW(keyPath, L"CLSID\\%s", clsidStr);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, NULL, 0, KEY_WRITE, NULL, &hKey, &disp) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)FILTER_NAME, (DWORD)(wcslen(FILTER_NAME) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }

    wsprintfW(keyPath, L"CLSID\\%s\\InprocServer32", clsidStr);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, NULL, 0, KEY_WRITE, NULL, &hKey, &disp) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)dllPath, (DWORD)(wcslen(dllPath) + 1) * sizeof(WCHAR));
        const WCHAR* threading = L"Both";
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (BYTE*)threading, (DWORD)(wcslen(threading) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }

    // Register as a video capture source filter
    // CLSID_VideoInputDeviceCategory = {860BB310-5D01-11d0-BD3B-00A0C911CE86}
    WCHAR catClsid[64];
    GuidToString(CLSID_VideoInputDeviceCategory, catClsid, 64);

    wsprintfW(keyPath, L"CLSID\\%s\\Instance\\%s", catClsid, clsidStr);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, NULL, 0, KEY_WRITE, NULL, &hKey, &disp) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ, (BYTE*)FILTER_NAME, (DWORD)(wcslen(FILTER_NAME) + 1) * sizeof(WCHAR));
        RegSetValueExW(hKey, L"CLSID", 0, REG_SZ, (BYTE*)clsidStr, (DWORD)(wcslen(clsidStr) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }

    return S_OK;
}

STDAPI DllUnregisterServer() {
    WCHAR clsidStr[64];
    GuidToString(CLSID_CustomMicVCam, clsidStr, 64);

    WCHAR catClsid[64];
    GuidToString(CLSID_VideoInputDeviceCategory, catClsid, 64);

    WCHAR keyPath[256];

    // Remove category instance
    wsprintfW(keyPath, L"CLSID\\%s\\Instance\\%s", catClsid, clsidStr);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyPath);

    // Remove CLSID
    wsprintfW(keyPath, L"CLSID\\%s\\InprocServer32", clsidStr);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyPath);
    wsprintfW(keyPath, L"CLSID\\%s", clsidStr);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyPath);

    return S_OK;
}
