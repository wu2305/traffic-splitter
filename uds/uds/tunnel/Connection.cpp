#include <uds/threading/Timer.h>
#include <uds/net/Ipep.h>
#include <uds/tunnel/Connection.h>

namespace uds {
    namespace tunnel {
        Connection::Connection(const AppConfigurationPtr& configuration, int id, const ITransmissionPtr& inbound, const ITransmissionPtr& outbound) noexcept
            : Id(id)
            , disposed_(false)
            , available_(false)
            , configuration_(configuration)
            , inbound_(inbound)
            , outbound_(outbound) {
            if (configuration) {
                int alignment = configuration->Alignment;
                if (alignment >= (UINT8_MAX << 1) && alignment <= ECONNECTION_MSS) {
                    constantof(ECONNECTION_MSS) = alignment;
                }
            }
        }

        bool Connection::Listen(const AsyncTcpSocketPtr& network) noexcept {
            typedef uds::net::Ipep Ipep;

            if (disposed_ || buffers_) {
                return false;
            }

            buffers_ = make_shared_alloc<Byte>(ECONNECTION_MSS);
            if (network) {
                remote_ = network;
                available_ = EstablishRemoteSocket();
                return available_;
            }
            else {
                const AsyncContextPtr context = GetContext();
                if (IsNone() || remote_ || !context) {
                    return false;
                }

                resolver_ = make_shared_object<boost::asio::ip::tcp::resolver>(*context);
                if (!resolver_) {
                    return false;
                }
                elif(configuration_->Domain) {
                    const std::shared_ptr<Reference> references = GetReference();
                    Ipep::GetAddressByHostName(resolver_, configuration_->IP, configuration_->Port,
                        make_shared_object<Ipep::GetAddressByHostNameCallback>(
                            [references, this](IPEndPoint* ep) noexcept {
                                boost::asio::ip::tcp::endpoint remoteEP;
                                if (ep) {
                                    remoteEP = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(*ep);
                                }
                                ConnectRemoteSocket(remoteEP);
                            }));
                    return true;
                }
                else {
                    boost::asio::ip::tcp::endpoint remoteEP = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(IPEndPoint(configuration_->IP.data(), configuration_->Port));
                    return ConnectRemoteSocket(remoteEP);
                }
            }
        }

        bool Connection::EstablishRemoteSocket() noexcept {
            bool available = InboundSocketToRemoteSocket() && RemoteSocketToOutboundSocket();
            if (available) {
                if (configuration_->KeepAlived) {
                    available = KeepAlivedReadCycle(outbound_) && KeepAlivedSendCycle(inbound_);
                }
            }
            return available;
        }

        bool Connection::ConnectRemoteSocket(boost::asio::ip::tcp::endpoint remoteEP) noexcept {
            const AsyncTcpSocketPtr socket = NewRemoteSocket(configuration_, GetContext(), remoteEP);
            if (!socket) {
                return false;
            }

            const std::shared_ptr<Reference> references = GetReference();
            socket->async_connect(remoteEP,
                [references, this](const boost::system::error_code& ec) noexcept {
                    if (!ec) {
                        available_ = EstablishRemoteSocket();
                    }

                    if (!available_) {
                        Close();
                    }
                });
            remote_ = std::move(socket);
            return true;
        }

        Connection::AsyncContextPtr Connection::GetContext() noexcept {
            const ITransmissionPtr inbound = inbound_;
            if (inbound) {
                return inbound->GetContext();
            }

            const ITransmissionPtr outbound = outbound_;
            if (outbound) {
                return outbound->GetContext();
            }
            return NULL;
        }

        Connection::AsyncTcpSocketPtr Connection::NewRemoteSocket(const AppConfigurationPtr& configuration, const AsyncContextPtr& context) noexcept {
            if (!configuration) {
                return NULL;
            }

            boost::system::error_code ec;
            boost::asio::ip::address address = boost::asio::ip::address::from_string(configuration->Inbound.IP, ec);
            if (ec || address.is_unspecified() || address.is_multicast()) {
                return NULL;
            }

            boost::asio::ip::tcp::endpoint remoteEP = boost::asio::ip::tcp::endpoint(address, configuration->Inbound.Port);
            return NewRemoteSocket(configuration, context, remoteEP);
        }

