// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/tunneling/TcpForward.h"

#include "gtest/gtest.h"
#include <aws/common/allocator.h>
#include <aws/crt/io/EventLoopGroup.h>
#include <chrono>
#include <thread>
#include <vector>

using namespace std;
using namespace Aws::Iot::DeviceClient;
using namespace Aws::Iot::DeviceClient::SecureTunneling;

namespace Aws
{
    namespace Iot
    {
        namespace DeviceClient
        {
            namespace SecureTunneling
            {
                class TcpForwardTestAccess
                {
                  public:
                    static void setDataReceiveCallback(
                        TcpForward &tcpForward,
                        const OnTcpForwardDataReceive &onTcpForwardDataReceive)
                    {
                        tcpForward.mOnTcpForwardDataReceive = onTcpForwardDataReceive;
                    }

                    static void setTerminatedCallback(
                        TcpForward &tcpForward,
                        const OnTcpForwardTerminated &onTcpForwardTerminated)
                    {
                        tcpForward.mOnTcpForwardTerminated = onTcpForwardTerminated;
                    }

                    static void retainCallbackLifetime(TcpForward &tcpForward)
                    {
                        tcpForward.mConnectStarted = true;
                        tcpForward.mLifetimeKeepAlive = tcpForward.shared_from_this();
                    }

                    static void invokeOnConnectionResult(TcpForward &tcpForward, int errorCode)
                    {
                        tcpForward.OnConnectionResult(nullptr, errorCode);
                    }

                    static void invokeOnReadable(TcpForward &tcpForward, int errorCode)
                    {
                        tcpForward.OnReadable(nullptr, errorCode);
                    }

                    static void setConnected(TcpForward &tcpForward, bool connected)
                    {
                        tcpForward.mConnected = connected;
                    }

                    static bool isStopped(const TcpForward &tcpForward) { return tcpForward.mStopped; }

                    static size_t sendBufferLength(const TcpForward &tcpForward)
                    {
                        return tcpForward.mSendBuffer.len;
                    }
                };
            } // namespace SecureTunneling
        }     // namespace DeviceClient
    }         // namespace Iot
} // namespace Aws

namespace
{
    class TestCrtResourceManager : public SharedCrtResourceManager
    {
      public:
        aws_allocator *getAllocator() override { return aws_default_allocator(); }
    };

    class SocketTestCrtResourceManager : public SharedCrtResourceManager
    {
      public:
        SocketTestCrtResourceManager()
            : apiHandle(), allocator(aws_default_allocator()), eventLoopGroup(1)
        {
        }

        aws_event_loop *getNextEventLoop() override
        {
            return aws_event_loop_group_get_next_loop(eventLoopGroup.GetUnderlyingHandle());
        }
        aws_allocator *getAllocator() override { return allocator; }

      private:
        Aws::Crt::ApiHandle apiHandle;
        aws_allocator *allocator;
        Aws::Crt::Io::EventLoopGroup eventLoopGroup;
    };

    class BufferedReadTcpForward : public TcpForward
    {
      public:
        BufferedReadTcpForward(
            const shared_ptr<SharedCrtResourceManager> &resourceManager,
            const string &bufferedData,
            int finalReadError)
            : TcpForward(resourceManager, 0), bufferedData(bufferedData), finalReadError(finalReadError)
        {
        }

        int ReadSocket(aws_byte_buf *buffer, size_t *amountRead) override
        {
            ++readCallCount;
            if (!dataRead)
            {
                const auto *data = reinterpret_cast<const uint8_t *>(bufferedData.data());
                aws_byte_buf_write(buffer, data, bufferedData.size());
                *amountRead = bufferedData.size();
                dataRead = true;
                return AWS_OP_SUCCESS;
            }

            *amountRead = 0;
            return aws_raise_error(finalReadError);
        }

        size_t getReadCallCount() const { return readCallCount; }

      private:
        string bufferedData;
        int finalReadError;
        bool dataRead{false};
        size_t readCallCount{0};
    };

    class SubscribeFailureTcpForward : public TcpForward
    {
      public:
        SubscribeFailureTcpForward(
            const shared_ptr<SharedCrtResourceManager> &resourceManager,
            const OnTcpForwardTerminated &onTcpForwardTerminated)
            : TcpForward(resourceManager, 0, nullptr, onTcpForwardTerminated)
        {
        }

