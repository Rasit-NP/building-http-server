# include "http/StaticFileHandler.h"
# include <filesystem>
# include <fstream>

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
        // 이후에 404로 수정
        res.status_code = 400;
        return res;
    }

    std::ifstream file(full, std::ios::binary);

    std::string body(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );

    res.status_code = 200;
    res.body = std::move(body);
    return res;
}