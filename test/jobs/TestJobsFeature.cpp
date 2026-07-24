// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/Feature.h"
#include "../../source/jobs/JobsFeature.h"
#include <aws/iotjobs/IotJobsClient.h>
#include <aws/iotjobs/NextJobExecutionChangedEvent.h>
#include <aws/iotjobs/NextJobExecutionChangedSubscriptionRequest.h>
#include <aws/iotjobs/StartNextPendingJobExecutionRequest.h>
#include <aws/iotjobs/StartNextPendingJobExecutionSubscriptionRequest.h>
#include <aws/iotjobs/UpdateJobExecutionRequest.h>
#include <aws/iotjobs/UpdateJobExecutionSubscriptionRequest.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <aws/iotjobs/RejectedError.h>
#include <aws/iotjobs/StartNextJobExecutionResponse.h>
#include <aws/iotjobs/UpdateJobExecutionResponse.h>

using namespace std;
using namespace testing;
using namespace Aws;
using namespace Aws::Crt;
using namespace Aws::Iotjobs;
using namespace Aws::Iot::DeviceClient;
using namespace Aws::Iot::DeviceClient::Jobs;

PlainConfig getSimpleConfig()
{
    constexpr char jsonString[] = R"(
{
    "endpoint": "endpoint value",
    "cert": "/tmp/aws-iot-device-client-test-file",
    "key": "/tmp/aws-iot-device-client-test-file",
    "root-ca": "/tmp/aws-iot-device-client-test-file",
    "thing-name": "thing-name value",
    "logging": {
        "level": "ERROR",
        "type": "file",
        "file": "./aws-iot-device-client.log"
    },
    "jobs": {
        "enabled": true
    }
})";

    JsonObject jsonObject(jsonString);
    JsonView jsonView = jsonObject.View();

    PlainConfig config;
    config.LoadFromJson(jsonView);

    return config;
}

JobExecutionData getSampleJobExecution(std::string jobId, int executionNumber)
{
    constexpr char jsonString[] = R"(
{
    "version": "1.0",
    "jobId": "test-job-id",
    "includeStdOut": "true",
    "conditions": [{
                    "key" : "operatingSystem",
                    "value": ["ubuntu", "redhat"],
                     "type": "stringEqual"
                 },
                 {
                    "key" : "OS",
                     "value": ["16.0"],
                     "type": "stringEqual"
    }],
    "steps": [{
            "action": {
                "name": "downloadJobHandler",
                "type": "runHandler",
                "input": {
                    "handler": "download-file.sh",
                    "args": ["presignedUrl", "/tmp/aws-iot-device-client/"],
                    "path": "path to handler"
                },
                "runAsUser": "user1",
                "allowStdErr": 8,
                "ignoreStepFailure": "true"
            }
        },
        {
            "action": {
                "name": "installApplicationAndReboot",
                "type": "runHandler",
                "input": {
                    "handler": "install-app.sh",
                    "args": [
                        "applicationName",
                        "active"
                    ],
                    "path": "path to handler"
                },
                "runAsUser": "user1",
                "allowStdErr": 8,
                "ignoreStepFailure": "true"
            }
        },
        {
            "action": {
                "name": "validateAppStatus",
                "type": "runHandler",
                "input": {
                    "handler": "validate-app-status.sh",
                    "args": [
                        "applicationName",
                        "active"
                    ],
                    "path": "path to handler"
                },
                "runAsUser": "user1",
                "allowStdErr": 8,
                "ignoreStepFailure": "true"
            }
        }
    ],
    "finalStep": {
        "action": {
            "name": "deleteDownloadedHandler",
            "type": "runHandler",
            "input": {
                 "handler": "validate-app-status.sh",
                 "args": [
                    "applicationName",
                    "active"
                ],
                "path": "path to handler"
             },
            "runAsUser": "user1",
            "allowStdErr": 8,
            "ignoreStepFailure": "true"
        }
    }
})";

    JsonObject jsonObject(jsonString);
    JsonView jsonView = jsonObject.View();

    JobExecutionData jobExecutionData(jsonView);
    jobExecutionData.JobDocument = Aws::Crt::Optional<JsonObject>(jsonObject);
    jobExecutionData.JobId =
        Aws::Crt::Optional<basic_string<char, char_traits<char>, StlAllocator<char>>>(jobId.c_str());
    jobExecutionData.ExecutionNumber = Aws::Crt::Optional<int64_t>(executionNumber);

    return jobExecutionData;
}

JobExecutionData getInvalidJobExecution(const char *jobId)
{
    JobExecutionData job;
    job.JobDocument = Aws::Crt::Optional<JsonObject>(JsonObject("{invalid}"));
    job.JobId =
        Aws::Crt::Optional<basic_string<char, char_traits<char>, StlAllocator<char>>>(jobId);
    job.ExecutionNumber = Aws::Crt::Optional<int64_t>(1);
    return job;
}

