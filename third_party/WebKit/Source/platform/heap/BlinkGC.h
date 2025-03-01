// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BlinkGC_h
#define BlinkGC_h

// BlinkGC.h is a file that defines common things used by Blink GC.

#include "wtf/Allocator.h"

namespace blink {

class Visitor;

#define PRINT_HEAP_STATS 0 // Enable this macro to print heap stats to stderr.

using Address = uint8_t*;

using FinalizationCallback = void (*)(void*);
using VisitorCallback = void (*)(Visitor*, void* self);
using TraceCallback = VisitorCallback;
using WeakCallback = VisitorCallback;
using EphemeronCallback = VisitorCallback;
using PreFinalizerCallback = bool(*)(void*);

// List of typed heaps. The list is used to generate the implementation
// of typed heap related methods.
//
// To create a new typed heap add a H(<ClassName>) to the
// FOR_EACH_TYPED_HEAP macro below.
#define FOR_EACH_TYPED_HEAP(H)              \
    H(Node)                                 \
    H(CSSValue)

#define TypedHeapEnumName(Type) Type##HeapIndex,

class PLATFORM_EXPORT BlinkGC final {
    STATIC_ONLY(BlinkGC);
public:
    // When garbage collecting we need to know whether or not there
    // can be pointers to Blink GC managed objects on the stack for
    // each thread. When threads reach a safe point they record
    // whether or not they have pointers on the stack.
    enum StackState {
        NoHeapPointersOnStack,
        HeapPointersOnStack
    };

    enum GCType {
        // Both of the marking task and the sweeping task run in
        // Heap::collectGarbage().
        GCWithSweep,
        // Only the marking task runs in Heap::collectGarbage().
        // The sweeping task is split into chunks and scheduled lazily.
        GCWithoutSweep,
        // Only the marking task runs just to take a heap snapshot.
        // The sweeping task doesn't run. The marks added in the marking task
        // are just cleared.
        TakeSnapshot,
        // The marking task does not mark objects outside the heap of the GCing
        // thread.
        ThreadTerminationGC,
        // Just run thread-local weak processing. The weak processing may trace
        // already marked objects but it must not trace any unmarked object.
        // It's unfortunate that the thread-local weak processing requires
        // a marking visitor. See TODO in HashTable::process.
        ThreadLocalWeakProcessing,
    };

    enum GCReason {
        IdleGC,
        PreciseGC,
        ConservativeGC,
        ForcedGC,
        MemoryPressureGC,
        PageNavigationGC,
        NumberOfGCReason,
    };

    enum HeapIndices {
        EagerSweepHeapIndex = 0,
        NormalPage1HeapIndex,
        NormalPage2HeapIndex,
        NormalPage3HeapIndex,
        NormalPage4HeapIndex,
        Vector1HeapIndex,
        Vector2HeapIndex,
        Vector3HeapIndex,
        Vector4HeapIndex,
        InlineVectorHeapIndex,
        HashTableHeapIndex,
        FOR_EACH_TYPED_HEAP(TypedHeapEnumName)
        LargeObjectHeapIndex,
        // Values used for iteration of heap segments.
        NumberOfHeaps,
    };

#if defined(ADDRESS_SANITIZER)
    // Heaps can have their object payloads be poisoned, or cleared
    // of their poisoning.
    enum Poisoning {
        SetPoison,
        ClearPoison,
    };

    enum ObjectsToPoison {
        UnmarkedOnly,
        MarkedAndUnmarked,
    };
#endif

    enum V8GCType {
        V8MinorGC,
        V8MajorGC,
    };
};

} // namespace blink

#endif
