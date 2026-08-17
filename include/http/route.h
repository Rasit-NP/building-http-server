#ifndef HTTP_SERVER_ROUTE_H
#define HTTP_SERVER_ROUTE_H

# include "http/HttpResponse.h"
# include "http/HttpRequest.h"
# include "http/StaticFileHandler.h"

HttpResponse make_echo_response(const HttpRequest& req);

HttpResponse route(const HttpRequest& req, const StaticFileHandler& handler);

#endif //HTTP_SERVER_ROUTE_H