class MockJobsClient : public AbstractIotJobsClient
{
  public:
    MOCK_METHOD(
        void,
        PublishStartNextPendingJobExecution,
        (const StartNextPendingJobExecutionRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotjobs::OnPublishComplete &onPubAck),
        (override));
    MOCK_METHOD(
        bool,
        SubscribeToStartNextPendingJobExecutionAccepted,
        (const Iotjobs::StartNextPendingJobExecutionSubscriptionRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotjobs::OnSubscribeToStartNextPendingJobExecutionAcceptedResponse &handler,
         const Iotjobs::OnSubscribeComplete &onSubAck),
        (override));
    MOCK_METHOD(
        bool,
        SubscribeToStartNextPendingJobExecutionRejected,
        (const Iotjobs::StartNextPendingJobExecutionSubscriptionRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotjobs::OnSubscribeToStartNextPendingJobExecutionRejectedResponse &handler,
         const Iotjobs::OnSubscribeComplete &onSubAck),
        (override));
    MOCK_METHOD(
        bool,
        SubscribeToNextJobExecutionChangedEvents,
        (const Iotjobs::NextJobExecutionChangedSubscriptionRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotjobs::OnSubscribeToNextJobExecutionChangedEventsResponse &handler,
         const Iotjobs::OnSubscribeComplete &onSubAck),
        (override));
    MOCK_METHOD(
        bool,
        SubscribeToUpdateJobExecutionAccepted,
        (const Iotjobs::UpdateJobExecutionSubscriptionRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotjobs::OnSubscribeToUpdateJobExecutionAcceptedResponse &handler,
         const Iotjobs::OnSubscribeComplete &onSubAck),
        (override));
    MOCK_METHOD(
        bool,
        SubscribeToUpdateJobExecutionRejected,
        (const Iotjobs::UpdateJobExecutionSubscriptionRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotjobs::OnSubscribeToUpdateJobExecutionRejectedResponse &handler,
         const Iotjobs::OnSubscribeComplete &onSubAck),
        (override));
    MOCK_METHOD(
        void,
        PublishUpdateJobExecution,
        (const Iotjobs::UpdateJobExecutionRequest &request,
         Aws::Crt::Mqtt::QOS qos,
         const Iotjobs::OnPublishComplete &onPubAck),
        (override));
};

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

class MockJobEngine : public JobEngine
{
  public:
    MOCK_METHOD(void, processCmdOutput, (int fd, bool isStdErr, int childPID), (override));
    MOCK_METHOD(int, exec_steps, (PlainJobDocument jobDocument, const std::string &jobHandlerDir), (override));
    MOCK_METHOD(int, hasErrors, (), (override));
    MOCK_METHOD(string, getReason, (int statusCode), (override));
    MOCK_METHOD(string, getStdOut, (), (override));
    MOCK_METHOD(string, getStdErr, (), (override));
};

class MockJobsFeature : public JobsFeature
{
  public:
    MockJobsFeature() : JobsFeature() {}
    void invokeRunJobs() { this->runJobs(); }
    MOCK_METHOD(shared_ptr<AbstractIotJobsClient>, createJobsClient, (), (override));
    MOCK_METHOD(shared_ptr<JobEngine>, createJobEngine, (), (override));
    MOCK_METHOD(
        void,
        publishUpdateJobExecutionStatusWithRetry,
        (const Aws::Iotjobs::JobExecutionData &data,
         const JobsFeature::JobExecutionStatusInfo &statusInfo,
         (const Aws::Crt::Map<Aws::Crt::String, Aws::Crt::String> &statusDetails),
         const std::function<void(void)> &onCompleteCallback),
        (override));
};

class TestJobsFeature : public ::testing::Test
{
  public:
    void SetUp()
    {
        // Initializing allocator, so we can use CJSON lib from SDK in our unit tests.
        resourceManager.initializeAllocator();

        ThingName = Aws::Crt::String("thing-name value");
        notifier = shared_ptr<MockNotifier>(new MockNotifier());
        config = getSimpleConfig();
        startNextJobExecutionResponse =
            std::unique_ptr<StartNextJobExecutionResponse>(new StartNextJobExecutionResponse());
        jobsMock = unique_ptr<MockJobsFeature>(new MockJobsFeature());
        mockClient = shared_ptr<MockJobsClient>(new MockJobsClient());
        mockEngine = shared_ptr<MockJobEngine>(new MockJobEngine());
    }
    Aws::Crt::String ThingName;
    shared_ptr<MockNotifier> notifier;
    SharedCrtResourceManager resourceManager;
    PlainConfig config;
    unique_ptr<StartNextJobExecutionResponse> startNextJobExecutionResponse;
    unique_ptr<MockJobsFeature> jobsMock;
    shared_ptr<MockJobsClient> mockClient;
    shared_ptr<MockJobEngine> mockEngine;
};

MATCHER_P(ThingNameEq, ThingName, "Matcher ThingName for all Aws request Objects using Aws::Crt::String")
{
    return arg.ThingName.value() == ThingName;
}

MATCHER_P(StatusInfoEq, statusInfo, "Matches JobExecutionStatusInfo status")
{
    return arg.status == statusInfo.status && arg.reason == statusInfo.reason &&
           arg.stdoutput == statusInfo.stdoutput && arg.stderror == statusInfo.stderror;
}

MATCHER_P(JobExecutionEq, job, "Matches JobExecutionData JobId & ExecutionNumber")
{
    return arg.JobId.value() == job.JobId.value() && arg.ExecutionNumber.value() == job.ExecutionNumber.value();
}

ACTION_P(InvokeSubAck, ioError)
{
    arg3(ioError);
    return true;
}

enum class StartupSubscription
{
    START_NEXT_ACCEPTED,
    START_NEXT_REJECTED,
    NEXT_JOB_CHANGED
};

ACTION_P3(InvokeOrCaptureSubAck, capture, savedCallback, callbackCaptured)
{
    if (capture)
    {
        *savedCallback = arg3;
        callbackCaptured->set_value();
    }
    else
    {
        arg3(0);
    }
    return true;
}

