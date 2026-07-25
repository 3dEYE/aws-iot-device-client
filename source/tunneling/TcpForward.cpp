// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "TcpForward.h"
#include "../logging/LoggerFactory.h"
#include <aws/crt/io/SocketOptions.h>
#include <aws/io/event_loop.h>
#include <memory>
#include <vector>

using namespace std;
using namespace Aws::Iot::DeviceClient::Logging;

namespace
{
    struct TcpForwardPendingWrite
    {
        std::vector<uint8_t> payload;
        std::weak_ptr<Aws::Iot::DeviceClient::SecureTunneling::TcpForward> owner;
    };

    struct TcpForwardStopTask
    {
        aws_task task;
        std::function<void(enum aws_task_status)> callback;
    };

    void RunTcpForwardStopTask(aws_task *task, void *arg, enum aws_task_status status)
    {
        (void)task;
        unique_ptr<TcpForwardStopTask> stopTask(static_cast<TcpForwardStopTask *>(arg));
        stopTask->callback(status);
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
                constexpr char TcpForward::TAG[];

                TcpForward::TcpForward(
                    std::shared_ptr<SharedCrtResourceManager> sharedCrtResourceManager,
                    uint16_t port,
                    const OnTcpForwardDataReceive &onTcpForwardDataReceive,
                    const OnTcpForwardTerminated &onTcpForwardTerminated)
                    : mSharedCrtResourceManager(sharedCrtResourceManager), mPort(port),
                      mOnTcpForwardDataReceive(onTcpForwardDataReceive),
                      mOnTcpForwardTerminated(onTcpForwardTerminated)
                {
                    AWS_ZERO_STRUCT(mSocket);
                    Aws::Crt::Io::SocketOptions socketOptions;
                    mSocketInitialized =
                        aws_socket_init(
                            &mSocket,
                            sharedCrtResourceManager->getAllocator(),
                            &socketOptions.GetImpl()) == AWS_OP_SUCCESS;
                    if (mSocketInitialized)
                    {
                        mSendBufferInitialized =
                            aws_byte_buf_init(
                                &mSendBuffer,
                                sharedCrtResourceManager->getAllocator(),
                                1) == AWS_OP_SUCCESS;
                    }
                }

                TcpForward::TcpForward(
                    std::shared_ptr<SharedCrtResourceManager> sharedCrtResourceManager,
                    uint16_t port)
                    : mSharedCrtResourceManager(sharedCrtResourceManager), mPort(port)
                {
                }

                TcpForward::~TcpForward()
                {
                    if (mSocketInitialized && !mEventLoop)
                    {
                        aws_socket_clean_up(&mSocket);
                    }
                    if (mSendBufferInitialized)
                    {
                        aws_byte_buf_clean_up(&mSendBuffer);
                    }
                }

                int TcpForward::Connect()
                {
                    aws_socket_endpoint endpoint{};
                    string localhost = "127.0.0.1";
                    snprintf(endpoint.address, AWS_ADDRESS_MAX_LEN, "%s", localhost.c_str());
                    endpoint.port = mPort;

                    lock_guard<mutex> lock(mLifecycleLock);
                    if (
                        !mSocketInitialized || !mSendBufferInitialized || mConnectStarted || mStopRequested ||
                        mStopped)
                    {
                        return AWS_OP_ERR;
                    }

                    mEventLoop = mSharedCrtResourceManager->getNextEventLoop();
                    if (!mEventLoop)
                    {
                        return AWS_OP_ERR;
                    }

                    mConnectStarted = true;
                    mLifetimeKeepAlive = shared_from_this();

                    int result = aws_socket_connect(&mSocket, &endpoint, mEventLoop, sOnConnectionResult, this);
                    if (result != AWS_OP_SUCCESS)
                    {
                        mEventLoop = nullptr;
                        mLifetimeKeepAlive.reset();
                    }

                    return result;
                }

                void TcpForward::Stop() { RequestStop(false, AWS_OP_SUCCESS); }

                void TcpForward::StopAfterError(int errorCode) { RequestStop(true, errorCode); }

                void TcpForward::RequestStop(bool notifyOwner, int errorCode)
                {
                    shared_ptr<TcpForward> self;
                    aws_event_loop *eventLoop;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopRequested || mStopped)
                        {
                            return;
                        }

                        mStopRequested = true;
                        if (!mLifetimeKeepAlive)
                        {
                            mLifetimeKeepAlive = shared_from_this();
                        }
                        self = mLifetimeKeepAlive;
                        eventLoop = mEventLoop;
                    }

