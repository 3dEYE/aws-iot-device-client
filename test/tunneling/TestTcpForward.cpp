// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/tunneling/TcpForward.h"

#include "gtest/gtest.h"
#include <aws/common/allocator.h>
#include <aws/crt/io/EventLoopGroup.h>
#include <chrono>
#include <thread>

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

                    static void invokeOnReadable(TcpForward &tcpForward, int errorCode)
                    {
                        tcpForward.OnReadable(nullptr, errorCode);
                    }

                    static void setConnected(TcpForward &tcpForward, bool connected)
                    {
                        tcpForward.mConnected = connected;
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
            const string &bufferedData)
            : TcpForward(resourceManager, 0), bufferedData(bufferedData)
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
            return aws_raise_error(AWS_IO_SOCKET_CLOSED);
        }

        size_t getReadCallCount() const { return readCallCount; }

      private:
        string bufferedData;
        bool dataRead{false};
        size_t readCallCount{0};
    };

    class DeferredWriteTcpForward : public TcpForward
    {
      public:
        explicit DeferredWriteTcpForward(const shared_ptr<SharedCrtResourceManager> &resourceManager)
            : TcpForward(resourceManager, 0)
        {
        }

        int WriteSocket(
            const aws_byte_cursor &data,
            aws_socket_on_write_completed_fn *onWriteCompleted,
            void *userData) override
        {
            pendingCursor = data;
            pendingCallback = onWriteCompleted;
            pendingUserData = userData;
            return writeResult;
        }

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

        int writeResult{AWS_OP_SUCCESS};

      private:
        aws_byte_cursor pendingCursor{};
        aws_socket_on_write_completed_fn *pendingCallback{nullptr};
        void *pendingUserData{nullptr};
    };
} // namespace

TEST(TcpForward, ReadableSocketErrorDoesNotForwardData)
{
    auto resourceManager = make_shared<SharedCrtResourceManager>();
    TcpForward tcpForward(resourceManager, 0);
    size_t receiveCallbackCount = 0;
    TcpForwardTestAccess::setDataReceiveCallback(
        tcpForward, [&receiveCallbackCount](const Aws::Crt::ByteBuf &) { ++receiveCallbackCount; });

    TcpForwardTestAccess::invokeOnReadable(tcpForward, AWS_IO_SOCKET_NOT_CONNECTED);

    EXPECT_EQ(0U, receiveCallbackCount);
}

TEST(TcpForward, PeerCloseDrainsBufferedDataBeforeReturning)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    BufferedReadTcpForward tcpForward(resourceManager, "tail");
    size_t receiveCallbackCount = 0;
    string forwardedData;
    TcpForwardTestAccess::setDataReceiveCallback(
        tcpForward, [&receiveCallbackCount, &forwardedData](const Aws::Crt::ByteBuf &data) {
            ++receiveCallbackCount;
            forwardedData.assign(reinterpret_cast<const char *>(data.buffer), data.len);
        });

    TcpForwardTestAccess::invokeOnReadable(tcpForward, AWS_IO_SOCKET_CLOSED);

    EXPECT_EQ(1U, receiveCallbackCount);
    EXPECT_EQ("tail", forwardedData);
    EXPECT_EQ(2U, tcpForward.getReadCallCount());
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

TEST(TcpForward, SocketWriteQueueFailureIsReturned)
{
    auto resourceManager = make_shared<TestCrtResourceManager>();
    auto tcpForward = make_shared<DeferredWriteTcpForward>(resourceManager);
    TcpForwardTestAccess::setConnected(*tcpForward, true);
    tcpForward->writeResult = AWS_OP_ERR;

    string source = "payload";
    auto cursor = aws_byte_cursor_from_array(source.data(), source.size());

    EXPECT_EQ(AWS_OP_ERR, tcpForward->SendData(cursor));
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
