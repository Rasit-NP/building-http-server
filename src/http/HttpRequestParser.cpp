# include "http/HttpRequestParser.h"
# include "http/HttpUtil.h"
# include <string>

static constexpr size_t kMaxBufferBytes = 8 * 1024;

size_t find_crlf(const std::string& buf, size_t start) {
    if (buf.size() < 2)
        return std::string::npos;
    size_t len = buf.size() - 1;
    for (size_t i = start; i<len; ++i) {
        if (buf[i] == '\r') {
            if (buf[i+1] == '\n') {
                return i;
            }
        }
    }
    return std::string::npos;
}

HttpRequestParser::Result
HttpRequestParser::parse(const char* data, size_t len) {
    buffer_.append(data, len);

    if (buffer_.size() > kMaxBufferBytes) {
        state_ = State::Error;
        return Result::Error;
    }

    if (state_ == State::Error) {
        return Result::Error;
    }

    while (true) {
        switch (state_) {
        case State::RequestLine: {
            Result r = parseRequestLine();
            if (r != Result::Ok) return r;
            break;
        }
        case State::Headers: {
            Result r = parseHeaders();
            if (r != Result::Ok) return r;
            break;
        }
        case State::Done:
            request_.method = std::string_view(buffer_.data() + request_.method_off, request_.method_len);
            request_.path = std::string_view(buffer_.data() + request_.path_off, request_.path_len);
            request_.version = std::string_view(buffer_.data() + request_.version_off, request_.version_len);

            if (!request_.isValid()) {
                state_ = State::Error;
                return Result::Error;
            }
            if (!request_.isValidVersion()) {
                state_ = State::Error;
                return Result::Error;
            }
            return Result::Ok;

        case State::Error:
            return Result::Error;
        }
    }
}

HttpRequestParser::Result
HttpRequestParser::parseRequestLine() {

    size_t crlf = find_crlf(buffer_, offset_);
    if (crlf == std::string::npos) {
        for (size_t i=offset_; i<buffer_.size(); ++i) {
            if (buffer_[i] == '\n') {
                if (i == offset_ || buffer_[i-1] != '\r') {
                    state_ = State::Error;
                    return Result::Error;
                }
            }
        }
        return Result::Incomplete;
    }

    size_t sp1 = buffer_.find(' ', offset_);
    if (sp1 == std::string::npos || sp1 >= crlf) {
        state_ = State::Error;
        return Result::Error;
    }

    size_t sp2 = buffer_.find(' ', sp1 + 1);
    if (sp2 == std::string::npos || sp2 >= crlf) {
        state_ = State::Error;
        return Result::Error;
    }

    size_t sp3 = buffer_.find(' ', sp2 + 1);
    if (sp3 != std::string::npos && sp3 < crlf) {
        state_ = State::Error;
        return Result::Error;
    }

    request_.method_off = offset_;
    request_.method_len = sp1 - request_.method_off;
    request_.path_off = offset_ + sp1 + 1;
    request_.path_len = sp2 - request_.path_off;
    request_.version_off = offset_ + sp2 + 1;
    request_.version_len = crlf - request_.version_off;

    if (!request_.isSuccessfullyParsed()) {
        state_ = State::Error;
        return Result::Error;
    }

    offset_ = crlf + 2;
    state_ = State::Headers;
    return parseHeaders();
}
HttpRequestParser::Result
HttpRequestParser::parseHeaders() {
    while (offset_ < buffer_.size()) {
        size_t line_end = find_crlf(buffer_, offset_);
        if (line_end == std::string::npos) return Result::Incomplete;

        std::string line = buffer_.substr(offset_, line_end-offset_);
        offset_ = line_end + 2;

        if (line.empty()) {
            state_ = State::Done;
            return Result::Ok;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            state_ = State::Error;
            return Result::Error;
        }

        std::string name  = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        request_.headers[http::to_lower(name)] = http::trim_ows(value);
    }

    return Result::Incomplete;
}

void HttpRequestParser::reset() {
    state_ = State::RequestLine;
    buffer_.clear();
    offset_ = 0;
    request_ = HttpRequest{};
}