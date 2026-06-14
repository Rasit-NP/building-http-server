# include <atomic>
# include <cerrno>
# include <sys/epoll.h>
# include <sys/socket.h>
# include <csignal>
# include <cstdio>
# include <socket.h>
# include <event_loop.h>
# include <stdexcept>

constexpr int MAX_EVENTS = 64;
std::atomic<bool> g_should_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void handle_signal(int) {
    g_should_stop = 1;
}

int main(){

    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Socket socket = Socket();
    socket.bind(8080);
    socket.listen();
    Socket::set_nonblocking(socket.fd());

    int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 0;
    }

    epoll_event lev{};
    lev.events = EPOLLIN;
    lev.data.ptr = nullptr;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket.fd(), &lev) < 0) {
        perror("epoll_ctl: listen");
        return 0;
    }

    EventLoop event_loop = EventLoop(std::move(socket), epoll_fd);

    event_loop.run(g_should_stop);
}