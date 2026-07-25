// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/tunneling/SecureTunnelingContext.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <aws/common/allocator.h>
#include <atomic>
#include <thread>
#include <vector>

using namespace testing;
using namespace std;
using namespace Aws;
using namespace Aws::Crt;
using namespace Aws::Crt::Io;
using namespace Aws::Iot;
using namespace Aws::Iotsecuretunneling;
using namespace Aws::Iot::DeviceClient;
using namespace Aws::Iot::DeviceClient::SecureTunneling;

class MockSecureTunnelingContext : public SecureTunnelingContext
{
  public:
    MockSecureTunnelingContext(
        shared_ptr<SharedCrtResourceManager> manager,
        const Aws::Crt::Optional<std::string> &rootCa,
        const string &accessToken,
        const string &endpoint,
        const int port,
        const OnStoppedFn &onStopped)
        : SecureTunnelingContext(manager, rootCa, accessToken, endpoint, port, onStopped)
    {
    }

    MOCK_METHOD(
        std::shared_ptr<SecureTunnelWrapper>,
        CreateSecureTunnel,
        (const Aws::Iotsecuretunneling::OnConnectionComplete &onConnectionComplete,
         const Aws::Iotsecuretunneling::OnConnectionShutdown &onConnectionShutdown,
         const Aws::Iotsecuretunneling::OnSendDataComplete &onSendDataComplete,
         const Aws::Iotsecuretunneling::OnDataReceive &onDataReceive,
         const Aws::Iotsecuretunneling::OnStreamStart &onStreamStart,
         const Aws::Iotsecuretunneling::OnStreamReset &onStreamReset,
         const Aws::Iotsecuretunneling::OnSessionReset &onSessionReset,
         const Aws::Iotsecuretunneling::OnStopped &onStopped),
        (override));

    MOCK_METHOD(std::shared_ptr<TcpForward>, CreateTcpForward, (), (override));
    MOCK_METHOD(void, DisconnectFromTcpForward, (), (override));

    bool ScheduleLifecycleTask(std::function<void()> task, std::chrono::milliseconds delay) override
    {
        (void)delay;
        if (deferLifecycleTasks)
        {
            scheduledLifecycleTasks.push_back(std::move(task));
        }
        else
        {
            task();
        }
        return true;
    }

    void RunNextLifecycleTask()
    {
        ASSERT_FALSE(scheduledLifecycleTasks.empty());
        auto task = std::move(scheduledLifecycleTasks.front());
        scheduledLifecycleTasks.erase(scheduledLifecycleTasks.begin());
        task();
    }

    bool deferLifecycleTasks{false};
    vector<std::function<void()>> scheduledLifecycleTasks;
};

class MockSecureTunnel : public SecureTunnelWrapper
{
  public:
    MockSecureTunnel() : SecureTunnelWrapper() {}
    MOCK_METHOD(int, Connect, (), (override));
    MOCK_METHOD(int, Close, (), (override));
    MOCK_METHOD(int, SendData, (const Aws::Crt::ByteCursor &data), (override));
    bool IsValid() override { return true; }
};

class MockTcpForward : public TcpForward
{
  public:
    MockTcpForward(std::shared_ptr<SharedCrtResourceManager> sharedCrtResourceManager, uint16_t port)
        : TcpForward(sharedCrtResourceManager, port)
    {
    }
    MOCK_METHOD(int, Connect, (), (override));
    MOCK_METHOD(void, Stop, (), (override));
    MOCK_METHOD(int, SendData, (const Crt::ByteCursor &data), (override));
};

class TestSecureTunnelContext : public testing::Test
{
  public:
    void SetUp() override
    {
        manager = shared_ptr<SharedCrtResourceManager>(new SharedCrtResourceManager());
        tunnel = shared_ptr<MockSecureTunnel>(new MockSecureTunnel());
        tcpForward = shared_ptr<MockTcpForward>(new MockTcpForward(manager, port));
        rootCa = "root-ca-value";
        accessToken = "access-token-value";
        endpoint = "endpoint-value";
        port = 5555;
    }

    void TearDown() override
    {
        if (onStopped)
        {
            auto callback = std::move(onStopped);
            callback(nullptr);
        }
        context.reset();
    }

    shared_ptr<MockSecureTunnelingContext> context;
    shared_ptr<MockSecureTunnel> tunnel;
    shared_ptr<MockTcpForward> tcpForward;
    shared_ptr<SharedCrtResourceManager> manager;
    Aws::Crt::Optional<std::string> rootCa;
    string accessToken;
    string endpoint;
    int port;
    OnStoppedFn onStoppedNotification;
    Aws::Iotsecuretunneling::OnStopped onStopped;
};

