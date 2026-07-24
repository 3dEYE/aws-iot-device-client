// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/tunneling/SecureTunnelingContext.h"
#include "../../source/tunneling/SecureTunnelingFeature.h"
#include "inttypes.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <aws/iotsecuretunneling/IotSecureTunnelingClient.h>
#include <aws/iotsecuretunneling/SubscribeToTunnelsNotifyRequest.h>
#include <chrono>
#include <future>
#include <thread>

using namespace testing;
using namespace std;
using namespace Aws;
using namespace Aws::Crt;
using namespace Aws::Iot;
using namespace Aws::Iotsecuretunneling;
using namespace Aws::Iot::DeviceClient;
using namespace Aws::Iot::DeviceClient::SecureTunneling;

PlainConfig getConfig()
{
    constexpr char jsonString[] = R"(
{
    "endpoint": "endpoint value",
    "cert": "/tmp/aws-iot-device-client-test-file",
    "root-ca": "/tmp/aws-iot-device-client-test/AmazonRootCA1.pem",
    "key": "/tmp/aws-iot-device-client-test-file",
    "thing-name": "thing-name value",
    "tunneling": {
        "enabled": true
    }
})";
    JsonObject jsonObject(jsonString);
    JsonView jsonView = jsonObject.View();

    PlainConfig config;
    config.LoadFromJson(jsonView);

    return config;
}

class MockNotifier : public Aws::Iot::DeviceClient::ClientBaseNotifier
{
  public:
    MOCK_METHOD(
        void,
        onEvent,
        (Aws::Iot::DeviceClient::Feature * feature, Aws::Iot::DeviceClient::ClientBaseEventNotification notification),
        (override));
    MOCK_METHOD(
        void,
        onError,
        (Aws::Iot::DeviceClient::Feature * feature,
         Aws::Iot::DeviceClient::ClientBaseErrorNotification notification,
         const std::string &message),
        (override));
};

class FakeSecureTunnelContext : public SecureTunnelingContext
{
    /**
     * Fake Context only needed to perform a few basic actions which could be hard coded here.
     * If more elaborate testing is required/desired in the future this can be changed to a mock to
     * give dynamic responses.
     */
  public:
    FakeSecureTunnelContext() : SecureTunnelingContext() {}
    ~FakeSecureTunnelContext() = default;
    bool ConnectToSecureTunnel() override { return true; }
    void StopSecureTunnel() override { return; }
    bool IsDuplicateNotification(const SecureTunnelingNotifyResponse &response) override { return true; }
};

struct BlockingSecureTunnelContextState
{
    shared_ptr<promise<void>> connectEntered;
    shared_future<void> allowConnect;
    shared_ptr<promise<void>> destroyed;
};

class BlockingSecureTunnelContext : public SecureTunnelingContext
{
  public:
    explicit BlockingSecureTunnelContext(const shared_ptr<BlockingSecureTunnelContextState> &state) : state(state) {}
    ~BlockingSecureTunnelContext() override { state->destroyed->set_value(); }

    bool ConnectToSecureTunnel() override
    {
        state->connectEntered->set_value();
        state->allowConnect.wait();
        return true;
    }

    void StopSecureTunnel() override {}
    bool IsDuplicateNotification(const SecureTunnelingNotifyResponse &) override { return false; }

  private:
    shared_ptr<BlockingSecureTunnelContextState> state;
};

class MockSecureTunnelingFeature : public SecureTunnelingFeature
{
  public:
    MockSecureTunnelingFeature() : SecureTunnelingFeature() {}
    MOCK_METHOD(
        std::unique_ptr<SecureTunnelingContext>,
        createContext,
        (const std::string &accessToken, const std::string &region, const uint16_t &port),
        (override));
    MOCK_METHOD(std::shared_ptr<AbstractIotSecureTunnelingClient>, createClient, (), (override));

