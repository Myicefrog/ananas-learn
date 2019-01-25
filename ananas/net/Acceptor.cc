#include <errno.h>
#include <cassert>
#include "EventLoop.h"
#include "Application.h"
#include "Connection.h"
#include "Acceptor.h"

#include "AnanasDebug.h"


namespace ananas {
namespace internal {

const int Acceptor::kListenQueue = 1024;

Acceptor::Acceptor(EventLoop* loop) :
    localSock_(kInvalid),
    localPort_(SocketAddr::kInvalidPort),
    loop_(loop) {
}

Acceptor::~Acceptor() {
    CloseSocket(localSock_);
    ANANAS_INF << "Close Acceptor " << localPort_ ;
}


void Acceptor::SetNewConnCallback(NewTcpConnCallback cb) {
    newConnCallback_ = std::move(cb);
}

bool Acceptor::Bind(const SocketAddr& addr) {
    if (!addr.IsValid())
        return false;

    if (localSock_ != kInvalid) {
        ANANAS_ERR << "Already listen " << localPort_;
        return false;
    }

    localSock_ = CreateTCPSocket();
    if (localSock_ == kInvalid)
        return false;

    localPort_ = addr.GetPort();

    SetNonBlock(localSock_);
    SetNodelay(localSock_);
    SetReuseAddr(localSock_);
    SetRcvBuf(localSock_);
    SetSndBuf(localSock_);

    auto serv = addr.GetAddr();

    int ret = ::bind(localSock_, (struct sockaddr*)&serv, sizeof serv);
    if (kError == ret) {
        ANANAS_ERR << "Cannot bind to " << addr.ToString();
        return false;
    }

    ret = ::listen(localSock_, kListenQueue);
    if (kError == ret) {
        ANANAS_ERR << "Cannot listen on " << addr.ToString();
        return false;
    }

    if (!loop_->Register(eET_Read, this->shared_from_this()))
        return false;

    ANANAS_INF << "Create listen socket " << localSock_
               << " on port " << localPort_;
    return  true;
}

int Acceptor::Identifier() const {
    return localSock_;
}

bool Acceptor::HandleReadEvent() {
    while (true) {
        int connfd = _Accept();
        if (connfd != kInvalid) {
            auto loop = Application::Instance().Next();
            auto func = [loop, newCb = newConnCallback_, connfd, peer = peer_]() {
                auto conn(std::make_shared<Connection>(loop));
                conn->Init(connfd, peer);
                if (loop->Register(eET_Read, conn)) {
                    newCb(conn.get());
                    conn->_OnConnect();
                } else {
                    ANANAS_ERR << "Failed to register socket " << conn->Identifier();
                }
            };
            loop->Execute(std::move(func));
        } else {
            bool goAhead = false;
            const int error = errno;
            switch (error) {
            //case EWOULDBLOCK:
            case EAGAIN:
                return true; // it's fine

            case EINTR:
            case ECONNABORTED:
            case EPROTO:
                goAhead = true; // should retry
                break;

            case EMFILE:
            case ENFILE:
                ANANAS_ERR << "Not enough file descriptor available, error is "
                           << error
                           << ", CPU may 100%";
                return true;

            case ENOBUFS:
            case ENOMEM:
                ANANAS_ERR << "Not enough memory, limited by the socket buffer limits"
                           << ", CPU may 100%";
                return true;

            case ENOTSOCK:
            case EOPNOTSUPP:
            case EINVAL:
            case EFAULT:
            case EBADF:
            default:
                ANANAS_ERR << "BUG: error = " << error;
                assert (false);
                break;
            }

            if (!goAhead)
                return false;
        }
    }

    return true;
}

bool Acceptor::HandleWriteEvent() {
    assert (false);
    return false;
}

void Acceptor::HandleErrorEvent() {
    ANANAS_ERR << "Acceptor::HandleErrorEvent";
    loop_->Unregister(eET_Read, shared_from_this());
}

int Acceptor::_Accept() {
    socklen_t addrLength = sizeof peer_;
    return ::accept(localSock_, (struct sockaddr *)&peer_, &addrLength);
}

} // end namespace internal
} // end namespace ananas