        int SubscribeToReadableEvents() override
        {
            return aws_raise_error(AWS_IO_SOCKET_NOT_CONNECTED);
        }
    };

    class DeferredWriteTcpForward : public TcpForward
    {
      public:
        explicit DeferredWriteTcpForward(
            const shared_ptr<SharedCrtResourceManager> &resourceManager,
            const OnTcpForwardTerminated &onTcpForwardTerminated = {})
            : TcpForward(resourceManager, 0, nullptr, onTcpForwardTerminated)
        {
        }

        int WriteSocket(
            const aws_byte_cursor &data,
            aws_socket_on_write_completed_fn *onWriteCompleted,
            void *userData) override
        {
            if (writeError != AWS_OP_SUCCESS)
            {
                return aws_raise_error(writeError);
            }
            pendingCursor = data;
            pendingCallback = onWriteCompleted;
            pendingUserData = userData;
            return AWS_OP_SUCCESS;
        }

        int SubscribeToReadableEvents() override { return AWS_OP_SUCCESS; }

        string PendingPayload() const
        {
            return string(
                reinterpret_cast<const char *>(pendingCursor.ptr),
                pendingCursor.len);
        }

        void CompleteWrite(int errorCode = AWS_OP_SUCCESS)
        {
            ASSERT_NE(nullptr, pendingCallback);
            auto callback = pendingCallback;
            pendingCallback = nullptr;
            callback(nullptr, errorCode, pendingCursor.len, pendingUserData);
            pendingUserData = nullptr;
        }

        int writeError{AWS_OP_SUCCESS};

      private:
        aws_byte_cursor pendingCursor{};
        aws_socket_on_write_completed_fn *pendingCallback{nullptr};
        void *pendingUserData{nullptr};
    };
} // namespace

TEST(TcpForward, ReadableSocketErrorDoesNotForwardData)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    size_t terminatedCallbackCount = 0;
    auto tcpForward = make_shared<TcpForward>(
        resourceManager,
        0,
        nullptr,
        [&terminatedCallbackCount](TcpForward *, int) { ++terminatedCallbackCount; });
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);
    size_t receiveCallbackCount = 0;
    TcpForwardTestAccess::setDataReceiveCallback(
        *tcpForward, [&receiveCallbackCount](const Aws::Crt::ByteBuf &) { ++receiveCallbackCount; });

    TcpForwardTestAccess::invokeOnReadable(*tcpForward, AWS_IO_SOCKET_NOT_CONNECTED);
    TcpForwardTestAccess::invokeOnReadable(*tcpForward, AWS_IO_SOCKET_NOT_CONNECTED);

    EXPECT_EQ(0U, receiveCallbackCount);
    EXPECT_EQ(1U, terminatedCallbackCount);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
}

TEST(TcpForward, PeerCloseDrainsBufferedDataBeforeTerminating)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    auto tcpForward =
        make_shared<BufferedReadTcpForward>(resourceManager, "tail", AWS_IO_SOCKET_CLOSED);
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);
    size_t receiveCallbackCount = 0;
    string forwardedData;
    vector<string> callbackOrder;
    TcpForwardTestAccess::setDataReceiveCallback(
        *tcpForward, [&receiveCallbackCount, &forwardedData, &callbackOrder](const Aws::Crt::ByteBuf &data) {
            ++receiveCallbackCount;
            forwardedData.assign(reinterpret_cast<const char *>(data.buffer), data.len);
            callbackOrder.emplace_back("data");
        });
    TcpForwardTestAccess::setTerminatedCallback(
        *tcpForward, [&callbackOrder](TcpForward *, int) { callbackOrder.emplace_back("terminated"); });

    TcpForwardTestAccess::invokeOnReadable(*tcpForward, AWS_IO_SOCKET_CLOSED);

    EXPECT_EQ(1U, receiveCallbackCount);
    EXPECT_EQ("tail", forwardedData);
    EXPECT_EQ(2U, tcpForward->getReadCallCount());
    EXPECT_EQ((vector<string>{"data", "terminated"}), callbackOrder);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
}

