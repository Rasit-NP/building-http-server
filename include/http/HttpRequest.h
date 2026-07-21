#ifndef HTTP_SERVER_HTTPREQUEST_H
#define HTTP_SERVER_HTTPREQUEST_H

# include <string>
# include <unordered_map>
using namespace std;

struct HttpRequest {
    string_view method;
    string_view path;
    string_view version;

    unordered_map<string, string> headers;

    bool isValid() const {
        return !method.empty() && !path.empty() && !version.empty();
    }

    bool isValidVersion() const {
        if (version.size() != 8)
            return false;
        if (version.compare(0, 5, "HTTP/") != 0)
            return false;
        if (!std::isdigit(static_cast<unsigned char>(version[5])))
            return false;
        if (version[6] != '.')
            return false;
        if (!std::isdigit(static_cast<unsigned char>(version[7])))
            return false;
        return true;
    }
};


#endif //HTTP_SERVER_HTTPREQUEST_H
