/*++

Module Name:

    loopbackring.h

Abstract:

    SoundRadar VAD: render -> loopback-capture byte ring.

    Derived work within the Microsoft sysvad virtual audio driver sample.
    Licensed under the Microsoft Public License (MS-PL); see the sysvad
    sample license terms.

--*/

#ifndef _SYSVAD_LOOPBACKRING_H_
#define _SYSVAD_LOOPBACKRING_H_

//
// Capacity of the global loopback ring, in bytes. All push/pop sizes must
// be multiples of the current audio frame size; the ring itself is a plain
// byte ring and is frame-size agnostic.
//
#define LOOPBACK_RING_CAPACITY_BYTES    65536

//
// One-time initialization (spinlock, offsets, counters). Call once from
// DriverEntry at PASSIVE_LEVEL.
//
_IRQL_requires_(PASSIVE_LEVEL)
VOID
LoopbackRingInitialize();

//
// Render side: append bytes to the ring. If the ring is full the oldest
// bytes are overwritten and the overrun counter is bumped.
// Callable at IRQL <= DISPATCH_LEVEL. No allocations, no prints.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
LoopbackRingPushBytes(
    _In_reads_bytes_(ByteCount) const BYTE *Data,
    _In_ ULONG ByteCount
    );

//
// Loopback capture side: remove bytes from the ring. If not enough data is
// available the remainder of the destination is zero-filled and the underrun
// counter is bumped.
// Callable at IRQL <= DISPATCH_LEVEL. No allocations, no prints.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
LoopbackRingPopBytes(
    _Out_writes_bytes_(ByteCount) BYTE *Data,
    _In_ ULONG ByteCount
    );

//
// Diagnostics: cumulative overrun/underrun counters.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
LoopbackRingGetStats(
    _Out_ PULONGLONG Overruns,
    _Out_ PULONGLONG Underruns
    );

//
// Format coupling helpers (PASSIVE_LEVEL only, called from stream Init).
// The render mix format is recorded; when a loopback capture stream is
// created with a different format a debug message is emitted. The ring is
// a byte pipe: no resampling or format conversion is performed in kernel.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
LoopbackRingSetRenderFormat(
    _In_ PWAVEFORMATEXTENSIBLE RenderFormat
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
LoopbackRingCheckCaptureFormat(
    _In_ PWAVEFORMATEXTENSIBLE CaptureFormat
    );

#endif // _SYSVAD_LOOPBACKRING_H_
