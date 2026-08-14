# include <string>
# include <vector>
# include "http/HttpResponse.h"

namespace {
    std::string reason_phrase(int code) {
        switch (code) {
        case 200:   return "OK";
        case 400:   return "Bad Request";
        case 404:   return "Not Found";
        default:    return "";
        }
    }

    std::string http_date(const std::time_t& now) {
        char buf[64];
        std::tm gmt{};
        gmtime_r(&now, &gmt);
        std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);

        return std::string{buf};
    }
}

std::string HttpResponse::serialize() const {
    return serialize(std::time(nullptr));
}

std::string HttpResponse::serialize(std::time_t now) const {
    std::string out;
    out.reserve(body.size() +  headers.size()*48 + 64);

    out += "HTTP/1.1 ";
    out += std::to_string(status_code);
    out += " ";
    out += reason_phrase(status_code);
    out += "\r\n";

    for (const auto& [name, value] : headers) {
        if (name == "Content-Length") {
            continue;
        }
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }

    out += "Date: ";
    out += http_date(now);
    out += "\r\n";

    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";

    out += body;

    return out;
}