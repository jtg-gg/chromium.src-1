// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "mojo/edk/embedder/embedder.h"
#include "mojo/edk/embedder/platform_channel_pair.h"
#include "mojo/edk/system/test_utils.h"
#include "mojo/edk/system/waiter.h"
#include "mojo/edk/test/mojo_test_base.h"
#include "mojo/public/c/system/data_pipe.h"
#include "mojo/public/c/system/functions.h"
#include "mojo/public/c/system/message_pipe.h"
#include "mojo/public/cpp/system/macros.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mojo {
namespace edk {
namespace {

const uint32_t kSizeOfOptions =
    static_cast<uint32_t>(sizeof(MojoCreateDataPipeOptions));

// In various places, we have to poll (since, e.g., we can't yet wait for a
// certain amount of data to be available). This is the maximum number of
// iterations (separated by a short sleep).
// TODO(vtl): Get rid of this.
const size_t kMaxPoll = 100;

// Used in Multiprocess test.
const size_t kMultiprocessCapacity = 37;
const char kMultiprocessTestData[] = "hello i'm a string that is 36 bytes";
const int kMultiprocessMaxIter = 5;

class DataPipeTest : public test::MojoTestBase {
 public:
  DataPipeTest() : producer_(MOJO_HANDLE_INVALID),
                   consumer_(MOJO_HANDLE_INVALID) {}

  ~DataPipeTest() override {
    if (producer_ != MOJO_HANDLE_INVALID)
      CHECK_EQ(MOJO_RESULT_OK, MojoClose(producer_));
    if (consumer_ != MOJO_HANDLE_INVALID)
      CHECK_EQ(MOJO_RESULT_OK, MojoClose(consumer_));
  }

  MojoResult Create(const MojoCreateDataPipeOptions* options) {
    return MojoCreateDataPipe(options, &producer_, &consumer_);
  }

  MojoResult WriteData(const void* elements,
                       uint32_t* num_bytes,
                       bool all_or_none = false) {
    return MojoWriteData(producer_, elements, num_bytes,
                         all_or_none ? MOJO_WRITE_DATA_FLAG_ALL_OR_NONE
                                     : MOJO_WRITE_DATA_FLAG_NONE);
  }

  MojoResult ReadData(void* elements,
                      uint32_t* num_bytes,
                      bool all_or_none = false,
                      bool peek = false) {
    MojoReadDataFlags flags = MOJO_READ_DATA_FLAG_NONE;
    if (all_or_none)
      flags |= MOJO_READ_DATA_FLAG_ALL_OR_NONE;
    if (peek)
      flags |= MOJO_READ_DATA_FLAG_PEEK;
    return MojoReadData(consumer_, elements, num_bytes, flags);
  }

  MojoResult QueryData(uint32_t* num_bytes) {
    return MojoReadData(consumer_, nullptr, num_bytes,
                        MOJO_READ_DATA_FLAG_QUERY);
  }

  MojoResult DiscardData(uint32_t* num_bytes, bool all_or_none = false) {
    MojoReadDataFlags flags = MOJO_READ_DATA_FLAG_DISCARD;
    if (all_or_none)
      flags |= MOJO_READ_DATA_FLAG_ALL_OR_NONE;
    return MojoReadData(consumer_, nullptr, num_bytes, flags);
  }

  MojoResult BeginReadData(const void** elements,
                           uint32_t* num_bytes,
                           bool all_or_none = false) {
    MojoReadDataFlags flags = MOJO_READ_DATA_FLAG_NONE;
    if (all_or_none)
      flags |= MOJO_READ_DATA_FLAG_ALL_OR_NONE;
    return MojoBeginReadData(consumer_, elements, num_bytes, flags);
  }

  MojoResult EndReadData(uint32_t num_bytes_read) {
    return MojoEndReadData(consumer_, num_bytes_read);
  }

  MojoResult BeginWriteData(void** elements,
                            uint32_t* num_bytes,
                            bool all_or_none = false) {
    MojoReadDataFlags flags = MOJO_WRITE_DATA_FLAG_NONE;
    if (all_or_none)
      flags |= MOJO_WRITE_DATA_FLAG_ALL_OR_NONE;
    return MojoBeginWriteData(producer_, elements, num_bytes, flags);
  }

  MojoResult EndWriteData(uint32_t num_bytes_written) {
    return MojoEndWriteData(producer_, num_bytes_written);
  }

  MojoResult CloseProducer() {
    MojoResult rv = MojoClose(producer_);
    producer_ = MOJO_HANDLE_INVALID;
    return rv;
  }

  MojoResult CloseConsumer() {
    MojoResult rv = MojoClose(consumer_);
    consumer_ = MOJO_HANDLE_INVALID;
    return rv;
  }

  MojoHandle producer_, consumer_;

 private:
  MOJO_DISALLOW_COPY_AND_ASSIGN(DataPipeTest);
};

TEST_F(DataPipeTest, Basic) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      1000 * sizeof(int32_t)                    // |capacity_num_bytes|.
  };

  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));

  // We can write to a data pipe handle immediately.
  int32_t elements[10] = {};
  uint32_t num_bytes = 0;

  num_bytes =
      static_cast<uint32_t>(MOJO_ARRAYSIZE(elements) * sizeof(elements[0]));

  elements[0] = 123;
  elements[1] = 456;
  num_bytes = static_cast<uint32_t>(2u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(&elements[0], &num_bytes));

  // Now wait for the other side to become readable.
  MojoHandleSignalsState state;
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &state));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, state.satisfied_signals);

  elements[0] = -1;
  elements[1] = -1;
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(&elements[0], &num_bytes));
  ASSERT_EQ(static_cast<uint32_t>(2u * sizeof(elements[0])), num_bytes);
  ASSERT_EQ(elements[0], 123);
  ASSERT_EQ(elements[1], 456);
}

// Tests creation of data pipes with various (valid) options.
TEST_F(DataPipeTest, CreateAndMaybeTransfer) {
  MojoCreateDataPipeOptions test_options[] = {
      // Default options.
      {},
      // Trivial element size, non-default capacity.
      {kSizeOfOptions,                           // |struct_size|.
       MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
       1,                                        // |element_num_bytes|.
       1000},                                    // |capacity_num_bytes|.
      // Nontrivial element size, non-default capacity.
      {kSizeOfOptions,                           // |struct_size|.
       MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
       4,                                        // |element_num_bytes|.
       4000},                                    // |capacity_num_bytes|.
      // Nontrivial element size, default capacity.
      {kSizeOfOptions,                           // |struct_size|.
       MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
       100,                                      // |element_num_bytes|.
       0}                                        // |capacity_num_bytes|.
  };
  for (size_t i = 0; i < arraysize(test_options); i++) {
    MojoHandle producer_handle, consumer_handle;
    MojoCreateDataPipeOptions* options =
        i ? &test_options[i] : nullptr;
    ASSERT_EQ(MOJO_RESULT_OK,
              MojoCreateDataPipe(options, &producer_handle, &consumer_handle));
    ASSERT_EQ(MOJO_RESULT_OK, MojoClose(producer_handle));
    ASSERT_EQ(MOJO_RESULT_OK, MojoClose(consumer_handle));
  }
}

