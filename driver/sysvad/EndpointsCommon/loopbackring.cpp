/*++

Module Name:

    loopbackring.cpp

Abstract:

    SoundRadar VAD: render -> loopback-capture byte ring.

    Derived work within the Microsoft sysvad virtual audio driver sample.
    Licensed under the Microsoft Public License (MS-PL); see the sysvad
    sample license terms.

    PushBytes/PopBytes run on the 1ms position-simulation timer at
    DISPATCH_LEVEL: they must stay non-paged, do no allocation and no
    debug output.

--*/

#include <sysvad.h>
#include "loopbackring.h"

//
// Single global ring: one SoundRadar VAD device per driver instance.
//
static KSPIN_LOCK g_LoopbackRingLock;
static BYTE       g_LoopbackRingBuffer[LOOPBACK_RING_CAPACITY_BYTES];
static ULONG      g_LoopbackRingReadOffset;
static ULONG      g_LoopbackRingWriteOffset;    // == read offset when empty
static ULONG      g_LoopbackRingBytesAvailable;
static ULONGLONG  g_LoopbackRingOverruns;       // bytes dropped (overwrite-oldest)
static ULONGLONG  g_LoopbackRingUnderruns;      // bytes zero-filled (pop underflow)

//
// Last render mix format seen, for render/loopback format coupling checks.
//
static BOOLEAN    g_LoopbackRenderFormatValid;
static ULONG      g_LoopbackRenderChannels;
static ULONG      g_LoopbackRenderSamplesPerSec;
static ULONG      g_LoopbackRenderBitsPerSample;

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
VOID
LoopbackRingInitialize()
{
    PAGED_CODE();

    KeInitializeSpinLock(&g_LoopbackRingLock);
    g_LoopbackRingReadOffset = 0;
    g_LoopbackRingWriteOffset = 0;
    g_LoopbackRingBytesAvailable = 0;
    g_LoopbackRingOverruns = 0;
    g_LoopbackRingUnderruns = 0;
    g_LoopbackRenderFormatValid = FALSE;
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
VOID
LoopbackRingPushBytes(
    const BYTE *Data,
    ULONG ByteCount
    )
{
    KIRQL oldIrql;

    if (ByteCount == 0)
    {
        return;
    }

    //
    // A push larger than the whole ring keeps only the newest bytes.
    //
    if (ByteCount > LOOPBACK_RING_CAPACITY_BYTES)
    {
        InterlockedAdd64((PLONG64)&g_LoopbackRingOverruns, ByteCount - LOOPBACK_RING_CAPACITY_BYTES);
        Data += ByteCount - LOOPBACK_RING_CAPACITY_BYTES;
        ByteCount = LOOPBACK_RING_CAPACITY_BYTES;
    }

    KeAcquireSpinLock(&g_LoopbackRingLock, &oldIrql);

    //
    // Overwrite-oldest on overflow.
    //
    if (ByteCount > LOOPBACK_RING_CAPACITY_BYTES - g_LoopbackRingBytesAvailable)
    {
        ULONG dropped = ByteCount - (LOOPBACK_RING_CAPACITY_BYTES - g_LoopbackRingBytesAvailable);
        g_LoopbackRingReadOffset = (g_LoopbackRingReadOffset + dropped) % LOOPBACK_RING_CAPACITY_BYTES;
        g_LoopbackRingBytesAvailable -= dropped;
        InterlockedAdd64((PLONG64)&g_LoopbackRingOverruns, dropped);
    }

    ULONG firstRun = min(ByteCount, LOOPBACK_RING_CAPACITY_BYTES - g_LoopbackRingWriteOffset);
    RtlCopyMemory(g_LoopbackRingBuffer + g_LoopbackRingWriteOffset, Data, firstRun);
    if (ByteCount > firstRun)
    {
        RtlCopyMemory(g_LoopbackRingBuffer, Data + firstRun, ByteCount - firstRun);
    }

    g_LoopbackRingWriteOffset = (g_LoopbackRingWriteOffset + ByteCount) % LOOPBACK_RING_CAPACITY_BYTES;
    g_LoopbackRingBytesAvailable += ByteCount;

    KeReleaseSpinLock(&g_LoopbackRingLock, oldIrql);
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
VOID
LoopbackRingPopBytes(
    BYTE *Data,
    ULONG ByteCount
    )
{
    KIRQL oldIrql;
    ULONG popped;

    if (ByteCount == 0)
    {
        return;
    }

    KeAcquireSpinLock(&g_LoopbackRingLock, &oldIrql);

    popped = min(ByteCount, g_LoopbackRingBytesAvailable);

    if (popped > 0)
    {
        ULONG firstRun = min(popped, LOOPBACK_RING_CAPACITY_BYTES - g_LoopbackRingReadOffset);
        RtlCopyMemory(Data, g_LoopbackRingBuffer + g_LoopbackRingReadOffset, firstRun);
        if (popped > firstRun)
        {
            RtlCopyMemory(Data + firstRun, g_LoopbackRingBuffer, popped - firstRun);
        }

        g_LoopbackRingReadOffset = (g_LoopbackRingReadOffset + popped) % LOOPBACK_RING_CAPACITY_BYTES;
        g_LoopbackRingBytesAvailable -= popped;
    }

    KeReleaseSpinLock(&g_LoopbackRingLock, oldIrql);

    //
    // Zero-fill on underflow, outside the lock.
    //
    if (popped < ByteCount)
    {
        RtlZeroMemory(Data + popped, ByteCount - popped);
        InterlockedAdd64((PLONG64)&g_LoopbackRingUnderruns, ByteCount - popped);
    }
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
VOID
LoopbackRingGetStats(
    PULONGLONG Overruns,
    PULONGLONG Underruns
    )
{
    *Overruns = (ULONGLONG)InterlockedCompareExchange64((PLONG64)&g_LoopbackRingOverruns, 0, 0);
    *Underruns = (ULONGLONG)InterlockedCompareExchange64((PLONG64)&g_LoopbackRingUnderruns, 0, 0);
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
VOID
LoopbackRingSetRenderFormat(
    PWAVEFORMATEXTENSIBLE RenderFormat
    )
{
    PAGED_CODE();

    g_LoopbackRenderChannels = RenderFormat->Format.nChannels;
    g_LoopbackRenderSamplesPerSec = RenderFormat->Format.nSamplesPerSec;
    g_LoopbackRenderBitsPerSample = RenderFormat->Format.wBitsPerSample;
    g_LoopbackRenderFormatValid = TRUE;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
VOID
LoopbackRingCheckCaptureFormat(
    PWAVEFORMATEXTENSIBLE CaptureFormat
    )
{
    PAGED_CODE();

    //
    // The ring passes bytes through unmodified; it assumes the render mix
    // format and the loopback capture mix format are identical
    // (default: 48KHz/16-bit/8ch). Warn when they differ.
    //
    if (g_LoopbackRenderFormatValid &&
        (CaptureFormat->Format.nChannels != g_LoopbackRenderChannels ||
         CaptureFormat->Format.nSamplesPerSec != g_LoopbackRenderSamplesPerSec ||
         CaptureFormat->Format.wBitsPerSample != g_LoopbackRenderBitsPerSample))
    {
        DPF(D_ERROR,
            ("SoundRadar VAD: loopback capture format (%uch/%uHz/%ubit) differs from render format (%uch/%uHz/%ubit); byte passthrough will misrepresent the audio",
             CaptureFormat->Format.nChannels, CaptureFormat->Format.nSamplesPerSec, CaptureFormat->Format.wBitsPerSample,
             g_LoopbackRenderChannels, g_LoopbackRenderSamplesPerSec, g_LoopbackRenderBitsPerSample));
    }
}
