#ifndef HTTP_SERVER_HTTPREQUESTPARSER_H
#define HTTP_SERVER_HTTPREQUESTPARSER_H

# include <cstddef>
# include <string>
# include "HttpRequest.h"

class HttpRequestParser {
public:
    enum class Result {
        Ok,
        Incomplete,
        Error
    };

    Result parse(const char* data, size_t len);

    const HttpRequest& request() const { return request_; }

private:
    enum class State {
        RequestLine,
        Headers,
        Done,
        Error
    };

    Result parseRequestLine();
    Result parseHeaders();

    std::string buffer_;
    size_t offset_ = 0;
    size_t method_off_, method_len_;
    size_t path_off_, path_len_;
    size_t version_off_, version_len_;
    State state_ = State::RequestLine;
    HttpRequest request_;
};

#endif //HTTP_SERVER_HTTPREQUESTPARSER_H