TEST_F(DataPipeTest, SimpleReadWrite) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      1000 * sizeof(int32_t)                    // |capacity_num_bytes|.
  };

  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  int32_t elements[10] = {};
  uint32_t num_bytes = 0;

  // Try reading; nothing there yet.
  num_bytes =
      static_cast<uint32_t>(MOJO_ARRAYSIZE(elements) * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_SHOULD_WAIT, ReadData(elements, &num_bytes));

  // Query; nothing there yet.
  num_bytes = 0;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);

  // Discard; nothing there yet.
  num_bytes = static_cast<uint32_t>(5u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_SHOULD_WAIT, DiscardData(&num_bytes));

  // Read with invalid |num_bytes|.
  num_bytes = sizeof(elements[0]) + 1;
  ASSERT_EQ(MOJO_RESULT_INVALID_ARGUMENT, ReadData(elements, &num_bytes));

  // Write two elements.
  elements[0] = 123;
  elements[1] = 456;
  num_bytes = static_cast<uint32_t>(2u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(elements, &num_bytes));
  // It should have written everything (even without "all or none").
  ASSERT_EQ(2u * sizeof(elements[0]), num_bytes);

  // Wait.
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Query.
  // TODO(vtl): It's theoretically possible (though not with the current
  // implementation/configured limits) that not all the data has arrived yet.
  // (The theoretically-correct assertion here is that |num_bytes| is |1 * ...|
  // or |2 * ...|.)
  num_bytes = 0;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(2 * sizeof(elements[0]), num_bytes);

  // Read one element.
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes));
  ASSERT_EQ(1u * sizeof(elements[0]), num_bytes);
  ASSERT_EQ(123, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Query.
  // TODO(vtl): See previous TODO. (If we got 2 elements there, however, we
  // should get 1 here.)
  num_bytes = 0;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(1 * sizeof(elements[0]), num_bytes);

  // Peek one element.
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes, false, true));
  ASSERT_EQ(1u * sizeof(elements[0]), num_bytes);
  ASSERT_EQ(456, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Query. Still has 1 element remaining.
  num_bytes = 0;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(1 * sizeof(elements[0]), num_bytes);

  // Try to read two elements, with "all or none".
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(2u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OUT_OF_RANGE,
            ReadData(elements, &num_bytes, true, false));
  ASSERT_EQ(-1, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Try to read two elements, without "all or none".
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(2u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes, false, false));
  ASSERT_EQ(1u * sizeof(elements[0]), num_bytes);
  ASSERT_EQ(456, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Query.
  num_bytes = 0;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);
}

// Note: The "basic" waiting tests test that the "wait states" are correct in
// various situations; they don't test that waiters are properly awoken on state
// changes. (For that, we need to use multiple threads.)
TEST_F(DataPipeTest, BasicProducerWaiting) {
  // Note: We take advantage of the fact that current for current
  // implementations capacities are strict maximums. This is not guaranteed by
  // the API.

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      2 * sizeof(int32_t)                       // |capacity_num_bytes|.
  };
  Create(&options);
  MojoHandleSignalsState hss;

  // Never readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_READABLE, 0, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Already writable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_WRITABLE, 0, &hss));

  // Write two elements.
  int32_t elements[2] = {123, 456};
  uint32_t num_bytes = static_cast<uint32_t>(2u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(elements, &num_bytes, true));
  ASSERT_EQ(static_cast<uint32_t>(2u * sizeof(elements[0])), num_bytes);

  // Wait for data to become available to the consumer.
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Peek one element.
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes, true, true));
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(elements[0])), num_bytes);
  ASSERT_EQ(123, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Read one element.
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes, true, false));
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(elements[0])), num_bytes);
  ASSERT_EQ(123, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Try writing, using a two-phase write.
  void* buffer = nullptr;
  num_bytes = static_cast<uint32_t>(3u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&buffer, &num_bytes));
  EXPECT_TRUE(buffer);
  ASSERT_GE(num_bytes, static_cast<uint32_t>(1u * sizeof(elements[0])));

  static_cast<int32_t*>(buffer)[0] = 789;
  ASSERT_EQ(MOJO_RESULT_OK, EndWriteData(static_cast<uint32_t>(
                                         1u * sizeof(elements[0]))));

  // Read one element, using a two-phase read.
  const void* read_buffer = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK,
            BeginReadData(&read_buffer, &num_bytes, false));
  EXPECT_TRUE(read_buffer);
  // The two-phase read should be able to read at least one element.
  ASSERT_GE(num_bytes, static_cast<uint32_t>(1u * sizeof(elements[0])));
  ASSERT_EQ(456, static_cast<const int32_t*>(read_buffer)[0]);
  ASSERT_EQ(MOJO_RESULT_OK, EndReadData(static_cast<uint32_t>(
                                        1u * sizeof(elements[0]))));

  // Write one element.
  elements[0] = 123;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(elements, &num_bytes));
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(elements[0])), num_bytes);

  // Close the consumer.
  CloseConsumer();

  // It should now be never-writable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_WRITABLE, 0, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfiable_signals);
}

TEST_F(DataPipeTest, PeerClosedProducerWaiting) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      2 * sizeof(int32_t)                       // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Close the consumer.
  CloseConsumer();

  // It should be signaled.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfiable_signals);
}

TEST_F(DataPipeTest, PeerClosedConsumerWaiting) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      2 * sizeof(int32_t)                       // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Close the producer.
  CloseProducer();

  // It should be signaled.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfiable_signals);
}

TEST_F(DataPipeTest, BasicConsumerWaiting) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      1000 * sizeof(int32_t)                    // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Never writable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_WRITABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(0u, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Write two elements.
  int32_t elements[2] = {123, 456};
  uint32_t num_bytes = static_cast<uint32_t>(2u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(elements, &num_bytes, true));

  // Wait for readability.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Discard one element.
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, DiscardData(&num_bytes, true));
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(elements[0])), num_bytes);

  // Should still be readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Peek one element.
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes, true, true));
  ASSERT_EQ(456, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Should still be readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Read one element.
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes, true));
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(elements[0])), num_bytes);
  ASSERT_EQ(456, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Write one element.
  elements[0] = 789;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(elements, &num_bytes, true));

  // Waiting should now succeed.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Close the producer.
  CloseProducer();

  // Should still be readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_TRUE((hss.satisfied_signals & MOJO_HANDLE_SIGNAL_READABLE) != 0);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Wait for the peer closed signal.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_TRUE((hss.satisfied_signals & MOJO_HANDLE_SIGNAL_PEER_CLOSED) != 0);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Read one element.
  elements[0] = -1;
  elements[1] = -1;
  num_bytes = static_cast<uint32_t>(1u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(elements, &num_bytes, true));
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(elements[0])), num_bytes);
  ASSERT_EQ(789, elements[0]);
  ASSERT_EQ(-1, elements[1]);

  // Should be never-readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfiable_signals);
}