static void expectSuccessfulJobsStartup(
    MockJobsFeature &jobsFeature,
    const shared_ptr<MockJobsClient> &client,
    const Aws::Crt::String &thingName)
{
    EXPECT_CALL(jobsFeature, createJobsClient()).Times(1).WillOnce(Return(client));
    EXPECT_CALL(
        *client,
        SubscribeToStartNextPendingJobExecutionAccepted(
            ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *client,
        SubscribeToStartNextPendingJobExecutionRejected(
            ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *client,
        SubscribeToNextJobExecutionChangedEvents(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *client,
        SubscribeToUpdateJobExecutionAccepted(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *client,
        SubscribeToUpdateJobExecutionRejected(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *client,
        PublishStartNextPendingJobExecution(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));
}

struct JobsRecoveryCallbacks
{
    Iotjobs::OnSubscribeToStartNextPendingJobExecutionAcceptedResponse startNextAcceptedResponse;
    Iotjobs::OnSubscribeToNextJobExecutionChangedEventsResponse nextJobChangedResponse;
    Iotjobs::OnSubscribeComplete startNextAccepted;
    Iotjobs::OnSubscribeComplete startNextRejected;
    Iotjobs::OnSubscribeComplete nextJobChanged;
    Iotjobs::OnSubscribeComplete updateJobAccepted;
    Iotjobs::OnSubscribeComplete updateJobRejected;
};

static void expectJobsRecoverySubscriptions(
    const shared_ptr<MockJobsClient> &client,
    const Aws::Crt::String &thingName,
    JobsRecoveryCallbacks &callbacks,
    bool startNextAcceptedSubmitted = true)
{
    EXPECT_CALL(
        *client,
        SubscribeToStartNextPendingJobExecutionAccepted(
            ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(
            SaveArg<2>(&callbacks.startNextAcceptedResponse),
            SaveArg<3>(&callbacks.startNextAccepted),
            Return(startNextAcceptedSubmitted)));
    EXPECT_CALL(
        *client,
        SubscribeToStartNextPendingJobExecutionRejected(
            ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(SaveArg<3>(&callbacks.startNextRejected), Return(true)));
    EXPECT_CALL(
        *client,
        SubscribeToNextJobExecutionChangedEvents(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(
            SaveArg<2>(&callbacks.nextJobChangedResponse), SaveArg<3>(&callbacks.nextJobChanged), Return(true)));
    EXPECT_CALL(
        *client,
        SubscribeToUpdateJobExecutionAccepted(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobAccepted), Return(true)));
    EXPECT_CALL(
        *client,
        SubscribeToUpdateJobExecutionRejected(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobRejected), Return(true)));
}

TEST_F(TestJobsFeature, GetName)
{
    /**
     * Simple test for GetName
     */
    ASSERT_STREQ(jobsMock->getName().c_str(), "Jobs");
}

TEST_F(TestJobsFeature, Init)
{
    /**
     * Test init Jobs with null MqttConnection, Mock notifier, and PlainConfig
     */
    ASSERT_EQ(0, jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config));
}

TEST_F(TestJobsFeature, RunJobsHappy)
{
    /**
     * Inject a MockJobsClient into Jobs Feature and invoke RunJobs
     * Verifies Subscription Requests to IotJobsClient and invokes SubAck Callback functions
     * Invokes SubAck callbacks rather than elaborate argument matching
     */
    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));

    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
}

TEST_F(TestJobsFeature, StartupIgnoresJobEventsUntilAllSubscriptionsAreReady)
{
    const JobExecutionData startNextJob = getInvalidJobExecution("start-next-job");
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(startNextJob);
    const JobExecutionData nextJob = getInvalidJobExecution("next-job-changed");
    NextJobExecutionChangedEvent nextJobEvent;
    nextJobEvent.Execution = Aws::Crt::Optional<JobExecutionData>(nextJob);

    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(
            InvokeArgument<3>(0), InvokeArgument<2>(startNextJobExecutionResponse.get(), 0), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient,
        SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), InvokeArgument<2>(&nextJobEvent, 0), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient,
        PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));
    EXPECT_CALL(*jobsMock, publishUpdateJobExecutionStatusWithRetry(_, _, _, _)).Times(0);

    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
}

TEST_F(TestJobsFeature, StartupQueueFailureRecoversSubscriptionsBeforePolling)
{
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    JobsRecoveryCallbacks callbacks;
    Iotjobs::OnSubscribeComplete failedStartupAck;
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(DoAll(SaveArg<3>(&failedStartupAck), Return(false)))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.startNextAccepted), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.startNextRejected), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.nextJobChanged), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobAccepted), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobRejected), Return(true)));
    EXPECT_CALL(
        *notifier,
        onError(
            jobsMock.get(),
            ClientBaseErrorNotification::SUBSCRIPTION_FAILED,
            AllOf(HasSubstr("Failed to queue"), HasSubstr("StartNextPendingJobExecution accepted"))))
        .Times(1);

    int publishCount = 0;
    EXPECT_CALL(
        *mockClient,
        PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(Invoke(
            [&publishCount](
                const StartNextPendingJobExecutionRequest &,
                Aws::Crt::Mqtt::QOS,
                const Iotjobs::OnPublishComplete &onPubAck) {
                ++publishCount;
                onPubAck(0);
            }));

    jobsMock->invokeRunJobs();

    ASSERT_TRUE(callbacks.startNextAccepted);
    ASSERT_TRUE(callbacks.startNextRejected);
    ASSERT_TRUE(callbacks.nextJobChanged);
    ASSERT_TRUE(callbacks.updateJobAccepted);
    ASSERT_TRUE(callbacks.updateJobRejected);
    EXPECT_EQ(0, publishCount);

    // A callback received after the subscription request returned false must not complete the startup result twice.
    ASSERT_TRUE(failedStartupAck);
    failedStartupAck(0);

    callbacks.startNextAccepted(0);
    callbacks.startNextRejected(0);
    callbacks.nextJobChanged(0);
    callbacks.updateJobAccepted(0);
    EXPECT_EQ(0, publishCount);

    callbacks.updateJobRejected(0);
    EXPECT_EQ(1, publishCount);
}

