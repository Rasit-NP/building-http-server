#ifndef HTTP_SERVER_STATICFILEHANDLER_H
#define HTTP_SERVER_STATICFILEHANDLER_H

# include <string>
# include "HttpRequest.h"
# include "HttpResponse.h"

class StaticFileHandler {
public:
    explicit StaticFileHandler(std::string root): root_(root){}
    HttpResponse handle(const HttpRequest& request) const;

private:
    std::string root_;
};

#endif //HTTP_SERVER_STATICFILEHANDLER_H