// Test with two-phase APIs and also closing the producer with an active
// consumer waiter.
TEST_F(DataPipeTest, ConsumerWaitingTwoPhase) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      1000 * sizeof(int32_t)                    // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Write two elements.
  int32_t* elements = nullptr;
  void* buffer = nullptr;
  // Request room for three (but we'll only write two).
  uint32_t num_bytes = static_cast<uint32_t>(3u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&buffer, &num_bytes, true));
  EXPECT_TRUE(buffer);
  EXPECT_GE(num_bytes, static_cast<uint32_t>(3u * sizeof(elements[0])));
  elements = static_cast<int32_t*>(buffer);
  elements[0] = 123;
  elements[1] = 456;
  ASSERT_EQ(MOJO_RESULT_OK, EndWriteData(2u * sizeof(elements[0])));

  // Wait for readability.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Read one element.
  // Request two in all-or-none mode, but only read one.
  const void* read_buffer = nullptr;
  num_bytes = static_cast<uint32_t>(2u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_buffer, &num_bytes, true));
  EXPECT_TRUE(read_buffer);
  ASSERT_EQ(static_cast<uint32_t>(2u * sizeof(elements[0])), num_bytes);
  const int32_t* read_elements = static_cast<const int32_t*>(read_buffer);
  ASSERT_EQ(123, read_elements[0]);
  ASSERT_EQ(MOJO_RESULT_OK, EndReadData(1u * sizeof(elements[0])));

  // Should still be readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Read one element.
  // Request three, but not in all-or-none mode.
  read_buffer = nullptr;
  num_bytes = static_cast<uint32_t>(3u * sizeof(elements[0]));
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_buffer, &num_bytes));
  EXPECT_TRUE(read_buffer);
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(elements[0])), num_bytes);
  read_elements = static_cast<const int32_t*>(read_buffer);
  ASSERT_EQ(456, read_elements[0]);
  ASSERT_EQ(MOJO_RESULT_OK, EndReadData(1u * sizeof(elements[0])));

  // Close the producer.
  CloseProducer();

  // Should be never-readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfiable_signals);
}

// Tests that data pipes aren't writable/readable during two-phase writes/reads.
TEST_F(DataPipeTest, BasicTwoPhaseWaiting) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      1000 * sizeof(int32_t)                    // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // It should be writable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_WRITABLE, 0, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  uint32_t num_bytes = static_cast<uint32_t>(1u * sizeof(int32_t));
  void* write_ptr = nullptr;
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&write_ptr, &num_bytes));
  EXPECT_TRUE(write_ptr);
  EXPECT_GE(num_bytes, static_cast<uint32_t>(1u * sizeof(int32_t)));

  // At this point, it shouldn't be writable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_DEADLINE_EXCEEDED,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_WRITABLE, 0, &hss));
  ASSERT_EQ(0u, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // It shouldn't be readable yet either (we'll wait later).
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_DEADLINE_EXCEEDED,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE, 0, &hss));
  ASSERT_EQ(0u, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  static_cast<int32_t*>(write_ptr)[0] = 123;
  ASSERT_EQ(MOJO_RESULT_OK, EndWriteData(1u * sizeof(int32_t)));

  // It should immediately be writable again.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_WRITABLE, 0, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // It should become readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Start another two-phase write and check that it's readable even in the
  // middle of it.
  num_bytes = static_cast<uint32_t>(1u * sizeof(int32_t));
  write_ptr = nullptr;
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&write_ptr, &num_bytes));
  EXPECT_TRUE(write_ptr);
  EXPECT_GE(num_bytes, static_cast<uint32_t>(1u * sizeof(int32_t)));

  // It should be readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // End the two-phase write without writing anything.
  ASSERT_EQ(MOJO_RESULT_OK, EndWriteData(0u));

  // Start a two-phase read.
  num_bytes = static_cast<uint32_t>(1u * sizeof(int32_t));
  const void* read_ptr = nullptr;
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_ptr, &num_bytes));
  EXPECT_TRUE(read_ptr);
  ASSERT_EQ(static_cast<uint32_t>(1u * sizeof(int32_t)), num_bytes);

  // At this point, it should still be writable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_WRITABLE, 0, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // But not readable.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_DEADLINE_EXCEEDED,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE, 0, &hss));
  ASSERT_EQ(0u, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // End the two-phase read without reading anything.
  ASSERT_EQ(MOJO_RESULT_OK, EndReadData(0u));

  // It should be readable again.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE, 0, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);
}

void Seq(int32_t start, size_t count, int32_t* out) {
  for (size_t i = 0; i < count; i++)
    out[i] = start + static_cast<int32_t>(i);
}

TEST_F(DataPipeTest, AllOrNone) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      10 * sizeof(int32_t)                      // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Try writing way too much.
  uint32_t num_bytes = 20u * sizeof(int32_t);
  int32_t buffer[100];
  Seq(0, MOJO_ARRAYSIZE(buffer), buffer);
  ASSERT_EQ(MOJO_RESULT_OUT_OF_RANGE, WriteData(buffer, &num_bytes, true));

  // Should still be empty.
  num_bytes = ~0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);

  // Write some data.
  num_bytes = 5u * sizeof(int32_t);
  Seq(100, MOJO_ARRAYSIZE(buffer), buffer);
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(buffer, &num_bytes, true));
  ASSERT_EQ(5u * sizeof(int32_t), num_bytes);

  // Wait for data.
  // TODO(vtl): There's no real guarantee that all the data will become
  // available at once (except that in current implementations, with reasonable
  // limits, it will). Eventually, we'll be able to wait for a specified amount
  // of data to become available.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Half full.
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(5u * sizeof(int32_t), num_bytes);

  /* TODO(jam): enable if we end up observing max capacity
  // Too much.
  num_bytes = 6u * sizeof(int32_t);
  Seq(200, MOJO_ARRAYSIZE(buffer), buffer);
  ASSERT_EQ(MOJO_RESULT_OUT_OF_RANGE, WriteData(buffer, &num_bytes, true));
  */

  // Try reading too much.
  num_bytes = 11u * sizeof(int32_t);
  memset(buffer, 0xab, sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_OUT_OF_RANGE, ReadData(buffer, &num_bytes, true));
  int32_t expected_buffer[100];
  memset(expected_buffer, 0xab, sizeof(expected_buffer));
  ASSERT_EQ(0, memcmp(buffer, expected_buffer, sizeof(buffer)));

  // Try discarding too much.
  num_bytes = 11u * sizeof(int32_t);
  ASSERT_EQ(MOJO_RESULT_OUT_OF_RANGE, DiscardData(&num_bytes, true));

  // Just a little.
  num_bytes = 2u * sizeof(int32_t);
  Seq(300, MOJO_ARRAYSIZE(buffer), buffer);
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(buffer, &num_bytes, true));
  ASSERT_EQ(2u * sizeof(int32_t), num_bytes);

  // Just right.
  num_bytes = 3u * sizeof(int32_t);
  Seq(400, MOJO_ARRAYSIZE(buffer), buffer);
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(buffer, &num_bytes, true));
  ASSERT_EQ(3u * sizeof(int32_t), num_bytes);

  // TODO(vtl): Hack (see also the TODO above): We can't currently wait for a
  // specified amount of data to be available, so poll.
  for (size_t i = 0; i < kMaxPoll; i++) {
    num_bytes = 0u;
    ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
    if (num_bytes >= 10u * sizeof(int32_t))
      break;

    test::Sleep(test::EpsilonDeadline());
  }
  ASSERT_EQ(10u * sizeof(int32_t), num_bytes);

  // Read half.
  num_bytes = 5u * sizeof(int32_t);
  memset(buffer, 0xab, sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(buffer, &num_bytes, true));
  ASSERT_EQ(5u * sizeof(int32_t), num_bytes);
  memset(expected_buffer, 0xab, sizeof(expected_buffer));
  Seq(100, 5, expected_buffer);
  ASSERT_EQ(0, memcmp(buffer, expected_buffer, sizeof(buffer)));

  // Try reading too much again.
  num_bytes = 6u * sizeof(int32_t);
  memset(buffer, 0xab, sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_OUT_OF_RANGE, ReadData(buffer, &num_bytes, true));
  memset(expected_buffer, 0xab, sizeof(expected_buffer));
  ASSERT_EQ(0, memcmp(buffer, expected_buffer, sizeof(buffer)));

  // Try discarding too much again.
  num_bytes = 6u * sizeof(int32_t);
  ASSERT_EQ(MOJO_RESULT_OUT_OF_RANGE, DiscardData(&num_bytes, true));

  // Discard a little.
  num_bytes = 2u * sizeof(int32_t);
  ASSERT_EQ(MOJO_RESULT_OK, DiscardData(&num_bytes, true));
  ASSERT_EQ(2u * sizeof(int32_t), num_bytes);

  // Three left.
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(3u * sizeof(int32_t), num_bytes);

  // Close the producer, then test producer-closed cases.
  CloseProducer();

  // Wait.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Try reading too much; "failed precondition" since the producer is closed.
  num_bytes = 4u * sizeof(int32_t);
  memset(buffer, 0xab, sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            ReadData(buffer, &num_bytes, true));
  memset(expected_buffer, 0xab, sizeof(expected_buffer));
  ASSERT_EQ(0, memcmp(buffer, expected_buffer, sizeof(buffer)));

  // Try discarding too much; "failed precondition" again.
  num_bytes = 4u * sizeof(int32_t);
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION, DiscardData(&num_bytes, true));

  // Read a little.
  num_bytes = 2u * sizeof(int32_t);
  memset(buffer, 0xab, sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(buffer, &num_bytes, true));
  ASSERT_EQ(2u * sizeof(int32_t), num_bytes);
  memset(expected_buffer, 0xab, sizeof(expected_buffer));
  Seq(400, 2, expected_buffer);
  ASSERT_EQ(0, memcmp(buffer, expected_buffer, sizeof(buffer)));

  // Discard the remaining element.
  num_bytes = 1u * sizeof(int32_t);
  ASSERT_EQ(MOJO_RESULT_OK, DiscardData(&num_bytes, true));
  ASSERT_EQ(1u * sizeof(int32_t), num_bytes);

  // Empty again.
  num_bytes = ~0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);
}

