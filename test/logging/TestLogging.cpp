// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/logging/FileLogger.h"
#include "../../source/logging/StdOutLogger.h"
#include "gtest/gtest.h"

using namespace std;
using namespace Aws::Iot::DeviceClient::Logging;
using DeviceClientLogLevel = Aws::Iot::DeviceClient::Logging::LogLevel;

namespace
{
    class RecordingLogger : public Logger
    {
      public:
        void configureLevel(DeviceClientLogLevel level) { setLogLevel(static_cast<int>(level)); }
        size_t queuedLogCount() const { return queueCount; }
        DeviceClientLogLevel lastQueuedLevel() const { return lastLevel; }

        bool start(const Aws::Iot::DeviceClient::PlainConfig &) override { return true; }
        void stop() override {}
        void shutdown() override {}
        unique_ptr<LogQueue> takeLogQueue() override { return unique_ptr<LogQueue>(new LogQueue); }
        void setLogQueue(unique_ptr<LogQueue>) override {}
        void flush() override {}

      protected:
        void queueLog(
            DeviceClientLogLevel level,
            const char *,
            chrono::time_point<chrono::system_clock>,
            const string &) override
        {
            ++queueCount;
            lastLevel = level;
        }

      private:
        size_t queueCount{0};
        DeviceClientLogLevel lastLevel{DeviceClientLogLevel::ERROR};
    };
} // namespace

TEST(Logging, marshalsTraceLogLevel)
{
    ASSERT_STREQ("[TRACE]", LogLevelMarshaller::ToString(DeviceClientLogLevel::TRACE));
}

TEST(Logging, queuesTraceMessagesOnlyAtTraceLevel)
{
    RecordingLogger logger;
    auto now = chrono::system_clock::now();

    logger.configureLevel(DeviceClientLogLevel::DEBUG);
    logger.trace("TAG", now, "hidden trace");
    EXPECT_EQ(0U, logger.queuedLogCount());

    logger.configureLevel(DeviceClientLogLevel::TRACE);
    logger.trace("TAG", now, "visible trace");
    EXPECT_EQ(1U, logger.queuedLogCount());
    EXPECT_EQ(DeviceClientLogLevel::TRACE, logger.lastQueuedLevel());
}

TEST(Logging, swapsLogQueue)
{
    unique_ptr<Logger> stdOutLogger = unique_ptr<Logger>(new StdOutLogger);
    stdOutLogger->error("TAG", std::chrono::system_clock::now(), "Message 1");
    stdOutLogger->error("TAG", std::chrono::system_clock::now(), "Message 2");

    unique_ptr<LogQueue> stdQueue = stdOutLogger->takeLogQueue();
    ASSERT_TRUE(stdQueue->hasNextLog());
    unique_ptr<LogQueue> emptyStdQueue = stdOutLogger->takeLogQueue();
    ASSERT_FALSE(emptyStdQueue->hasNextLog());

    unique_ptr<Logger> fileLogger = unique_ptr<Logger>(new FileLogger);
    fileLogger->setLogQueue(std::move(stdQueue));
    unique_ptr<LogQueue> fileQueue = fileLogger->takeLogQueue();
    ASSERT_TRUE(fileQueue->hasNextLog());
    unique_ptr<LogQueue> emptyFileQueue = fileLogger->takeLogQueue();
}

TEST(Logging, queueNotNullAfterTake)
{
    unique_ptr<Logger> stdOutLogger = unique_ptr<Logger>(new StdOutLogger);
    stdOutLogger->error("TAG", std::chrono::system_clock::now(), "Message 1");
    stdOutLogger->error("TAG", std::chrono::system_clock::now(), "Message 2");

    unique_ptr<LogQueue> stdQueue = stdOutLogger->takeLogQueue();
    ASSERT_TRUE(stdQueue->hasNextLog());

    ASSERT_TRUE(NULL != stdOutLogger->takeLogQueue());
    ASSERT_FALSE(stdOutLogger->takeLogQueue()->hasNextLog());
}
