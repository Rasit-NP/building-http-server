# include "event_loop.h"
# include "socket.h"
# include <cerrno>
# include <sys/socket.h>

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
            if (events[i].data.ptr == nullptr) {
                accept_new();
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