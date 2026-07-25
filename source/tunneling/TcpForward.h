// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#ifndef DEVICE_CLIENT_TCPFORWARD_H
#define DEVICE_CLIENT_TCPFORWARD_H

#include "../SharedCrtResourceManager.h"
#include <aws/crt/Types.h>
#include <aws/io/socket.h>
#include <memory>
#include <mutex>

namespace Aws
{
    namespace Iot
    {
        namespace DeviceClient
        {
            namespace SecureTunneling
            {
                // Client callback
                using OnTcpForwardDataReceive = std::function<void(const Crt::ByteBuf &data)>;

                class TcpForwardTestAccess;

                /**
                 * \brief A class that represents a local TCP socket. It implements all callbacks required by using
                 * aws_socket.
                 *
                 * Instances must be managed by std::shared_ptr before Connect() or Stop() is called.
                 */
                class TcpForward : public std::enable_shared_from_this<TcpForward>
                {
                    friend class TcpForwardTestAccess;

                  public:
                    /**
                     * \brief Constructor
                     *
                     * @param sharedCrtResourceManager the shared resource manager
                     * @param port the local TCP port to connect to
                     * @param onTcpForwardDataReceive callback when there is data received from the
                     * local TCP port
                     */
                    TcpForward(
                        std::shared_ptr<SharedCrtResourceManager> sharedCrtResourceManager,
                        uint16_t port,
                        const OnTcpForwardDataReceive &onTcpForwardDataReceive);

                    /**
                     * \brief Constructor with no callback
                     */
                    TcpForward(std::shared_ptr<SharedCrtResourceManager> sharedCrtResourceManager, uint16_t port);

                    /**
                     * \brief Destructor
                     */
                    virtual ~TcpForward();

                    // Non-copyable.
                    TcpForward(const TcpForward &) = delete;
                    TcpForward &operator=(const TcpForward &) = delete;

                    /**
                     * \brief Connect to the local TCP socket
                     */
                    virtual int Connect();

                    /**
                     * \brief Stop the local TCP socket and release its resources
                     */
                    virtual void Stop();

                    /**
                     * \brief Send the given payload to the TCP socket
                     *
                     * @param data the payload to send
                     */
                    virtual int SendData(const Crt::ByteCursor &data);

                  private:
                    //
                    // static callbacks for aws_socket
                    //

                    /**
                     * \brief Callback when connection to a socket is complete
                     */
                    static void sOnConnectionResult(struct aws_socket *socket, int error_code, void *user_data);

                    /**
                     * \brief Callback when writing to the socket is complete
                     */
                    static void sOnWriteCompleted(
                        struct aws_socket *socket,
                        int error_code,
                        size_t bytes_written,
                        void *user_data);

                    /**
                     * \brief Callback when the socket has data to read
                     */
                    static void sOnReadable(struct aws_socket *socket, int error_code, void *user_data);

                    //
                    // Corresponding member callbacks for aws_socket
                    //

                    /**
                     * \brief Callback when connection to a socket is complete
                     */
                    void OnConnectionResult(struct aws_socket *socket, int error_code);

                    /**
                     * \brief Wrapper around aws_socket_read
                     */
                    virtual int ReadSocket(aws_byte_buf *buffer, size_t *amountRead);

                    /**
                     * \brief Wrapper around aws_socket_write
                     */
                    virtual int WriteSocket(
                        const aws_byte_cursor &data,
                        aws_socket_on_write_completed_fn *onWriteCompleted,
                        void *userData);

                    /**
                     * \brief Callback when the socket has data to read
                     */
                    void OnReadable(struct aws_socket *socket, int error_code);

                    /**
                     * \brief Flush any buffered data (saved before the socket is ready) to the socket
                     */
                    int FlushSendBuffer();

                    /**
                     * \brief Queue a socket write with storage that remains valid until completion
                     */
                    int QueueWrite(const Crt::ByteCursor &data);

                    /**
                     * \brief Close and clean up the socket on its owning event loop
                     */
                    void StopOnEventLoop();

                    /**
                     * \brief Preserve callback lifetime when the event loop cancels the stop task
                     */
                    void OnStopTaskCanceled();

                    //
                    // Member data
                    //

                    /**
                     * \brief Used by the logger to specify that log messages are coming from this class
                     */
                    static constexpr char TAG[] = "TcpForward.cpp";

                    /**
                     * \brief The resource manager used to manage CRT resources
                     */
                    std::shared_ptr<SharedCrtResourceManager> mSharedCrtResourceManager;

                    /**
                     * \brief The local TCP port to connect to
                     */
                    uint16_t mPort;

                    /**
                     * \brief Callback when data is received from the local TCP port
                     */
                    OnTcpForwardDataReceive mOnTcpForwardDataReceive;

                    /**
                     * \brief An AWS SDK socket object. It manages the connection to the local TCP port.
                     */
                    aws_socket mSocket{};

                    /**
                     * \brief The event loop assigned to the socket
                     */
                    aws_event_loop *mEventLoop{nullptr};

                    /**
                     * \brief Protects connection and stop state
                     */
                    std::mutex mLifecycleLock;

                    /**
                     * \brief Keeps this object alive while aws_socket can invoke raw-pointer callbacks
                     */
                    std::shared_ptr<TcpForward> mLifetimeKeepAlive;

                    /**
                     * \brief Was the socket initialized successfully?
                     */
                    bool mSocketInitialized{false};

                    /**
                     * \brief Was the pre-connection send buffer initialized successfully?
                     */
                    bool mSendBufferInitialized{false};

                    /**
                     * \brief Has a connection attempt already been submitted?
                     */
                    bool mConnectStarted{false};

                    /**
                     * \brief Is the socket connected yet?
                     */
                    bool mConnected{false};

                    /**
                     * \brief Has socket shutdown been requested?
                     */
                    bool mStopRequested{false};

                    /**
                     * \brief Has the socket completed cleanup?
                     */
                    bool mStopped{false};

                    /**
                     * \brief A buffer to store data from the secure tunnel. This is only used before the socket is
                     * connected.
                     */
                    Aws::Crt::ByteBuf mSendBuffer{};
                };
            } // namespace SecureTunneling
        }     // namespace DeviceClient
    }         // namespace Iot
} // namespace Aws

#endif // DEVICE_CLIENT_TCPFORWARD_H
