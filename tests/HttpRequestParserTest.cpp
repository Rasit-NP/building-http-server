# include <cstring>
# include <gtest/gtest.h>
# include "http/HttpRequestParser.h"

TEST(HttpRequestParserTest, normal) {
    HttpRequestParser parser;
    char input[] = "GET /index.html HTTP/1.1\r\n\r\n";
    EXPECT_TRUE(parser.parse(input, sizeof(input)) == HttpRequestParser::Result::Ok);
}

TEST(HttpRequestParserTest, threeSP){
    HttpRequestParser parser;
    char input[] = "GET /index.html HTTP/1.1 loss\r\n";
    EXPECT_TRUE(parser.parse(input, sizeof(input)) == HttpRequestParser::Result::Error);
}

TEST(HttpRequestParserTest, isEmptyToken) {
    HttpRequestParser parser;
    char input[] = "GET  HTTP/1.1\r\n";
    EXPECT_TRUE(parser.parse(input, sizeof(input)) == HttpRequestParser::Result::Error);
}

TEST(HttpRequestParserTest, noSP) {
    HttpRequestParser parser;
    char input[] = "GET/index.htmlHTTP/1.1\r\n";
    EXPECT_TRUE(parser.parse(input, sizeof(input)) == HttpRequestParser::Result::Error);
}

TEST(HttpRequestParserTest, extract) {
    const char* raw =
        "GET /path HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: curl/8.0\r\n"
        "\r\n";
    HttpRequestParser parser;
    HttpRequestParser::Result r = parser.parse(raw, std::strlen(raw));
    HttpRequest req = parser.request();

    EXPECT_TRUE(r == HttpRequestParser::Result::Ok);
    EXPECT_TRUE(req.method == "GET");
    EXPECT_TRUE(req.path == "/path");
    EXPECT_TRUE(req.version == "HTTP/1.1");
    EXPECT_TRUE(req.headers.at("host")       == "localhost:8080");
    EXPECT_TRUE(req.headers.at("user-agent") == "curl/8.0");
}