static void verifyStartupSubAckFailureRecovery(
    MockJobsFeature &jobsFeature,
    const shared_ptr<MockJobsClient> &client,
    const shared_ptr<MockNotifier> &notifier,
    const PlainConfig &config,
    const Aws::Crt::String &thingName,
    StartupSubscription failedStartupSubscription)
{
    jobsFeature.init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    EXPECT_CALL(jobsFeature, createJobsClient()).Times(1).WillOnce(Return(client));

    promise<void> delayedAckCaptured;
    future<void> delayedAckCapturedFuture = delayedAckCaptured.get_future();
    Iotjobs::OnSubscribeComplete delayedAck;
    JobsRecoveryCallbacks callbacks;

    EXPECT_CALL(
        *client,
        SubscribeToStartNextPendingJobExecutionAccepted(
            ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeOrCaptureSubAck(
            failedStartupSubscription == StartupSubscription::START_NEXT_ACCEPTED,
            &delayedAck,
            &delayedAckCaptured))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.startNextAccepted), Return(true)));
    EXPECT_CALL(
        *client,
        SubscribeToStartNextPendingJobExecutionRejected(
            ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeOrCaptureSubAck(
            failedStartupSubscription == StartupSubscription::START_NEXT_REJECTED,
            &delayedAck,
            &delayedAckCaptured))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.startNextRejected), Return(true)));
    EXPECT_CALL(
        *client,
        SubscribeToNextJobExecutionChangedEvents(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeOrCaptureSubAck(
            failedStartupSubscription == StartupSubscription::NEXT_JOB_CHANGED,
            &delayedAck,
            &delayedAckCaptured))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.nextJobChanged), Return(true)));
    EXPECT_CALL(
        *client,
        SubscribeToUpdateJobExecutionAccepted(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobAccepted), Return(true)));
    EXPECT_CALL(
        *client,
        SubscribeToUpdateJobExecutionRejected(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobRejected), Return(true)));

    const char *failedSubscription = "";
    switch (failedStartupSubscription)
    {
        case StartupSubscription::START_NEXT_ACCEPTED:
            failedSubscription = "StartNextJobAccepted";
            break;
        case StartupSubscription::START_NEXT_REJECTED:
            failedSubscription = "StartNextJobRejected";
            break;
        case StartupSubscription::NEXT_JOB_CHANGED:
            failedSubscription = "NextJobChanged";
            break;
    }
    EXPECT_CALL(
        *notifier,
        onError(
            &jobsFeature,
            ClientBaseErrorNotification::SUBSCRIPTION_FAILED,
            HasSubstr(failedSubscription)))
        .Times(1);

    atomic<int> publishCount{0};
    EXPECT_CALL(
        *client,
        PublishStartNextPendingJobExecution(ThingNameEq(thingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(Invoke(
            [&publishCount](
                const StartNextPendingJobExecutionRequest &,
                Aws::Crt::Mqtt::QOS,
                const Iotjobs::OnPublishComplete &onPubAck) {
                ++publishCount;
                onPubAck(0);
            }));

    thread startupThread([&jobsFeature]() { jobsFeature.invokeRunJobs(); });
    if (future_status::ready != delayedAckCapturedFuture.wait_for(chrono::seconds(3)))
    {
        startupThread.join();
        FAIL() << "Startup subscription acknowledgement was not captured";
    }

    ASSERT_TRUE(delayedAck);
    EXPECT_EQ(0, publishCount.load());
    delayedAck(42);
    startupThread.join();

    ASSERT_TRUE(callbacks.startNextAccepted);
    ASSERT_TRUE(callbacks.startNextRejected);
    ASSERT_TRUE(callbacks.nextJobChanged);
    ASSERT_TRUE(callbacks.updateJobAccepted);
    ASSERT_TRUE(callbacks.updateJobRejected);
    EXPECT_EQ(0, publishCount.load());

    callbacks.startNextAccepted(0);
    callbacks.startNextRejected(0);
    callbacks.nextJobChanged(0);
    callbacks.updateJobAccepted(0);
    EXPECT_EQ(0, publishCount.load());

    callbacks.updateJobRejected(0);
    EXPECT_EQ(1, publishCount.load());
}

TEST_F(TestJobsFeature, StartupStartNextAcceptedSubAckFailureRecoversBeforePolling)
{
    verifyStartupSubAckFailureRecovery(
        *jobsMock, mockClient, notifier, config, ThingName, StartupSubscription::START_NEXT_ACCEPTED);
}

TEST_F(TestJobsFeature, StartupStartNextRejectedSubAckFailureRecoversBeforePolling)
{
    verifyStartupSubAckFailureRecovery(
        *jobsMock, mockClient, notifier, config, ThingName, StartupSubscription::START_NEXT_REJECTED);
}

TEST_F(TestJobsFeature, StartupNextJobChangedSubAckFailureRecoversBeforePolling)
{
    verifyStartupSubAckFailureRecovery(
        *jobsMock, mockClient, notifier, config, ThingName, StartupSubscription::NEXT_JOB_CHANGED);
}

TEST_F(TestJobsFeature, ConnectionResumedWithExistingSessionPollsWithoutResubscribing)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    EXPECT_CALL(*mockClient, SubscribeToStartNextPendingJobExecutionAccepted(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToStartNextPendingJobExecutionRejected(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToNextJobExecutionChangedEvents(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToUpdateJobExecutionAccepted(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToUpdateJobExecutionRejected(_, _, _, _)).Times(0);
    EXPECT_CALL(
        *mockClient,
        PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));

    jobsMock->onConnectionResumed(true);
}

TEST_F(TestJobsFeature, ConnectionResumedAfterSessionLossPollsOnlyAfterAllSubscriptionsRecover)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks callbacks;
    expectJobsRecoverySubscriptions(mockClient, ThingName, callbacks);

    int publishCount = 0;
    EXPECT_CALL(
        *mockClient,
        PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(Invoke(
            [&publishCount](
                const StartNextPendingJobExecutionRequest &,
                Aws::Crt::Mqtt::QOS,
                const Iotjobs::OnPublishComplete &onPubAck) {
                ++publishCount;
                onPubAck(0);
            }));

    jobsMock->onConnectionResumed(false);

    ASSERT_TRUE(callbacks.startNextAccepted);
    ASSERT_TRUE(callbacks.startNextRejected);
    ASSERT_TRUE(callbacks.nextJobChanged);
    ASSERT_TRUE(callbacks.updateJobAccepted);
    ASSERT_TRUE(callbacks.updateJobRejected);

    callbacks.startNextAccepted(0);
    callbacks.startNextRejected(0);
    callbacks.nextJobChanged(0);
    callbacks.updateJobAccepted(0);
    EXPECT_EQ(0, publishCount);

    callbacks.updateJobRejected(0);
    EXPECT_EQ(1, publishCount);
}

TEST_F(TestJobsFeature, SubscriptionRecoveryIgnoresJobEventsUntilAllSubscriptionsRecover)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks callbacks;
    expectJobsRecoverySubscriptions(mockClient, ThingName, callbacks);
    EXPECT_CALL(
        *mockClient,
        PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));
    EXPECT_CALL(*jobsMock, publishUpdateJobExecutionStatusWithRetry(_, _, _, _)).Times(0);

    jobsMock->onConnectionResumed(false);

    ASSERT_TRUE(callbacks.startNextAcceptedResponse);
    ASSERT_TRUE(callbacks.nextJobChangedResponse);
    callbacks.startNextAccepted(0);
    callbacks.nextJobChanged(0);

    const JobExecutionData startNextJob = getInvalidJobExecution("start-next-job");
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(startNextJob);
    callbacks.startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);

    const JobExecutionData nextJob = getInvalidJobExecution("next-job-changed");
    NextJobExecutionChangedEvent nextJobEvent;
    nextJobEvent.Execution = Aws::Crt::Optional<JobExecutionData>(nextJob);
    callbacks.nextJobChangedResponse(&nextJobEvent, 0);

    callbacks.startNextRejected(0);
    callbacks.updateJobAccepted(0);
    callbacks.updateJobRejected(0);
}

TEST_F(TestJobsFeature, SessionLossDuringStartupRecoversSubscriptionsBeforePolling)
{
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    JobsRecoveryCallbacks callbacks;
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.startNextAccepted), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.startNextRejected), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.nextJobChanged), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(Invoke(
            [this](
                const Iotjobs::UpdateJobExecutionSubscriptionRequest &,
                Aws::Crt::Mqtt::QOS,
                const Iotjobs::OnSubscribeToUpdateJobExecutionAcceptedResponse &,
                const Iotjobs::OnSubscribeComplete &onSubAck) {
                jobsMock->onConnectionResumed(false);
                jobsMock->onConnectionResumed(true);
                onSubAck(0);
                return true;
            }))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobAccepted), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(2)
        .WillOnce(InvokeSubAck(0))
        .WillOnce(DoAll(SaveArg<3>(&callbacks.updateJobRejected), Return(true)));

    int publishCount = 0;
    EXPECT_CALL(
        *mockClient,
        PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(Invoke(
            [&publishCount](
                const StartNextPendingJobExecutionRequest &,
                Aws::Crt::Mqtt::QOS,
                const Iotjobs::OnPublishComplete &onPubAck) {
                ++publishCount;
                onPubAck(0);
            }));

    jobsMock->invokeRunJobs();

    ASSERT_TRUE(callbacks.startNextAccepted);
    ASSERT_TRUE(callbacks.startNextRejected);
    ASSERT_TRUE(callbacks.nextJobChanged);
    ASSERT_TRUE(callbacks.updateJobAccepted);
    ASSERT_TRUE(callbacks.updateJobRejected);
    EXPECT_EQ(0, publishCount);

    callbacks.startNextAccepted(0);
    callbacks.startNextRejected(0);
    callbacks.nextJobChanged(0);
    callbacks.updateJobAccepted(0);
    EXPECT_EQ(0, publishCount);

    callbacks.updateJobRejected(0);
    EXPECT_EQ(1, publishCount);
}

