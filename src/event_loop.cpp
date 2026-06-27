# include "event_loop.h"
# include "socket.h"
# include <cerrno>
# include <sys/socket.h>

namespace {
    char g_wake_sentinel;
}
constexpr void* WAKE_MARKER = &g_wake_sentinel;

void EventLoop::register_wake_fd(int wake_read_fd) {
    wake_fd = wake_read_fd;

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = WAKE_MARKER;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &ev);
}

void EventLoop::accept_new() {
    while (true) {
        int client_fd = ::accept(listen_socket.fd(), nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)    break;
            if (errno == EINTR)                             continue;
            break;
        }

        Socket::set_nonblocking(client_fd);
        auto connection = std::make_unique<Connection>(Socket{client_fd});


        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = connection.get();
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

        connections.emplace(client_fd, std::move(connection));
    }
}

void EventLoop::close_conn(int fd) {
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    connections.erase(fd);
}

void EventLoop::run(const std::atomic<bool>& stop) {
    constexpr int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    while (!stop.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i=0; i<n; ++i) {
            void* tag = events[i].data.ptr;

            if (tag == nullptr) {
                accept_new();
                continue;
            }
            if (tag == WAKE_MARKER) {
                drain_wake_fd();
                continue;
            }
            auto* connection = static_cast<Connection*>(events[i].data.ptr);

            bool alive = true;
            if (events[i].events & EPOLLIN)
                alive = connection->on_readable();
            if (alive && (events[i].events & EPOLLOUT))
                alive = connection->on_writable();

            if (!alive) {
                close_conn(connection->fd());
                continue;
            }
            update_interest(connection);
        }
    }
}

void EventLoop::drain_wake_fd() {
    char buf[64];
    while (true) {
        ssize_t n = ::read(wake_fd, buf, sizeof(buf));
        if (n > 0) {
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

void EventLoop::update_interest(Connection* connection) const {
    bool want = connection->want_write();
    bool now = connection->is_writing();
    if (want == now) {
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | (want ? EPOLLOUT : 0);
    ev.data.ptr = connection;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, connection->fd(), &ev);
    connection->set_writing(want);
}