// Tests that |ProducerWriteData()| and |ConsumerReadData()| writes and reads,
// respectively, as much as possible, even if it may have to "wrap around" the
// internal circular buffer. (Note that the two-phase write and read need not do
// this.)
TEST_F(DataPipeTest, WrapAround) {
  unsigned char test_data[1000];
  for (size_t i = 0; i < MOJO_ARRAYSIZE(test_data); i++)
    test_data[i] = static_cast<unsigned char>(i);

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1u,                                       // |element_num_bytes|.
      100u                                      // |capacity_num_bytes|.
  };

  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Write 20 bytes.
  uint32_t num_bytes = 20u;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(&test_data[0], &num_bytes, true));
  ASSERT_EQ(20u, num_bytes);

  // Wait for data.
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_TRUE((hss.satisfied_signals & MOJO_HANDLE_SIGNAL_READABLE) != 0);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Read 10 bytes.
  unsigned char read_buffer[1000] = {0};
  num_bytes = 10u;
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(read_buffer, &num_bytes, true));
  ASSERT_EQ(10u, num_bytes);
  ASSERT_EQ(0, memcmp(read_buffer, &test_data[0], 10u));

  // Check that a two-phase write can now only write (at most) 80 bytes. (This
  // checks an implementation detail; this behavior is not guaranteed.)
  void* write_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK,
            BeginWriteData(&write_buffer_ptr, &num_bytes, false));
  EXPECT_TRUE(write_buffer_ptr);
  ASSERT_EQ(80u, num_bytes);
  ASSERT_EQ(MOJO_RESULT_OK, EndWriteData(0));

  size_t total_num_bytes = 0;
  while (total_num_bytes < 90) {
    // Wait to write.
    ASSERT_EQ(MOJO_RESULT_OK,
              MojoWait(producer_, MOJO_HANDLE_SIGNAL_WRITABLE,
                       MOJO_DEADLINE_INDEFINITE, &hss));
    ASSERT_EQ(hss.satisfied_signals, MOJO_HANDLE_SIGNAL_WRITABLE);
    ASSERT_EQ(hss.satisfiable_signals,
              MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED);

    // Write as much as we can.
    num_bytes = 100;
    ASSERT_EQ(MOJO_RESULT_OK,
              WriteData(&test_data[20 + total_num_bytes], &num_bytes, false));
    total_num_bytes += num_bytes;
  }

  ASSERT_EQ(90u, total_num_bytes);

  num_bytes = 0;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(100u, num_bytes);

  // Check that a two-phase read can now only read (at most) 90 bytes. (This
  // checks an implementation detail; this behavior is not guaranteed.)
  const void* read_buffer_ptr = nullptr;
  num_bytes = 0;
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_buffer_ptr, &num_bytes, false));
  EXPECT_TRUE(read_buffer_ptr);
  ASSERT_EQ(90u, num_bytes);
  ASSERT_EQ(MOJO_RESULT_OK, EndReadData(0));

  // Read as much as possible. We should read 100 bytes.
  num_bytes = static_cast<uint32_t>(MOJO_ARRAYSIZE(read_buffer) *
                                    sizeof(read_buffer[0]));
  memset(read_buffer, 0, num_bytes);
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(read_buffer, &num_bytes));
  ASSERT_EQ(100u, num_bytes);
  ASSERT_EQ(0, memcmp(read_buffer, &test_data[10], 100u));
}

// Tests the behavior of writing (simple and two-phase), closing the producer,
// then reading (simple and two-phase).
TEST_F(DataPipeTest, WriteCloseProducerRead) {
  const char kTestData[] = "hello world";
  const uint32_t kTestDataSize = static_cast<uint32_t>(sizeof(kTestData));

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1u,                                       // |element_num_bytes|.
      1000u                                     // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));

  // Write some data, so we'll have something to read.
  uint32_t num_bytes = kTestDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kTestData, &num_bytes, false));
  ASSERT_EQ(kTestDataSize, num_bytes);

  // Write it again, so we'll have something left over.
  num_bytes = kTestDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kTestData, &num_bytes, false));
  ASSERT_EQ(kTestDataSize, num_bytes);

  // Start two-phase write.
  void* write_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK,
            BeginWriteData(&write_buffer_ptr, &num_bytes, false));
  EXPECT_TRUE(write_buffer_ptr);
  EXPECT_GT(num_bytes, 0u);

  // TODO(vtl): (See corresponding TODO in TwoPhaseAllOrNone.)
  for (size_t i = 0; i < kMaxPoll; i++) {
    num_bytes = 0u;
    ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
    if (num_bytes >= 2u * kTestDataSize)
      break;

    test::Sleep(test::EpsilonDeadline());
  }
  ASSERT_EQ(2u * kTestDataSize, num_bytes);

  // Start two-phase read.
  const void* read_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK,
            BeginReadData(&read_buffer_ptr, &num_bytes));
  EXPECT_TRUE(read_buffer_ptr);
  ASSERT_EQ(2u * kTestDataSize, num_bytes);

  // Close the producer.
  CloseProducer();

  // The consumer can finish its two-phase read.
  ASSERT_EQ(0, memcmp(read_buffer_ptr, kTestData, kTestDataSize));
  ASSERT_EQ(MOJO_RESULT_OK, EndReadData(kTestDataSize));

  // And start another.
  read_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK,
            BeginReadData(&read_buffer_ptr, &num_bytes));
  EXPECT_TRUE(read_buffer_ptr);
  ASSERT_EQ(kTestDataSize, num_bytes);
}


