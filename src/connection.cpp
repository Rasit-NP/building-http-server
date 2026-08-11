# include "connection.h"
# include "http/HttpResponse.h"
# include <unistd.h>
# include <cerrno>
# include <cstdio>


bool Connection::on_readable() {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(socket.fd(), buf, sizeof(buf));
        if (n > 0) {
            auto r = parser_.parse(buf, static_cast<size_t>(n));

            if (r == HttpRequestParser::Result::Ok) {
                const HttpRequest& req = parser_.request();
                std::string out = handler_.handle(req).serialize();
                write_buf.append(out);

                close_after_write = true;
                break;
            }
            else if (r == HttpRequestParser::Result::Error) {
                std::fprintf(stderr, "parse error on fd=%d\n", fd());
                HttpResponse res;
                res.status_code = 400;
                res.body = "Bad Request";
                res.headers.emplace_back("Connection", "close");
                write_buf.append(res.serialize());
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