                    if (!eventLoop || aws_event_loop_thread_is_callers_thread(eventLoop))
                    {
                        StopOnEventLoop(notifyOwner, errorCode);
                        return;
                    }

                    unique_ptr<TcpForwardStopTask> stopTask(new TcpForwardStopTask());
                    stopTask->callback = [self, notifyOwner, errorCode](enum aws_task_status status) {
                        if (status == AWS_TASK_STATUS_RUN_READY)
                        {
                            self->StopOnEventLoop(notifyOwner, errorCode);
                        }
                        else
                        {
                            self->OnStopTaskCanceled();
                        }
                    };
                    aws_task_init(
                        &stopTask->task,
                        RunTcpForwardStopTask,
                        stopTask.get(),
                        "tcp_forward_stop");
                    aws_event_loop_schedule_task_now(eventLoop, &stopTask->task);
                    stopTask.release();
                }

                int TcpForward::SendData(const Crt::ByteCursor &data)
                {
                    int result = AWS_OP_SUCCESS;
                    int errorCode = AWS_OP_SUCCESS;
                    bool stopAfterError = false;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopRequested || mStopped)
                        {
                            return AWS_OP_ERR;
                        }

                        if (!mConnected)
                        {
                            LOG_DEBUG(TAG, "Not connected yet. Saving the data to send");
                            if (
                                mSendBuffer.len > MAX_SEND_BUFFER_SIZE ||
                                data.len > MAX_SEND_BUFFER_SIZE - mSendBuffer.len)
                            {
                                LOG_ERROR(TAG, "Local TCP pre-connection buffer limit exceeded.");
                                result = aws_raise_error(AWS_ERROR_SHORT_BUFFER);
                            }
                            else
                            {
                                result = aws_byte_buf_append_dynamic(&mSendBuffer, &data);
                            }

                            if (result != AWS_OP_SUCCESS)
                            {
                                errorCode = aws_last_error();
                                stopAfterError = true;
                            }
                        }
                        else
                        {
                            result = QueueWrite(data);
                            if (result != AWS_OP_SUCCESS)
                            {
                                errorCode = aws_last_error();
                                stopAfterError = true;
                            }
                        }
                    }

                    if (stopAfterError)
                    {
                        StopAfterError(errorCode);
                    }
                    return result;
                }

                void TcpForward::sOnConnectionResult(struct aws_socket *socket, int error_code, void *user_data)
                {
                    auto *self = static_cast<TcpForward *>(user_data);
                    self->OnConnectionResult(socket, error_code);
                }

                void TcpForward::sOnWriteCompleted(
                    struct aws_socket *,
                    int error_code,
                    size_t bytes_written,
                    void *user_data)
                {
                    unique_ptr<TcpForwardPendingWrite> pendingWrite(
                        static_cast<TcpForwardPendingWrite *>(user_data));
                    if (error_code)
                    {
                        LOGM_ERROR(
                            TAG,
                            "TcpForward::sOnWriteCompleted error_code=%d, bytes_written=%d",
                            error_code,
                            bytes_written);
                        auto owner = pendingWrite->owner.lock();
                        if (owner)
                        {
                            owner->StopAfterError(error_code);
                        }
                    }
                }

                void TcpForward::sOnReadable(struct aws_socket *socket, int error_code, void *user_data)
                {
                    auto *self = static_cast<TcpForward *>(user_data);
                    self->OnReadable(socket, error_code);
                }

                void TcpForward::OnConnectionResult(struct aws_socket *, int error_code)
                {
                    LOG_DEBUG(TAG, "TcpForward::OnConnectionResult");
                    if (error_code)
                    {
                        LOGM_ERROR(TAG, "TcpForward::OnConnectionResult error_code=%d", error_code);
                        StopAfterError(error_code);
                        return;
                    }

                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopRequested || mStopped)
                        {
                            return;
                        }
                    }

                    if (SubscribeToReadableEvents() != AWS_OP_SUCCESS)
                    {
                        int subscribeError = aws_last_error();
                        LOGM_ERROR(
                            TAG,
                            "Cannot subscribe to local TCP socket read events. error_code=%d",
                            subscribeError);
                        StopAfterError(subscribeError);
                        return;
                    }

                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopRequested || mStopped)
                        {
                            return;
                        }
                        mConnected = true;
                    }

                    if (FlushSendBuffer() != AWS_OP_SUCCESS)
                    {
                        int flushError = aws_last_error();
                        LOGM_ERROR(
                            TAG,
                            "Cannot flush buffered data to the local TCP socket. error_code=%d",
                            flushError);
                        StopAfterError(flushError);
                    }
                }

                int TcpForward::ReadSocket(aws_byte_buf *buffer, size_t *amountRead)
                {
                    return aws_socket_read(&mSocket, buffer, amountRead);
                }

                int TcpForward::WriteSocket(
                    const aws_byte_cursor &data,
                    aws_socket_on_write_completed_fn *onWriteCompleted,
                    void *userData)
                {
                    return aws_socket_write(&mSocket, &data, onWriteCompleted, userData);
                }

                int TcpForward::SubscribeToReadableEvents()
                {
                    return aws_socket_subscribe_to_readable_events(&mSocket, sOnReadable, this);
                }

                void TcpForward::OnReadable(struct aws_socket *, int error_code)
                {
                    if (error_code && error_code != AWS_IO_SOCKET_CLOSED)
                    {
                        LOGM_ERROR(TAG, "TcpForward::OnReadable error_code=%d", error_code);
                        StopAfterError(error_code);
                        return;
                    }
                    if (error_code == AWS_IO_SOCKET_CLOSED)
                    {
                        LOG_TRACE(TAG, "TcpForward::OnReadable peer closed; draining buffered data");
                    }
                    else
                    {
                        LOG_TRACE(TAG, "TcpForward::OnReadable");
                    }

                    bool terminal = error_code == AWS_IO_SOCKET_CLOSED;
                    int terminalError = error_code;

                    Aws::Crt::ByteBuf everything{}; // For cumulating everything available
                    if (aws_byte_buf_init(&everything, mSharedCrtResourceManager->getAllocator(), 0) != AWS_OP_SUCCESS)
                    {
                        StopAfterError(aws_last_error());
                        return;
                    }

                    constexpr size_t chunkCapacity = 1024;
                    Aws::Crt::ByteBuf chunk{};
                    if (
                        aws_byte_buf_init(&chunk, mSharedCrtResourceManager->getAllocator(), chunkCapacity) !=
                        AWS_OP_SUCCESS)
                    {
                        int bufferError = aws_last_error();
                        aws_byte_buf_clean_up(&everything);
                        StopAfterError(bufferError);
                        return;
                    }

                    size_t amountRead = 0;
                    do
                    {
                        aws_byte_buf_reset(&chunk, false);
                        amountRead = 0;
                        int readResult = ReadSocket(&chunk, &amountRead);
                        if (readResult == AWS_OP_SUCCESS && amountRead > 0)
                        {
                            aws_byte_cursor chunkCursor = aws_byte_cursor_from_buf(&chunk);
                            if (aws_byte_buf_append_dynamic(&everything, &chunkCursor) != AWS_OP_SUCCESS)
                            {
                                terminal = true;
                                terminalError = aws_last_error();
                                break;
                            }
                        }
                        else if (readResult != AWS_OP_SUCCESS)
                        {
                            int readError = aws_last_error();
                            if (readError != AWS_IO_READ_WOULD_BLOCK)
                            {
                                terminal = true;
                                terminalError = readError;
                                if (readError != AWS_IO_SOCKET_CLOSED)
                                {
                                    LOGM_ERROR(TAG, "Cannot read from local TCP socket. error_code=%d", readError);
                                }
                            }
                            break;
                        }
                    } while (amountRead > 0);

                    // Send everything
                    if (everything.len > 0 && mOnTcpForwardDataReceive)
                    {
                        mOnTcpForwardDataReceive(everything);
                    }

                    aws_byte_buf_clean_up(&chunk);
                    aws_byte_buf_clean_up(&everything);

                    if (terminal)
                    {
                        StopAfterError(terminalError);
                    }
                }

                int TcpForward::FlushSendBuffer()
                {
                    lock_guard<mutex> lock(mLifecycleLock);
                    if (mStopRequested || mStopped || !mConnected || mSendBuffer.len == 0)
                    {
                        return AWS_OP_SUCCESS;
                    }

                    LOG_DEBUG(TAG, "Flushing send buffer");
                    aws_byte_cursor cursor = aws_byte_cursor_from_buf(&mSendBuffer);
                    int result = QueueWrite(cursor);
                    if (result == AWS_OP_SUCCESS)
                    {
                        aws_byte_buf_reset(&mSendBuffer, false);
                    }
                    return result;
                }

                int TcpForward::QueueWrite(const Crt::ByteCursor &data)
                {
                    if (data.len == 0)
                    {
                        return AWS_OP_SUCCESS;
                    }

                    unique_ptr<TcpForwardPendingWrite> pendingWrite(new TcpForwardPendingWrite());
                    pendingWrite->payload.assign(data.ptr, data.ptr + data.len);
                    pendingWrite->owner = shared_from_this();
                    aws_byte_cursor cursor =
                        aws_byte_cursor_from_array(pendingWrite->payload.data(), pendingWrite->payload.size());

                    int result = WriteSocket(cursor, sOnWriteCompleted, pendingWrite.get());
                    if (result == AWS_OP_SUCCESS)
                    {
                        pendingWrite.release();
                    }
                    return result;
                }

                void TcpForward::StopOnEventLoop(bool notifyOwner, int errorCode)
                {
                    shared_ptr<TcpForward> self;
                    OnTcpForwardTerminated onTcpForwardTerminated;
                    bool cleanUpSocket = false;
                    bool cleanUpSendBuffer = false;
                    {
                        lock_guard<mutex> lock(mLifecycleLock);
                        if (mStopped)
                        {
                            return;
                        }

                        self = mLifetimeKeepAlive;
                        cleanUpSocket = mSocketInitialized;
                        cleanUpSendBuffer = mSendBufferInitialized;
                        mSocketInitialized = false;
                        mSendBufferInitialized = false;
                        mConnected = false;
                        mStopped = true;
                        if (notifyOwner)
                        {
                            onTcpForwardTerminated = mOnTcpForwardTerminated;
                        }
                        mLifetimeKeepAlive.reset();
                    }

                    // Socket cleanup may synchronously complete pending writes. Mark the
                    // forward stopped and release the lock before invoking those callbacks.
                    if (cleanUpSocket)
                    {
                        aws_socket_clean_up(&mSocket);
                    }
                    if (cleanUpSendBuffer)
                    {
                        aws_byte_buf_clean_up(&mSendBuffer);
                    }

                    if (onTcpForwardTerminated)
                    {
                        onTcpForwardTerminated(this, errorCode);
                    }
                }

                void TcpForward::OnStopTaskCanceled()
                {
                    lock_guard<mutex> lock(mLifecycleLock);
                    LOG_ERROR(TAG, "Local TCP socket stop was canceled; retaining it for callback safety.");
                }

            } // namespace SecureTunneling
        }     // namespace DeviceClient
    }         // namespace Iot
} // namespace Aws
