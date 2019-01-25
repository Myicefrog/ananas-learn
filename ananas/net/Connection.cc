#include <cassert>

#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>

#include "EventLoop.h"
#include "Connection.h"
#include "AnanasDebug.h"
#include "util/Util.h"

namespace ananas {

using internal::eET_Read;
using internal::eET_Write;

Connection::Connection(EventLoop* loop) :
    loop_(loop),
    localSock_(kInvalid),
    minPacketSize_(1),
    sendBufHighWater_(10 * 1024 * 1024) {
}

Connection::~Connection() {
    if (localSock_ != kInvalid) {
        Shutdown(ShutdownMode::eSM_Both); // Force send FIN
        CloseSocket(localSock_);
    }
}

bool Connection::Init(int fd, const SocketAddr& peer) {
    if (fd == kInvalid)
        return false;

    localSock_ = fd;
    SetNonBlock(localSock_);

    peer_ = peer;

    assert (state_ == State::eS_None);
    state_ = State::eS_Connected;
    return true;
}

void Connection::ActiveClose() {
    if (localSock_ == kInvalid)
        return;

    state_ = State::eS_ActiveClose;
    loop_->Modify(eET_Write, shared_from_this());
}

void Connection::Shutdown(ShutdownMode mode) {
    switch (mode) {
    case ShutdownMode::eSM_Read:
        ::shutdown(localSock_, SHUT_RD);
        break;

    case ShutdownMode::eSM_Write:
        if (!sendBuf_.Empty()) {
            ANANAS_WRN << localSock_ << " shutdown write, but still has data to send";
            sendBuf_.Clear();
        }

        ::shutdown(localSock_, SHUT_WR);
        break;

    case ShutdownMode::eSM_Both:
        if (!sendBuf_.Empty()) {
            ANANAS_WRN << localSock_ << " shutdown both, but still has data to send";
            sendBuf_.Clear();
        }

        ::shutdown(localSock_, SHUT_RDWR);
        break;
    }
}

void Connection::SetNodelay(bool enable) {
    ananas::SetNodelay(localSock_, enable);
}

int Connection::Identifier() const {
    return localSock_;
}

bool Connection::HandleReadEvent() {
    if (state_ != State::eS_Connected) {
        ANANAS_ERR << localSock_ << "[fd] HandleReadEvent error state:" << state_;
        return false;
    }

    processingRead_ = true;
    ANANAS_DEFER {
        processingRead_ = false;
        if (!batchSendBuf_.IsEmpty()) {
            SendPacket(batchSendBuf_);
            batchSendBuf_.Clear();
        }
    };

    bool busy = false;
    while (true) {
        recvBuf_.AssureSpace(8 * 1024);
        char stack[128 * 1024];

        struct iovec vecs[2];
        vecs[0].iov_base = recvBuf_.WriteAddr();
        vecs[0].iov_len = recvBuf_.WritableSize();
        vecs[1].iov_base = stack;
        vecs[1].iov_len = sizeof stack;

        int bytes = ::readv(localSock_, vecs, 2);
        if (bytes == kError) {
            if (EAGAIN == errno || EWOULDBLOCK == errno)
                return true;

            if (EINTR == errno)
                continue; // restart ::readv
        }

        if (bytes == 0) {
            ANANAS_WRN << localSock_ << " HandleReadEvent EOF ";
            if (sendBuf_.Empty()) {
                state_ = State::eS_PassiveClose;
            } else {
                state_ = State::eS_CloseWaitWrite;
                loop_->Modify(eET_Write, shared_from_this()); // disable read
            }

            return false;
        }

        if (bytes < 0) {
            ANANAS_ERR << localSock_ << " HandleReadEvent Error";
            state_ = State::eS_Error;
            return false;
        }

        if (static_cast<size_t>(bytes) <= vecs[0].iov_len) {
            recvBuf_.Produce(static_cast<size_t>(bytes));
        } else {
            recvBuf_.Produce(vecs[0].iov_len);

            auto stackBytes = static_cast<size_t>(bytes) - vecs[0].iov_len;
            recvBuf_.PushData(stack, stackBytes);
        }

        while (recvBuf_.ReadableSize() >= minPacketSize_) {
            auto bytes = onMessage_(this,
                                    recvBuf_.ReadAddr(),
                                    recvBuf_.ReadableSize());

            if (bytes == 0) {
                break;
            } else {
                recvBuf_.Consume(bytes);
                busy = true;
            }
        }
    }

    if (busy)
        recvBuf_.Shrink();

    return true;
}


int Connection::_Send(const void* data, size_t len) {
    if (len == 0)
        return 0;

    int bytes = ::send(localSock_, data, len, 0);
    if (kError == bytes) {
        if (EAGAIN == errno || EWOULDBLOCK == errno)
            bytes = 0;

        if (EINTR == errno)
            bytes = 0; // later try ::send
    }

    if (kError == bytes) {
        ANANAS_ERR << localSock_ << " _Send Error";
        state_ = State::eS_Error;
    }

    return bytes;
}

namespace {
int WriteV(int , const std::vector<iovec>& );
void ConsumeBufferVectors(BufferVector& , size_t );
void CollectBuffer(const std::vector<iovec>& , size_t , BufferVector& );
}

bool Connection::HandleWriteEvent() {

    if (state_ != State::eS_Connected &&
        state_ != State::eS_CloseWaitWrite) {
        ANANAS_ERR << localSock_ << " HandleWriteEvent wrong state " << state_;
        return false;
    }

    // it's connected or half-close, whatever, we can send.

    size_t expectSend = 0;
    std::vector<iovec> iovecs;
    for (auto& e : sendBuf_) {
        assert (!e.IsEmpty());

        iovec ivc;
        ivc.iov_base = (void*)(e.ReadAddr());
        ivc.iov_len = e.ReadableSize();

        iovecs.push_back(ivc);
        expectSend += e.ReadableSize();
    }

    int ret = WriteV(localSock_, iovecs);
    if (ret == kError) {
        ANANAS_ERR << localSock_ << " HandleWriteEvent ERROR ";
        state_ = State::eS_Error;
        return false;
    }

    assert (ret >= 0);

    size_t alreadySent = static_cast<size_t>(ret);
    ConsumeBufferVectors(sendBuf_, alreadySent);

    if (alreadySent == expectSend) {
        loop_->Modify(eET_Read, shared_from_this());

        if (onWriteComplete_)
            onWriteComplete_(this);

        if (state_ == State::eS_CloseWaitWrite) {
            state_ = State::eS_PassiveClose;
            return false;
        }
    }

    return true;
}

void  Connection::HandleErrorEvent() {
    ANANAS_ERR << localSock_ << " HandleErrorEvent " << state_;

    switch (state_) {
    case State::eS_PassiveClose:
    case State::eS_ActiveClose:
    case State::eS_Error:
        break;

    case State::eS_None:
    case State::eS_Connected: // should not happen
    case State::eS_CloseWaitWrite:
    case State::eS_Closed: // should not happen
    default:
        return;
    }

    state_ = State::eS_Closed;

    if (onDisconnect_)
        onDisconnect_(this);

    if (onConnFail_)
        onConnFail_(loop_, peer_);

    loop_->Unregister(eET_Read | eET_Write, shared_from_this());
}

bool Connection::SafeSend(const void* data, std::size_t size) {
    if (loop_->InThisLoop())
        return this->SendPacket(data, size);
    else
        return SafeSend(std::string((const char*)data, size));
}

bool Connection::SafeSend(const std::string& data) {
    if (loop_->InThisLoop())
        return this->SendPacket(data);
    else
        loop_->Execute([this, data]() {
                          this->SendPacket(data);
                       });

    return true;
}

bool Connection::SendPacket(const void* data, std::size_t size) {
    assert (loop_->InThisLoop());

    if (size == 0)
        return true;

    if (state_ != State::eS_Connected &&
        state_ != State::eS_CloseWaitWrite)
        return false;

    const size_t oldSendBytes = sendBuf_.TotalBytes();
    ANANAS_DEFER {
        size_t nowSendBytes = sendBuf_.TotalBytes();
        if (oldSendBytes < sendBufHighWater_ &&
            nowSendBytes >= sendBufHighWater_) {
            if (onWriteHighWater)
                onWriteHighWater(this, nowSendBytes);
        }
    };

    if (oldSendBytes > 0) {
        sendBuf_.Push(data, size);
        return true;
    }

    if (processingRead_ && batchSend_) {
        batchSendBuf_.PushData(data, size);
        return true;
    }

    auto bytes = _Send(data, size);
    if (bytes == kError) {
        state_ = State::eS_Error;
        loop_->Modify(eET_Write, shared_from_this());
        return false;
    }

    if (bytes < static_cast<int>(size)) {
        ANANAS_WRN << localSock_
                   << " want send "
                   << size
                   << " bytes, but only send "
                   << bytes;
        sendBuf_.Push((char*)data + bytes, size - static_cast<std::size_t>(bytes));
        loop_->Modify(eET_Read | eET_Write, shared_from_this());
    } else {
        if (onWriteComplete_)
            onWriteComplete_(this);
    }

    return true;
}

bool Connection::SendPacket(const std::string& data) {
    return SendPacket(data.data(), data.size());
}

bool Connection::SendPacket(const Buffer& data) {
    return SendPacket(const_cast<Buffer&>(data).ReadAddr(), data.ReadableSize());
}

// iovec for writev
namespace {

int WriteV(int sock, const std::vector<iovec>& buffers) {
    const int kIOVecCount = 64; // be care of IOV_MAX

    size_t sentVecs = 0;
    size_t sentBytes = 0;
    while (sentVecs < buffers.size()) {
        const int vc = std::min<int>(buffers.size() - sentVecs, kIOVecCount);

        int expectBytes = 0;
        for (size_t i = sentVecs; i < sentVecs + vc; ++ i) {
            expectBytes += static_cast<int>(buffers[i].iov_len);
        }

        assert (expectBytes > 0);
        int bytes = static_cast<int>(::writev(sock, &buffers[sentVecs], vc));
        assert (bytes != 0);

        if (kError == bytes) {
            assert (errno != EINVAL);

            if (EAGAIN == errno || EWOULDBLOCK == errno)
                return static_cast<int>(sentBytes);

            if (EINTR == errno)
                continue; // retry

            return kError;  // can not send any more
        } else {
            assert (bytes > 0);
            sentBytes += bytes;
            if (bytes == expectBytes)
                sentVecs += vc;
            else
                return sentBytes;
        }
    }

    return sentBytes;
}

void ConsumeBufferVectors(BufferVector& buffers, size_t toSkippedBytes) {
    size_t skippedVecs = 0;
    for (auto& e : buffers) {
        assert (e.ReadableSize() > 0);

        if (toSkippedBytes >= e.ReadableSize()) {
            toSkippedBytes -= e.ReadableSize();
            ++ skippedVecs;
        } else {
            if (toSkippedBytes > 0) {
                e.Consume(toSkippedBytes);
                buffers.totalBytes -= toSkippedBytes;
            }

            break;
        }
    }

    while (skippedVecs-- > 0)
        buffers.Pop();
}

void CollectBuffer(const std::vector<iovec>& buffers, size_t skipped, BufferVector& dst) {
    for (auto e : buffers) {
        if (skipped >= e.iov_len) {
            skipped -= e.iov_len;
        } else {
            dst.Push((char*)e.iov_base + skipped, e.iov_len - skipped);

            if (skipped != 0)
                skipped = 0;
        }
    }
}

} // end namespace

bool Connection::SendPacket(const BufferVector& data) {
    if (state_ != State::eS_Connected &&
            state_ != State::eS_CloseWaitWrite)
        return false;

    SliceVector s;
    for (const auto& d : data) {
        s.PushBack(const_cast<Buffer&>(d).ReadAddr(), d.ReadableSize());
    }

    return SendPacket(s);
}

bool Connection::SendPacket(const SliceVector& slices) {
    if (slices.Empty())
        return true;

    const size_t oldSendBytes = sendBuf_.TotalBytes();
    ANANAS_DEFER {
        size_t nowSendBytes = sendBuf_.TotalBytes();
        if (oldSendBytes < sendBufHighWater_ &&
            nowSendBytes >= sendBufHighWater_) {
            if (onWriteHighWater)
                onWriteHighWater(this, nowSendBytes);
        }
    };

    if (oldSendBytes > 0) {
        for (const auto& e : slices) {
            sendBuf_.Push(e.data, e.len);
        }

        return true;
    }

    if (processingRead_ && batchSend_) {
        for (const auto& e : slices) {
            batchSendBuf_.PushData(e.data, e.len);
        }

        return true;
    }

    size_t expectSend = 0;
    std::vector<iovec> iovecs;
    for (const auto& e : slices) {
        if (e.len == 0)
            continue;

        iovec ivc;
        ivc.iov_base = const_cast<void*>(e.data);
        ivc.iov_len = e.len;

        iovecs.push_back(ivc);
        expectSend += e.len;
    }

    int ret = WriteV(localSock_, iovecs);
    if (ret == kError) {
        state_ = State::eS_Error;
        loop_->Modify(eET_Write, shared_from_this());
        return false;
    }

    assert (ret >= 0);

    size_t alreadySent = static_cast<size_t>(ret);
    if (alreadySent < expectSend) {
        CollectBuffer(iovecs, alreadySent, sendBuf_);
        loop_->Modify(eET_Read | eET_Write, shared_from_this());
    } else {
        if (onWriteComplete_)
            onWriteComplete_(this);
    }

    return true;
}

void Connection::SetBatchSend(bool batch) {
    batchSend_ = batch;
}

void Connection::SetOnConnect(std::function<void (Connection* )> cb) {
    onConnect_ = std::move(cb);
}

void Connection::SetOnDisconnect(std::function<void (Connection* )> cb) {
    onDisconnect_ = std::move(cb);
}

void Connection::SetOnMessage(TcpMessageCallback cb) {
    onMessage_ = std::move(cb);
}

void Connection::_OnConnect() {
    if (state_ != State::eS_Connected)
        return;

    if (onConnect_)
        onConnect_(this);
}

void Connection::SetFailCallback(TcpConnFailCallback cb) {
    onConnFail_ = std::move(cb);
}

void Connection::SetOnWriteComplete(TcpWriteCompleteCallback wccb) {
    onWriteComplete_ = std::move(wccb);
}

void Connection::SetMinPacketSize(size_t s) {
    minPacketSize_ = s;
}

void Connection::SetWriteHighWater(size_t s) {
    sendBufHighWater_ = s;
}

void Connection::SetOnWriteHighWater(TcpWriteHighWaterCallback whwcb) {
    onWriteHighWater = std::move(whwcb);
}

void Connection::SetUserData(std::shared_ptr<void> user) {
    userData_ = std::move(user);
}

size_t Connection::GetMinPacketSize() const {
    return minPacketSize_;
}

} // end namespace ananas

