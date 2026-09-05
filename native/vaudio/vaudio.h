/*
 * vaudio.h - Custom Mic Virtual Audio Cable
 *
 * Minimal WDM/PortCls virtual audio miniport driver.
 * Creates a loopback pair: render endpoint ("Custom Mic Output") feeds
 * audio into capture endpoint ("Custom Mic Input") via a shared ring buffer.
 *
 * Based on the Microsoft MSVAD sample pattern, stripped to essentials.
 *
 * Requirements: Windows Driver Kit (WDK) 10+, PortCls framework.
 */

#ifndef _VAUDIO_H_
#define _VAUDIO_H_

#include <ntddk.h>
#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>
#include <ntstrsafe.h>

/* ── GUIDs ─────────────────────────────────────────────────────────── */

// {A5DCBF10-6530-11D2-901F-00C04FB951ED}  — device interface
// We reuse a unique GUID for our miniport topology / wave factories.

// Miniport CLSID — uniquely identifies this driver's miniport
// {8E5E2CE1-B2F4-4A3E-9C1D-7F6A8D3E5B01}
DEFINE_GUID(CLSID_VaudioMiniport,
    0x8e5e2ce1, 0xb2f4, 0x4a3e,
    0x9c, 0x1d, 0x7f, 0x6a, 0x8d, 0x3e, 0x5b, 0x01);

/* ── Format constants ──────────────────────────────────────────────── */

#define VAUDIO_SAMPLE_RATE      48000
#define VAUDIO_BITS_PER_SAMPLE  16
#define VAUDIO_CHANNELS         2
#define VAUDIO_BLOCK_ALIGN      (VAUDIO_CHANNELS * VAUDIO_BITS_PER_SAMPLE / 8)
#define VAUDIO_BYTES_PER_SEC    (VAUDIO_SAMPLE_RATE * VAUDIO_BLOCK_ALIGN)

/* Ring buffer: 100 ms at 48 kHz stereo 16-bit = 19200 bytes.
   Round up to a power-of-two-friendly size for simplicity. */
#define VAUDIO_RING_BUFFER_SIZE (32 * 1024)   /* 32 KB */

/* ── Shared ring buffer (global, kernel-mode only) ─────────────────── */

typedef struct _VAUDIO_RING_BUFFER {
    UCHAR   Data[VAUDIO_RING_BUFFER_SIZE];
    volatile LONG WriteOffset;   /* updated by render stream */
    volatile LONG ReadOffset;    /* updated by capture stream */
    KSPIN_LOCK Lock;
} VAUDIO_RING_BUFFER, *PVAUDIO_RING_BUFFER;

/* ── Pin / node / filter descriptors ───────────────────────────────── */

/* Pin IDs */
#define VAUDIO_PIN_RENDER_SINK      0   /* from system mixer → driver */
#define VAUDIO_PIN_RENDER_SOURCE    1   /* driver → speaker (bridge) */
#define VAUDIO_PIN_CAPTURE_SOURCE   0   /* bridge → driver */
#define VAUDIO_PIN_CAPTURE_SINK     1   /* driver → system (capture client) */

/* Node IDs */
#define VAUDIO_NODE_VOLUME          0

/* ── Forward declarations ──────────────────────────────────────────── */

/* Driver entry & PnP */
extern "C" DRIVER_INITIALIZE   DriverEntry;
extern "C" DRIVER_ADD_DEVICE   VaudioAddDevice;

NTSTATUS VaudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList);

/* Adapter / miniport creation */
NTSTATUS CreateMiniportWaveCyclic(
    _Out_ PUNKNOWN *Unknown,
    _In_  REFCLSID ClassId,
    _In_  PUNKNOWN UnkOuter,
    _In_  POOL_TYPE PoolType);

NTSTATUS CreateMiniportTopology(
    _Out_ PUNKNOWN *Unknown,
    _In_  REFCLSID ClassId,
    _In_  PUNKNOWN UnkOuter,
    _In_  POOL_TYPE PoolType);

/* ── Miniport class: WaveCyclic ────────────────────────────────────── */

/*
 * CVaudioMiniportWaveCyclic
 *   Implements IMiniportWaveCyclic for both render and capture.
 */
