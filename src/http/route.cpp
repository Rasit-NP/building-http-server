# include "http/route.h"

HttpResponse make_echo_response(const HttpRequest& req) {
    HttpResponse res;
    res.status_code = 200;
    res.body = req.body;

    auto it = req.headers.find("content-type");
    if (it != req.headers.end()) {
        res.headers.emplace_back("Content-Type", it->second);
    }
    else {
        res.headers.emplace_back("Content-Type", "application/octet-stream");
    }

    return res;
}

HttpResponse route(const HttpRequest& req, const StaticFileHandler& handler) {
    if (req.path == "/echo") {
        return make_echo_response(req);
    }
    return handler.handle(req);
}