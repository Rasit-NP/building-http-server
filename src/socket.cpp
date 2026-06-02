# include "socket.h"

# include <arpa/inet.h>
# include <netinet/in.h>
# include <sys/socket.h>
# include <unistd.h>
# include <cstdio>
# include <stdexcept>

Socket::Socket() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0)
        throw std::runtime_error("socket() failed");

    int opt = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
        throw std::runtime_error("setsockopt() failed");
    }
}

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() noexcept {
    if (fd_ >= 0)
        close(fd_);
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            close(fd_);
        fd_ = other.fd_;
        other.fd_= -1;
    }

    return *this;
}

void Socket::bind(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("bind() failed");
}

void Socket::listen(int backlog) {
    if (::listen(fd_, backlog) < 0)
        throw std::runtime_error("listen() failed");
}

Socket Socket::accept() {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

    if (client_fd < 0)
        throw std::runtime_error("accept() failed");

    return Socket(client_fd);
}

ssize_t Socket::read(char* buf, size_t len) {
    ssize_t n = ::read(fd_, buf, len);

    return n;
}

bool Socket::write(const char* buf, size_t len) {
    ssize_t written = ::write(fd_, buf, len);
    if (written < 0) {
        perror("write() failed");
        return false;
    }
    return true;
}