# include <string>
# include <vector>
# include "http/HttpResponse.h"

namespace {
    std::string reason_phrase(int code) {
        switch (code) {
        case 200:   return "OK";
        case 400:   return "Bad Request";
        default:    return "";
        }
    }
}

std::string HttpResponse::serialize() const {
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

    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";

    out += body;

    return out;
}