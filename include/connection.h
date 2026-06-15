#ifndef HTTP_SERVER_CONNECTION_H
#define HTTP_SERVER_CONNECTION_H

# include <string>
# include "socket.h"

class Connection {
private:
    Socket          socket;
    std::string     read_buf;
    std::string     write_buf;
    bool            writing = false;

public:
    explicit Connection(Socket sock) : socket(std::move(sock)) {}

    int fd() const noexcept {
        return socket.fd();
    }

    bool on_readable();
    bool on_writable();

    bool want_write() const noexcept {
        return !write_buf.empty();
    }
    bool is_writing() const noexcept {
        return writing;
    }
    void set_writing(bool on) noexcept {
        writing = on;
    }

private:
    bool flush_write();
};

#endif //HTTP_SERVER_CONNECTION_H