    void scheduleDelayedSubscriptionRecoveryRetry(
        std::function<void()> retry,
        std::chrono::milliseconds delay) override
    {
        scheduledSubscriptionRecoveryRetry = retry;
        scheduledSubscriptionRecoveryRetryDelay = delay;
    }

    bool hasScheduledSubscriptionRecoveryRetry() const
    {
        return static_cast<bool>(scheduledSubscriptionRecoveryRetry);
    }

    void invokeScheduledSubscriptionRecoveryRetry()
    {
        auto retry = scheduledSubscriptionRecoveryRetry;
        scheduledSubscriptionRecoveryRetry = nullptr;
        retry();
    }

    std::function<void()> scheduledSubscriptionRecoveryRetry;
    std::chrono::milliseconds scheduledSubscriptionRecoveryRetryDelay{0};
};

class MockIotSecureTunnelingClient : public AbstractIotSecureTunnelingClient
{
  public:
    MockIotSecureTunnelingClient() = default;
    MOCK_METHOD(
        bool,
        SubscribeToTunnelsNotify,
        (const Iotsecuretunneling::SubscribeToTunnelsNotifyRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotsecuretunneling::OnSubscribeToTunnelsNotifyResponse &handler,
         const Iotsecuretunneling::OnSubscribeComplete &onSubAck),
        (override));
};

class TestSecureTunnelingFeature : public testing::Test
{
  public:
    void SetUp() override
    {
        manager = shared_ptr<SharedCrtResourceManager>(new SharedCrtResourceManager());
        // Initializing allocator, so we can use CJSON lib from SDK in our unit tests.
        manager->initializeAllocator();

        thingName = Aws::Crt::String("thing-name value");
        secureTunnelingFeature = shared_ptr<MockSecureTunnelingFeature>(new MockSecureTunnelingFeature());
        mockClient = shared_ptr<MockIotSecureTunnelingClient>(new MockIotSecureTunnelingClient());
        notifier = shared_ptr<MockNotifier>(new MockNotifier());
        fakeContext = unique_ptr<FakeSecureTunnelContext>(new FakeSecureTunnelContext());
        response = unique_ptr<SecureTunnelingNotifyResponse>(new SecureTunnelingNotifyResponse());
        config = getConfig();
    }
    Aws::Crt::String thingName;
    shared_ptr<MockIotSecureTunnelingClient> mockClient;
    shared_ptr<MockSecureTunnelingFeature> secureTunnelingFeature;
    shared_ptr<SharedCrtResourceManager> manager;
    shared_ptr<MockNotifier> notifier;
    unique_ptr<FakeSecureTunnelContext> fakeContext;
    unique_ptr<SecureTunnelingNotifyResponse> response;
    PlainConfig config;
};

MATCHER_P(ThingNameEq, ThingName, "Matcher ThingName for all Aws request Objects using Aws::Crt::String")
{
    return arg.ThingName.value() == ThingName;
}

TEST_F(TestSecureTunnelingFeature, GetName)
{
    /**
     * Simple test for getName
     */
    ASSERT_EQ("Secure Tunneling", secureTunnelingFeature->getName());
}

TEST_F(TestSecureTunnelingFeature, Init)
{
    /**
     * Simple init of SecureTunnelingFeature
     */
    ASSERT_EQ(0, secureTunnelingFeature->init(manager, notifier, config));
}