// Tests the behavior of interrupting a two-phase read and write by closing the
// consumer.
TEST_F(DataPipeTest, TwoPhaseWriteReadCloseConsumer) {
  const char kTestData[] = "hello world";
  const uint32_t kTestDataSize = static_cast<uint32_t>(sizeof(kTestData));

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1u,                                       // |element_num_bytes|.
      1000u                                     // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Write some data, so we'll have something to read.
  uint32_t num_bytes = kTestDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kTestData, &num_bytes));
  ASSERT_EQ(kTestDataSize, num_bytes);

  // Start two-phase write.
  void* write_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&write_buffer_ptr, &num_bytes));
  EXPECT_TRUE(write_buffer_ptr);
  ASSERT_GT(num_bytes, kTestDataSize);

  // Wait for data.
  // TODO(vtl): (See corresponding TODO in AllOrNone.)
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Start two-phase read.
  const void* read_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_buffer_ptr, &num_bytes));
  EXPECT_TRUE(read_buffer_ptr);
  ASSERT_EQ(kTestDataSize, num_bytes);

  // Close the consumer.
  CloseConsumer();

  // Wait for producer to know that the consumer is closed.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(producer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfiable_signals);

  // Actually write some data. (Note: Premature freeing of the buffer would
  // probably only be detected under ASAN or similar.)
  memcpy(write_buffer_ptr, kTestData, kTestDataSize);
  // Note: Even though the consumer has been closed, ending the two-phase
  // write will report success.
  ASSERT_EQ(MOJO_RESULT_OK, EndWriteData(kTestDataSize));

  // But trying to write should result in failure.
  num_bytes = kTestDataSize;
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION, WriteData(kTestData, &num_bytes));

  // As will trying to start another two-phase write.
  write_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            BeginWriteData(&write_buffer_ptr, &num_bytes));
}

// Tests the behavior of "interrupting" a two-phase write by closing both the
// producer and the consumer.
TEST_F(DataPipeTest, TwoPhaseWriteCloseBoth) {
  const uint32_t kTestDataSize = 15u;

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1u,                                       // |element_num_bytes|.
      1000u                                     // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));

  // Start two-phase write.
  void* write_buffer_ptr = nullptr;
  uint32_t num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&write_buffer_ptr, &num_bytes));
  EXPECT_TRUE(write_buffer_ptr);
  ASSERT_GT(num_bytes, kTestDataSize);
}

// Tests the behavior of writing, closing the producer, and then reading (with
// and without data remaining).
TEST_F(DataPipeTest, WriteCloseProducerReadNoData) {
  const char kTestData[] = "hello world";
  const uint32_t kTestDataSize = static_cast<uint32_t>(sizeof(kTestData));

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1u,                                       // |element_num_bytes|.
      1000u                                     // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Write some data, so we'll have something to read.
  uint32_t num_bytes = kTestDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kTestData, &num_bytes));
  ASSERT_EQ(kTestDataSize, num_bytes);

  // Close the producer.
  CloseProducer();

  // Wait. (Note that once the consumer knows that the producer is closed, it
  // must also know about all the data that was sent.)
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Peek that data.
  char buffer[1000];
  num_bytes = static_cast<uint32_t>(sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(buffer, &num_bytes, false, true));
  ASSERT_EQ(kTestDataSize, num_bytes);
  ASSERT_EQ(0, memcmp(buffer, kTestData, kTestDataSize));

  // Read that data.
  memset(buffer, 0, 1000);
  num_bytes = static_cast<uint32_t>(sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(buffer, &num_bytes));
  ASSERT_EQ(kTestDataSize, num_bytes);
  ASSERT_EQ(0, memcmp(buffer, kTestData, kTestDataSize));

  // A second read should fail.
  num_bytes = static_cast<uint32_t>(sizeof(buffer));
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION, ReadData(buffer, &num_bytes));

  // A two-phase read should also fail.
  const void* read_buffer_ptr = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            BeginReadData(&read_buffer_ptr, &num_bytes));

  // Ditto for discard.
  num_bytes = 10u;
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION, DiscardData(&num_bytes));
}

// Test that during a two phase read the memory stays valid even if more data
// comes in.
TEST_F(DataPipeTest, TwoPhaseReadMemoryStable) {
  const char kTestData[] = "hello world";
  const uint32_t kTestDataSize = static_cast<uint32_t>(sizeof(kTestData));

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1u,                                       // |element_num_bytes|.
      1000u                                     // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Write some data.
  uint32_t num_bytes = kTestDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kTestData, &num_bytes));
  ASSERT_EQ(kTestDataSize, num_bytes);

  // Wait for the data.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Begin a two-phase read.
  const void* read_buffer_ptr = nullptr;
  uint32_t read_buffer_size = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_buffer_ptr, &read_buffer_size));

  // Write more data.
  const char kExtraData[] = "bye world";
  const uint32_t kExtraDataSize = static_cast<uint32_t>(sizeof(kExtraData));
  num_bytes = kExtraDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kExtraData, &num_bytes));
  ASSERT_EQ(kExtraDataSize, num_bytes);

  // Close the producer.
  CloseProducer();

  // Wait. (Note that once the consumer knows that the producer is closed, it
  // must also have received the extra data).
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_PEER_CLOSED, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Read the two phase memory to check it's still valid.
  ASSERT_EQ(0, memcmp(read_buffer_ptr, kTestData, kTestDataSize));
  EndReadData(read_buffer_size);
}

