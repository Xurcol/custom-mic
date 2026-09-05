#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>

#include "vcam_guids.h"

// Shared memory layout:
// offset 0:  uint32_t width
// offset 4:  uint32_t height
// offset 8:  uint32_t frameIndex
// offset 12: uint32_t ready (1 = data available)
// offset 16: RGBA pixel data (width * height * 4 bytes)

static const WCHAR SHMEM_NAME[] = L"Local\\CustomMicVCam";
static const WCHAR FILTER_NAME[] = L"Custom Mic Virtual Camera";
static const int FRAME_WIDTH = 1280;
static const int FRAME_HEIGHT = 720;
static const int FRAME_RATE = 30;

struct ShmHeader {
    volatile LONG width;
    volatile LONG height;
    volatile LONG frameIndex;
    volatile LONG ready;
};

static const DWORD SHMEM_SIZE = sizeof(ShmHeader) + FRAME_WIDTH * FRAME_HEIGHT * 4;

// Forward declarations
class CVCamFilter;
class CVCamPin;

// ── IEnumPins ──
class CEnumPins : public IEnumPins {
    LONG m_ref;
    CVCamFilter* m_filter;
    int m_pos;
public:
    CEnumPins(CVCamFilter* f, int pos = 0);
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();
    STDMETHODIMP Next(ULONG c, IPin** pp, ULONG* fetched);
    STDMETHODIMP Skip(ULONG c);
    STDMETHODIMP Reset();
    STDMETHODIMP Clone(IEnumPins** pp);
};

// ── IEnumMediaTypes ──
class CEnumMediaTypes : public IEnumMediaTypes {
    LONG m_ref;
    int m_pos;
public:
    CEnumMediaTypes(int pos = 0);
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();
    STDMETHODIMP Next(ULONG c, AM_MEDIA_TYPE** pp, ULONG* fetched);
    STDMETHODIMP Skip(ULONG c);
    STDMETHODIMP Reset();
    STDMETHODIMP Clone(IEnumMediaTypes** pp);
};

// ── Output Pin ──
class CVCamPin : public IPin, public IKsPropertySet, public IAMStreamConfig, public IQualityControl {
public:
    LONG m_ref;
    CVCamFilter* m_filter;
    IPin* m_connected;
    CRITICAL_SECTION m_lock;   // guards m_connected against Disconnect racing PushLoop
    AM_MEDIA_TYPE m_mt;
    HANDLE m_thread;
    HANDLE m_stopEvent;
    bool m_flushing;

    HANDLE m_shmHandle;
    void* m_shmPtr;
    LONG m_lastFrameIndex;

    static DWORD WINAPI ThreadProc(LPVOID param);
    void PushLoop();
    void OpenSharedMemory();
    void CloseSharedMemory();
    bool ReadFrame(BYTE* rgb24buf, int w, int h);

public:
    CVCamPin(CVCamFilter* f);
    ~CVCamPin();

    void FillMediaType(AM_MEDIA_TYPE* pmt);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP Disconnect();
    STDMETHODIMP ConnectedTo(IPin** ppPin);
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt);
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo);
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pDir);
    STDMETHODIMP QueryId(LPWSTR* Id);
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum);
    STDMETHODIMP QueryInternalConnections(IPin** ppPins, ULONG* nPin);
    STDMETHODIMP EndOfStream();
    STDMETHODIMP BeginFlush();
    STDMETHODIMP EndFlush();
    STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate);

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt);
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt);
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize);
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC);

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData);
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned);
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport);

    // IQualityControl
    STDMETHODIMP Notify(IBaseFilter* pSelf, Quality q);
    STDMETHODIMP SetSink(IQualityControl* piqc);
};

// ── Filter ──
class CVCamFilter : public IBaseFilter, public IAMFilterMiscFlags {
    LONG m_ref;
    FILTER_STATE m_state;
    IFilterGraph* m_graph;
    IReferenceClock* m_clock;
    CVCamPin* m_pin;
    WCHAR m_name[128];

public:
    CVCamFilter();
    ~CVCamFilter();

    CVCamPin* GetPin() { return m_pin; }
    FILTER_STATE GetState() { return m_state; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClsID);

    // IMediaFilter
    STDMETHODIMP Stop();
    STDMETHODIMP Pause();
    STDMETHODIMP Run(REFERENCE_TIME tStart);
    STDMETHODIMP GetState(DWORD dwMSecs, FILTER_STATE* pState);
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock);
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock);

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum);
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin);
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo);
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName);
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo);

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags();
};

// ── Class Factory ──
class CVCamClassFactory : public IClassFactory {
    LONG m_ref;
public:
    CVCamClassFactory();
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();
    STDMETHODIMP CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv);
    STDMETHODIMP LockServer(BOOL fLock);
};

// DLL exports
extern LONG g_cServerLocks;
extern HMODULE g_hModule;