TEST_F(TestSecureTunnelingFeature, CleanSessionRestoresTunnelNotificationSubscription)
{
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(3)
        .WillRepeatedly(DoAll(InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(false);
    secureTunnelingFeature->onConnectionResumed(false);
}

TEST_F(TestSecureTunnelingFeature, PresentSessionDoesNotResubscribeTunnelNotifications)
{
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(true);
}

TEST_F(TestSecureTunnelingFeature, PresentSessionRecoversInitialSubscriptionQueueFailure)
{
    Iotsecuretunneling::OnSubscribeComplete recoverySubAck;

    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(Return(false))
        .WillOnce(DoAll(SaveArg<3>(&recoverySubAck), Return(true)));
    EXPECT_CALL(
        *notifier,
        onError(
            secureTunnelingFeature.get(),
            ClientBaseErrorNotification::SUBSCRIPTION_FAILED,
            AllOf(HasSubstr("Failed to queue"), HasSubstr("tunnel notification"))))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(true);

    ASSERT_TRUE(recoverySubAck);
    recoverySubAck(0);
    secureTunnelingFeature->onConnectionResumed(true);

    ASSERT_TRUE(secureTunnelingFeature->hasScheduledSubscriptionRecoveryRetry());
    secureTunnelingFeature->invokeScheduledSubscriptionRecoveryRetry();
}

TEST_F(TestSecureTunnelingFeature, PresentSessionRecoversInitialSubscriptionSubAckFailure)
{
    Iotsecuretunneling::OnSubscribeComplete initialSubAck;
    Iotsecuretunneling::OnSubscribeComplete recoverySubAck;

    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(DoAll(SaveArg<3>(&initialSubAck), Return(true)))
        .WillOnce(DoAll(SaveArg<3>(&recoverySubAck), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();

    ASSERT_TRUE(initialSubAck);
    initialSubAck(42);
    secureTunnelingFeature->onConnectionResumed(true);

    ASSERT_TRUE(recoverySubAck);
    recoverySubAck(0);
    secureTunnelingFeature->onConnectionResumed(true);
}

TEST_F(TestSecureTunnelingFeature, CleanSessionHandlesImmediateSubscriptionQueueFailure)
{
    Iotsecuretunneling::OnSubscribeComplete retrySubAck;

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(3)
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)))
        .WillOnce(Return(false))
        .WillOnce(DoAll(SaveArg<3>(&retrySubAck), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(false);

    ASSERT_TRUE(secureTunnelingFeature->hasScheduledSubscriptionRecoveryRetry());
    EXPECT_EQ(chrono::milliseconds(5000), secureTunnelingFeature->scheduledSubscriptionRecoveryRetryDelay);
    secureTunnelingFeature->invokeScheduledSubscriptionRecoveryRetry();

    ASSERT_TRUE(retrySubAck);
    retrySubAck(0);
    secureTunnelingFeature->onConnectionResumed(true);
}

TEST_F(TestSecureTunnelingFeature, FailedRecoverySubAckRetriesWithoutReconnect)
{
    Iotsecuretunneling::OnSubscribeComplete failedRecoverySubAck;
    Iotsecuretunneling::OnSubscribeComplete retrySubAck;

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(3)
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)))
        .WillOnce(DoAll(SaveArg<3>(&failedRecoverySubAck), Return(true)))
        .WillOnce(DoAll(SaveArg<3>(&retrySubAck), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(false);

    ASSERT_TRUE(failedRecoverySubAck);
    failedRecoverySubAck(42);

    ASSERT_TRUE(secureTunnelingFeature->hasScheduledSubscriptionRecoveryRetry());
    secureTunnelingFeature->invokeScheduledSubscriptionRecoveryRetry();

    ASSERT_TRUE(retrySubAck);
    retrySubAck(0);
    secureTunnelingFeature->onConnectionResumed(true);
}

TEST_F(TestSecureTunnelingFeature, ScheduledRecoveryRetryAfterStopIsIgnored)
{
    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)))
        .WillOnce(Return(false));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STOPPED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(false);

    ASSERT_TRUE(secureTunnelingFeature->hasScheduledSubscriptionRecoveryRetry());
    secureTunnelingFeature->stop();
    secureTunnelingFeature->invokeScheduledSubscriptionRecoveryRetry();
}

