// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SecureTunnelingContext.h"
#include "../logging/LoggerFactory.h"
#include "SecureTunnelingFeature.h"
#include <aws/iotsecuretunneling/SecureTunnel.h>
#include <aws/io/event_loop.h>

using namespace std;
using namespace Aws::Iotsecuretunneling;
using namespace Aws::Iot::DeviceClient::Logging;

namespace
{
    struct SecureTunnelingLifecycleTask
    {
        aws_task task;
        std::function<void()> callback;
    };

    void RunSecureTunnelingLifecycleTask(aws_task *task, void *arg, enum aws_task_status status)
    {
        (void)task;
        unique_ptr<SecureTunnelingLifecycleTask> lifecycleTask(
            static_cast<SecureTunnelingLifecycleTask *>(arg));
        if (status == AWS_TASK_STATUS_RUN_READY)
        {
            lifecycleTask->callback();
        }
    }
} // namespace

namespace Aws
{
    namespace Iot
    {
        namespace DeviceClient
        {
            namespace SecureTunneling
            {
                constexpr char SecureTunnelingContext::TAG[];

                SecureTunnelingContext::SecureTunnelingContext(
                    shared_ptr<SharedCrtResourceManager> manager,
                    const Aws::Crt::Optional<std::string> &rootCa,
                    const string &accessToken,
                    const string &endpoint,
                    const int port,
                    const OnStoppedFn &onStopped)
                    : mSharedCrtResourceManager(manager), mRootCa(rootCa.has_value() ? rootCa.value() : ""),
                      mAccessToken(accessToken), mEndpoint(endpoint), mPort(port), mOnStopped(onStopped)
                {
                }

                SecureTunnelingContext::SecureTunnelingContext(
                    shared_ptr<SharedCrtResourceManager> manager,
                    const Aws::Crt::Http::HttpClientConnectionProxyOptions &proxyOptions,
                    const Aws::Crt::Optional<std::string> &rootCa,
                    const string &accessToken,
                    const string &endpoint,
                    const int port,
                    const OnStoppedFn &onStopped)
                    : mSharedCrtResourceManager(manager), mProxyOptions(proxyOptions),
                      mRootCa(rootCa.has_value() ? rootCa.value() : ""), mAccessToken(accessToken), mEndpoint(endpoint),
                      mPort(port), mOnStopped(onStopped)
                {
                }

                SecureTunnelingContext::~SecureTunnelingContext() = default;

                template <typename T>
                static bool operator==(const Aws::Crt::Optional<T> &lhs, const Aws::Crt::Optional<T> &rhs)
                {
                    if (!lhs.has_value() && !rhs.has_value())
                    {
                        return true;
                    }
                    else if (lhs.has_value() && rhs.has_value())
                    {
                        return lhs.value() == rhs.value();
                    }
                    else
                    {
                        return false;
                    }
                }

                static bool operator==(
                    const SecureTunnelingNotifyResponse &lhs,
                    const SecureTunnelingNotifyResponse &rhs)
                {
                    return lhs.Region == rhs.Region && lhs.ClientMode == rhs.ClientMode &&
                           lhs.Services == rhs.Services && lhs.ClientAccessToken == rhs.ClientAccessToken;
                }

                bool SecureTunnelingContext::IsDuplicateNotification(
                    const Aws::Iotsecuretunneling::SecureTunnelingNotifyResponse &response)
                {
                    if (mLastSeenNotifyResponse.has_value() && mLastSeenNotifyResponse.value() == response)
                    {
                        return true;
                    }

                    mLastSeenNotifyResponse = response;
                    return false;
                }

                bool SecureTunnelingContext::ConnectToSecureTunnel()
                {
                    if (mAccessToken.empty())
                    {
                        LOG_ERROR(TAG, "Cannot connect to secure tunnel. Access token is missing.");
                        return false;
                    }

                    if (mEndpoint.empty())
                    {
                        LOG_ERROR(TAG, "Cannot connect to secure tunnel. Endpoint is missing.");
                        return false;
                    }

                    auto secureTunnel = CreateSecureTunnel(
                        bind(&SecureTunnelingContext::OnConnectionComplete, this),
                        nullptr,
                        bind(&SecureTunnelingContext::OnSendDataComplete, this, placeholders::_1),
                        bind(&SecureTunnelingContext::OnDataReceive, this, placeholders::_1),
                        bind(&SecureTunnelingContext::OnStreamStart, this),
                        bind(&SecureTunnelingContext::OnStreamReset, this),
                        bind(&SecureTunnelingContext::OnSessionReset, this),
                        bind(&SecureTunnelingContext::OnStopped, this, placeholders::_1));

                    if (!secureTunnel || !secureTunnel->IsValid())
                    {
                        LOG_ERROR(TAG, "Cannot create secure tunnel. Please see the SDK log for detail.");
                        return false;
                    }

                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopRequested)
                        {
                            return false;
                        }
                        mSecureTunnel = secureTunnel;
                        mConnectInProgress = true;
                    }

