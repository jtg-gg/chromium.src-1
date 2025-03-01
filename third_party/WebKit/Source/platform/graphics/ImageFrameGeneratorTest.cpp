/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/graphics/ImageFrameGenerator.h"

#include "platform/SharedBuffer.h"
#include "platform/ThreadSafeFunctional.h"
#include "platform/graphics/ImageDecodingStore.h"
#include "platform/graphics/test/MockImageDecoder.h"
#include "public/platform/Platform.h"
#include "public/platform/WebTaskRunner.h"
#include "public/platform/WebThread.h"
#include "public/platform/WebTraceLocation.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace blink {

namespace {

// Helper methods to generate standard sizes.
SkISize fullSize() { return SkISize::Make(100, 100); }

SkImageInfo imageInfo()
{
    return SkImageInfo::Make(100, 100, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
}

} // namespace

class ImageFrameGeneratorTest : public ::testing::Test, public MockImageDecoderClient {
public:
    void SetUp() override
    {
        ImageDecodingStore::instance().setCacheLimitInBytes(1024 * 1024);
        m_data = SharedBuffer::create();
        m_generator = ImageFrameGenerator::create(fullSize(), m_data, false);
        useMockImageDecoderFactory();
        m_decodersDestroyed = 0;
        m_decodeRequestCount = 0;
        m_status = ImageFrame::FrameEmpty;
        m_frameCount = 1;
        m_requestedClearExceptFrame = kNotFound;
    }

    void TearDown() override
    {
        ImageDecodingStore::instance().clear();
    }

    void decoderBeingDestroyed() override
    {
        ++m_decodersDestroyed;
    }

    void decodeRequested() override
    {
        ++m_decodeRequestCount;
    }

    ImageFrame::Status status() override
    {
        ImageFrame::Status currentStatus = m_status;
        m_status = m_nextFrameStatus;
        return currentStatus;
    }

    void clearCacheExceptFrameRequested(size_t clearExceptFrame) override
    {
        m_requestedClearExceptFrame = clearExceptFrame;
    };

    size_t frameCount() override { return m_frameCount; }
    int repetitionCount() const override { return m_frameCount == 1 ? cAnimationNone:cAnimationLoopOnce; }
    float frameDuration() const override { return 0; }

protected:
    void useMockImageDecoderFactory()
    {
        m_generator->setImageDecoderFactory(MockImageDecoderFactory::create(this, fullSize()));
    }

    void addNewData(bool allDataReceived = false)
    {
        m_data->append("g", 1u);
        m_generator->setData(m_data, allDataReceived);
    }

    void setFrameStatus(ImageFrame::Status status)  { m_status = m_nextFrameStatus = status; }
    void setNextFrameStatus(ImageFrame::Status status)  { m_nextFrameStatus = status; }
    void setFrameCount(size_t count)
    {
        m_frameCount = count;
        if (count > 1) {
            m_generator.clear();
            m_generator = ImageFrameGenerator::create(fullSize(), m_data, true, true);
            useMockImageDecoderFactory();
        }
    }