TEST_F(TestSecureTunnelingFeature, ExistingSessionResumeRestartsInterruptedSubscriptionRecovery)
{
    Iotsecuretunneling::OnSubscribeComplete firstRecovery;
    Iotsecuretunneling::OnSubscribeComplete secondRecovery;
    Iotsecuretunneling::OnSubscribeComplete thirdRecovery;

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(4)
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)))
        .WillOnce(DoAll(SaveArg<3>(&firstRecovery), Return(true)))
        .WillOnce(DoAll(SaveArg<3>(&secondRecovery), Return(true)))
        .WillOnce(DoAll(SaveArg<3>(&thirdRecovery), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(false);
    secureTunnelingFeature->onConnectionResumed(true);

    firstRecovery(0);
    secureTunnelingFeature->onConnectionResumed(true);

    secondRecovery(0);
    thirdRecovery(0);
    secureTunnelingFeature->onConnectionResumed(true);
}

TEST_F(TestSecureTunnelingFeature, PreservedSessionAcceptsNotificationFromPendingRecoverySubscription)
{
    string accessToken = "12345";
    string region = "us-west-2";
    uint16_t port = 22;
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    bool contextCreated = false;
    Iotsecuretunneling::OnSubscribeToTunnelsNotifyResponse lostSessionHandler;
    Iotsecuretunneling::OnSubscribeToTunnelsNotifyResponse preservedRecoveryHandler;
    Iotsecuretunneling::OnSubscribeComplete pendingRecoverySubAck;
    Iotsecuretunneling::OnSubscribeComplete replacementRecoverySubAck;

    EXPECT_CALL(*secureTunnelingFeature, createContext(StrEq(accessToken), StrEq(region), Eq(port)))
        .Times(1)
        .WillOnce(DoAll(Assign(&contextCreated, true), Return(ByMove(std::move(fakeContext)))));
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(3)
        .WillOnce(DoAll(SaveArg<2>(&lostSessionHandler), InvokeArgument<3>(0), Return(true)))
        .WillOnce(
            DoAll(SaveArg<2>(&preservedRecoveryHandler), SaveArg<3>(&pendingRecoverySubAck), Return(true)))
        .WillOnce(DoAll(SaveArg<3>(&replacementRecoverySubAck), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(false);
    secureTunnelingFeature->onConnectionResumed(true);

    ASSERT_TRUE(static_cast<bool>(lostSessionHandler));
    ASSERT_TRUE(static_cast<bool>(preservedRecoveryHandler));
    ASSERT_TRUE(static_cast<bool>(pendingRecoverySubAck));
    ASSERT_TRUE(static_cast<bool>(replacementRecoverySubAck));
    lostSessionHandler(response.get(), 0);
    EXPECT_FALSE(contextCreated);
    preservedRecoveryHandler(response.get(), 0);
    EXPECT_TRUE(contextCreated);

    replacementRecoverySubAck(0);
    pendingRecoverySubAck(0);
    secureTunnelingFeature->onConnectionResumed(true);
}

TEST_F(TestSecureTunnelingFeature, CleanSessionDoesNotSubscribeBeforeFeatureStarts)
{
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(_, _, _, _)).Times(0);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->onConnectionResumed(false);
}

TEST_F(TestSecureTunnelingFeature, CleanSessionDoesNotResubscribeAfterFeatureStops)
{
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STOPPED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
    secureTunnelingFeature->onConnectionResumed(false);
}

TEST_F(TestSecureTunnelingFeature, StopWaitsForRecoverySubscriptionAndIgnoresLateNotification)
{
    string accessToken = "12345";
    string region = "us-west-2";
    uint16_t port = 22;
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    Iotsecuretunneling::OnSubscribeToTunnelsNotifyResponse recoveryHandler;
    promise<void> recoverySubscribeEntered;
    future<void> recoverySubscribeEnteredFuture = recoverySubscribeEntered.get_future();
    promise<void> allowRecoverySubscribe;
    shared_future<void> allowRecoverySubscribeFuture = allowRecoverySubscribe.get_future().share();
    promise<void> stopStarted;
    future<void> stopStartedFuture = stopStarted.get_future();
    promise<void> stopCompleted;
    future<void> stopCompletedFuture = stopCompleted.get_future();

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)))
        .WillOnce(DoAll(
            SaveArg<2>(&recoveryHandler),
            InvokeWithoutArgs([&]() {
                recoverySubscribeEntered.set_value();
                allowRecoverySubscribeFuture.wait();
            }),
            Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STOPPED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();

    thread recoveryThread([&]() { secureTunnelingFeature->onConnectionResumed(false); });
    if (future_status::ready != recoverySubscribeEnteredFuture.wait_for(chrono::seconds(3)))
    {
        allowRecoverySubscribe.set_value();
        recoveryThread.join();
        FAIL() << "Recovery subscription did not start";
    }

    thread stopThread([&]() {
        stopStarted.set_value();
        secureTunnelingFeature->stop();
        stopCompleted.set_value();
    });
    if (future_status::ready != stopStartedFuture.wait_for(chrono::seconds(3)))
    {
        allowRecoverySubscribe.set_value();
        recoveryThread.join();
        stopThread.join();
        FAIL() << "Feature stop did not start";
    }
    EXPECT_EQ(future_status::timeout, stopCompletedFuture.wait_for(chrono::seconds(3)));

    allowRecoverySubscribe.set_value();
    recoveryThread.join();
    stopThread.join();

    EXPECT_EQ(future_status::ready, stopCompletedFuture.wait_for(chrono::seconds(0)));
    ASSERT_TRUE(static_cast<bool>(recoveryHandler));
    recoveryHandler(response.get(), 0);
}

TEST_F(TestSecureTunnelingFeature, TunnelNotificationAfterStopIsIgnored)
{
    string accessToken = "12345";
    string region = "us-west-2";
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    Iotsecuretunneling::OnSubscribeToTunnelsNotifyResponse notificationHandler;
    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(SaveArg<2>(&notificationHandler), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STOPPED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();

    ASSERT_TRUE(static_cast<bool>(notificationHandler));
    notificationHandler(response.get(), 0);
}

TEST_F(TestSecureTunnelingFeature, SessionRecoveryPreservesTunnelConnectionAlreadyInProgress)
{
    string accessToken = "12345";
    string region = "us-west-2";
    uint16_t port = 22;
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    auto connectEntered = make_shared<promise<void>>();
    auto connectEnteredFuture = connectEntered->get_future();
    auto allowConnect = make_shared<promise<void>>();
    auto allowConnectFuture = allowConnect->get_future().share();
    auto contextDestroyed = make_shared<promise<void>>();
    auto contextDestroyedFuture = contextDestroyed->get_future();
    auto contextState = make_shared<BlockingSecureTunnelContextState>(
        BlockingSecureTunnelContextState{connectEntered, allowConnectFuture, contextDestroyed});
    unique_ptr<SecureTunnelingContext> blockingContext(new BlockingSecureTunnelContext(contextState));

    Iotsecuretunneling::OnSubscribeToTunnelsNotifyResponse notificationHandler;
    promise<void> recoveryCompleted;
    future<void> recoveryCompletedFuture = recoveryCompleted.get_future();

    EXPECT_CALL(*secureTunnelingFeature, createContext(StrEq(accessToken), StrEq(region), Eq(port)))
        .Times(1)
        .WillOnce(Return(ByMove(std::move(blockingContext))));
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(DoAll(SaveArg<2>(&notificationHandler), InvokeArgument<3>(0), Return(true)))
        .WillOnce(DoAll(InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STOPPED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    ASSERT_TRUE(static_cast<bool>(notificationHandler));

    thread notificationThread([&]() { notificationHandler(response.get(), 0); });
    if (future_status::ready != connectEnteredFuture.wait_for(chrono::seconds(3)))
    {
        allowConnect->set_value();
        notificationThread.join();
        secureTunnelingFeature->stop();
        FAIL() << "Tunnel connection did not start";
    }

    thread recoveryThread([&]() {
        secureTunnelingFeature->onConnectionResumed(false);
        recoveryCompleted.set_value();
    });
    if (future_status::ready != recoveryCompletedFuture.wait_for(chrono::seconds(3)))
    {
        allowConnect->set_value();
        notificationThread.join();
        recoveryThread.join();
        secureTunnelingFeature->stop();
        FAIL() << "Subscription recovery waited for the tunnel connection";
    }
    recoveryThread.join();

    allowConnect->set_value();
    notificationThread.join();

    EXPECT_EQ(future_status::timeout, contextDestroyedFuture.wait_for(chrono::seconds(0)));
    secureTunnelingFeature->stop();
    EXPECT_EQ(future_status::timeout, contextDestroyedFuture.wait_for(chrono::seconds(0)));
    secureTunnelingFeature.reset();
    EXPECT_EQ(future_status::ready, contextDestroyedFuture.wait_for(chrono::seconds(0)));
}

TEST_F(TestSecureTunnelingFeature, StopDoesNotWaitForTunnelConnectionAndDiscardsStaleContext)
{
    string accessToken = "12345";
    string region = "us-west-2";
    uint16_t port = 22;
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    auto connectEntered = make_shared<promise<void>>();
    auto connectEnteredFuture = connectEntered->get_future();
    auto allowConnect = make_shared<promise<void>>();
    auto allowConnectFuture = allowConnect->get_future().share();
    auto contextDestroyed = make_shared<promise<void>>();
    auto contextDestroyedFuture = contextDestroyed->get_future();
    auto contextState = make_shared<BlockingSecureTunnelContextState>(
        BlockingSecureTunnelContextState{connectEntered, allowConnectFuture, contextDestroyed});
    unique_ptr<SecureTunnelingContext> blockingContext(new BlockingSecureTunnelContext(contextState));

    Iotsecuretunneling::OnSubscribeToTunnelsNotifyResponse notificationHandler;
    promise<void> stopCompleted;
    future<void> stopCompletedFuture = stopCompleted.get_future();

    EXPECT_CALL(*secureTunnelingFeature, createContext(StrEq(accessToken), StrEq(region), Eq(port)))
        .Times(1)
        .WillOnce(Return(ByMove(std::move(blockingContext))));
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(SaveArg<2>(&notificationHandler), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STOPPED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    ASSERT_TRUE(static_cast<bool>(notificationHandler));

    weak_ptr<MockSecureTunnelingFeature> weakFeature = secureTunnelingFeature;
    thread notificationThread([&]() { notificationHandler(response.get(), 0); });
    if (future_status::ready != connectEnteredFuture.wait_for(chrono::seconds(3)))
    {
        allowConnect->set_value();
        notificationThread.join();
        secureTunnelingFeature->stop();
        FAIL() << "Tunnel connection did not start";
    }

    auto feature = secureTunnelingFeature.get();
    thread stopThread([&]() {
        feature->stop();
        stopCompleted.set_value();
    });
    if (future_status::ready != stopCompletedFuture.wait_for(chrono::seconds(3)))
    {
        allowConnect->set_value();
        notificationThread.join();
        stopThread.join();
        FAIL() << "Feature stop waited for the tunnel connection";
    }
    stopThread.join();

    secureTunnelingFeature.reset();
    EXPECT_FALSE(weakFeature.expired());

    allowConnect->set_value();
    notificationThread.join();

    EXPECT_EQ(future_status::ready, contextDestroyedFuture.wait_for(chrono::seconds(0)));
    EXPECT_TRUE(weakFeature.expired());
}

TEST_F(TestSecureTunnelingFeature, CleanSessionDoesNotSubscribeWhenTunnelNotificationsAreDisabled)
{
    config.tunneling.subscribeNotification = false;
    config.tunneling.destinationAccessToken = "access-token";
    config.tunneling.region = "us-west-2";
    config.tunneling.port = 22;

    EXPECT_CALL(*secureTunnelingFeature, createContext(StrEq("access-token"), StrEq("us-west-2"), Eq(22)))
        .Times(1)
        .WillOnce(Return(ByMove(std::move(fakeContext))));
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(_, _, _, _)).Times(0);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STARTED))
        .Times(1);
    EXPECT_CALL(*notifier, onEvent(secureTunnelingFeature.get(), ClientBaseEventNotification::FEATURE_STOPPED))
        .Times(1);

    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->onConnectionResumed(false);
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, CreateSSHContextHappy)
{
    /**
     * Invokes NotifyResponse handler for SSH service, verifies SecureTunnelContext params
     */
    string accessToken = "12345";
    string region = "us-west-2";
    uint16_t port = 22;
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(StrEq(accessToken), StrEq(region), Eq(port)))
        .Times(1)
        .WillOnce(Return(ByMove(std::move(fakeContext))));
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(response.get(), 0), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, CreateVNCContextHappy)
{
    /**
     * Invokes NotifyResponse handler for VNC service, verifies SecureTunnelContext params
     */
    string accessToken = "12345";
    string region = "us-west-2";
    uint16_t port = 5900;
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("VNC");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(StrEq(accessToken), StrEq(region), Eq(port)))
        .Times(1)
        .WillOnce(Return(ByMove(std::move(fakeContext))));
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(response.get(), 0), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, ResponseNULL)
{
    /**
     * Invokes NotifyResponse handler with null response
     * Expect no creation of SecureTunnelContext
     */
    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(nullptr, 1), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, ResponseIoError)
{
    /**
     * Invokes NotifyResponse handler with error code 1
     * Expect no creation of SecureTunnelContext
     */

    string accessToken = "12345";
    string region = "us-west-2";
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(response.get(), 1), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, DuplicateResponse)
{
    /**
     * Invokes NotifyResponse with duplicate responses
     * Expect single SecureTunnelingContext
     */
    string accessToken = "12345";
    string region = "us-west-2";
    uint16_t port = 22;
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(StrEq(accessToken), StrEq(region), Eq(port)))
        .Times(1)
        .WillOnce(Return(ByMove(std::move(fakeContext))));
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(
            DoAll(
                InvokeArgument<2>(response.get(), 0),
                InvokeArgument<2>(response.get(), 0),
                InvokeArgument<3>(0),
                Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, MultipleServices)
{
    /**
     * Invokes NotifyResponse with multiple services
     * Expect no SecureTunnelContext (multi-port tunneling unsupported on device client)
     */

    string accessToken = "12345";
    string region = "us-west-2";
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");
    services.push_back("VNC");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(response.get(), 1), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, UnsupportedService)
{
    /**
     * Invokes NotifyResponse with zero services
     * Expect no SecureTunnelContext
     */

    string accessToken = "12345";
    string region = "us-west-2";
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("UnsupportedService");

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(response.get(), 1), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, NoServices)
{
    /**
     * Invokes NotifyResponse with zero services
     * Expect no SecureTunnelContext
     */

    string accessToken = "12345";
    string region = "us-west-2";
    Aws::Crt::Vector<Aws::Crt::String> services;

    response->ClientMode = "destination";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(response.get(), 1), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}

TEST_F(TestSecureTunnelingFeature, SourceMode)
{
    /**
     * Invokes NotifyResponse with in source mode
     * Expect no SecureTunnelContext source ClientMode not supported on Device Client
     */

    string accessToken = "12345";
    string region = "us-west-2";
    Aws::Crt::Vector<Aws::Crt::String> services;
    services.push_back("SSH");

    response->ClientMode = "source";
    response->Services = services;
    response->ClientAccessToken = accessToken.c_str();
    response->Region = region.c_str();

    EXPECT_CALL(*secureTunnelingFeature, createContext(_, _, _)).Times(0);
    EXPECT_CALL(*secureTunnelingFeature, createClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(*mockClient, SubscribeToTunnelsNotify(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<2>(response.get(), 1), InvokeArgument<3>(0), Return(true)));
    EXPECT_CALL(*notifier, onEvent(_, _)).Times(2);
    secureTunnelingFeature->init(manager, notifier, config);
    secureTunnelingFeature->start();
    secureTunnelingFeature->stop();
}