TEST(TcpForward, ReadWouldBlockDoesNotTerminate)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    auto tcpForward =
        make_shared<BufferedReadTcpForward>(resourceManager, "available", AWS_IO_READ_WOULD_BLOCK);
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);
    size_t receiveCallbackCount = 0;
    size_t terminatedCallbackCount = 0;
    TcpForwardTestAccess::setDataReceiveCallback(
        *tcpForward, [&receiveCallbackCount](const Aws::Crt::ByteBuf &) { ++receiveCallbackCount; });
    TcpForwardTestAccess::setTerminatedCallback(
        *tcpForward, [&terminatedCallbackCount](TcpForward *, int) { ++terminatedCallbackCount; });

    TcpForwardTestAccess::invokeOnReadable(*tcpForward, AWS_OP_SUCCESS);

    EXPECT_EQ(1U, receiveCallbackCount);
    EXPECT_EQ(0U, terminatedCallbackCount);
    EXPECT_FALSE(TcpForwardTestAccess::isStopped(*tcpForward));

    tcpForward->Stop();
}

TEST(TcpForward, HardReadErrorDrainsBufferedDataBeforeTerminating)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    auto tcpForward =
        make_shared<BufferedReadTcpForward>(resourceManager, "tail", AWS_IO_SOCKET_NOT_CONNECTED);
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);
    string forwardedData;
    int terminalError = AWS_OP_SUCCESS;
    vector<string> callbackOrder;
    TcpForwardTestAccess::setDataReceiveCallback(
        *tcpForward, [&forwardedData, &callbackOrder](const Aws::Crt::ByteBuf &data) {
            forwardedData.assign(reinterpret_cast<const char *>(data.buffer), data.len);
            callbackOrder.emplace_back("data");
        });
    TcpForwardTestAccess::setTerminatedCallback(
        *tcpForward, [&terminalError, &callbackOrder](TcpForward *, int errorCode) {
            terminalError = errorCode;
            callbackOrder.emplace_back("terminated");
        });

    TcpForwardTestAccess::invokeOnReadable(*tcpForward, AWS_OP_SUCCESS);

    EXPECT_EQ("tail", forwardedData);
    EXPECT_EQ(AWS_IO_SOCKET_NOT_CONNECTED, terminalError);
    EXPECT_EQ((vector<string>{"data", "terminated"}), callbackOrder);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
}

TEST(TcpForward, AsyncConnectFailureReleasesCallbackLifetime)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    size_t terminatedCallbackCount = 0;
    auto tcpForward = make_shared<TcpForward>(
        resourceManager,
        0,
        nullptr,
        [&terminatedCallbackCount](TcpForward *, int) { ++terminatedCallbackCount; });
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);
    weak_ptr<TcpForward> weakTcpForward = tcpForward;

    TcpForwardTestAccess::invokeOnConnectionResult(*tcpForward, AWS_IO_SOCKET_CONNECTION_REFUSED);

    EXPECT_EQ(1U, terminatedCallbackCount);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
    tcpForward.reset();
    EXPECT_TRUE(weakTcpForward.expired());
}

TEST(TcpForward, ReadableSubscriptionFailureReleasesCallbackLifetime)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    size_t terminatedCallbackCount = 0;
    auto tcpForward = make_shared<SubscribeFailureTcpForward>(
        resourceManager,
        [&terminatedCallbackCount](TcpForward *, int) { ++terminatedCallbackCount; });
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);
    weak_ptr<TcpForward> weakTcpForward = tcpForward;

    TcpForwardTestAccess::invokeOnConnectionResult(*tcpForward, AWS_OP_SUCCESS);

    EXPECT_EQ(1U, terminatedCallbackCount);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
    tcpForward.reset();
    EXPECT_TRUE(weakTcpForward.expired());
}

TEST(TcpForward, PreConnectionBufferLimitTerminatesForward)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    int terminalError = AWS_OP_SUCCESS;
    auto tcpForward = make_shared<TcpForward>(
        resourceManager,
        0,
        nullptr,
        [&terminalError](TcpForward *, int errorCode) { terminalError = errorCode; });
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);

    vector<uint8_t> maximumPayload(63 * 1024, 0x1);
    auto maximumPayloadCursor =
        aws_byte_cursor_from_array(maximumPayload.data(), maximumPayload.size());
    ASSERT_EQ(AWS_OP_SUCCESS, tcpForward->SendData(maximumPayloadCursor));
    EXPECT_EQ(maximumPayload.size(), TcpForwardTestAccess::sendBufferLength(*tcpForward));

    uint8_t extraByte = 0x2;
    auto extraByteCursor = aws_byte_cursor_from_array(&extraByte, 1);
    EXPECT_EQ(AWS_OP_ERR, tcpForward->SendData(extraByteCursor));
    EXPECT_EQ(AWS_ERROR_SHORT_BUFFER, terminalError);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
}

