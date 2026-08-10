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
    void reset();

private:
    enum class State {
        RequestLine,
        Headers,
        Body,
        Done,
        Error
    };

    Result parseRequestLine();
    Result parseHeaders();
    Result parseBody();

    std::string buffer_;
    size_t offset_ = 0;
    size_t body_remaining_ = 0;
    State state_ = State::RequestLine;
    HttpRequest request_;
};

#endif //HTTP_SERVER_HTTPREQUESTPARSER_H
