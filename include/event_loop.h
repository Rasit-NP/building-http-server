#ifndef HTTP_SERVER_EVENT_LOOP_H
#define HTTP_SERVER_EVENT_LOOP_H

# include <sys/epoll.h>
# include <unordered_map>
# include <memory>
# include <atomic>
# include <unistd.h>
# include "socket.h"
# include "connection.h"

class EventLoop {
private:
    void accept_new();
    void close_conn(int fd);

    Socket  listen_socket;
    int     epoll_fd;
    std::unordered_map<int, std::unique_ptr<Connection>> connections;
public:
    EventLoop(Socket listen_socket, int epoll_fd)
        : listen_socket(std::move(listen_socket)), epoll_fd(epoll_fd) {}
    ~EventLoop(){ if (epoll_fd >= 0) ::close(epoll_fd); }

    void run(std::atomic<bool>& stop);
};
#endif //HTTP_SERVER_EVENT_LOOP_H
