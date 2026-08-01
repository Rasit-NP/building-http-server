#ifndef HTTP_SERVER_HTTPRESPONSE_H
#define HTTP_SERVER_HTTPRESPONSE_H

# include <utility>
# include <vector>
# include <string>

class HttpResponse {
public:
    int status_code = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::string serialize() const;
};
#endif //HTTP_SERVER_HTTPRESPONSE_H