// Test that two-phase reads/writes behave correctly when given invalid
// arguments.
TEST_F(DataPipeTest, TwoPhaseMoreInvalidArguments) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      10 * sizeof(int32_t)                      // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // No data.
  uint32_t num_bytes = 1000u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);

  // Try "ending" a two-phase write when one isn't active.
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
            EndWriteData(1u * sizeof(int32_t)));

  // Wait a bit, to make sure that if a signal were (incorrectly) sent, it'd
  // have time to propagate.
  test::Sleep(test::EpsilonDeadline());

  // Still no data.
  num_bytes = 1000u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);

  // Try ending a two-phase write with an invalid amount (too much).
  num_bytes = 0u;
  void* write_ptr = nullptr;
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&write_ptr, &num_bytes));
  ASSERT_EQ(MOJO_RESULT_INVALID_ARGUMENT,
            EndWriteData(num_bytes + static_cast<uint32_t>(sizeof(int32_t))));

  // But the two-phase write still ended.
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION, EndWriteData(0u));

  // Wait a bit (as above).
  test::Sleep(test::EpsilonDeadline());

  // Still no data.
  num_bytes = 1000u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);

  // Try ending a two-phase write with an invalid amount (not a multiple of the
  // element size).
  num_bytes = 0u;
  write_ptr = nullptr;
  ASSERT_EQ(MOJO_RESULT_OK, BeginWriteData(&write_ptr, &num_bytes));
  EXPECT_GE(num_bytes, 1u);
  ASSERT_EQ(MOJO_RESULT_INVALID_ARGUMENT, EndWriteData(1u));

  // But the two-phase write still ended.
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION, EndWriteData(0u));

  // Wait a bit (as above).
  test::Sleep(test::EpsilonDeadline());

  // Still no data.
  num_bytes = 1000u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(0u, num_bytes);

  // Now write some data, so we'll be able to try reading.
  int32_t element = 123;
  num_bytes = 1u * sizeof(int32_t);
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(&element, &num_bytes));

  // Wait for data.
  // TODO(vtl): (See corresponding TODO in AllOrNone.)
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // One element available.
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(1u * sizeof(int32_t), num_bytes);

  // Try "ending" a two-phase read when one isn't active.
  ASSERT_EQ(MOJO_RESULT_FAILED_PRECONDITION, EndReadData(1u * sizeof(int32_t)));

  // Still one element available.
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(1u * sizeof(int32_t), num_bytes);

  // Try ending a two-phase read with an invalid amount (too much).
  num_bytes = 0u;
  const void* read_ptr = nullptr;
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_ptr, &num_bytes));
  ASSERT_EQ(MOJO_RESULT_INVALID_ARGUMENT,
            EndReadData(num_bytes + static_cast<uint32_t>(sizeof(int32_t))));

  // Still one element available.
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(1u * sizeof(int32_t), num_bytes);

  // Try ending a two-phase read with an invalid amount (not a multiple of the
  // element size).
  num_bytes = 0u;
  read_ptr = nullptr;
  ASSERT_EQ(MOJO_RESULT_OK, BeginReadData(&read_ptr, &num_bytes));
  ASSERT_EQ(1u * sizeof(int32_t), num_bytes);
  ASSERT_EQ(123, static_cast<const int32_t*>(read_ptr)[0]);
  ASSERT_EQ(MOJO_RESULT_INVALID_ARGUMENT, EndReadData(1u));

  // Still one element available.
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK, QueryData(&num_bytes));
  ASSERT_EQ(1u * sizeof(int32_t), num_bytes);
}

// Test that a producer can be sent over a MP.
TEST_F(DataPipeTest, SendProducer) {
  const char kTestData[] = "hello world";
  const uint32_t kTestDataSize = static_cast<uint32_t>(sizeof(kTestData));

  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1u,                                       // |element_num_bytes|.
      1000u                                     // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));
  MojoHandleSignalsState hss;

  // Write some data.
  uint32_t num_bytes = kTestDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kTestData, &num_bytes));
  ASSERT_EQ(kTestDataSize, num_bytes);

  // Wait for the data.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Check the data.
  const void* read_buffer = nullptr;
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK,
            BeginReadData(&read_buffer, &num_bytes, false));
  ASSERT_EQ(0, memcmp(read_buffer, kTestData, kTestDataSize));
  EndReadData(num_bytes);

  // Now send the producer over a MP so that it's serialized.
  MojoHandle pipe0, pipe1;
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoCreateMessagePipe(nullptr, &pipe0, &pipe1));

  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWriteMessage(pipe0, nullptr, 0, &producer_, 1,
                             MOJO_WRITE_MESSAGE_FLAG_NONE));
  producer_ = MOJO_HANDLE_INVALID;
  ASSERT_EQ(MOJO_RESULT_OK, MojoWait(pipe1, MOJO_HANDLE_SIGNAL_READABLE,
                                     MOJO_DEADLINE_INDEFINITE, &hss));
  uint32_t num_handles = 1;
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoReadMessage(pipe1, nullptr, 0, &producer_, &num_handles,
                            MOJO_READ_MESSAGE_FLAG_NONE));
  ASSERT_EQ(num_handles, 1u);

  // Write more data.
  const char kExtraData[] = "bye world";
  const uint32_t kExtraDataSize = static_cast<uint32_t>(sizeof(kExtraData));
  num_bytes = kExtraDataSize;
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(kExtraData, &num_bytes));
  ASSERT_EQ(kExtraDataSize, num_bytes);

  // Wait for it.
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_READABLE,
                     MOJO_DEADLINE_INDEFINITE, &hss));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE, hss.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            hss.satisfiable_signals);

  // Check the second write.
  num_bytes = 0u;
  ASSERT_EQ(MOJO_RESULT_OK,
            BeginReadData(&read_buffer, &num_bytes, false));
  ASSERT_EQ(0, memcmp(read_buffer, kExtraData, kExtraDataSize));
  EndReadData(num_bytes);

  ASSERT_EQ(MOJO_RESULT_OK, MojoClose(pipe0));
  ASSERT_EQ(MOJO_RESULT_OK, MojoClose(pipe1));
}

// Ensures that if a data pipe consumer whose producer has closed is passed over
// a message pipe, the deserialized dispatcher is also marked as having a closed
// peer.
TEST_F(DataPipeTest, ConsumerWithClosedProducerSent) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      static_cast<uint32_t>(sizeof(int32_t)),   // |element_num_bytes|.
      1000 * sizeof(int32_t)                    // |capacity_num_bytes|.
  };

  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));

  // We can write to a data pipe handle immediately.
  int32_t data = 123;
  uint32_t num_bytes = sizeof(data);
  ASSERT_EQ(MOJO_RESULT_OK, WriteData(&data, &num_bytes));
  ASSERT_EQ(MOJO_RESULT_OK, CloseProducer());

  // Now wait for the other side to become readable and to see the peer closed.
  MojoHandleSignalsState state;
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &state));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            state.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            state.satisfiable_signals);

  // Now send the consumer over a MP so that it's serialized.
  MojoHandle pipe0, pipe1;
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoCreateMessagePipe(nullptr, &pipe0, &pipe1));

  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWriteMessage(pipe0, nullptr, 0, &consumer_, 1,
                             MOJO_WRITE_MESSAGE_FLAG_NONE));
  consumer_ = MOJO_HANDLE_INVALID;
  ASSERT_EQ(MOJO_RESULT_OK, MojoWait(pipe1, MOJO_HANDLE_SIGNAL_READABLE,
                                     MOJO_DEADLINE_INDEFINITE, &state));
  uint32_t num_handles = 1;
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoReadMessage(pipe1, nullptr, 0, &consumer_, &num_handles,
                            MOJO_READ_MESSAGE_FLAG_NONE));
  ASSERT_EQ(num_handles, 1u);

  ASSERT_EQ(MOJO_RESULT_OK,
            MojoWait(consumer_, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                     MOJO_DEADLINE_INDEFINITE, &state));
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            state.satisfied_signals);
  ASSERT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            state.satisfiable_signals);

  int32_t read_data;
  ASSERT_EQ(MOJO_RESULT_OK, ReadData(&read_data, &num_bytes));
  ASSERT_EQ(sizeof(read_data), num_bytes);
  ASSERT_EQ(data, read_data);

  ASSERT_EQ(MOJO_RESULT_OK, MojoClose(pipe0));
  ASSERT_EQ(MOJO_RESULT_OK, MojoClose(pipe1));
}