        Connection::AsyncTcpSocketPtr Connection::NewRemoteSocket(const AppConfigurationPtr& configuration, const AsyncContextPtr& context, const boost::asio::ip::tcp::endpoint& remoteEP) noexcept {
            if (!context || !configuration) {
                return NULL;
            }

            boost::asio::ip::address address = remoteEP.address();
            if (address.is_unspecified() || address.is_multicast()) {
                return NULL;
            }

            int port = remoteEP.port();
            if (port <= IPEndPoint::MinPort || port > IPEndPoint::MaxPort) {
                return NULL;
            }

            AsyncTcpSocketPtr socket = make_shared_object<boost::asio::ip::tcp::socket>(*context);
            if (!socket) {
                return NULL;
            }

            boost::system::error_code ec;
            if (address.is_v4()) {
                socket->open(boost::asio::ip::tcp::v4(), ec);
            }
            else {
                socket->open(boost::asio::ip::tcp::v6(), ec);
            }

            if (ec) {
                return NULL;
            }

            int handle = socket->native_handle();
            Socket::AdjustDefaultSocketOptional(handle, false);
            Socket::SetTypeOfService(handle);
            Socket::SetSignalPipeline(handle, false);
            Socket::SetDontFragment(handle, false);

            socket->set_option(boost::asio::ip::tcp::no_delay(configuration->Turbo), ec);
            socket->set_option(boost::asio::detail::socket_option::boolean<IPPROTO_TCP, TCP_FASTOPEN>(configuration->FastOpen), ec);
            return std::move(socket);
        }

        bool Connection::IsNone() noexcept {
            if (disposed_ || !inbound_ || !outbound_ || !configuration_) {
                return true;
            }
            else {
                return false;
            }
        }

        bool Connection::IsDisposed() noexcept {
            return IsNone() || !remote_ || !buffers_;
        }

        bool Connection::Available() noexcept {
            return available_ && !IsDisposed();
        }

        void Connection::Close() noexcept {
            Dispose();
        }

        void Connection::Dispose() noexcept {
            if (!disposed_.exchange(true)) {
                const ITransmissionPtr inbound = std::move(inbound_);
                if (inbound) {
                    inbound->Close();
                }

                const ITransmissionPtr outbound = std::move(outbound_);
                if (outbound) {
                    outbound->Close();
                }

                const AsyncTcpSocketPtr remote = std::move(remote_);
                if (remote) {
                    Socket::Closesocket(*remote);
                }

                const std::shared_ptr<boost::asio::ip::tcp::resolver> resolver = std::move(resolver_);
                if (resolver) {
                    try {
                        resolver->cancel();
                    }
                    catch (std::exception&) {}
                }

                buffers_.reset();
                remote_.reset();
                inbound_.reset();
                outbound_.reset();
                resolver_.reset();
                uds::threading::ClearTimeout(timeout_);

                DisposedEventHandler disposedEvent = std::move(DisposedEvent);
                if (disposedEvent) {
                    DisposedEvent = NULL;
                    disposedEvent(this);
                }
            }
        }

        bool Connection::RemoteSocketToOutboundSocket() noexcept {
            if (disposed_) {
                return false;
            }

            const AsyncTcpSocketPtr socket = remote_;
            if (!socket) {
                return false;
            }

            const std::shared_ptr<Byte> buffers = buffers_;
            if (!buffers) {
                return false;
            }

            const std::shared_ptr<Reference> references = GetReference();
            socket->async_read_some(boost::asio::buffer(buffers.get(), ECONNECTION_MSS),
                [references, this, socket, buffers](const boost::system::error_code& ec, size_t sz) noexcept {
                    int length = std::max<int>(ec ? -1 : sz, -1);
                    if (!SendToOutboundSocket(buffers.get(), length)) {
                        Close();
                    }
                });
            return true;
        }

        bool Connection::InboundSocketToRemoteSocket() noexcept {
            if (disposed_) {
                return false;
            }

            const ITransmissionPtr socket = inbound_;
            if (!socket) {
                return false;
            }

            const std::shared_ptr<Reference> references = GetReference();
            return socket->ReadAsync(
                [references, this, socket](const std::shared_ptr<Byte>& buffers, int length) noexcept {
                    if (!SendToRemoteSocket(buffers.get(), length)) {
                        Close();
                    }
                });
        }

