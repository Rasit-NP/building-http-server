# include "http/StaticFileHandler.h"
# include <filesystem>
# include <fstream>
# include <cctype>
# include <algorithm>

HttpResponse make_not_found() {
    HttpResponse res;
    res.status_code = 404;
    res.body = "<html><body><h1>404 Not Found</h1></body></html>";
    res.headers.emplace_back("Content-Type", "text/html");
    return res;
}

std::string mime_from_extension(const std::string& ext) {
    if (ext == ".html")     return "text/html";
    if (ext == ".css")      return "text/css";
    if (ext == ".js")       return "application/javascript";
    if (ext == ".png")      return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")  return "image/jpeg";
    if (ext == ".ico")      return "image/x-icon";
    return "application/octet-stream";
}

HttpResponse StaticFileHandler::handle(const HttpRequest& request) const {
    std::string target(request.path);

    if (target == "/")
        target = "/index.html";

    namespace fs = std::filesystem;

    fs::path base = fs::path(root_).lexically_normal();
    fs::path full = (base / target.substr(1)).lexically_normal();

    const std::string base_str = base.string();
    const std::string full_str = full.string();

    bool inside =
        full_str.size() >= base_str.size() &&
        full_str.compare(0, base_str.size(), base_str) == 0 &&
        (full_str.size() == base_str.size() || full_str[base_str.size()] == '/');

    HttpResponse res;

    if (!inside) {
        return make_not_found();
    }

    std::ifstream file(full, std::ios::binary);

    if (fs::is_directory(full)) {
        return make_not_found();
    }

    if (!file.is_open()) {
        return make_not_found();
    }

    std::string body(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );

    std::string ext = full.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c){ return std::tolower(c); });

    res.status_code = 200;
    res.body = std::move(body);
    res.headers.emplace_back("Content-Type", mime_from_extension(ext));
    return res;
}