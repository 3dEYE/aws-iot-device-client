// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SecureTunnelingFeature.h"
#include "../logging/LoggerFactory.h"
#include "SecureTunnelingContext.h"
#include "TcpForward.h"
#include <aws/crt/mqtt/MqttClient.h>
#include <aws/iotsecuretunneling/SubscribeToTunnelsNotifyRequest.h>
#include <csignal>
#include <map>
#include <memory>
#include <thread>

using namespace std;
using namespace Aws::Iotsecuretunneling;
using namespace Aws::Iot::DeviceClient::Logging;
using namespace Aws::Iot::DeviceClient::Util;

namespace Aws
{
    namespace Iot
    {
        namespace DeviceClient
        {
            namespace SecureTunneling
            {
                constexpr char SecureTunnelingFeature::TAG[];
                constexpr char SecureTunnelingFeature::NAME[];
                constexpr char SecureTunnelingFeature::DEFAULT_PROXY_ENDPOINT_HOST_FORMAT[];
                std::map<std::string, uint16_t> SecureTunnelingFeature::mServiceToPortMap;

                SecureTunnelingFeature::SecureTunnelingFeature() = default;

                SecureTunnelingFeature::~SecureTunnelingFeature() = default;

                int SecureTunnelingFeature::init(
                    shared_ptr<SharedCrtResourceManager> sharedCrtResourceManager,
                    shared_ptr<ClientBaseNotifier> notifier,
                    const PlainConfig &config)
                {
                    sharedCrtResourceManager->initializeAWSHttpLib();

                    this->mSharedCrtResourceManager = sharedCrtResourceManager;
                    mClientBaseNotifier = notifier;

                    LoadFromConfig(config);

                    return 0;
                }

                string SecureTunnelingFeature::getName() { return NAME; }

                int SecureTunnelingFeature::start()
                {
                    {
                        lock_guard<mutex> lock(mConnectionRecoveryLock);
                        mStarted = true;
                        mSubscriptionNeedsRecovery = false;
                        ++mConnectionRecoveryGeneration;
                        mOldestValidSubscriptionGeneration = mConnectionRecoveryGeneration;
                    }
                    RunSecureTunneling();
                    auto self = static_cast<Feature *>(this);
                    mClientBaseNotifier->onEvent(self, ClientBaseEventNotification::FEATURE_STARTED);
                    return 0;
                }

                int SecureTunnelingFeature::stop()
                {
                    LOG_DEBUG(TAG, "SecureTunnelingFeature::stop");
                    {
                        lock_guard<mutex> subscriptionLifecycleLock(mSubscriptionLifecycleLock);
                        lock_guard<mutex> lock(mConnectionRecoveryLock);
                        mStarted = false;
                        mSubscriptionNeedsRecovery = false;
                        ++mConnectionRecoveryGeneration;
                        mOldestValidSubscriptionGeneration = mConnectionRecoveryGeneration;
                        ++mFeatureLifecycleGeneration;
                    }
                    for (auto &c : mContexts)
                    {
                        c->StopSecureTunnel();
                    }

                    auto self = static_cast<Feature *>(this);
                    mClientBaseNotifier->onEvent(self, ClientBaseEventNotification::FEATURE_STOPPED);
                    return 0;
                }

                void SecureTunnelingFeature::onConnectionResumed(bool sessionPresent)
                {
                    lock_guard<mutex> subscriptionLifecycleLock(mSubscriptionLifecycleLock);
                    std::uint64_t recoveryGeneration;
                    {
                        lock_guard<mutex> lock(mConnectionRecoveryLock);
                        if (!mStarted)
                        {
                            LOG_DEBUG(
                                TAG,
                                "Skipping tunnel notification subscription recovery because the feature is not "
                                "running");
                            return;
                        }

                        if (!mSubscribeNotification)
                        {
                            LOG_DEBUG(
                                TAG,
                                "Skipping tunnel notification subscription recovery because notifications are "
                                "disabled");
                            return;
                        }

                        if (!iotSecureTunnelingClient)
                        {
                            LOG_ERROR(
                                TAG,
                                "Cannot restore tunnel notification subscription because the client is not ready");
                            return;
                        }

                        if (!sessionPresent)
                        {
                            mSubscriptionNeedsRecovery = true;
                        }

                        if (!mSubscriptionNeedsRecovery)
                        {
                            LOG_DEBUG(
                                TAG,
                                "MQTT session resumed with the tunnel notification subscription intact");
                            return;
                        }

                        recoveryGeneration = ++mConnectionRecoveryGeneration;
                        if (!sessionPresent)
                        {
                            mOldestValidSubscriptionGeneration = recoveryGeneration;
                        }
                    }

                    LOG_INFO(
                        TAG,
                        "Restoring tunnel notification subscription after a lost or interrupted MQTT session recovery");
                    SubscribeToTunnelNotifications(true, recoveryGeneration);
                }

