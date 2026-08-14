#ifndef HTTP_SERVER_HTTPRESPONSE_H
#define HTTP_SERVER_HTTPRESPONSE_H

# include <utility>
# include <vector>
# include <string>
# include <ctime>

class HttpResponse {
public:
    int status_code = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::string serialize() const;
    std::string serialize(std::time_t now) const;
};
#endif //HTTP_SERVER_HTTPRESPONSE_H