        bool Connection::SendToRemoteSocket(const void* buffer, int length) noexcept {
            if (disposed_ || !buffer || length < 1) {
                return false;
            }

            const AsyncTcpSocketPtr socket = remote_;
            if (!socket) {
                return false;
            }

            const std::shared_ptr<Reference> references = GetReference();
            boost::asio::async_write(*socket, boost::asio::buffer(buffer, length),
                [references, this, socket](const boost::system::error_code& ec, size_t sz) noexcept {
                    int length = std::max<int>(ec ? -1 : sz, -1);
                    if (length < 1 || !InboundSocketToRemoteSocket()) {
                        Close();
                    }
                });
            return true;
        }

        bool Connection::SendToOutboundSocket(const void* buffer, int length) noexcept {
            if (disposed_ || !buffer || length < 1) {
                return false;
            }

            const ITransmissionPtr socket = outbound_;
            if (!socket) {
                return false;
            }

            const std::shared_ptr<Reference> references = GetReference();
            return socket->WriteAsync(std::shared_ptr<Byte>((Byte*)buffer, [](const void*) noexcept {}), 0, length,
                [references, this, socket](bool success) noexcept {
                    if (!success || !RemoteSocketToOutboundSocket()) {
                        Close();
                    }
                });
        }

        bool Connection::HandshakeServer(const ITransmissionPtr& transmission, int alignment, int channelId, AcceptAsyncCallback&& handler) noexcept {
            if (!transmission || !handler || alignment < (UINT8_MAX << 1) || !channelId) {
                return false;
            }

            MemoryStream messages;
            if (!PackPlaintextHeaders(messages, channelId, alignment)) {
                return false;
            }

            const AcceptAsyncCallback callback = std::move(handler);
            const ITransmissionPtr ctransmission = transmission;
            return ctransmission->WriteAsync(messages.GetBuffer(), 0, messages.GetPosition(),
                [ctransmission, callback, channelId](bool success) noexcept {
                    callback(success, channelId);
                });;
        }

        bool Connection::HandshakeClient(const ITransmissionPtr& transmission, ConnectAsyncCallback&& handler) noexcept {
            if (!transmission || !handler) {
                return false;
            }

            const ConnectAsyncCallback callback = std::move(handler);
            const ITransmissionPtr ctransmission = transmission;
            return ctransmission->ReadAsync(
                [ctransmission, callback](const std::shared_ptr<Byte>& buffer, int length) noexcept {
                    if (!buffer || length < 1) {
                        callback(false, 0);
                        return;
                    }

                    Int64 v = UnpackPlaintextLength(buffer.get(), 0, length);
                    if (!v) {
                        callback(false, 0);
                        return;
                    }

                    int messages_size = (int)(v);
                    if (messages_size != length) {
                        callback(false, 0);
                        return;
                    }

                    int channelId = (int)(v >> 32);
                    if (!channelId) {
                        callback(false, 0);
                        return;
                    }
                    callback(true, channelId);
                });
        }

        bool Connection::AcceptAsync(const ITransmissionPtr& inbound, int alignment, AcceptAsyncMeasureChannelId&& measure, AcceptAsyncCallback&& handler) noexcept {
            if (!inbound || !handler || !measure || alignment < (1 << 9)) {
                return false;
            }

            int channelId = measure(inbound);
            if (!channelId) {
                return false;
            }

            return HandshakeServer(inbound, alignment, channelId, std::forward<AcceptAsyncCallback>(handler));
        }

        bool Connection::AcceptAsync(const ITransmissionPtr& outbound, AcceptAsyncCallback&& handler) noexcept {
            return HandshakeClient(outbound, std::forward<AcceptAsyncCallback>(handler));
        }

        bool Connection::ConnectAsync(const ITransmissionPtr& outbound, int alignment, int channelId, ConnectAsyncCallback&& handler) noexcept {
            return HandshakeServer(outbound, alignment, channelId, std::forward<ConnectAsyncCallback>(handler));
        }

        bool Connection::ConnectAsync(const ITransmissionPtr& inbound, ConnectAsyncCallback&& handler) noexcept {
            return HandshakeClient(inbound, std::forward<ConnectAsyncCallback>(handler));
        }