                uint16_t SecureTunnelingFeature::GetPortFromService(const std::string &service)
                {
                    if (mServiceToPortMap.empty())
                    {
                        mServiceToPortMap["SSH"] = 22;
                        mServiceToPortMap["VNC"] = 5900;
                    }

                    auto result = mServiceToPortMap.find(service);
                    if (result == mServiceToPortMap.end())
                    {
                        LOGM_ERROR(TAG, "Requested unsupported service. service=%s", service.c_str());
                        return 0; // TODO: Consider throw
                    }

                    return result->second;
                }

                bool SecureTunnelingFeature::IsValidPort(int port) { return 1 <= port && port <= 65535; }

                void SecureTunnelingFeature::LoadFromConfig(const PlainConfig &config)
                {
                    PlainConfig::HttpProxyConfig proxyConfig = config.httpProxyConfig;

                    if (proxyConfig.httpProxyEnabled)
                    {
                        proxyOptions.HostName = proxyConfig.proxyHost->c_str();
                        proxyOptions.Port = proxyConfig.proxyPort.value();
                        proxyOptions.ProxyConnectionType = Aws::Crt::Http::AwsHttpProxyConnectionType::Tunneling;

                        LOGM_INFO(
                            TAG,
                            "Attempting to establish tunneling connection with proxy: %s:%u",
                            proxyOptions.HostName.c_str(),
                            proxyOptions.Port);

                        if (proxyConfig.httpProxyAuthEnabled)
                        {
                            LOG_INFO(TAG, "Proxy Authentication is enabled");
                            Aws::Crt::Http::HttpProxyStrategyBasicAuthConfig basicAuthConfig;
                            basicAuthConfig.ConnectionType = Aws::Crt::Http::AwsHttpProxyConnectionType::Tunneling;
                            proxyOptions.AuthType = Aws::Crt::Http::AwsHttpProxyAuthenticationType::Basic;
                            basicAuthConfig.Username = proxyConfig.proxyUsername->c_str();
                            basicAuthConfig.Password = proxyConfig.proxyPassword->c_str();
                            proxyOptions.ProxyStrategy =
                                Aws::Crt::Http::HttpProxyStrategy::CreateBasicHttpProxyStrategy(
                                    basicAuthConfig, Aws::Crt::g_allocator);
                        }
                        else
                        {
                            LOG_INFO(TAG, "Proxy Authentication is disabled");
                            proxyOptions.AuthType = Aws::Crt::Http::AwsHttpProxyAuthenticationType::None;
                        }
                    }
                    mThingName = *config.thingName;
                    mRootCa = config.rootCa;
                    mSubscribeNotification = config.tunneling.subscribeNotification;
                    mEndpoint = config.tunneling.endpoint;

                    if (!config.tunneling.subscribeNotification)
                    {
                        auto context = createContext(
                            *config.tunneling.destinationAccessToken,
                            *config.tunneling.region,
                            static_cast<uint16_t>(config.tunneling.port.value()));
                        mContexts.push_back(std::move(context));
                    }
                }

                void SecureTunnelingFeature::RunSecureTunneling()
                {
                    LOGM_INFO(TAG, "Running %s!", getName().c_str());

                    if (mSubscribeNotification)
                    {
                        auto client = createClient();
                        std::uint64_t subscriptionGeneration;
                        {
                            lock_guard<mutex> lock(mConnectionRecoveryLock);
                            iotSecureTunnelingClient = move(client);
                            subscriptionGeneration = mConnectionRecoveryGeneration;
                        }
                        SubscribeToTunnelNotifications(false, subscriptionGeneration);
                    }
                    else
                    {
                        // Access token and region were loaded from config and have already been validated
                        for (auto &c : mContexts)
                        {
                            c->ConnectToSecureTunnel();
                        }
                    }
                }

