// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "../../source/util/LockFile.h"
#include "gtest/gtest.h"

#include <fstream>
#include <thread>
#include <unistd.h>

using namespace std;
using namespace Aws::Iot::DeviceClient::Util;

TEST(LockFile, normalCreation)
{
    string path = "/run/lock/";
    string fileName = "devicecl.lock";
    string thingName = "thing";
    unique_ptr<LockFile> lockFile = unique_ptr<LockFile>(new LockFile{path, "./aws-iot-device-client", thingName});

    ifstream fileIn(path + fileName);
    ASSERT_TRUE(fileIn);

    string storedPid;
    string storedName;
    if (fileIn >> storedName && fileIn >> storedPid)
    {
        ASSERT_STREQ(thingName.c_str(), storedName.c_str());
        ASSERT_STREQ(to_string(getpid()).c_str(), storedPid.c_str());
    }
}

TEST(LockFile, earlyDeletion)
{
    string path = "/run/lock/";
    string fileName = "devicecl.lock";
    string thingName = "thing";
    unique_ptr<LockFile> lockFile = unique_ptr<LockFile>(new LockFile{path, "test-aws-iot-device-client", thingName});
    lockFile.reset();

    ifstream fileIn(path + fileName);
    ASSERT_FALSE(fileIn);
}

TEST(LockFile, multipleFiles)
{
    string path = "/run/lock/";
    string thingName = "thing";
    unique_ptr<LockFile> lockFile = unique_ptr<LockFile>(new LockFile{path, "test-aws-iot-device-client", thingName});

    EXPECT_THROW(unique_ptr<LockFile>(new LockFile{path, "test-aws-iot-device-client", thingName}), std::runtime_error);
}

TEST(LockFile, multipleFilesWithExtendedPath)
{
    string path = "/run/lock/";
    string thingName = "thing";
    unique_ptr<LockFile> lockFile = unique_ptr<LockFile>(new LockFile{path, "test-aws-iot-device-client", thingName});

    EXPECT_THROW(
        unique_ptr<LockFile>(new LockFile{path, "directory/test-aws-iot-device-client", thingName}),
        std::runtime_error);
}

TEST(LockFile, staleFile)
{
    string path = "/run/lock/";
    string fileName = "devicecl.lock";
    string thingName = "thing";
    string pidMax;
    ifstream pidFile("/proc/sys/kernel/pid_max");
    if (pidFile && pidFile >> pidMax)
    {
        ofstream fileOut(path + fileName);
        if (fileOut)
        {
            fileOut << pidMax;
        }
        fileOut.close();

        unique_ptr<LockFile> lockFile =
            unique_ptr<LockFile>(new LockFile{path, "test-aws-iot-device-client", thingName});

        ifstream fileIn(path + fileName);
        ASSERT_TRUE(fileIn);

        string storedPid;
        string storedName;
        if (fileIn >> storedName && fileIn >> storedPid)
        {
            ASSERT_STREQ(thingName.c_str(), storedName.c_str());
            ASSERT_STREQ(to_string(getpid()).c_str(), storedPid.c_str());
        }
    }
}
