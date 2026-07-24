// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/tunneling/TcpForward.h"

#include "gtest/gtest.h"
#include <aws/common/allocator.h>

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