TEST_F(TestJobsFeature, ConnectionResumedAfterSessionLossDoesNotPollWhenSubscriptionSubmissionFails)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks callbacks;
    expectJobsRecoverySubscriptions(mockClient, ThingName, callbacks, false);
    EXPECT_CALL(
        *notifier,
        onError(
            jobsMock.get(),
            ClientBaseErrorNotification::SUBSCRIPTION_FAILED,
            AllOf(
                HasSubstr("Failed to queue"),
                HasSubstr("StartNextPendingJobExecution accepted"))))
        .Times(1);
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(_, _, _)).Times(0);

    jobsMock->onConnectionResumed(false);

    callbacks.startNextRejected(0);
    callbacks.nextJobChanged(0);
    callbacks.updateJobAccepted(0);
    callbacks.updateJobRejected(0);

    // A misbehaving implementation that invokes the callback despite returning false must not be double-counted.
    callbacks.startNextAccepted(0);
}

TEST_F(TestJobsFeature, ConnectionResumedAfterSessionLossDoesNotPollWhenSubscriptionCallbackFails)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks callbacks;
    expectJobsRecoverySubscriptions(mockClient, ThingName, callbacks);
    EXPECT_CALL(
        *notifier,
        onError(
            jobsMock.get(),
            ClientBaseErrorNotification::SUBSCRIPTION_FAILED,
            AllOf(
                HasSubstr("UpdateJobExecution accepted"),
                HasSubstr("ioError {42}"))))
        .Times(1);
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(_, _, _)).Times(0);
    EXPECT_CALL(*jobsMock, publishUpdateJobExecutionStatusWithRetry(_, _, _, _)).Times(0);

    jobsMock->onConnectionResumed(false);

    callbacks.startNextAccepted(0);
    callbacks.startNextRejected(0);
    callbacks.nextJobChanged(0);
    callbacks.updateJobAccepted(42);
    callbacks.updateJobRejected(0);

    ASSERT_TRUE(callbacks.startNextAcceptedResponse);
    ASSERT_TRUE(callbacks.nextJobChangedResponse);

    const JobExecutionData startNextJob = getInvalidJobExecution("start-next-job");
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(startNextJob);
    callbacks.startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);

    const JobExecutionData nextJob = getInvalidJobExecution("next-job-changed");
    NextJobExecutionChangedEvent nextJobEvent;
    nextJobEvent.Execution = Aws::Crt::Optional<JobExecutionData>(nextJob);
    callbacks.nextJobChangedResponse(&nextJobEvent, 0);
}

