#ifndef HTTP_SERVER_CONNECTION_H
#define HTTP_SERVER_CONNECTION_H

# include <string>
# include "socket.h"

class Connection {
private:
    Socket          socket;
    std::string     read_buf;
    std::string     write_buf;

public:
    explicit Connection(Socket sock) : socket(std::move(sock)) {}

    int fd() const noexcept {
        return socket.fd();
    }

    bool on_readable();
};

#endif //HTTP_SERVER_CONNECTION_H