bool WriteAllData(MojoHandle producer,
                  const void* elements,
                  uint32_t num_bytes) {
  for (size_t i = 0; i < kMaxPoll; i++) {
    // Write as much data as we can.
    uint32_t write_bytes = num_bytes;
    MojoResult result = MojoWriteData(producer, elements, &write_bytes,
                                      MOJO_WRITE_DATA_FLAG_NONE);
    if (result == MOJO_RESULT_OK) {
      num_bytes -= write_bytes;
      elements = static_cast<const uint8_t*>(elements) + write_bytes;
      if (num_bytes == 0)
        return true;
    } else {
      EXPECT_EQ(MOJO_RESULT_SHOULD_WAIT, result);
    }

    MojoHandleSignalsState hss = MojoHandleSignalsState();
    EXPECT_EQ(MOJO_RESULT_OK, MojoWait(producer, MOJO_HANDLE_SIGNAL_WRITABLE,
                                       MOJO_DEADLINE_INDEFINITE, &hss));
    EXPECT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE, hss.satisfied_signals);
    EXPECT_EQ(MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
              hss.satisfiable_signals);
  }

  return false;
}

// If |expect_empty| is true, expect |consumer| to be empty after reading.
bool ReadAllData(MojoHandle consumer,
                 void* elements,
                 uint32_t num_bytes,
                 bool expect_empty) {
  for (size_t i = 0; i < kMaxPoll; i++) {
    // Read as much data as we can.
    uint32_t read_bytes = num_bytes;
    MojoResult result =
        MojoReadData(consumer, elements, &read_bytes, MOJO_READ_DATA_FLAG_NONE);
    if (result == MOJO_RESULT_OK) {
      num_bytes -= read_bytes;
      elements = static_cast<uint8_t*>(elements) + read_bytes;
      if (num_bytes == 0) {
        if (expect_empty) {
          // Expect no more data.
          test::Sleep(test::TinyDeadline());
          MojoReadData(consumer, nullptr, &num_bytes,
                       MOJO_READ_DATA_FLAG_QUERY);
          EXPECT_EQ(0u, num_bytes);
        }
        return true;
      }
    } else {
      EXPECT_EQ(MOJO_RESULT_SHOULD_WAIT, result);
    }

    MojoHandleSignalsState hss = MojoHandleSignalsState();
    EXPECT_EQ(MOJO_RESULT_OK, MojoWait(consumer, MOJO_HANDLE_SIGNAL_READABLE,
                                       MOJO_DEADLINE_INDEFINITE, &hss));
    // Peer could have become closed while we're still waiting for data.
    EXPECT_TRUE(MOJO_HANDLE_SIGNAL_READABLE & hss.satisfied_signals);
    EXPECT_EQ(MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
              hss.satisfiable_signals);
  }

  return num_bytes == 0;
}

#if !defined(OS_IOS)

#if defined(OS_ANDROID)
// Android multi-process tests are not executing the new process. This is flaky.
#define MAYBE_Multiprocess DISABLED_Multiprocess
#else
#define MAYBE_Multiprocess Multiprocess
#endif  // defined(OS_ANDROID)
TEST_F(DataPipeTest, MAYBE_Multiprocess) {
  const uint32_t kTestDataSize =
      static_cast<uint32_t>(sizeof(kMultiprocessTestData));
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1,                                        // |element_num_bytes|.
      kMultiprocessCapacity                     // |capacity_num_bytes|.
  };
  ASSERT_EQ(MOJO_RESULT_OK, Create(&options));

  RUN_CHILD_ON_PIPE(MultiprocessClient, server_mp)
    // Send some data before serialising and sending the data pipe over.
    // This is the first write so we don't need to use WriteAllData.
    uint32_t num_bytes = kTestDataSize;
    ASSERT_EQ(MOJO_RESULT_OK, WriteData(kMultiprocessTestData, &num_bytes,
                                        MOJO_WRITE_DATA_FLAG_ALL_OR_NONE));
    ASSERT_EQ(kTestDataSize, num_bytes);

    // Send child process the data pipe.
    ASSERT_EQ(MOJO_RESULT_OK,
              MojoWriteMessage(server_mp, nullptr, 0, &consumer_, 1,
                               MOJO_WRITE_MESSAGE_FLAG_NONE));

    // Send a bunch of data of varying sizes.
    uint8_t buffer[100];
    int seq = 0;
    for (int i = 0; i < kMultiprocessMaxIter; ++i) {
      for (uint32_t size = 1; size <= kMultiprocessCapacity; size++) {
        for (unsigned int j = 0; j < size; ++j)
          buffer[j] = seq + j;
        EXPECT_TRUE(WriteAllData(producer_, buffer, size));
        seq += size;
      }
    }

    // Write the test string in again.
    ASSERT_TRUE(WriteAllData(producer_, kMultiprocessTestData, kTestDataSize));

    // Swap ends.
    ASSERT_EQ(MOJO_RESULT_OK,
              MojoWriteMessage(server_mp, nullptr, 0, &producer_, 1,
                               MOJO_WRITE_MESSAGE_FLAG_NONE));

    // Receive the consumer from the other side.
    producer_ = MOJO_HANDLE_INVALID;
    MojoHandleSignalsState hss = MojoHandleSignalsState();
    ASSERT_EQ(MOJO_RESULT_OK, MojoWait(server_mp, MOJO_HANDLE_SIGNAL_READABLE,
                                       MOJO_DEADLINE_INDEFINITE, &hss));
    MojoHandle handles[2];
    uint32_t num_handles = MOJO_ARRAYSIZE(handles);
    ASSERT_EQ(MOJO_RESULT_OK,
              MojoReadMessage(server_mp, nullptr, 0, handles, &num_handles,
                              MOJO_READ_MESSAGE_FLAG_NONE));
    ASSERT_EQ(1u, num_handles);
    consumer_ = handles[0];

    // Read the test string twice. Once for when we sent it, and once for the
    // other end sending it.
    for (int i = 0; i < 2; ++i) {
      EXPECT_TRUE(ReadAllData(consumer_, buffer, kTestDataSize, i == 1));
      EXPECT_EQ(0, memcmp(buffer, kMultiprocessTestData, kTestDataSize));
    }

    WriteMessage(server_mp, "quit");

    // Don't have to close the consumer here because it will be done for us.
  END_CHILD()
}

DEFINE_TEST_CLIENT_TEST_WITH_PIPE(MultiprocessClient, DataPipeTest, client_mp) {
  const uint32_t kTestDataSize =
      static_cast<uint32_t>(sizeof(kMultiprocessTestData));

  // Receive the data pipe from the other side.
  MojoHandle consumer = MOJO_HANDLE_INVALID;
  MojoHandleSignalsState hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK, MojoWait(client_mp, MOJO_HANDLE_SIGNAL_READABLE,
                                     MOJO_DEADLINE_INDEFINITE, &hss));
  MojoHandle handles[2];
  uint32_t num_handles = MOJO_ARRAYSIZE(handles);
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoReadMessage(client_mp, nullptr, 0, handles, &num_handles,
                            MOJO_READ_MESSAGE_FLAG_NONE));
  ASSERT_EQ(1u, num_handles);
  consumer = handles[0];

  // Read the initial string that was sent.
  int32_t buffer[100];
  EXPECT_TRUE(ReadAllData(consumer, buffer, kTestDataSize, false));
  EXPECT_EQ(0, memcmp(buffer, kMultiprocessTestData, kTestDataSize));

  // Receive the main data and check it is correct.
  int seq = 0;
  uint8_t expected_buffer[100];
  for (int i = 0; i < kMultiprocessMaxIter; ++i) {
    for (uint32_t size = 1; size <= kMultiprocessCapacity; ++size) {
      for (unsigned int j = 0; j < size; ++j)
        expected_buffer[j] = seq + j;
      EXPECT_TRUE(ReadAllData(consumer, buffer, size, false));
      EXPECT_EQ(0, memcmp(buffer, expected_buffer, size));

      seq += size;
    }
  }

  // Swap ends.
  ASSERT_EQ(MOJO_RESULT_OK, MojoWriteMessage(client_mp, nullptr, 0, &consumer,
                                             1, MOJO_WRITE_MESSAGE_FLAG_NONE));

  // Receive the producer from the other side.
  MojoHandle producer = MOJO_HANDLE_INVALID;
  hss = MojoHandleSignalsState();
  ASSERT_EQ(MOJO_RESULT_OK, MojoWait(client_mp, MOJO_HANDLE_SIGNAL_READABLE,
                                     MOJO_DEADLINE_INDEFINITE, &hss));
  num_handles = MOJO_ARRAYSIZE(handles);
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoReadMessage(client_mp, nullptr, 0, handles, &num_handles,
                            MOJO_READ_MESSAGE_FLAG_NONE));
  ASSERT_EQ(1u, num_handles);
  producer = handles[0];

  // Write the test string one more time.
  EXPECT_TRUE(WriteAllData(producer, kMultiprocessTestData, kTestDataSize));

  // We swapped ends, so close the producer.
  EXPECT_EQ(MOJO_RESULT_OK, MojoClose(producer));

  // Wait to receive a "quit" message before exiting.
  EXPECT_EQ("quit", ReadMessage(client_mp));
}