TEST_F(TestSecureTunnelContext, ConnectToSecureTunnelHappy)
{
    /**
     * Create a MockSecureTunnelingContext and inject a MockSecureTunnel
     * Verify ConnectToSecureTunnel returns true
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), Return(tunnel)));
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, ConnectToSecureTunnelMissingAccessToken)
{
    /**
     * Create a MockSecureTunnelingContext with an empty Access Token
     * Verify ConnectToSecureTunnel returns false
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, "", "12345", port, nullptr);

    ASSERT_FALSE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, ConnectToSecureTunnelMissingEndpoint)
{
    /**
     * Create a MockSecureTunnelingContext with an empty endpoint
     * Verify ConnectToSecureTunnel returns false
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, "12345", "", port, nullptr);

    ASSERT_FALSE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, OnStreamStartHappy)
{
    /**
     * Create a MockSecureTunnelingContext and inject a mock SecureTunnel and mock TcpForward
     * Invoke OnStreamStart callback
     * Verify calls on TcpForward, SecureTunnel, and that ConnectToSecureTunnel returns true
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), InvokeArgument<4>(), Return(tunnel)));
    EXPECT_CALL(*context, CreateTcpForward()).WillOnce(Return(tcpForward));
    EXPECT_CALL(*tcpForward, Connect()).WillOnce(Return(0));
    EXPECT_CALL(*tcpForward, Stop()).Times(1);
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, TcpForwardConnectFailureStopsForward)
{
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), InvokeArgument<4>(), Return(tunnel)));
    EXPECT_CALL(*context, CreateTcpForward()).WillOnce(Return(tcpForward));
    EXPECT_CALL(*tcpForward, Connect()).WillOnce(Return(AWS_OP_ERR));
    EXPECT_CALL(*tcpForward, Stop()).Times(1);
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(AWS_OP_SUCCESS));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, OnStreamStartInvalidPortLow)
{
    /**
     * Create a MockSecureTunnelContext with invalid (too low) port number and inject a mock SecureTunnel
     * Verify no create TcpForward
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, 0, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), InvokeArgument<4>(), Return(tunnel)));
    EXPECT_CALL(*context, CreateTcpForward()).Times(0);
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, OnStreamStartInvalidPortHigh)
{
    /**
     * Create a MockSecureTunnelContext with invalid (too high) port number and inject a mock SecureTunnel
     * Verify no create TcpForward
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, 65536, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), InvokeArgument<4>(), Return(tunnel)));
    EXPECT_CALL(*context, CreateTcpForward()).Times(0);
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, OnStreamReset)
{
    /**
     * Create a MockSecureTunnelingContext and inject a MockSecureTunnel and mock TcpForward
     * Invoke OnStreamReset callback
     * Verify calls on tunnel and DisconnectTcpForward, ConnectToSecureTunnel returns true
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), InvokeArgument<5>(), Return(tunnel)));
    EXPECT_CALL(*context, DisconnectFromTcpForward()).Times(1);
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, OnSessionReset)
{
    /**
     * Create a MockSecureTunnelingContext and inject a MockSecureTunnel and mock TcpForward
     * Invoke OnSessionReset callback
     * Verify calls on tunnel and DisconnectTcpForward, ConnectToSecureTunnel returns true
     */
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), InvokeArgument<6>(), Return(tunnel)));
    EXPECT_CALL(*context, DisconnectFromTcpForward()).Times(1);
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, OnDataReceive)
{
    /**
     * Create a MockSecureTunnelingContext and inject a MockSecureTunnel and mock TcpForward
     * Invoke OnDataReceive callback with test data
     * Verify calls on tunnel and TcpForward, ConnectToSecureTunnel returns true
     */
    Crt::ByteBuf data = ByteBufFromCString("Test Data");

    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(
            DoAll(SaveArg<7>(&onStopped), InvokeArgument<4>(), InvokeArgument<3>(data), Return(tunnel)));
    EXPECT_CALL(*context, CreateTcpForward()).WillOnce(Return(tcpForward));
    EXPECT_CALL(*tcpForward, Connect()).WillOnce(Return(0));
    EXPECT_CALL(*tcpForward, Stop()).Times(1);
    EXPECT_CALL(*tcpForward, SendData(_)).Times(1);
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
}

TEST_F(TestSecureTunnelContext, StopRetainsContextUntilOnStopped)
{
    /**
     * Requests asynchronous tunnel shutdown, releases the caller's context reference,
     * and verifies the context remains alive until the SDK reports OnStopped.
     */
    std::promise<void> promise;
    auto stoppedFuture = promise.get_future();
    onStoppedNotification = [&](SecureTunnelingContext *) -> void { promise.set_value(); };
    context =
        make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, onStoppedNotification);

    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), Return(tunnel)));
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(0));
    EXPECT_CALL(*tunnel, Close()).WillOnce(Return(0));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
    context->StopSecureTunnel();

    weak_ptr<MockSecureTunnelingContext> weakContext = context;
    context->deferLifecycleTasks = true;
    context.reset();
    EXPECT_FALSE(weakContext.expired());
    EXPECT_EQ(std::future_status::timeout, stoppedFuture.wait_for(std::chrono::seconds(0)));

    auto callback = std::move(onStopped);
    ASSERT_TRUE(static_cast<bool>(callback));
    callback(nullptr);

    EXPECT_EQ(std::future_status::ready, stoppedFuture.wait_for(std::chrono::seconds(0)));
    EXPECT_FALSE(weakContext.expired());

    auto retainedContext = weakContext.lock();
    ASSERT_TRUE(static_cast<bool>(retainedContext));
    retainedContext->RunNextLifecycleTask();
    retainedContext.reset();

    EXPECT_TRUE(weakContext.expired());
}