TEST_F(TestJobsFeature, ExistingSessionResumeRestartsInterruptedSubscriptionRecovery)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks firstRecovery;
    expectJobsRecoverySubscriptions(mockClient, ThingName, firstRecovery);
    jobsMock->onConnectionResumed(false);
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks secondRecovery;
    expectJobsRecoverySubscriptions(mockClient, ThingName, secondRecovery);

    int publishCount = 0;
    EXPECT_CALL(
        *mockClient,
        PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(Invoke(
            [&publishCount](
                const StartNextPendingJobExecutionRequest &,
                Aws::Crt::Mqtt::QOS,
                const Iotjobs::OnPublishComplete &onPubAck) {
                ++publishCount;
                onPubAck(0);
            }));

    jobsMock->onConnectionResumed(true);

    firstRecovery.startNextAccepted(0);
    firstRecovery.startNextRejected(0);
    firstRecovery.nextJobChanged(0);
    firstRecovery.updateJobAccepted(0);
    firstRecovery.updateJobRejected(0);
    EXPECT_EQ(0, publishCount);

    secondRecovery.startNextAccepted(0);
    secondRecovery.startNextRejected(0);
    secondRecovery.nextJobChanged(0);
    secondRecovery.updateJobAccepted(0);
    EXPECT_EQ(0, publishCount);

    secondRecovery.updateJobRejected(0);
    EXPECT_EQ(1, publishCount);
}

TEST_F(TestJobsFeature, ConnectionResumedBeforeStartupDoesNotPoll)
{
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(_, _, _)).Times(0);

    jobsMock->onConnectionResumed(false);
    jobsMock->onConnectionResumed(true);
}

TEST_F(TestJobsFeature, ConnectionResumedAfterStopDoesNotRecoverOrPoll)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    EXPECT_CALL(*notifier, onEvent(jobsMock.get(), ClientBaseEventNotification::FEATURE_STOPPED)).Times(1);
    jobsMock->stop();
    EXPECT_CALL(*mockClient, SubscribeToStartNextPendingJobExecutionAccepted(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToStartNextPendingJobExecutionRejected(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToNextJobExecutionChangedEvents(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToUpdateJobExecutionAccepted(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, SubscribeToUpdateJobExecutionRejected(_, _, _, _)).Times(0);
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(_, _, _)).Times(0);

    jobsMock->onConnectionResumed(false);
}

TEST_F(TestJobsFeature, SubscriptionRecoveryCallbacksAfterStopAreIgnored)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks callbacks;
    expectJobsRecoverySubscriptions(mockClient, ThingName, callbacks);
    jobsMock->onConnectionResumed(false);
    Mock::VerifyAndClearExpectations(mockClient.get());

    EXPECT_CALL(*notifier, onEvent(jobsMock.get(), ClientBaseEventNotification::FEATURE_STOPPED)).Times(1);
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(_, _, _)).Times(0);
    jobsMock->stop();

    callbacks.startNextAccepted(0);
    callbacks.startNextRejected(0);
    callbacks.nextJobChanged(0);
    callbacks.updateJobAccepted(0);
    callbacks.updateJobRejected(0);
}

TEST_F(TestJobsFeature, StartNextResponseAfterStopNotifiesOnce)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    JobsRecoveryCallbacks callbacks;
    expectJobsRecoverySubscriptions(mockClient, ThingName, callbacks);
    jobsMock->onConnectionResumed(false);
    Mock::VerifyAndClearExpectations(mockClient.get());

    EXPECT_CALL(*notifier, onEvent(jobsMock.get(), ClientBaseEventNotification::FEATURE_STOPPED)).Times(1);
    jobsMock->stop();

    JobExecutionData job = getSampleJobExecution("job1", 1);
    job.Status = Aws::Crt::Optional<JobStatus>(JobStatus::QUEUED);
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(job);

    EXPECT_CALL(
        *notifier,
        onError(
            jobsMock.get(),
            ClientBaseErrorNotification::MESSAGE_RECEIVED_AFTER_SHUTDOWN,
            "Incoming QUEUED job: job1"))
        .Times(1);

    ASSERT_TRUE(callbacks.startNextAcceptedResponse);
    callbacks.startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);
}

