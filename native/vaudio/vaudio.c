/*
 * vaudio.c - Custom Mic Virtual Audio Cable
 *
 * A minimal PortCls-based WDM virtual audio miniport driver that creates
 * two endpoints:
 *   "Custom Mic Output" (render/playback) — apps play audio here
 *   "Custom Mic Input"  (capture)         — apps record from here
 *
 * Audio written to the render endpoint is looped back into the capture
 * endpoint via a shared kernel-mode circular buffer, so anything played
 * to "Custom Mic Output" can be recorded from "Custom Mic Input".
 *
 * Format: 48 kHz, 16-bit, stereo PCM (fixed).
 *
 * Build with WDK 10+ (see accompanying build instructions).
 */

/* NOTE: Despite the .c extension this file uses C++ features (classes,
   COM-style inheritance) required by PortCls. Compile as C++ (.cpp)
   or rename accordingly. The .c name is kept per the user's request. */

#include "vaudio.h"

/* ================================================================== */
/*  Global shared ring buffer (allocated once, used by all streams)    */
/* ================================================================== */

static VAUDIO_RING_BUFFER g_RingBuffer;

static void RingBuffer_Init(PVAUDIO_RING_BUFFER rb) {
    RtlZeroMemory(rb, sizeof(*rb));
    KeInitializeSpinLock(&rb->Lock);
}

/* Write `count` bytes from `src` into the ring buffer (render side). */
static void RingBuffer_Write(PVAUDIO_RING_BUFFER rb, const UCHAR *src, ULONG count) {
    LONG wr = InterlockedCompareExchange(&rb->WriteOffset, 0, 0);
    for (ULONG i = 0; i < count; i++) {
        rb->Data[wr] = src[i];
        wr = (wr + 1) % VAUDIO_RING_BUFFER_SIZE;
    }
    InterlockedExchange(&rb->WriteOffset, wr);
}

/* Read `count` bytes from the ring buffer into `dst` (capture side).
   If not enough data is available, the remaining bytes are zeroed. */
static void RingBuffer_Read(PVAUDIO_RING_BUFFER rb, UCHAR *dst, ULONG count) {
    LONG rd = InterlockedCompareExchange(&rb->ReadOffset, 0, 0);
    LONG wr = InterlockedCompareExchange(&rb->WriteOffset, 0, 0);

    for (ULONG i = 0; i < count; i++) {
        if (rd != wr) {
            dst[i] = rb->Data[rd];
            rd = (rd + 1) % VAUDIO_RING_BUFFER_SIZE;
        } else {
            dst[i] = 0;  /* silence when no data available */
        }
    }
    InterlockedExchange(&rb->ReadOffset, rd);
}

/* ================================================================== */
/*  Data format descriptors                                           */
/* ================================================================== */

static KSDATARANGE_AUDIO g_VaudioDataRangeAudio = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    VAUDIO_CHANNELS,        /* MaximumChannels */
    VAUDIO_BITS_PER_SAMPLE, /* MinimumBitsPerSample */
    VAUDIO_BITS_PER_SAMPLE, /* MaximumBitsPerSample */
    VAUDIO_SAMPLE_RATE,     /* MinimumSampleFrequency */
    VAUDIO_SAMPLE_RATE      /* MaximumSampleFrequency */
};

static PKSDATARANGE g_VaudioDataRanges[] = {
    PKSDATARANGE(&g_VaudioDataRangeAudio)
};

/* ── Render pin descriptors ────────────────────────────────────────── */

static PCPIN_DESCRIPTOR g_RenderPins[] = {
    /* Pin 0: Sink from system mixer */
    {
        1, 1, 0,
        NULL, /* AutomationTable */
        {
            0, NULL,        /* Interfaces */
            0, NULL,        /* Mediums */
            SIZEOF_ARRAY(g_VaudioDataRanges),
            g_VaudioDataRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL, 0
        }
    },
    /* Pin 1: Source bridge (speaker endpoint) */
    {
        0, 0, 0,
        NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_VaudioDataRanges),
            g_VaudioDataRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL, 0
        }
    }
};

/* ── Capture pin descriptors ───────────────────────────────────────── */

