#pragma once

// IPv4 UDP multicast sockets for the replayer and consumer (Linux).
//
// On one host the replayer sends with IP_MULTICAST_LOOP on, and the kernel
// hands a copy to local group members itself. The consumer asks for the
// kernel's receive timestamp (SO_TIMESTAMPNS) and the socket's cumulative
// drop count (SO_RXQ_OVFL). Both arrive as control messages with each datagram.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "net/os.hpp"

namespace net {

struct McastAddr {
    std::string group = "239.1.1.1";
    std::uint16_t port = 30001;
    std::string iface = "0.0.0.0";  // local interface address; 0.0.0.0 lets the routing table pick
};

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { reset(); }

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int fd_ = -1;
};

namespace detail {

[[noreturn]] inline void fail(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

inline in_addr parse_ip(const std::string& s) {
    in_addr a{};
    if (::inet_pton(AF_INET, s.c_str(), &a) != 1) throw std::invalid_argument("bad IPv4 address: " + s);
    return a;
}

template <class T>
void setopt(int fd, int level, int name, const T& v, const char* what) {
    if (::setsockopt(fd, level, name, &v, sizeof v) != 0) fail(what);
}

}  // namespace detail

// Connected socket: send()/sendmsg() go to the group without an address each time.
inline Socket open_mcast_sender(const McastAddr& a, int ttl = 1) {
    Socket s(::socket(AF_INET, SOCK_DGRAM, 0));
    if (s.fd() < 0) detail::fail("socket");
    const in_addr iface = detail::parse_ip(a.iface);
    const int loop = 1;
    detail::setopt(s.fd(), IPPROTO_IP, IP_MULTICAST_IF, iface, "IP_MULTICAST_IF");
    detail::setopt(s.fd(), IPPROTO_IP, IP_MULTICAST_TTL, ttl, "IP_MULTICAST_TTL");
    detail::setopt(s.fd(), IPPROTO_IP, IP_MULTICAST_LOOP, loop, "IP_MULTICAST_LOOP");
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(a.port);
    dst.sin_addr = detail::parse_ip(a.group);
    if (::connect(s.fd(), reinterpret_cast<const sockaddr*>(&dst), sizeof dst) != 0)
        detail::fail("connect " + a.group);
    return s;
}

// Non-blocking member of the group. `granted` receives the buffer size the
// kernel actually set: it caps the request at net.core.rmem_max and reports
// double the usable size.
inline Socket open_mcast_receiver(const McastAddr& a, int rcvbuf, int* granted = nullptr) {
    Socket s(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0));
    if (s.fd() < 0) detail::fail("socket");
    const int one = 1;
    detail::setopt(s.fd(), SOL_SOCKET, SO_REUSEADDR, one, "SO_REUSEADDR");
    detail::setopt(s.fd(), SOL_SOCKET, SO_RCVBUF, rcvbuf, "SO_RCVBUF");
    detail::setopt(s.fd(), SOL_SOCKET, SO_TIMESTAMPNS, one, "SO_TIMESTAMPNS");
    detail::setopt(s.fd(), SOL_SOCKET, SO_RXQ_OVFL, one, "SO_RXQ_OVFL");
    const in_addr group = detail::parse_ip(a.group);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(a.port);
    local.sin_addr = group;  // bind to the group: only its traffic arrives here
    if (::bind(s.fd(), reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0)
        detail::fail("bind " + a.group);
    ip_mreq m{};
    m.imr_multiaddr = group;
    m.imr_interface = detail::parse_ip(a.iface);
    detail::setopt(s.fd(), IPPROTO_IP, IP_ADD_MEMBERSHIP, m, "IP_ADD_MEMBERSHIP");
    if (granted) {
        socklen_t len = sizeof *granted;
        if (::getsockopt(s.fd(), SOL_SOCKET, SO_RCVBUF, granted, &len) != 0) detail::fail("getsockopt SO_RCVBUF");
    }
    return s;
}

// One recvmmsg() batch of up to N datagrams of up to MaxLen bytes, with each
// datagram's kernel timestamp and socket drop counter.
template <std::size_t N = 64, std::size_t MaxLen = 2048>
class RecvBatch {
public:
    RecvBatch() {
        for (std::size_t i = 0; i < N; ++i) {
            iov_[i] = {bufs_[i].data(), MaxLen};
            msgs_[i].msg_hdr.msg_iov = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
            msgs_[i].msg_hdr.msg_control = ctrl_[i].b;
        }
    }

    // Datagrams received; 0 if none are ready. Throws on real errors.
    int receive(int fd) {
        for (auto& m : msgs_) m.msg_hdr.msg_controllen = kCtrl;  // the kernel overwrites it
        const int n = ::recvmmsg(fd, msgs_.data(), N, MSG_DONTWAIT, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            detail::fail("recvmmsg");
        }
        return n;
    }

    [[nodiscard]] const unsigned char* data(int i) const noexcept { return bufs_[i].data(); }
    [[nodiscard]] std::size_t size(int i) const noexcept { return msgs_[i].msg_len; }
    [[nodiscard]] bool truncated(int i) const noexcept { return msgs_[i].msg_hdr.msg_flags & MSG_TRUNC; }

    // rx_ns: kernel receive time (CLOCK_REALTIME ns), 0 if absent.
    // drops: the socket's cumulative drop count (SO_RXQ_OVFL), -1 if absent.
    // The kernel attaches that cmsg only once the count is non-zero, so -1 is
    // the normal "no drops yet" case. The count is a snapshot from when this
    // datagram was queued: drops after the last delivered datagram are not
    // visible. MoldUDP64 sequence gaps (including the end-of-session tail
    // check) are the authoritative loss signal.
    void meta(int i, std::uint64_t& rx_ns, std::int64_t& drops) {
        rx_ns = 0;
        drops = -1;
        msghdr& h = msgs_[i].msg_hdr;
        for (cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
            if (c->cmsg_level != SOL_SOCKET) continue;
            if (c->cmsg_type == SCM_TIMESTAMPNS) {
                timespec t;
                std::memcpy(&t, CMSG_DATA(c), sizeof t);
                rx_ns = to_ns(t);
            } else if (c->cmsg_type == SO_RXQ_OVFL) {
                std::uint32_t d;
                std::memcpy(&d, CMSG_DATA(c), sizeof d);
                drops = d;
            }
        }
    }

private:
    static constexpr std::size_t kCtrl = 256;
    struct alignas(cmsghdr) Ctrl {
        unsigned char b[kCtrl];
    };
    std::array<mmsghdr, N> msgs_{};
    std::array<iovec, N> iov_{};
    std::array<std::array<unsigned char, MaxLen>, N> bufs_{};
    std::array<Ctrl, N> ctrl_{};
};

}  // namespace net