TEST_F(TestJobsFeature, StopWaitsForSubscriptionRecoveryToFinishQueueing)
{
    expectSuccessfulJobsStartup(*jobsMock, mockClient, ThingName);
    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    Mock::VerifyAndClearExpectations(mockClient.get());

    promise<void> recoverySubscribeEntered;
    future<void> recoverySubscribeEnteredFuture = recoverySubscribeEntered.get_future();
    promise<void> allowRecoverySubscribe;
    shared_future<void> allowRecoverySubscribeFuture = allowRecoverySubscribe.get_future().share();
    promise<void> stopStarted;
    future<void> stopStartedFuture = stopStarted.get_future();
    promise<void> stopCompleted;
    future<void> stopCompletedFuture = stopCompleted.get_future();

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(
            InvokeWithoutArgs([&]() {
                recoverySubscribeEntered.set_value();
                allowRecoverySubscribeFuture.wait();
            }),
            Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(
            ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(Return(true));
    EXPECT_CALL(
        *mockClient,
        SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(Return(true));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(Return(true));
    EXPECT_CALL(
        *mockClient,
        SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(Return(true));
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(_, _, _)).Times(0);
    EXPECT_CALL(*jobsMock, publishUpdateJobExecutionStatusWithRetry(_, _, _, _)).Times(0);
    EXPECT_CALL(*notifier, onEvent(jobsMock.get(), ClientBaseEventNotification::FEATURE_STOPPED)).Times(1);

    thread recoveryThread([&]() { jobsMock->onConnectionResumed(false); });
    if (future_status::ready != recoverySubscribeEnteredFuture.wait_for(chrono::seconds(3)))
    {
        allowRecoverySubscribe.set_value();
        recoveryThread.join();
        FAIL() << "Recovery subscription did not start";
    }

    thread stopThread([&]() {
        stopStarted.set_value();
        jobsMock->stop();
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
}

TEST_F(TestJobsFeature, ExecuteJobHappy)
{
    /**
     * Inject a MockJobsClient and MockJobEngine into Jobs Feature and invoke RunJobs
     * Verifies Subscription Requests to IotJobsClient and invokes SubAck Callback functions
     * Invokes the StartNextPendingJobExecution Handler with a Test JobExecution
     * Verifies JobExecution is updated to IN_PROGRESS then SUCCEEDED
     */
    const JobExecutionData job = getSampleJobExecution("job1", 1);
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(job);

    // As JobEngine is run in a separate thread this is needed so that the tests wait for that thread to update JE
    std::promise<void> promise;
    auto setPromise = [&promise]() -> void { promise.set_value(); };

    string stdoutput = "test output";
    Iotjobs::OnSubscribeToStartNextPendingJobExecutionAcceptedResponse startNextAcceptedResponse;

    EXPECT_CALL(*jobsMock, createJobEngine()).Times(1).WillOnce(Return(mockEngine));
    EXPECT_CALL(*mockEngine, exec_steps(_, _)).WillOnce(Return(0));
    EXPECT_CALL(*mockEngine, hasErrors()).WillOnce(Return(1));
    EXPECT_CALL(*mockEngine, getReason(_)).WillOnce(Return(""));
    EXPECT_CALL(*mockEngine, getStdOut()).WillOnce(Return(stdoutput));
    EXPECT_CALL(*mockEngine, getStdErr()).WillOnce(Return(""));

    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), SaveArg<2>(&startNextAcceptedResponse), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));

    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::IN_PROGRESS, "", "", "")),
            IsEmpty(),
            IsNull()))
        .Times(1);
    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::SUCCEEDED, "", stdoutput, "")),
            _,
            _))
        .WillOnce(InvokeWithoutArgs(setPromise));

    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    ASSERT_TRUE(startNextAcceptedResponse);
    startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);

    EXPECT_EQ(std::future_status::ready, promise.get_future().wait_for(std::chrono::seconds(3)));
}

TEST_F(TestJobsFeature, ExecuteJobStderror)
{
    /**
     * Inject a MockJobsClient and MockJobEngine into Jobs Feature and invoke RunJobs
     * Verifies Subscription Requests to IotJobsClient and invokes SubAck Callback functions
     * Invokes the StartNextPendingJobExecution Handler with a Test JobExecution
     * Verifies JobExecution is updated to IN_PROGRESS then SUCCEEDED
     * Verifies stdout from JobEngine
     */
    const JobExecutionData job = getSampleJobExecution("job1", 1);
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(job);

    // As JobEngine is run in a separate thread this is needed so that the tests wait for that thread to update JE
    std::promise<void> promise;
    auto setPromise = [&promise]() -> void { promise.set_value(); };

    string stderror = "error output";
    Iotjobs::OnSubscribeToStartNextPendingJobExecutionAcceptedResponse startNextAcceptedResponse;

    EXPECT_CALL(*jobsMock, createJobEngine()).Times(1).WillOnce(Return(mockEngine));

    EXPECT_CALL(*mockEngine, exec_steps(_, _)).WillOnce(Return(0));
    EXPECT_CALL(*mockEngine, hasErrors()).WillOnce(Return(1));
    EXPECT_CALL(*mockEngine, getReason(_)).WillOnce(Return(""));
    EXPECT_CALL(*mockEngine, getStdOut()).WillOnce(Return(""));
    EXPECT_CALL(*mockEngine, getStdErr()).WillOnce(Return(stderror));

    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), SaveArg<2>(&startNextAcceptedResponse), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));

    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::IN_PROGRESS, "", "", "")),
            IsEmpty(),
            IsNull()))
        .Times(1);
    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::SUCCEEDED, "", "", stderror)),
            _,
            _))
        .WillOnce(InvokeWithoutArgs(setPromise));

    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    ASSERT_TRUE(startNextAcceptedResponse);
    startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);

    EXPECT_EQ(std::future_status::ready, promise.get_future().wait_for(std::chrono::seconds(3)));
}

