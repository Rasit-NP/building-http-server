# include "connection.h"
# include <unistd.h>
# include <cerrno>

bool Connection::on_readable() {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(socket.fd(), buf, sizeof(buf));
        if (n > 0) {
            write_buf.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return flush_write();
}

bool Connection::flush_write() {
    while (!write_buf.empty()) {
        ssize_t w = ::write(socket.fd(), write_buf.data(), write_buf.size());
        if (w > 0) {
            write_buf.erase(0, static_cast<size_t>(w));
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool Connection::on_writable() {
    return flush_write();
}