        bool Connection::HelloAsync(const ITransmissionPtr& outbound) noexcept {
            if (!outbound) {
                return false;
            }

            ITransmissionPtr transmission = outbound;
            return HandshakeServer(outbound, UINT8_MAX << 1, RandomNext(1, INT_MAX),
                [transmission](bool success, int) noexcept {
                    if (!success) {
                        transmission->Close();
                    }
                });
        }

        bool Connection::HelloAsync(const ITransmissionPtr& inbound, HelloAsyncCallback&& handler) noexcept {
            if (!inbound || !handler) {
                return false;
            }

            HelloAsyncCallback callback = handler;
            ITransmissionPtr transmission = inbound;

            return HandshakeClient(inbound,
                [transmission, callback](bool success, int) noexcept {
                    if (!success) {
                        transmission->Close();
                    }
                    callback(success);
                });
        }

        bool Connection::PackPlaintextHeaders(Stream& stream, int channelId, int alignment) noexcept {
            if (!stream.CanWrite()) {
                return false;
            }

            Char messages[uds::threading::Hosting::BufferSize];
            int messages_size = RandomNext(UINT8_MAX << 1, std::min<int>(alignment, sizeof(messages)));
            for (int i = 0; i < messages_size; i++) {
                messages[i] = RandomAscii();
            }

            int offset = sprintf((char*)messages + 1, "%04X%08X", messages_size, channelId ^ (messages_size << 16 | messages_size));
            if (offset < 1) {
                return false;
            }

            for (int i = 0; i < offset; i++) {
                Char& ch = messages[1 + i];
                ch = RandomNext(0, 1) ? tolower(ch) : toupper(ch);
            }
            messages[1 + offset] = RandomAscii();
            return stream.Write(messages, 0, messages_size);
        }

        Int64 Connection::UnpackPlaintextLength(const void* buffer, int offset, int length) noexcept {
            if (!buffer || offset < 0 || length < 1) {
                return 0;
            }

            char* data = ((char*)buffer) + offset;
            if (13 > length) {
                return 0;
            }

            char len[4];
            memcpy(len, data + 1, sizeof(len));

            Int64 messages_size = strtoll(len, NULL, 16);
            if (13 >= messages_size) {
                return 0;
            }

            char id[8];
            memcpy(id, data + 5, sizeof(id));

            Int64 channelid = strtoll(id, NULL, 16);
            channelid ^= messages_size << 16 | messages_size;

            return channelid << 32 | messages_size;
        }

        bool Connection::KeepAlivedReadCycle(const ITransmissionPtr& transmission) noexcept {
            if (disposed_ || !transmission) {
                return false;
            }

            const std::shared_ptr<Reference> references = GetReference();
            const ITransmissionPtr network = transmission;
            return network->ReadAsync(
                [network, references, this](const std::shared_ptr<Byte>& buffers, int length) noexcept {
                    if (length < 1) {
                        Close();
                    }
                    else {
                        KeepAlivedReadCycle(network);
                    }
                });
        }

        bool Connection::KeepAlivedSendCycle(const ITransmissionPtr& transmission) noexcept {
            if (disposed_ || !transmission) {
                return false;
            }

            const AsyncContextPtr context = GetContext();
            if (!context) {
                return false;
            }

            const std::shared_ptr<Reference> references = GetReference();
            const ITransmissionPtr network = transmission;
            if (timeout_) {
                uds::threading::ClearTimeout(timeout_);
            }

            timeout_ = uds::threading::SetTimeout(context,
                [references, this, network](void*) noexcept {
                    if (timeout_) {
                        uds::threading::ClearTimeout(timeout_);
                    }

                    std::shared_ptr<Byte> messages = make_shared_alloc<Byte>(64);
                    if (!messages) {
                        Close();
                        return false;
                    }

                    Byte* packet = messages.get();
                    int packet_size = RandomNext(8, 64);
                    for (int i = 0; i < packet_size; i++) {
                        packet[i] = RandomAscii();
                    }

                    if (!network->WriteAsync(messages, 0, packet_size,
                        [references, this, network](bool success) noexcept {
                            if (success) {
                                success = KeepAlivedSendCycle(network);
                            }

                            if (!success) {
                                Close();
                            }
                        })) {
                        Close();
                        return false;
                    }
                    return true;
                }, RandomNext(100, 500));
            return NULL != timeout_;
        }
    }
}