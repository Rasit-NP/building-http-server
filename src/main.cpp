# include <atomic>
# include <cerrno>
# include <unordered_map>
# include <sys/epoll.h>
# include <sys/socket.h>
# include <csignal>
# include <cstdlib>
# include <cstdio>
# include <socket.h>
# include <stdexcept>
#include <unistd.h>

constexpr int MAX_EVENTS = 64;
std::atomic<bool> g_should_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void handle_signal(int) {
    g_should_stop = 1;
}

// void handle_client(Socket& client) {
//     char buf[1024];
//     while (true) {
//         ssize_t n = client.read(buf, sizeof(buf));
//         if (n > 0) {
//             client.write(buf, n);
//             continue;
//         }
//         if (n == 0) break;
//         if (errno == EAGAIN || errno == EWOULDBLOCK)    continue;
//         if (errno == EINTR)                             continue;
//         perror("read");
//         break;
//     }
// }

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
    socket.set_nonblocking();

    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        return 0;
    }
    Socket epoll_socket = Socket(epfd);

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = socket.fd();
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, socket.fd(), &ev) < 0) {
        perror("epoll_ctl: listen");
        return 0;
    }

    struct epoll_event events[MAX_EVENTS];
    std::unordered_map<int, Socket> clients;

    while (!g_should_stop.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i=0; i<n; i++) {
            int fd = events[i].data.fd;

            if (fd == socket.fd()) {
                std::optional<Socket> client = socket.accept();

                if (client == std::nullopt) {
                    continue;
                }
                client->set_nonblocking();

                struct epoll_event cev{};
                cev.events = EPOLLIN;
                cev.data.fd = client->fd();
                ::epoll_ctl(epfd, EPOLL_CTL_ADD, client->fd(), &cev);

                clients.emplace(client->fd(), std::move(*client));
            }
            else {
                Socket& client = clients.at(fd);
                char buf[1024];
                ssize_t r = client.read(buf, sizeof(buf));
                if (r > 0) {
                    client.write(buf, r);
                }
                else if (r == 0) {
                    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    clients.erase(fd);
                }
                else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)    continue;
                    if (errno == EINTR) continue;
                    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    clients.erase(fd);
                }
            }
        }
    }
}