                    bool connectionSuccess = secureTunnel->Connect() == AWS_OP_SUCCESS;
                    bool stopAfterConnect = false;
                    shared_ptr<SecureTunnelingContext> failedConnectKeepAlive;

                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        mConnectInProgress = false;
                        if (!connectionSuccess)
                        {
                            mSecureTunnel.reset();
                            if (mStopRequested)
                            {
                                mStopped = true;
                                failedConnectKeepAlive = std::move(mLifetimeKeepAlive);
                            }
                        }
                        else if (mStopRequested)
                        {
                            stopAfterConnect = true;
                        }
                    }

                    if (!connectionSuccess)
                    {
                        LOG_ERROR(TAG, "Cannot connect to secure tunnel. Please see the SDK log for detail.");
                    }
                    else if (stopAfterConnect)
                    {
                        StopSecureTunnel();
                    }

                    return connectionSuccess;
                }

                void SecureTunnelingContext::ConnectToTcpForward()
                {
                    if (!SecureTunnelingFeature::IsValidPort(mPort))
                    {
                        LOGM_ERROR(TAG, "Cannot connect to invalid local port. port=%u", mPort);
                        ResetSecureTunnelStream();
                        return;
                    }

                    StopTcpForward();
                    auto tcpForward = CreateTcpForward();
                    if (!tcpForward)
                    {
                        LOG_ERROR(TAG, "Cannot create local TCP forward.");
                        ResetSecureTunnelStream();
                        return;
                    }

                    bool shouldConnect;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        shouldConnect = !mStopRequested && !mStopped;
                        if (shouldConnect)
                        {
                            mTcpForward = tcpForward;
                        }
                    }
                    if (!shouldConnect)
                    {
                        tcpForward->Stop();
                        return;
                    }