class CVaudioMiniportWaveCyclic :
    public IMiniportWaveCyclic,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CVaudioMiniportWaveCyclic);
    ~CVaudioMiniportWaveCyclic();

    /* IMiniportWaveCyclic */
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PUNKNOWN UnkAdapter,
        _In_ PRESOURCELIST ResourceList,
        _In_ PPORTWAVECYCLIC Port);

    STDMETHODIMP_(NTSTATUS) GetDescription(
        _Out_ PPCFILTER_DESCRIPTOR *Description);

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
        _In_ ULONG PinId,
        _In_ PKSDATARANGE ClientDataRange,
        _In_ PKSDATARANGE MyDataRange,
        _In_ ULONG OutputBufferLength,
        _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
             PVOID ResultantFormat,
        _Out_ PULONG ResultantFormatLength);

    STDMETHODIMP_(NTSTATUS) NewStream(
        _Out_ PMINIPORTWAVECYCLICSTREAM *Stream,
        _In_  PUNKNOWN OuterUnknown,
        _In_  POOL_TYPE PoolType,
        _In_  ULONG Pin,
        _In_  BOOLEAN Capture,
        _In_  PKSDATAFORMAT DataFormat,
        _Out_ PDMACHANNEL *DmaChannel,
        _Out_ PSERVICEGROUP *ServiceGroup);

private:
    PPORTWAVECYCLIC     m_Port;
    PVAUDIO_RING_BUFFER m_RingBuffer;

    friend class CVaudioStream;
};

/* ── Stream class ──────────────────────────────────────────────────── */

class CVaudioStream :
    public IMiniportWaveCyclicStream,
    public IDmaChannel,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CVaudioStream);
    ~CVaudioStream();

    NTSTATUS Init(
        _In_ CVaudioMiniportWaveCyclic *Miniport,
        _In_ BOOLEAN Capture,
        _In_ PKSDATAFORMAT DataFormat);

    /* IMiniportWaveCyclicStream */
    STDMETHODIMP_(NTSTATUS) SetFormat(_In_ PKSDATAFORMAT DataFormat);
    STDMETHODIMP_(ULONG) SetNotificationFreq(_In_ ULONG Interval, _Out_ PULONG FrameSize);
    STDMETHODIMP_(NTSTATUS) SetState(_In_ KSSTATE State);
    STDMETHODIMP_(NTSTATUS) GetPosition(_Out_ PULONG Position);
    STDMETHODIMP_(NTSTATUS) NormalizePhysicalPosition(_Inout_ PLONGLONG Position);
    STDMETHODIMP_(void) Silence(_Out_writes_bytes_(Count) PVOID Buffer, _In_ ULONG Count);

    /* IDmaChannel */
    STDMETHODIMP_(NTSTATUS) AllocateBuffer(_In_ ULONG BufferSize, _In_opt_ PPHYSICAL_ADDRESS Constraint);
    STDMETHODIMP_(void) FreeBuffer();
    STDMETHODIMP_(ULONG) TransferCount();
    STDMETHODIMP_(ULONG) MaximumBufferSize();
    STDMETHODIMP_(ULONG) AllocatedBufferSize();
    STDMETHODIMP_(ULONG) BufferSize();
    STDMETHODIMP_(void) SetBufferSize(_In_ ULONG BufferSize);
    STDMETHODIMP_(PVOID) SystemAddress();
    STDMETHODIMP_(PHYSICAL_ADDRESS) PhysicalAddress();
    STDMETHODIMP_(PADAPTER_OBJECT) AdapterObject();
    STDMETHODIMP_(void) CopyTo(_In_ PVOID Destination, _In_ PVOID Source, _In_ ULONG ByteCount);
    STDMETHODIMP_(void) CopyFrom(_In_ PVOID Destination, _In_ PVOID Source, _In_ ULONG ByteCount);

private:
    CVaudioMiniportWaveCyclic *m_Miniport;
    PVAUDIO_RING_BUFFER m_RingBuffer;
    BOOLEAN             m_Capture;
    KSSTATE             m_State;
    ULONG               m_DmaBufferSize;
    PVOID               m_DmaBuffer;
    ULONG               m_DmaPosition;
};

/* ── Topology miniport ─────────────────────────────────────────────── */

class CVaudioMiniportTopology :
    public IMiniportTopology,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CVaudioMiniportTopology);
    ~CVaudioMiniportTopology();

    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PUNKNOWN UnkAdapter,
        _In_ PRESOURCELIST ResourceList,
        _In_ PPORTTOPOLOGY Port);

    STDMETHODIMP_(NTSTATUS) GetDescription(
        _Out_ PPCFILTER_DESCRIPTOR *Description);

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
        _In_ ULONG PinId,
        _In_ PKSDATARANGE ClientDataRange,
        _In_ PKSDATARANGE MyDataRange,
        _In_ ULONG OutputBufferLength,
        _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
             PVOID ResultantFormat,
        _Out_ PULONG ResultantFormatLength);

private:
    PPORTTOPOLOGY m_Port;
};

#endif /* _VAUDIO_H_ */
