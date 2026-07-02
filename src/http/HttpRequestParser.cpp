# include "http/HttpRequestParser.h"
# include <string>

HttpRequestParser::Result
HttpRequestParser::parse(const char* data, size_t len) {
    switch (state_) {
    case State::RequestLine:
        return parseRequestLine(data, len);
    default:
        state_ = State::Error;
        return Result::Error;
    }
}

HttpRequestParser::Result
HttpRequestParser::parseRequestLine(const char* data, size_t len) {
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

    state_ = State::Done;
    return Result::Ok;
}