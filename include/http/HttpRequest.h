#ifndef HTTP_SERVER_HTTPREQUEST_H
#define HTTP_SERVER_HTTPREQUEST_H

# include <string>
# include <unordered_map>
using namespace std;

struct HttpRequest {
    string method;
    string path;
    string version;

    unordered_map<string, string> headers;

    bool isValid() const {
        return !method.empty() && !path.empty() && !version.empty();
    }
};


#endif //HTTP_SERVER_HTTPREQUEST_H
