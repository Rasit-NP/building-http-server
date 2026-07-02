# include <gtest/gtest.h>
# include "http/HttpRequestParser.h"

TEST(HttpRequestParserTest, normal) {
    HttpRequestParser parser;
    char input[] = "GET /index.html HTTP/1.1\r\n";
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