                    if (tcpForward->Connect() != AWS_OP_SUCCESS)
                    {
                        int connectError = aws_last_error();
                        LOG_ERROR(TAG, "Cannot connect to local TCP port.");
                        tcpForward->Stop();
                        OnTcpForwardTerminated(tcpForward.get(), connectError);
                    }
                }

                void SecureTunnelingContext::DisconnectFromTcpForward() { StopTcpForward(); }

                void SecureTunnelingContext::StopTcpForward()
                {
                    shared_ptr<TcpForward> tcpForward;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        tcpForward = std::move(mTcpForward);
                    }
                    if (tcpForward)
                    {
                        tcpForward->Stop();
                    }
                }

                void SecureTunnelingContext::ResetSecureTunnelStream()
                {
                    shared_ptr<SecureTunnelWrapper> secureTunnel;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopRequested || mStopped || !mSecureTunnel)
                        {
                            return;
                        }
                        secureTunnel = mSecureTunnel;
                    }

                    if (secureTunnel->SendStreamReset() != AWS_OP_SUCCESS)
                    {
                        LOG_ERROR(TAG, "Cannot reset secure tunnel stream; stopping the tunnel.");
                        StopSecureTunnel();
                    }
                }

                void SecureTunnelingContext::OnConnectionComplete() const
                {
                    LOG_DEBUG(TAG, "SecureTunnelingContext::OnConnectionComplete");
                }

                void SecureTunnelingContext::OnSendDataComplete(int errorCode) const
                {
                    LOG_TRACE(TAG, "SecureTunnelingContext::OnSendDataComplete");
                    if (errorCode)
                    {
                        LOGM_ERROR(TAG, "SecureTunnelingContext::OnSendDataComplete errorCode=%d", errorCode);
                    }
                }

                void SecureTunnelingContext::OnDataReceive(const Crt::ByteBuf &data)
                {
                    LOGM_TRACE(TAG, "SecureTunnelingContext::OnDataReceive data.len=%zu", data.len);
                    shared_ptr<TcpForward> tcpForward;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        tcpForward = mTcpForward;
                    }
                    if (tcpForward)
                    {
                        tcpForward->SendData(aws_byte_cursor_from_buf(&data));
                    }
                }

                void SecureTunnelingContext::OnStreamStart()
                {
                    LOG_DEBUG(TAG, "SecureTunnelingContext::OnStreamStart");
                    ConnectToTcpForward();
                }

                void SecureTunnelingContext::OnStreamReset()
                {
                    LOG_DEBUG(TAG, "SecureTunnelingContext::OnStreamReset");
                    DisconnectFromTcpForward();
                }

                void SecureTunnelingContext::OnSessionReset()
                {
                    LOG_DEBUG(TAG, "SecureTunnelingContext::OnSessionReset");
                    DisconnectFromTcpForward();
                }

                void SecureTunnelingContext::OnStopped(Aws::Iotsecuretunneling::SecureTunnel *secureTunnel)
                {
                    (void)secureTunnel;
                    LOG_DEBUG(TAG, "SecureTunnelingContext::OnStopped");

                    OnStoppedFn onStopped;
                    shared_ptr<SecureTunnelingContext> self;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopped)
                        {
                            return;
                        }
                        mStopped = true;
                        if (!mLifetimeKeepAlive)
                        {
                            mLifetimeKeepAlive = shared_from_this();
                        }
                        onStopped = mOnStopped;
                        self = mLifetimeKeepAlive;
                    }

                    if (onStopped)
                    {
                        onStopped(this);
                    }

                    bool releaseScheduled =
                        ScheduleLifecycleTask([self]() { self->ReleaseAfterStopped(); }, chrono::milliseconds(0));
                    if (!releaseScheduled)
                    {
                        LOG_ERROR(TAG, "Cannot schedule secure tunnel cleanup; retaining context for callback safety.");
                    }
                }

                void SecureTunnelingContext::OnTcpForwardDataReceive(const Crt::ByteBuf &data) const
                {
                    LOGM_TRACE(TAG, "SecureTunnelingContext::OnTcpForwardDataReceive data.len=%zu", data.len);
                    mSecureTunnel->SendData(aws_byte_cursor_from_buf(&data));
                }

                void SecureTunnelingContext::OnTcpForwardTerminated(TcpForward *tcpForward, int errorCode)
                {
                    LOGM_DEBUG(TAG, "Local TCP connection terminated. errorCode=%d", errorCode);
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mTcpForward.get() != tcpForward)
                        {
                            return;
                        }
                        mTcpForward.reset();
                    }

                    ResetSecureTunnelStream();
                }

                void SecureTunnelingContext::StopSecureTunnel()
                {
                    LOG_DEBUG(TAG, "SecureTunnelingContext::StopSecureTunnel");

                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        mStopRequested = true;
                        if (mSecureTunnel && !mLifetimeKeepAlive)
                        {
                            mLifetimeKeepAlive = shared_from_this();
                        }
                        if (
                            mConnectInProgress || mStopQueued || mStopTaskScheduled || mStopped || !mSecureTunnel)
                        {
                            return;
                        }
                    }

                    ScheduleStop(chrono::milliseconds(0));
                }

                void SecureTunnelingContext::ScheduleStop(chrono::milliseconds delay)
                {
                    shared_ptr<SecureTunnelingContext> self;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (
                            mConnectInProgress || mStopQueued || mStopTaskScheduled || mStopped || !mSecureTunnel)
                        {
                            return;
                        }

                        mStopTaskScheduled = true;
                        self = mLifetimeKeepAlive;
                    }

                    if (!ScheduleLifecycleTask([self]() { self->QueueStop(); }, delay))
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        mStopTaskScheduled = false;
                        LOG_ERROR(TAG, "Cannot schedule secure tunnel stop; retaining context for callback safety.");
                    }
                }

                void SecureTunnelingContext::QueueStop()
                {
                    shared_ptr<SecureTunnelWrapper> secureTunnel;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        mStopTaskScheduled = false;
                        if (mStopQueued || mStopped || !mSecureTunnel)
                        {
                            return;
                        }

                        mStopQueued = true;
                        secureTunnel = mSecureTunnel;
                    }

                    if (secureTunnel->Close() != AWS_OP_SUCCESS)
                    {
                        LOG_ERROR(TAG, "Cannot stop secure tunnel. Please see the SDK log for detail.");
                        {
                            lock_guard<mutex> lock(mLifecycleLock);
                            mStopQueued = false;
                        }
                        ScheduleStop(chrono::seconds(1));
                    }
                }

                void SecureTunnelingContext::ReleaseAfterStopped()
                {
                    StopTcpForward();
                    lock_guard<mutex> lock(mLifecycleLock);
                    mSecureTunnel.reset();
                    mLifetimeKeepAlive.reset();
                }

                std::shared_ptr<SecureTunnelWrapper> SecureTunnelingContext::CreateSecureTunnel(
                    const Aws::Iotsecuretunneling::OnConnectionComplete &onConnectionComplete,
                    const Aws::Iotsecuretunneling::OnConnectionShutdown &onConnectionShutdown,
                    const Aws::Iotsecuretunneling::OnSendDataComplete &onSendDataComplete,
                    const Aws::Iotsecuretunneling::OnDataReceive &onDataReceive,
                    const Aws::Iotsecuretunneling::OnStreamStart &onStreamStart,
                    const Aws::Iotsecuretunneling::OnStreamReset &onStreamReset,
                    const Aws::Iotsecuretunneling::OnSessionReset &onSessionReset,
                    const Aws::Iotsecuretunneling::OnStopped &onStopped)
                {
                    if (mProxyOptions.HostName.length() > 0)
                    {
                        LOGM_INFO(TAG, "Creating Secure Tunneling with proxy to: %s", mProxyOptions.HostName.c_str());
                        return std::make_shared<SecureTunnelWrapper>(
                            mSharedCrtResourceManager->getAllocator(),
                            mSharedCrtResourceManager->getClientBootstrap(),
                            Crt::Io::SocketOptions(),
                            mProxyOptions,
                            mAccessToken,
                            AWS_SECURE_TUNNELING_DESTINATION_MODE,
                            mEndpoint,
                            mRootCa,
                            onConnectionComplete,
                            onConnectionShutdown,
                            onSendDataComplete,
                            onDataReceive,
                            onStreamStart,
                            onStreamReset,
                            onSessionReset,
                            onStopped);
                    }
                    else
                    {
                        return std::make_shared<SecureTunnelWrapper>(
                            mSharedCrtResourceManager->getAllocator(),
                            mSharedCrtResourceManager->getClientBootstrap(),
                            Crt::Io::SocketOptions(),
                            mAccessToken,
                            AWS_SECURE_TUNNELING_DESTINATION_MODE,
                            mEndpoint,
                            mRootCa,
                            onConnectionComplete,
                            onConnectionShutdown,
                            onSendDataComplete,
                            onDataReceive,
                            onStreamStart,
                            onStreamReset,
                            onSessionReset,
                            onStopped);
                    }
                }

                bool SecureTunnelingContext::ScheduleLifecycleTask(
                    std::function<void()> task,
                    std::chrono::milliseconds delay)
                {
                    if (!mSharedCrtResourceManager)
                    {
                        return false;
                    }

                    aws_event_loop *eventLoop = mSharedCrtResourceManager->getNextEventLoop();
                    if (!eventLoop)
                    {
                        return false;
                    }

                    unique_ptr<SecureTunnelingLifecycleTask> lifecycleTask(new SecureTunnelingLifecycleTask());
                    lifecycleTask->callback = std::move(task);
                    aws_task_init(
                        &lifecycleTask->task,
                        RunSecureTunnelingLifecycleTask,
                        lifecycleTask.get(),
                        "secure_tunneling_lifecycle");
                    if (delay.count() > 0)
                    {
                        uint64_t runAtNanos;
                        if (aws_event_loop_current_clock_time(eventLoop, &runAtNanos) != AWS_OP_SUCCESS)
                        {
                            return false;
                        }
                        runAtNanos += chrono::duration_cast<chrono::nanoseconds>(delay).count();
                        aws_event_loop_schedule_task_future(eventLoop, &lifecycleTask->task, runAtNanos);
                    }
                    else
                    {
                        aws_event_loop_schedule_task_now(eventLoop, &lifecycleTask->task);
                    }
                    lifecycleTask.release();
                    return true;
                }

                std::shared_ptr<TcpForward> SecureTunnelingContext::CreateTcpForward()
                {
                    weak_ptr<SecureTunnelingContext> weakSelf = shared_from_this();
                    return std::make_shared<TcpForward>(
                        mSharedCrtResourceManager,
                        mPort,
                        [weakSelf](const Crt::ByteBuf &data) {
                            auto self = weakSelf.lock();
                            if (self)
                            {
                                self->OnTcpForwardDataReceive(data);
                            }
                        },
                        [weakSelf](TcpForward *tcpForward, int errorCode) {
                            auto self = weakSelf.lock();
                            if (self)
                            {
                                self->OnTcpForwardTerminated(tcpForward, errorCode);
                            }
                        });
                }
            } // namespace SecureTunneling
        }     // namespace DeviceClient
    }         // namespace Iot
} // namespace Aws