static PCPIN_DESCRIPTOR g_CapturePins[] = {
    /* Pin 0: Source bridge (mic endpoint) */
    {
        0, 0, 0,
        NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_VaudioDataRanges),
            g_VaudioDataRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL, 0
        }
    },
    /* Pin 1: Sink to capture client */
    {
        1, 1, 0,
        NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_VaudioDataRanges),
            g_VaudioDataRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL, 0
        }
    }
};

/* ── Filter descriptors (render and capture) ───────────────────────── */

static PCFILTER_DESCRIPTOR g_RenderFilterDescriptor = {
    0, NULL,    /* Version, AutomationTable */
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_RenderPins),
    g_RenderPins,
    0, 0, NULL, /* Nodes */
    0, NULL,    /* Connections */
    0, NULL     /* Categories */
};

static PCFILTER_DESCRIPTOR g_CaptureFilterDescriptor = {
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_CapturePins),
    g_CapturePins,
    0, 0, NULL,
    0, NULL,
    0, NULL
};

/* ── Topology filter descriptor ────────────────────────────────────── */

static PCPIN_DESCRIPTOR g_TopologyPins[] = {
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, 0, NULL,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO, NULL, 0 }
    },
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, 0, NULL,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO, NULL, 0 }
    }
};

static PCCONNECTION_DESCRIPTOR g_TopologyConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }
};

static PCFILTER_DESCRIPTOR g_TopologyFilterDescriptor = {
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_TopologyPins),
    g_TopologyPins,
    0, 0, NULL,
    SIZEOF_ARRAY(g_TopologyConnections),
    g_TopologyConnections,
    0, NULL
};

/* ================================================================== */
/*  CVaudioMiniportWaveCyclic implementation                          */
/* ================================================================== */

CVaudioMiniportWaveCyclic::CVaudioMiniportWaveCyclic(PUNKNOWN pUnkOuter)
    : CUnknown(pUnkOuter), m_Port(NULL), m_RingBuffer(NULL) {}

CVaudioMiniportWaveCyclic::~CVaudioMiniportWaveCyclic() {
    if (m_Port) { m_Port->Release(); m_Port = NULL; }
}

