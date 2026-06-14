#ifndef HTTP_SERVER_SOCKET_H
#define HTTP_SERVER_SOCKET_H

# include <optional>
# include <cstdint>
# include <sys/types.h>

class Socket {
public:
    Socket();
    explicit Socket(int fd);
    ~Socket() noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    void bind(uint16_t port);
    void listen(int backlog = 10);
    std::optional<Socket> accept();
    ssize_t read(char* buf, size_t len);
    bool write(const char* buf, size_t len);

    int fd() const { return fd_; }

    static void set_nonblocking(int fd);

private:
    int fd_ = -1;
};

#endif //HTTP_SERVER_SOCKET_H