TEST_F(TestJobsFeature, ExecuteJobStdOutAndStderror)
{
    /**
     * Inject a MockJobsClient and MockJobEngine into Jobs Feature and invoke RunJobs
     * Verifies Subscription Requests to IotJobsClient and invokes SubAck Callback functions
     * Invokes the StartNextPendingJobExecution Handler with a Test JobExecution
     * Verifies JobExecution is updated to IN_PROGRESS then SUCCEEDED
     * Verifies stdout from JobEngine
     */
    const JobExecutionData job = getSampleJobExecution("job1", 1);
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(job);

    // As JobEngine is run in a separate thread this is needed so that the tests wait for that thread to update JE
    std::promise<void> promise;
    auto setPromise = [&promise]() -> void { promise.set_value(); };

    string stdoutput = "test output";
    string stderror = "error output";
    Iotjobs::OnSubscribeToStartNextPendingJobExecutionAcceptedResponse startNextAcceptedResponse;

    EXPECT_CALL(*jobsMock, createJobEngine()).Times(1).WillOnce(Return(mockEngine));

    EXPECT_CALL(*mockEngine, exec_steps(_, _)).WillOnce(Return(0));
    EXPECT_CALL(*mockEngine, hasErrors()).WillOnce(Return(1));
    EXPECT_CALL(*mockEngine, getReason(_)).WillOnce(Return(""));
    EXPECT_CALL(*mockEngine, getStdOut()).WillOnce(Return(stdoutput));
    EXPECT_CALL(*mockEngine, getStdErr()).WillOnce(Return(stderror));

    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), SaveArg<2>(&startNextAcceptedResponse), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));

    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::IN_PROGRESS, "", "", "")),
            IsEmpty(),
            IsNull()))
        .Times(1);
    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::SUCCEEDED, "", stdoutput, stderror)),
            _,
            _))
        .WillOnce(InvokeWithoutArgs(setPromise));

    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    ASSERT_TRUE(startNextAcceptedResponse);
    startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);

    EXPECT_EQ(std::future_status::ready, promise.get_future().wait_for(std::chrono::seconds(3)));
}

TEST_F(TestJobsFeature, ExecuteJobDuplicateNotificaton)
{
    /**
     * Sends duplicate StartNextJobExecutionResponse to handler callback. Expect to only update and execute 1
     * JobExecution
     */
    const JobExecutionData job = getSampleJobExecution("job1", 1);
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(job);

    // As JobEngine is run in a separate thread this is needed so that the tests wait for that thread to update JE
    std::promise<void> promise;
    auto setPromise = [&promise]() -> void { promise.set_value(); };

    string stdoutput = "test output";
    Iotjobs::OnSubscribeToStartNextPendingJobExecutionAcceptedResponse startNextAcceptedResponse;

    EXPECT_CALL(*jobsMock, createJobEngine()).Times(1).WillOnce(Return(mockEngine));
    EXPECT_CALL(*mockEngine, exec_steps(_, _)).WillOnce(Return(0));
    EXPECT_CALL(*mockEngine, hasErrors()).WillOnce(Return(1));
    EXPECT_CALL(*mockEngine, getReason(_)).WillOnce(Return(""));
    EXPECT_CALL(*mockEngine, getStdOut()).WillOnce(Return(stdoutput));
    EXPECT_CALL(*mockEngine, getStdErr()).WillOnce(Return(""));

    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), SaveArg<2>(&startNextAcceptedResponse), Return(true)));

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));

    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::IN_PROGRESS, "", "", "")),
            IsEmpty(),
            IsNull()))
        .Times(1);
    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(Iotjobs::JobStatus::SUCCEEDED, "", stdoutput, "")),
            _,
            _))
        .WillOnce(InvokeWithoutArgs(setPromise));

    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    ASSERT_TRUE(startNextAcceptedResponse);
    startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);
    startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);

    EXPECT_EQ(std::future_status::ready, promise.get_future().wait_for(std::chrono::seconds(3)));
}

TEST_F(TestJobsFeature, InvalidJobDocument)
{
    /**
     * Invoke handler callback with invalid JobDocument, expect JobExecution rejected
     */
    JobExecutionData job;
    job.JobDocument = Aws::Crt::Optional<JsonObject>(JsonObject("{invalid}"));
    job.JobId = Aws::Crt::Optional<basic_string<char, char_traits<char>, StlAllocator<char>>>("invalid-job");
    job.ExecutionNumber = Aws::Crt::Optional<int64_t>(1);
    startNextJobExecutionResponse->Execution = Aws::Crt::Optional<JobExecutionData>(job);
    Iotjobs::OnSubscribeToStartNextPendingJobExecutionAcceptedResponse startNextAcceptedResponse;

    EXPECT_CALL(*jobsMock, createJobsClient()).Times(1).WillOnce(Return(mockClient));

    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(DoAll(InvokeArgument<3>(0), SaveArg<2>(&startNextAcceptedResponse), Return(true)));
    EXPECT_CALL(
        *mockClient,
        SubscribeToStartNextPendingJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToNextJobExecutionChangedEvents(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionAccepted(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(
        *mockClient, SubscribeToUpdateJobExecutionRejected(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _, _))
        .Times(1)
        .WillOnce(InvokeSubAck(0));
    EXPECT_CALL(*mockClient, PublishStartNextPendingJobExecution(ThingNameEq(ThingName), AWS_MQTT_QOS_AT_LEAST_ONCE, _))
        .Times(1)
        .WillOnce(InvokeArgument<2>(0));

    EXPECT_CALL(
        *jobsMock,
        publishUpdateJobExecutionStatusWithRetry(
            JobExecutionEq(job),
            StatusInfoEq(JobsFeature::JobExecutionStatusInfo(
                Iotjobs::JobStatus::REJECTED, "Unable to execute job, invalid job document provided!", "", "")),
            _,
            _))
        .Times(1);

    jobsMock->init(std::shared_ptr<Mqtt::MqttConnection>(), notifier, config);
    jobsMock->invokeRunJobs();
    ASSERT_TRUE(startNextAcceptedResponse);
    startNextAcceptedResponse(startNextJobExecutionResponse.get(), 0);
}