    RefPtr<SharedBuffer> m_data;
    RefPtr<ImageFrameGenerator> m_generator;
    int m_decodersDestroyed;
    int m_decodeRequestCount;
    ImageFrame::Status m_status;
    ImageFrame::Status m_nextFrameStatus;
    size_t m_frameCount;
    size_t m_requestedClearExceptFrame;
};

TEST_F(ImageFrameGeneratorTest, incompleteDecode)
{
    setFrameStatus(ImageFrame::FramePartial);

    char buffer[100 * 100 * 4];
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(1, m_decodeRequestCount);

    addNewData();
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(2, m_decodeRequestCount);
    EXPECT_EQ(0, m_decodersDestroyed);
}

TEST_F(ImageFrameGeneratorTest, incompleteDecodeBecomesComplete)
{
    setFrameStatus(ImageFrame::FramePartial);

    char buffer[100 * 100 * 4];
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(1, m_decodeRequestCount);
    EXPECT_EQ(0, m_decodersDestroyed);

    setFrameStatus(ImageFrame::FrameComplete);
    addNewData();

    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(2, m_decodeRequestCount);
    EXPECT_EQ(1, m_decodersDestroyed);

    // Decoder created again.
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(3, m_decodeRequestCount);
}

static void decodeThreadMain(ImageFrameGenerator* generator)
{
    char buffer[100 * 100 * 4];
    generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
}

static void decodeThreadWithRefEncodedMain(ImageFrameGenerator* generator)
{
    // Image must be complete - refEncodedData otherwise returns null.
    char buffer[100 * 100 * 4];
    SkData* data = generator->refEncodedData();
    generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    data->unref();
}

TEST_F(ImageFrameGeneratorTest, incompleteDecodeBecomesCompleteMultiThreaded)
{
    setFrameStatus(ImageFrame::FramePartial);

    char buffer[100 * 100 * 4];
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(1, m_decodeRequestCount);
    EXPECT_EQ(0, m_decodersDestroyed);
    SkData* data = m_generator->refEncodedData();
    EXPECT_EQ(nullptr, data);

    // LocalFrame can now be decoded completely.
    setFrameStatus(ImageFrame::FrameComplete);
    addNewData();
    // addNewData is calling m_generator->setData with allDataReceived == false, which means that
    // refEncodedData should return null.
    data = m_generator->refEncodedData();
    EXPECT_EQ(nullptr, data);
    OwnPtr<WebThread> thread = adoptPtr(Platform::current()->createThread("DecodeThread"));
    thread->taskRunner()->postTask(BLINK_FROM_HERE, threadSafeBind(&decodeThreadMain, AllowCrossThreadAccess(m_generator.get())));
    thread.clear();
    EXPECT_EQ(2, m_decodeRequestCount);
    EXPECT_EQ(1, m_decodersDestroyed);

    // Decoder created again.
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(3, m_decodeRequestCount);

    addNewData(true);
    data = m_generator->refEncodedData();
    ASSERT_TRUE(data);
    // To prevent data writting, SkData::unique() should be false.
    ASSERT_TRUE(!data->unique());

    // Thread will also ref and unref the data.
    thread = adoptPtr(Platform::current()->createThread("RefEncodedDataThread"));
    thread->taskRunner()->postTask(BLINK_FROM_HERE, threadSafeBind(&decodeThreadWithRefEncodedMain, AllowCrossThreadAccess(m_generator.get())));
    thread.clear();
    EXPECT_EQ(4, m_decodeRequestCount);

    data->unref();
    // m_generator is holding the only reference to SkData now.
    ASSERT_TRUE(data->unique());

    data = m_generator->refEncodedData();
    ASSERT_TRUE(data && !data->unique());

    // Delete generator, and SkData should have the only reference.
    m_generator = nullptr;
    ASSERT_TRUE(data->unique());
    data->unref();
}

TEST_F(ImageFrameGeneratorTest, frameHasAlpha)
{
    setFrameStatus(ImageFrame::FramePartial);

    char buffer[100 * 100 * 4];
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_TRUE(m_generator->hasAlpha(0));
    EXPECT_EQ(1, m_decodeRequestCount);

    ImageDecoder* tempDecoder = 0;
    EXPECT_TRUE(ImageDecodingStore::instance().lockDecoder(m_generator.get(), fullSize(), &tempDecoder));
    ASSERT_TRUE(tempDecoder);
    tempDecoder->frameBufferAtIndex(0)->setHasAlpha(false);
    ImageDecodingStore::instance().unlockDecoder(m_generator.get(), tempDecoder);
    EXPECT_EQ(2, m_decodeRequestCount);

    setFrameStatus(ImageFrame::FrameComplete);
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(3, m_decodeRequestCount);
    EXPECT_FALSE(m_generator->hasAlpha(0));
}

TEST_F(ImageFrameGeneratorTest, clearMultiFrameDecoder)
{
    setFrameCount(3);
    setFrameStatus(ImageFrame::FrameComplete);

    char buffer[100 * 100 * 4];
    m_generator->decodeAndScale(0, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(1, m_decodeRequestCount);
    EXPECT_EQ(0, m_decodersDestroyed);
    EXPECT_EQ(0U, m_requestedClearExceptFrame);

    setFrameStatus(ImageFrame::FrameComplete);

    m_generator->decodeAndScale(1, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(2, m_decodeRequestCount);
    EXPECT_EQ(0, m_decodersDestroyed);
    EXPECT_EQ(1U, m_requestedClearExceptFrame);

    setFrameStatus(ImageFrame::FrameComplete);

    // Decoding the last frame of a multi-frame images should trigger clearing
    // all the frame data, but not destroying the decoder.  See comments in
    // ImageFrameGenerator::tryToResumeDecode().
    m_generator->decodeAndScale(2, imageInfo(), buffer, 100 * 4);
    EXPECT_EQ(3, m_decodeRequestCount);
    EXPECT_EQ(0, m_decodersDestroyed);
    EXPECT_EQ(kNotFound, m_requestedClearExceptFrame);
}

} // namespace blink