STDMETHODIMP_(NTSTATUS) CVaudioMiniportWaveCyclic::Init(
    PUNKNOWN UnkAdapter, PRESOURCELIST ResourceList, PPORTWAVECYCLIC Port)
{
    UNREFERENCED_PARAMETER(UnkAdapter);
    UNREFERENCED_PARAMETER(ResourceList);

    m_Port = Port;
    m_Port->AddRef();
    m_RingBuffer = &g_RingBuffer;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioMiniportWaveCyclic::GetDescription(
    PPCFILTER_DESCRIPTOR *Description)
{
    /* The adapter decides render vs capture at subdevice creation;
       for simplicity both use the render descriptor (pin layout is
       overridden by the port driver based on data-flow). */
    *Description = &g_RenderFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioMiniportWaveCyclic::DataRangeIntersection(
    ULONG PinId, PKSDATARANGE ClientDataRange, PKSDATARANGE MyDataRange,
    ULONG OutputBufferLength, PVOID ResultantFormat, PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);

    /* Build a WAVEFORMATEX for our fixed format */
    if (OutputBufferLength == 0) {
        *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    KSDATAFORMAT_WAVEFORMATEX *fmt = (KSDATAFORMAT_WAVEFORMATEX *)ResultantFormat;
    RtlZeroMemory(fmt, sizeof(*fmt));

    fmt->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt->DataFormat.SampleSize = VAUDIO_BLOCK_ALIGN;

    fmt->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    fmt->WaveFormatEx.nChannels = VAUDIO_CHANNELS;
    fmt->WaveFormatEx.nSamplesPerSec = VAUDIO_SAMPLE_RATE;
    fmt->WaveFormatEx.wBitsPerSample = VAUDIO_BITS_PER_SAMPLE;
    fmt->WaveFormatEx.nBlockAlign = VAUDIO_BLOCK_ALIGN;
    fmt->WaveFormatEx.nAvgBytesPerSec = VAUDIO_BYTES_PER_SEC;

    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioMiniportWaveCyclic::NewStream(
    PMINIPORTWAVECYCLICSTREAM *Stream, PUNKNOWN OuterUnknown,
    POOL_TYPE PoolType, ULONG Pin, BOOLEAN Capture,
    PKSDATAFORMAT DataFormat, PDMACHANNEL *DmaChannel,
    PSERVICEGROUP *ServiceGroup)
{
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(OuterUnknown);
    UNREFERENCED_PARAMETER(PoolType);

    CVaudioStream *stream = new (NonPagedPool, 'VaSt')
        CVaudioStream(NULL);
    if (!stream) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = stream->Init(this, Capture, DataFormat);
    if (!NT_SUCCESS(status)) {
        stream->Release();
        return status;
    }

    *Stream = (PMINIPORTWAVECYCLICSTREAM)stream;
    *DmaChannel = (PDMACHANNEL)stream;
    *ServiceGroup = NULL;

    stream->AddRef(); /* for DmaChannel interface */
    return STATUS_SUCCESS;
}

/* ================================================================== */
/*  CVaudioStream implementation                                       */
/* ================================================================== */

CVaudioStream::CVaudioStream(PUNKNOWN pUnkOuter)
    : CUnknown(pUnkOuter),
      m_Miniport(NULL), m_RingBuffer(NULL),
      m_Capture(FALSE), m_State(KSSTATE_STOP),
      m_DmaBufferSize(0), m_DmaBuffer(NULL), m_DmaPosition(0) {}

CVaudioStream::~CVaudioStream() {
    FreeBuffer();
}

NTSTATUS CVaudioStream::Init(
    CVaudioMiniportWaveCyclic *Miniport, BOOLEAN Capture,
    PKSDATAFORMAT DataFormat)
{
    UNREFERENCED_PARAMETER(DataFormat);
    m_Miniport = Miniport;
    m_RingBuffer = Miniport->m_RingBuffer;
    m_Capture = Capture;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioStream::SetFormat(PKSDATAFORMAT DataFormat) {
    UNREFERENCED_PARAMETER(DataFormat);
    return STATUS_SUCCESS; /* fixed format, nothing to change */
}

STDMETHODIMP_(ULONG) CVaudioStream::SetNotificationFreq(ULONG Interval, PULONG FrameSize) {
    *FrameSize = (VAUDIO_BYTES_PER_SEC * Interval) / 1000;
    return Interval;
}

STDMETHODIMP_(NTSTATUS) CVaudioStream::SetState(KSSTATE State) {
    m_State = State;
    if (State == KSSTATE_STOP) {
        m_DmaPosition = 0;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioStream::GetPosition(PULONG Position) {
    *Position = m_DmaPosition;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioStream::NormalizePhysicalPosition(PLONGLONG Position) {
    UNREFERENCED_PARAMETER(Position);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CVaudioStream::Silence(PVOID Buffer, ULONG Count) {
    RtlZeroMemory(Buffer, Count);
}

/* ── IDmaChannel ───────────────────────────────────────────────────── */

STDMETHODIMP_(NTSTATUS) CVaudioStream::AllocateBuffer(
    ULONG BufferSize, PPHYSICAL_ADDRESS Constraint)
{
    UNREFERENCED_PARAMETER(Constraint);
    m_DmaBuffer = ExAllocatePoolWithTag(NonPagedPool, BufferSize, 'VaBf');
    if (!m_DmaBuffer) return STATUS_INSUFFICIENT_RESOURCES;
    m_DmaBufferSize = BufferSize;
    RtlZeroMemory(m_DmaBuffer, BufferSize);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CVaudioStream::FreeBuffer() {
    if (m_DmaBuffer) {
        ExFreePoolWithTag(m_DmaBuffer, 'VaBf');
        m_DmaBuffer = NULL;
        m_DmaBufferSize = 0;
    }
}

STDMETHODIMP_(ULONG) CVaudioStream::TransferCount()     { return m_DmaPosition; }
STDMETHODIMP_(ULONG) CVaudioStream::MaximumBufferSize()  { return m_DmaBufferSize; }
STDMETHODIMP_(ULONG) CVaudioStream::AllocatedBufferSize(){ return m_DmaBufferSize; }
STDMETHODIMP_(ULONG) CVaudioStream::BufferSize()         { return m_DmaBufferSize; }
STDMETHODIMP_(void)  CVaudioStream::SetBufferSize(ULONG s){ m_DmaBufferSize = s; }
STDMETHODIMP_(PVOID) CVaudioStream::SystemAddress()      { return m_DmaBuffer; }

STDMETHODIMP_(PHYSICAL_ADDRESS) CVaudioStream::PhysicalAddress() {
    PHYSICAL_ADDRESS pa = { 0 };
    return pa;
}

STDMETHODIMP_(PADAPTER_OBJECT) CVaudioStream::AdapterObject() { return NULL; }

/*
 * CopyTo — called by PortCls when render data arrives.
 * The system has written audio into the DMA buffer; we copy it to the
 * ring buffer so the capture side can pick it up.
 */
STDMETHODIMP_(void) CVaudioStream::CopyTo(
    PVOID Destination, PVOID Source, ULONG ByteCount)
{
    RtlCopyMemory(Destination, Source, ByteCount);

    /* If this is a render stream, push data into the shared ring buffer */
    if (!m_Capture) {
        RingBuffer_Write(m_RingBuffer, (const UCHAR *)Source, ByteCount);
    }

    m_DmaPosition += ByteCount;
    if (m_DmaPosition >= m_DmaBufferSize) {
        m_DmaPosition = 0;
    }
}

/*
 * CopyFrom — called by PortCls when capture needs data.
 * We pull audio from the shared ring buffer (loopback).
 */
STDMETHODIMP_(void) CVaudioStream::CopyFrom(
    PVOID Destination, PVOID Source, ULONG ByteCount)
{
    if (m_Capture) {
        /* Read loopback audio from the ring buffer */
        RingBuffer_Read(m_RingBuffer, (UCHAR *)Destination, ByteCount);
    } else {
        RtlCopyMemory(Destination, Source, ByteCount);
    }

    m_DmaPosition += ByteCount;
    if (m_DmaPosition >= m_DmaBufferSize) {
        m_DmaPosition = 0;
    }
}

/* ================================================================== */
/*  CVaudioMiniportTopology implementation                             */
/* ================================================================== */

CVaudioMiniportTopology::CVaudioMiniportTopology(PUNKNOWN pUnkOuter)
    : CUnknown(pUnkOuter), m_Port(NULL) {}

CVaudioMiniportTopology::~CVaudioMiniportTopology() {
    if (m_Port) { m_Port->Release(); m_Port = NULL; }
}

STDMETHODIMP_(NTSTATUS) CVaudioMiniportTopology::Init(
    PUNKNOWN UnkAdapter, PRESOURCELIST ResourceList, PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(UnkAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    m_Port = Port;
    m_Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioMiniportTopology::GetDescription(
    PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &g_TopologyFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CVaudioMiniportTopology::DataRangeIntersection(
    ULONG PinId, PKSDATARANGE ClientDataRange, PKSDATARANGE MyDataRange,
    ULONG OutputBufferLength, PVOID ResultantFormat, PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    *ResultantFormatLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}

/* ================================================================== */
/*  Factory functions                                                  */
/* ================================================================== */

NTSTATUS CreateMiniportWaveCyclic(
    PUNKNOWN *Unknown, REFCLSID ClassId,
    PUNKNOWN UnkOuter, POOL_TYPE PoolType)
{
    UNREFERENCED_PARAMETER(ClassId);
    UNREFERENCED_PARAMETER(PoolType);

    CVaudioMiniportWaveCyclic *p = new (NonPagedPool, 'VaWv')
        CVaudioMiniportWaveCyclic(UnkOuter);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;

    *Unknown = PUNKNOWN((PMINIPORTWAVECYCLIC)p);
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

NTSTATUS CreateMiniportTopology(
    PUNKNOWN *Unknown, REFCLSID ClassId,
    PUNKNOWN UnkOuter, POOL_TYPE PoolType)
{
    UNREFERENCED_PARAMETER(ClassId);
    UNREFERENCED_PARAMETER(PoolType);

    CVaudioMiniportTopology *p = new (NonPagedPool, 'VaTp')
        CVaudioMiniportTopology(UnkOuter);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;

    *Unknown = PUNKNOWN((PMINIPORTTOPOLOGY)p);
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

/* ================================================================== */
/*  Adapter / PnP startup                                             */
/* ================================================================== */

/*
 * StartDevice — called by the port driver when the device starts.
 * We create and register two subdevices: WaveCyclic (render) and
 * WaveCyclic (capture), plus their topology miniports.
 */
#pragma code_seg("PAGE")
NTSTATUS VaudioStartDevice(
    PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);

    NTSTATUS status;
    PUNKNOWN unknownWave = NULL;
    PUNKNOWN unknownTopo = NULL;
    PPORTWAVECYCLIC portWave = NULL;
    PPORTTOPOLOGY portTopo = NULL;

    /* Initialize the global ring buffer */
    RingBuffer_Init(&g_RingBuffer);

    /* ── Render subdevice ──────────────────────────────────────────── */

    /* Create wave port */
    status = PcNewPort(&portWave, IID_IPortWaveCyclic);
    if (!NT_SUCCESS(status)) goto cleanup;

    /* Create wave miniport */
    status = CreateMiniportWaveCyclic(&unknownWave, CLSID_VaudioMiniport,
                                      NULL, NonPagedPool);
    if (!NT_SUCCESS(status)) goto cleanup;

    /* Create topology port */
    status = PcNewPort(&portTopo, IID_IPortTopology);
    if (!NT_SUCCESS(status)) goto cleanup;

    /* Create topology miniport */
    status = CreateMiniportTopology(&unknownTopo, CLSID_VaudioMiniport,
                                    NULL, NonPagedPool);
    if (!NT_SUCCESS(status)) goto cleanup;

    /* Register render subdevice */
    {
        UNICODE_STRING renderName;
        RtlInitUnicodeString(&renderName, L"CustomMicOutput");
        status = PcRegisterSubdevice(DeviceObject, &renderName,
                                      portWave);
        if (!NT_SUCCESS(status)) goto cleanup;
    }

    /* ── Capture subdevice (reuse the same miniport type) ──────────── */
    {
        PUNKNOWN unknownWaveCapture = NULL;
        PPORTWAVECYCLIC portWaveCapture = NULL;

        status = PcNewPort(&portWaveCapture, IID_IPortWaveCyclic);
        if (!NT_SUCCESS(status)) goto cleanup;

        status = CreateMiniportWaveCyclic(&unknownWaveCapture,
                                          CLSID_VaudioMiniport,
                                          NULL, NonPagedPool);
        if (!NT_SUCCESS(status)) {
            portWaveCapture->Release();
            goto cleanup;
        }

        UNICODE_STRING captureName;
        RtlInitUnicodeString(&captureName, L"CustomMicInput");
        status = PcRegisterSubdevice(DeviceObject, &captureName,
                                      portWaveCapture);
        if (unknownWaveCapture) unknownWaveCapture->Release();
        if (portWaveCapture) portWaveCapture->Release();
        if (!NT_SUCCESS(status)) goto cleanup;
    }

    /* Register topology */
    {
        UNICODE_STRING topoName;
        RtlInitUnicodeString(&topoName, L"CustomMicTopology");
        status = PcRegisterSubdevice(DeviceObject, &topoName,
                                      portTopo);
    }

cleanup:
    if (unknownWave) unknownWave->Release();
    if (unknownTopo) unknownTopo->Release();
    if (portWave) portWave->Release();
    if (portTopo) portTopo->Release();

    return status;
}
#pragma code_seg()

/* ================================================================== */
/*  DriverEntry & AddDevice                                            */
/* ================================================================== */

#pragma code_seg("INIT")
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    return PcInitializeAdapterDriver(DriverObject, RegistryPath,
                                      (PDRIVER_ADD_DEVICE)VaudioAddDevice);
}
#pragma code_seg()

#pragma code_seg("PAGE")
extern "C" NTSTATUS VaudioAddDevice(
    PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    PAGED_CODE();
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
                               VaudioStartDevice, MAX_MINIPORTS, 0);
}
#pragma code_seg()
