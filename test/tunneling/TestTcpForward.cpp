// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/tunneling/TcpForward.h"

#include "gtest/gtest.h"

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
