#ifndef HTTP_SERVER_HTTPREQUESTPARSER_H
#define HTTP_SERVER_HTTPREQUESTPARSER_H

# include <cstddef>
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

    Result parseRequestLine(const char* data, size_t len);

    State state_ = State::RequestLine;
    HttpRequest request_;
};

#endif //HTTP_SERVER_HTTPREQUESTPARSER_H