TEST_F(TestSecureTunnelContext, StopDuringConnectIsQueuedAfterConnect)
{
    auto connectEntered = make_shared<promise<void>>();
    auto connectEnteredFuture = connectEntered->get_future();
    auto allowConnect = make_shared<promise<void>>();
    auto allowConnectFuture = allowConnect->get_future().share();
    atomic<bool> closeCalled{false};
    promise<bool> connectResult;
    auto connectResultFuture = connectResult.get_future();

    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);
    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), Return(tunnel)));
    EXPECT_CALL(*tunnel, Connect())
        .WillOnce(InvokeWithoutArgs([&]() {
            connectEntered->set_value();
            allowConnectFuture.wait();
            return AWS_OP_SUCCESS;
        }));
    EXPECT_CALL(*tunnel, Close())
        .WillOnce(InvokeWithoutArgs([&]() {
            closeCalled = true;
            return AWS_OP_SUCCESS;
        }));

    thread connectThread([&]() { connectResult.set_value(context->ConnectToSecureTunnel()); });
    if (future_status::ready != connectEnteredFuture.wait_for(chrono::seconds(3)))
    {
        allowConnect->set_value();
        connectThread.join();
        FAIL() << "Tunnel connection did not start";
    }

    context->StopSecureTunnel();
    EXPECT_FALSE(closeCalled);

    allowConnect->set_value();
    connectThread.join();

    EXPECT_TRUE(connectResultFuture.get());
    EXPECT_TRUE(closeCalled);

    weak_ptr<MockSecureTunnelingContext> weakContext = context;
    context.reset();
    EXPECT_FALSE(weakContext.expired());

    auto callback = std::move(onStopped);
    ASSERT_TRUE(static_cast<bool>(callback));
    callback(nullptr);

    EXPECT_TRUE(weakContext.expired());
}

TEST_F(TestSecureTunnelContext, StopDuringFailedConnectDoesNotRetainContext)
{
    auto connectEntered = make_shared<promise<void>>();
    auto connectEnteredFuture = connectEntered->get_future();
    auto allowConnect = make_shared<promise<void>>();
    auto allowConnectFuture = allowConnect->get_future().share();
    promise<bool> connectResult;
    auto connectResultFuture = connectResult.get_future();

    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);
    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), Return(tunnel)));
    EXPECT_CALL(*tunnel, Connect())
        .WillOnce(InvokeWithoutArgs([&]() {
            connectEntered->set_value();
            allowConnectFuture.wait();
            return AWS_OP_ERR;
        }));
    EXPECT_CALL(*tunnel, Close()).Times(0);

    thread connectThread([&]() { connectResult.set_value(context->ConnectToSecureTunnel()); });
    if (future_status::ready != connectEnteredFuture.wait_for(chrono::seconds(3)))
    {
        allowConnect->set_value();
        connectThread.join();
        FAIL() << "Tunnel connection did not start";
    }

    context->StopSecureTunnel();
    allowConnect->set_value();
    connectThread.join();

    EXPECT_FALSE(connectResultFuture.get());
    weak_ptr<MockSecureTunnelingContext> weakContext = context;
    onStopped = nullptr;
    context.reset();

    EXPECT_TRUE(weakContext.expired());
}

TEST_F(TestSecureTunnelContext, StopQueueFailureIsRetried)
{
    context = make_shared<MockSecureTunnelingContext>(manager, rootCa, accessToken, endpoint, port, nullptr);
    EXPECT_CALL(*context, CreateSecureTunnel(_, _, _, _, _, _, _, _))
        .WillOnce(DoAll(SaveArg<7>(&onStopped), Return(tunnel)));
    EXPECT_CALL(*tunnel, Connect()).WillOnce(Return(AWS_OP_SUCCESS));
    EXPECT_CALL(*tunnel, Close())
        .WillOnce(Return(AWS_OP_ERR))
        .WillOnce(Return(AWS_OP_SUCCESS));

    ASSERT_TRUE(context->ConnectToSecureTunnel());
    context->deferLifecycleTasks = true;
    context->StopSecureTunnel();
    ASSERT_EQ(1u, context->scheduledLifecycleTasks.size());

    context->RunNextLifecycleTask();
    ASSERT_EQ(1u, context->scheduledLifecycleTasks.size());

    context->RunNextLifecycleTask();
    EXPECT_TRUE(context->scheduledLifecycleTasks.empty());

    weak_ptr<MockSecureTunnelingContext> weakContext = context;
    context.reset();
    EXPECT_FALSE(weakContext.expired());

    auto callback = std::move(onStopped);
    ASSERT_TRUE(static_cast<bool>(callback));
    callback(nullptr);

    auto retainedContext = weakContext.lock();
    ASSERT_TRUE(static_cast<bool>(retainedContext));
    ASSERT_EQ(1u, retainedContext->scheduledLifecycleTasks.size());
    retainedContext->RunNextLifecycleTask();
    retainedContext.reset();

    EXPECT_TRUE(weakContext.expired());
}
