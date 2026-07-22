#ifndef HTTP_SERVER_CONNECTION_H
#define HTTP_SERVER_CONNECTION_H

# include <string>
# include "socket.h"
# include "http/HttpRequestParser.h"

class Connection {
private:
    Socket            socket;
    HttpRequestParser parser_;
    bool              writing = false;
    bool              close_after_write = false;
    std::string       write_buf;

public:
    explicit Connection(Socket sock) : socket(std::move(sock)) {}
    Connection(const Connection&)             = delete;
    Connection& operator=(const Connection&)  = delete;
    Connection(Connection&&)                  = default;
    Connection& operator=(Connection&&)       = default;

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
    bool finish_io();
};

#endif //HTTP_SERVER_CONNECTION_H