                bool SecureTunnelingFeature::SubscribeToTunnelNotifications(
                    bool isRecovery,
                    std::uint64_t recoveryGeneration)
                {
                    SubscribeToTunnelsNotifyRequest request;
                    request.ThingName = mThingName.c_str();
                    weak_ptr<SecureTunnelingFeature> weakSelf = shared_from_this();

                    bool queued = iotSecureTunnelingClient->SubscribeToTunnelsNotify(
                        request,
                        AWS_MQTT_QOS_AT_LEAST_ONCE,
                        [weakSelf, recoveryGeneration](SecureTunnelingNotifyResponse *response, int ioErr) {
                            auto self = weakSelf.lock();
                            if (self)
                            {
                                self->OnSubscribeToTunnelsNotifyResponse(response, ioErr, recoveryGeneration);
                            }
                        },
                        [weakSelf, isRecovery, recoveryGeneration](int ioErr) {
                            auto self = weakSelf.lock();
                            if (self)
                            {
                                self->OnSubscribeComplete(ioErr, isRecovery, recoveryGeneration);
                            }
                        });

                    if (!queued)
                    {
                        if (isRecovery)
                        {
                            LOG_ERROR(TAG, "Couldn't queue tunnel notification subscription recovery");
                        }
                        else
                        {
                            LOG_ERROR(TAG, "Couldn't queue tunnel notification subscription");
                        }

                        lock_guard<mutex> lock(mConnectionRecoveryLock);
                        if (mStarted && recoveryGeneration == mConnectionRecoveryGeneration)
                        {
                            mSubscriptionNeedsRecovery = true;
                        }
                    }

                    return queued;
                }

                void SecureTunnelingFeature::OnSubscribeToTunnelsNotifyResponse(
                    SecureTunnelingNotifyResponse *response,
                    int ioErr,
                    std::uint64_t subscriptionGeneration)
                {
                    lock_guard<mutex> notificationLock(mTunnelNotificationLock);
                    std::uint64_t featureLifecycleGeneration;
                    {
                        lock_guard<mutex> lock(mConnectionRecoveryLock);
                        if (!mStarted || subscriptionGeneration < mOldestValidSubscriptionGeneration ||
                            subscriptionGeneration > mConnectionRecoveryGeneration)
                        {
                            LOG_DEBUG(TAG, "Ignoring tunnel notification from a stale MQTT session");
                            return;
                        }
                        featureLifecycleGeneration = mFeatureLifecycleGeneration;
                    }

                    LOG_DEBUG(TAG, "Received MQTT Tunnel Notification");

                    if (ioErr || !response)
                    {
                        LOGM_ERROR(TAG, "OnSubscribeToTunnelsNotifyResponse received error. ioErr=%d", ioErr);
                        return;
                    }

                    string clientMode = response->ClientMode->c_str();
                    if (clientMode != "destination")
                    {
                        LOGM_ERROR(TAG, "Unexpected client mode: %s", clientMode.c_str());
                        return;
                    }

                    if (!response->Services.has_value() || response->Services->empty())
                    {
                        LOG_ERROR(TAG, "no service requested");
                        return;
                    }
                    if (response->Services->size() > 1)
                    {
                        LOG_ERROR(
                            TAG,
                            "Received a multi-port tunnel request, but multi-port tunneling is not currently supported "
                            "by Device Client.");
                        return;
                    }

                    if (!response->ClientAccessToken.has_value() || response->ClientAccessToken->empty())
                    {
                        LOG_ERROR(TAG, "access token cannot be empty");
                        return;
                    }
                    string accessToken = response->ClientAccessToken->c_str();

                    if (!response->Region.has_value() || response->Region->empty())
                    {
                        LOG_ERROR(TAG, "region cannot be empty");
                        return;
                    }
                    string region = response->Region->c_str();

                    string service = response->Services->at(0).c_str();
                    uint16_t port = GetPortFromService(service);
                    if (!IsValidPort(port))
                    {
                        LOGM_ERROR(TAG, "Requested service is not supported: %s", service.c_str());
                        return;
                    }

                    {
                        lock_guard<mutex> lock(mConnectionRecoveryLock);
                        if (!mStarted || featureLifecycleGeneration != mFeatureLifecycleGeneration)
                        {
                            LOG_DEBUG(TAG, "Ignoring tunnel notification invalidated by a feature lifecycle change");
                            return;
                        }

                        for (auto &c : mContexts)
                        {
                            if (c->IsDuplicateNotification(*response))
                            {
                                LOG_INFO(TAG, "Received duplicate MQTT Tunnel Notification. Ignoring...");
                                return;
                            }
                        }
                    }

                    LOGM_DEBUG(TAG, "Region=%s, Service=%s", region.c_str(), service.c_str());

                    auto context = createContext(accessToken, region, port);

                    if (!context->ConnectToSecureTunnel())
                    {
                        return;
                    }

                    lock_guard<mutex> lock(mConnectionRecoveryLock);
                    if (!mStarted || featureLifecycleGeneration != mFeatureLifecycleGeneration)
                    {
                        LOG_DEBUG(TAG, "Discarding tunnel opened after a feature lifecycle change");
                        return;
                    }

                    mContexts.push_back(std::move(context));
                }

