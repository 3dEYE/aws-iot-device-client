// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SecureTunnelWrapper.h"
#include <aws/iotdevice/secure_tunneling.h>

using namespace Aws;
using namespace Aws::Iot;
using namespace Aws::Iot::DeviceClient;
using namespace Aws::Iot::DeviceClient::SecureTunneling;

SecureTunnelWrapper::SecureTunnelWrapper(
    Aws::Crt::Allocator *allocator,
    Aws::Crt::Io::ClientBootstrap *bootstrap,
    const Aws::Crt::Io::SocketOptions &socketOptions,
    const std::string &accessToken,
    aws_secure_tunneling_local_proxy_mode localProxyMode,
    const std::string &endpoint,
    const std::string &rootCa,
    const Aws::Iotsecuretunneling::OnConnectionComplete &onConnectionComplete,
    const Aws::Iotsecuretunneling::OnConnectionShutdown &onConnectionShutdown,
    const Aws::Iotsecuretunneling::OnSendDataComplete &onSendDataComplete,
    const Aws::Iotsecuretunneling::OnDataReceive &onDataReceive,
    const Aws::Iotsecuretunneling::OnStreamStart &onStreamStart,
    const Aws::Iotsecuretunneling::OnStreamReset &onStreamReset,
    const Aws::Iotsecuretunneling::OnSessionReset &onSessionReset,
    const Aws::Iotsecuretunneling::OnStopped &onStopped)
    : secureTunnel((Aws::Iotsecuretunneling::SecureTunnelBuilder(
                        allocator,
                        *bootstrap,
                        socketOptions,
                        accessToken,
                        localProxyMode,
                        endpoint))
                       .WithRootCa(rootCa)
                       .WithOnConnectionComplete(onConnectionComplete)
                       .WithOnConnectionShutdown(onConnectionShutdown)
                       .WithOnSendDataComplete(onSendDataComplete)
                       .WithOnDataReceive(onDataReceive)
                       .WithOnStreamStart(onStreamStart)
                       .WithOnStreamReset(onStreamReset)
                       .WithOnSessionReset(onSessionReset)
                       .WithOnStopped(onStopped)
                       .Build())
{
}

SecureTunnelWrapper::SecureTunnelWrapper(
    Aws::Crt::Allocator *allocator,
    Aws::Crt::Io::ClientBootstrap *bootstrap,
    const Aws::Crt::Io::SocketOptions &socketOptions,
    const Aws::Crt::Http::HttpClientConnectionProxyOptions &proxyOptions,
    const std::string &accessToken,
    aws_secure_tunneling_local_proxy_mode localProxyMode,
    const std::string &endpoint,
    const std::string &rootCa,
    const Aws::Iotsecuretunneling::OnConnectionComplete &onConnectionComplete,
    const Aws::Iotsecuretunneling::OnConnectionShutdown &onConnectionShutdown,
    const Aws::Iotsecuretunneling::OnSendDataComplete &onSendDataComplete,
    const Aws::Iotsecuretunneling::OnDataReceive &onDataReceive,
    const Aws::Iotsecuretunneling::OnStreamStart &onStreamStart,
    const Aws::Iotsecuretunneling::OnStreamReset &onStreamReset,
    const Aws::Iotsecuretunneling::OnSessionReset &onSessionReset,
    const Aws::Iotsecuretunneling::OnStopped &onStopped)
    : secureTunnel((Aws::Iotsecuretunneling::SecureTunnelBuilder(
                        allocator,
                        *bootstrap,
                        socketOptions,
                        accessToken,
                        localProxyMode,
                        endpoint))
                       .WithHttpClientConnectionProxyOptions(proxyOptions)
                       .WithRootCa(rootCa)
                       .WithOnConnectionComplete(onConnectionComplete)
                       .WithOnConnectionShutdown(onConnectionShutdown)
                       .WithOnSendDataComplete(onSendDataComplete)
                       .WithOnDataReceive(onDataReceive)
                       .WithOnStreamStart(onStreamStart)
                       .WithOnStreamReset(onStreamReset)
                       .WithOnSessionReset(onSessionReset)
                       .WithOnStopped(onStopped)
                       .Build())
{
}

int SecureTunnelWrapper::Connect()
{
    return secureTunnel ? secureTunnel->Connect() : AWS_OP_ERR;
}

int SecureTunnelWrapper::Close()
{
    return secureTunnel ? secureTunnel->Close() : AWS_OP_ERR;
}

int SecureTunnelWrapper::SendData(const Aws::Crt::ByteCursor &data)
{
    return secureTunnel ? secureTunnel->SendData(data) : AWS_OP_ERR;
}

int SecureTunnelWrapper::SendStreamReset()
{
    if (!secureTunnel || !secureTunnel->GetUnderlyingHandle())
    {
        return AWS_OP_ERR;
    }

    // The pinned SDK's C++ helper passes a null options pointer to a non-null C API contract.
    // Supply the empty V1 message view explicitly.
    aws_secure_tunnel_message_view messageOptions{};
    return aws_secure_tunnel_stream_reset(secureTunnel->GetUnderlyingHandle(), &messageOptions);
}

void SecureTunnelWrapper::Shutdown()
{
    if (secureTunnel)
    {
        secureTunnel->Shutdown();
    }
}
bool SecureTunnelWrapper::IsValid()
{
    return secureTunnel && secureTunnel->IsValid();
}
