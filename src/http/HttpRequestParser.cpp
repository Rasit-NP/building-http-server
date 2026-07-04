# include "http/HttpRequestParser.h"
# include "http/HttpUtil.h"
# include <string>

constexpr size_t kNpos = static_cast<size_t>(-1);
size_t find_crlf(const char* data, size_t size, size_t begin) {
    for (size_t i = begin; i + 1 < size; ++i) {
        if (data[i] == '\r' && data[i+1] == '\n') {
            return i;
        }
    }
    return kNpos;
}

HttpRequestParser::Result
HttpRequestParser::parse(const char* data, size_t len) {
    size_t offset = 0;
    switch (state_) {
    case State::RequestLine:
        return parseRequestLine(data, len, offset);
    case State::Headers:
        return parseHeaders(data, len, offset);
    case State::Done:
        return Result::Ok;
    default:
        state_ = State::Error;
        return Result::Error;
    }
}

HttpRequestParser::Result
HttpRequestParser::parseRequestLine(const char* data, size_t len, size_t& offset) {
    std::string line(data, len);

    size_t crlf = line.find("\r\n");
    if (crlf == std::string::npos) {
        state_ = State::Error;
        return Result::Error;
    }

    std::string request_line = line.substr(0, crlf);

    size_t sp1 = request_line.find(' ');
    if (sp1 == std::string::npos) {
        state_ = State::Error;
        return Result::Error;
    }

    size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        state_ = State::Error;
        return Result::Error;
    }

    if (request_line.find(' ', sp2+1) != std::string::npos) {
        state_ = State::Error;
        return Result::Error;
    }

    request_.method  = request_line.substr(0, sp1);
    request_.path    = request_line.substr(sp1+1, sp2-sp1-1);
    request_.version = request_line.substr(sp2+1);

    if (!request_.isValid()) {
        state_ = State::Error;
        return Result::Error;
    }

    offset = crlf + 2;
    state_ = State::Headers;
    return parseHeaders(data, len, offset);
}
HttpRequestParser::Result
HttpRequestParser::parseHeaders(const char* data, size_t len, size_t& offset) {
    while (offset < len) {
        size_t line_end = find_crlf(data, len, offset);
        if (line_end == std::string::npos) return Result::Incomplete;

        std::string line(data + offset, line_end - offset);
        offset = line_end + 2;

        if (line.empty()) {
            state_ = State::Done;
            return Result::Ok;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        std::string name  = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        request_.headers[http::to_lower(name)] = http::trim_ows(value);
    }

    return Result::Incomplete;
}