TEST(TcpForward, SocketWriteOwnsPayloadUntilCompletion)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    auto tcpForward = make_shared<DeferredWriteTcpForward>(resourceManager);
    TcpForwardTestAccess::setConnected(*tcpForward, true);

    string source = "forwarded payload";
    auto cursor = aws_byte_cursor_from_array(source.data(), source.size());
    ASSERT_EQ(AWS_OP_SUCCESS, tcpForward->SendData(cursor));

    source.assign(source.size(), 'x');
    EXPECT_EQ("forwarded payload", tcpForward->PendingPayload());

    tcpForward->CompleteWrite();
}

TEST(TcpForward, SocketWriteQueueFailureTerminatesForward)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    int terminalError = AWS_OP_SUCCESS;
    auto tcpForward = make_shared<DeferredWriteTcpForward>(
        resourceManager,
        [&terminalError](TcpForward *, int errorCode) { terminalError = errorCode; });
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);
    TcpForwardTestAccess::setConnected(*tcpForward, true);
    tcpForward->writeError = AWS_IO_SOCKET_NOT_CONNECTED;

    string source = "payload";
    auto cursor = aws_byte_cursor_from_array(source.data(), source.size());

    EXPECT_EQ(AWS_OP_ERR, tcpForward->SendData(cursor));
    EXPECT_EQ(AWS_IO_SOCKET_NOT_CONNECTED, terminalError);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
}

TEST(TcpForward, BufferedWriteQueueFailureTerminatesAfterConnection)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    int terminalError = AWS_OP_SUCCESS;
    auto tcpForward = make_shared<DeferredWriteTcpForward>(
        resourceManager,
        [&terminalError](TcpForward *, int errorCode) { terminalError = errorCode; });
    TcpForwardTestAccess::retainCallbackLifetime(*tcpForward);

    string source = "buffered payload";
    auto cursor = aws_byte_cursor_from_array(source.data(), source.size());
    ASSERT_EQ(AWS_OP_SUCCESS, tcpForward->SendData(cursor));
    tcpForward->writeError = AWS_IO_SOCKET_NOT_CONNECTED;

    TcpForwardTestAccess::invokeOnConnectionResult(*tcpForward, AWS_OP_SUCCESS);

    EXPECT_EQ(AWS_IO_SOCKET_NOT_CONNECTED, terminalError);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
}

TEST(TcpForward, SocketWriteCompletionFailureTerminatesForward)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    int terminalError = AWS_OP_SUCCESS;
    auto tcpForward = make_shared<DeferredWriteTcpForward>(
        resourceManager,
        [&terminalError](TcpForward *, int errorCode) { terminalError = errorCode; });
    TcpForwardTestAccess::setConnected(*tcpForward, true);

    string source = "payload";
    auto cursor = aws_byte_cursor_from_array(source.data(), source.size());
    ASSERT_EQ(AWS_OP_SUCCESS, tcpForward->SendData(cursor));

    tcpForward->CompleteWrite(AWS_IO_SOCKET_NOT_CONNECTED);

    EXPECT_EQ(AWS_IO_SOCKET_NOT_CONNECTED, terminalError);
    EXPECT_TRUE(TcpForwardTestAccess::isStopped(*tcpForward));
}

TEST(TcpForward, StopReleasesPendingConnectCallbackLifetime)
{
    auto resourceManager = make_shared<SocketTestCrtResourceManager>();
    auto tcpForward = make_shared<TcpForward>(
        resourceManager,
        1,
        [](const Aws::Crt::ByteBuf &) {});

    tcpForward->Connect();
    weak_ptr<TcpForward> weakTcpForward = tcpForward;
    tcpForward->Stop();
    tcpForward.reset();

    for (size_t attempt = 0; attempt < 300 && !weakTcpForward.expired(); ++attempt)
    {
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    EXPECT_TRUE(weakTcpForward.expired());
}
