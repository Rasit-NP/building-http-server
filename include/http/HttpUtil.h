#ifndef HTTP_SERVER_HTTPUTIL_H
#define HTTP_SERVER_HTTPUTIL_H

# include <string>
# include <cctype>

namespace http {
    inline std::string to_lower(std::string s) {
        for (char& c : s) {
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))
            );
        }

        return s;
    }

    inline std::string trim_ows(const std::string& s) {
        const char* ws = " \t";
        size_t begin = s.find_first_not_of(ws);
        if (begin == std::string::npos) return "";
        size_t end = s.find_last_not_of(ws);
        return s.substr(begin, end - begin + 1);
    }
}

#endif //HTTP_SERVER_HTTPUTIL_H
