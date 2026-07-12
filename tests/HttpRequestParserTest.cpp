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

TEST(HttpRequestParserTest, incremental) {
    const char* raw1 = "GET /path H";
    const char* raw2 = "TTP/1.1\r\n";
    const char* raw3 = "Host: localhost:8080";
    const char* raw4 =
        "\r\n"
        "User-Agent: curl/8.0\r";
    const char* raw5 =
        "\n"
        "\r\n";

    HttpRequestParser parser;
    parser.parse(raw1, strlen(raw1));
    parser.parse(raw2, strlen(raw2));
    parser.parse(raw3, strlen(raw3));
    parser.parse(raw4, strlen(raw4));
    HttpRequestParser::Result r = parser.parse(raw5, strlen(raw5));

    HttpRequest req = parser.request();

    EXPECT_TRUE(r == HttpRequestParser::Result::Ok);
    EXPECT_TRUE(req.method == "GET");
    EXPECT_TRUE(req.path == "/path");
    EXPECT_TRUE(req.version == "HTTP/1.1");
    EXPECT_TRUE(req.headers.at("host")       == "localhost:8080");
    EXPECT_TRUE(req.headers.at("user-agent") == "curl/8.0");
}

TEST(HttpRequestParserTest, RequestLine1) {
    HttpRequestParser parser;
    const char* raw = "GET / FOO\r\n\r\n";
    EXPECT_TRUE(parser.parse(raw, std::strlen(raw)) == HttpRequestParser::Result::Error);
}

TEST(HttpRequestParserTest, RequestLine2) {
    HttpRequestParser parser;
    const char* raw = "GET / HTTP/1.1\r\nHost localhost\r\n\r\n";
    EXPECT_TRUE(parser.parse(raw, std::strlen(raw)) == HttpRequestParser::Result::Error);
}

TEST(HttpRequestParserTest, RequestLine3) {
    HttpRequestParser parser;
    const char* raw = "GET / HTTP/1.1\n";
    EXPECT_TRUE(parser.parse(raw, std::strlen(raw)) == HttpRequestParser::Result::Error);
}

TEST(HttpRequestParserTest, ErrorLatch) {
    HttpRequestParser parser;

    const char* bad = "GET / HTTP/1.1\n";
    EXPECT_TRUE(parser.parse(bad, std::strlen(bad)) == HttpRequestParser::Result::Error);

    const char* good = "GET / HTTP/1.1\r\n\r\n";
    EXPECT_TRUE(parser.parse(good, std::strlen(good)) == HttpRequestParser::Result::Error);
}
