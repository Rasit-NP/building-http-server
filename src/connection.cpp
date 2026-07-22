# include "connection.h"
# include <unistd.h>
# include <cerrno>
# include <cstdio>

namespace {
    std::string build_response(const char* status_line, const std::string& body) {
        std::string res;
        res += "HTTP/1.1 ";
        res += status_line;
        res += "\r\nContent-Length: ";
        res += std::to_string(body.size());
        res += "\r\nConnection: close\r\n\r\n";
        res += body;
        return res;
    }
}

bool Connection::on_readable() {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(socket.fd(), buf, sizeof(buf));
        if (n > 0) {
            auto r = parser_.parse(buf, static_cast<size_t>(n));

            if (r == HttpRequestParser::Result::Ok) {
                const HttpRequest& req = parser_.request();

                std::printf("method=%.*s path=%.*s version=%.*s\n",
                            static_cast<int>(req.method.size()), req.method.data(),
                            static_cast<int>(req.path.size()), req.path.data(),
                            static_cast<int>(req.version.size()), req.version.data());
                write_buf.append(build_response("200 OK", "Success"));
                close_after_write = true;
                break;
            }
            else if (r == HttpRequestParser::Result::Error) {
                std::fprintf(stderr, "parse error on fd=%d\n", fd());
                write_buf.append(build_response("400 Bad Request", "Bad Request"));
                close_after_write = true;
                break;
            }
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
    return finish_io();
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

bool Connection::finish_io() {
    if (!flush_write()) {
        return false;
    }
    if (close_after_write && write_buf.empty()) {
        return false;
    }
    return true;
}

bool Connection::on_writable() {
    return finish_io();
}