DEFINE_TEST_CLIENT_TEST_WITH_PIPE(WriteAndCloseProducer, DataPipeTest, h) {
  MojoHandle p;
  std::string message = ReadMessageWithHandles(h, &p, 1);

  // Write some data to the producer and close it.
  uint32_t num_bytes = static_cast<uint32_t>(message.size());
  EXPECT_EQ(MOJO_RESULT_OK, MojoWriteData(p, message.data(), &num_bytes,
                                          MOJO_WRITE_DATA_FLAG_NONE));
  EXPECT_EQ(num_bytes, static_cast<uint32_t>(message.size()));

  // Close the producer before quitting.
  EXPECT_EQ(MOJO_RESULT_OK, MojoClose(p));

  // Wait for a quit message.
  EXPECT_EQ("quit", ReadMessage(h));
}

DEFINE_TEST_CLIENT_TEST_WITH_PIPE(ReadAndCloseConsumer, DataPipeTest, h) {
  MojoHandle c;
  std::string expected_message = ReadMessageWithHandles(h, &c, 1);

  // Wait for the consumer to become readable.
  EXPECT_EQ(MOJO_RESULT_OK, MojoWait(c, MOJO_HANDLE_SIGNAL_READABLE,
                                     MOJO_DEADLINE_INDEFINITE, nullptr));

  // Drain the consumer and expect to find the given message.
  uint32_t num_bytes = static_cast<uint32_t>(expected_message.size());
  std::vector<char> bytes(expected_message.size());
  EXPECT_EQ(MOJO_RESULT_OK, MojoReadData(c, bytes.data(), &num_bytes,
                                         MOJO_READ_DATA_FLAG_NONE));
  EXPECT_EQ(num_bytes, static_cast<uint32_t>(bytes.size()));

  std::string message(bytes.data(), bytes.size());
  EXPECT_EQ(expected_message, message);

  EXPECT_EQ(MOJO_RESULT_OK, MojoClose(c));

  // Wait for a quit message.
  EXPECT_EQ("quit", ReadMessage(h));
}

#if defined(OS_ANDROID)
// Android multi-process tests are not executing the new process. This is flaky.
#define MAYBE_SendConsumerAndCloseProducer DISABLED_SendConsumerAndCloseProducer
#else
#define MAYBE_SendConsumerAndCloseProducer SendConsumerAndCloseProducer
#endif  // defined(OS_ANDROID)
TEST_F(DataPipeTest, MAYBE_SendConsumerAndCloseProducer) {
  // Create a new data pipe.
  MojoHandle p, c;
  EXPECT_EQ(MOJO_RESULT_OK, MojoCreateDataPipe(nullptr, &p ,&c));

  RUN_CHILD_ON_PIPE(WriteAndCloseProducer, producer_client)
    RUN_CHILD_ON_PIPE(ReadAndCloseConsumer, consumer_client)
      const std::string kMessage = "Hello, world!";
      WriteMessageWithHandles(producer_client, kMessage, &p, 1);
      WriteMessageWithHandles(consumer_client, kMessage, &c, 1);

      WriteMessage(consumer_client, "quit");
    END_CHILD()

    WriteMessage(producer_client, "quit");
  END_CHILD()
}

DEFINE_TEST_CLIENT_TEST_WITH_PIPE(CreateAndWrite, DataPipeTest, h) {
  const MojoCreateDataPipeOptions options = {
      kSizeOfOptions,                           // |struct_size|.
      MOJO_CREATE_DATA_PIPE_OPTIONS_FLAG_NONE,  // |flags|.
      1,                                        // |element_num_bytes|.
      kMultiprocessCapacity                     // |capacity_num_bytes|.
  };

  MojoHandle p, c;
  ASSERT_EQ(MOJO_RESULT_OK, MojoCreateDataPipe(&options, &p, &c));

  const std::string kMessage = "Hello, world!";
  WriteMessageWithHandles(h, kMessage, &c, 1);

  // Write some data to the producer and close it.
  uint32_t num_bytes = static_cast<uint32_t>(kMessage.size());
  EXPECT_EQ(MOJO_RESULT_OK, MojoWriteData(p, kMessage.data(), &num_bytes,
                                          MOJO_WRITE_DATA_FLAG_NONE));
  EXPECT_EQ(num_bytes, static_cast<uint32_t>(kMessage.size()));
  EXPECT_EQ(MOJO_RESULT_OK, MojoClose(p));

  // Wait for a quit message.
  EXPECT_EQ("quit", ReadMessage(h));
}

#if defined(OS_ANDROID)
// Android multi-process tests are not executing the new process. This is flaky.
#define MAYBE_CreateInChild DISABLED_CreateInChild
#else
#define MAYBE_CreateInChild CreateInChild
#endif  // defined(OS_ANDROID)
TEST_F(DataPipeTest, MAYBE_CreateInChild) {
  RUN_CHILD_ON_PIPE(CreateAndWrite, child)
    MojoHandle c;
    std::string expected_message = ReadMessageWithHandles(child, &c, 1);

    // Wait for the consumer to become readable.
    EXPECT_EQ(MOJO_RESULT_OK, MojoWait(c, MOJO_HANDLE_SIGNAL_READABLE,
                                       MOJO_DEADLINE_INDEFINITE, nullptr));

    // Drain the consumer and expect to find the given message.
    uint32_t num_bytes = static_cast<uint32_t>(expected_message.size());
    std::vector<char> bytes(expected_message.size());
    EXPECT_EQ(MOJO_RESULT_OK, MojoReadData(c, bytes.data(), &num_bytes,
                                           MOJO_READ_DATA_FLAG_NONE));
    EXPECT_EQ(num_bytes, static_cast<uint32_t>(bytes.size()));

    std::string message(bytes.data(), bytes.size());
    EXPECT_EQ(expected_message, message);

    EXPECT_EQ(MOJO_RESULT_OK, MojoClose(c));
    WriteMessage(child, "quit");
  END_CHILD()
}

#endif  // !defined(OS_IOS)

}  // namespace
}  // namespace edk
}  // namespace mojo