                void SecureTunnelingFeature::OnSubscribeComplete(
                    int ioErr,
                    bool isRecovery,
                    std::uint64_t recoveryGeneration)
                {
                    if (!isRecovery)
                    {
                        if (ioErr)
                        {
                            LOGM_ERROR(TAG, "Couldn't subscribe to tunnel notification topic. ioErr=%d", ioErr);
                            lock_guard<mutex> lock(mConnectionRecoveryLock);
                            if (mStarted && recoveryGeneration == mConnectionRecoveryGeneration)
                            {
                                mSubscriptionNeedsRecovery = true;
                            }
                        }
                        else
                        {
                            LOG_DEBUG(TAG, "Subscribed to tunnel notification topic");
                        }
                        return;
                    }

                    lock_guard<mutex> lock(mConnectionRecoveryLock);
                    if (!mStarted || recoveryGeneration != mConnectionRecoveryGeneration ||
                        !mSubscriptionNeedsRecovery)
                    {
                        LOG_DEBUG(TAG, "Ignoring stale tunnel notification subscription recovery callback");
                        return;
                    }

                    if (ioErr)
                    {
                        LOGM_ERROR(TAG, "Couldn't restore tunnel notification subscription. ioErr=%d", ioErr);
                        return;
                    }

                    mSubscriptionNeedsRecovery = false;
                    LOG_INFO(TAG, "Restored tunnel notification subscription");
                }

                string SecureTunnelingFeature::GetEndpoint(const string &region)
                {
                    if (mEndpoint.has_value())
                    {
                        return mEndpoint.value();
                    }

                    string endpoint = FormatMessage(DEFAULT_PROXY_ENDPOINT_HOST_FORMAT, region.c_str());

                    if (region.substr(0, 3) == "cn-")
                    {
                        // Chinese regions have ".cn" at the end:
                        // data.tunneling.iot.<region>.amazonaws.com.cn
                        // Examples of Chinese region name: "cn-north-1", "cn-northwest-1"
                        endpoint = endpoint + ".cn";
                    }

                    return endpoint;
                }

                std::unique_ptr<SecureTunnelingContext> SecureTunnelingFeature::createContext(
                    const std::string &accessToken,
                    const std::string &region,
                    const uint16_t &port)
                {
                    return std::unique_ptr<SecureTunnelingContext>(new SecureTunnelingContext(
                        mSharedCrtResourceManager,
                        proxyOptions,
                        mRootCa,
                        accessToken,
                        GetEndpoint(region),
                        port,
                        bind(&SecureTunnelingFeature::OnConnectionShutdown, this, placeholders::_1)));
                }

                std::shared_ptr<AbstractIotSecureTunnelingClient> SecureTunnelingFeature::createClient()
                {
                    return std::make_shared<IotSecureTunnelingClientWrapper>(
                        mSharedCrtResourceManager->getConnection());
                }

                void SecureTunnelingFeature::OnConnectionShutdown(SecureTunnelingContext *contextToRemove)
                {
                    LOG_DEBUG(TAG, "SecureTunnelingFeature::OnConnectionShutdown");
                    auto it =
                        find_if(mContexts.begin(), mContexts.end(), [&](const unique_ptr<SecureTunnelingContext> &c) {
                            return c.get() == contextToRemove;
                        });
                    mContexts.erase(std::remove(mContexts.begin(), mContexts.end(), *it));

#if defined(DISABLE_MQTT)
                    LOG_INFO(TAG, "Secure Tunnel closed, component cleaning up open thread");
                    raise(SIGTERM);
#endif
                }

            } // namespace SecureTunneling
        }     // namespace DeviceClient
    }         // namespace Iot
} // namespace Aws
