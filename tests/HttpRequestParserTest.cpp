# include <cstring>
# include <gtest/gtest.h>
# include "http/HttpRequestParser.h"

struct HeaderCase {
    std::string input;
    std::string expected_method;
    std::string expected_path;
    std::string expected_version;
    std::vector<std::pair<std::string, std::string>> expected_headers;
};

struct ErrorCase {
    std::string input;
    std::string error_name;
};

struct BodyCase {
    std::string input;
    std::string expected_body;
    HttpRequestParser::Result expected_result;
};

class ParserTest : public testing::TestWithParam<HeaderCase> {
protected:
    HttpRequestParser parser;
};

class ChunkTest : public testing::TestWithParam<size_t> {
protected:
    HttpRequestParser parser;
};

class ErrorTest : public testing::TestWithParam<ErrorCase> {
protected:
    HttpRequestParser parser;
};

class BodyTest : public testing::TestWithParam<BodyCase> {
protected:
    HttpRequestParser parser;
};

struct CaseNameGenerator {
    std::string operator()(
        const testing::TestParamInfo<HeaderCase>& info) const {
        return "ParserTest_" + std::to_string(info.index);
    }
};

struct ChunkNameGenerator {
    std::string operator()(
        const testing::TestParamInfo<size_t>& info) const {
        return "ChunkTest_" + std::to_string(info.param);
    }
};

struct ErrorNameGenerator {
    std::string operator()(
        const testing::TestParamInfo<ErrorCase>& info) const {
        return "ErrorTest_" + info.param.error_name;
    }
};

struct BodyNameGenerator {
    std::string operator()(
        const testing::TestParamInfo<BodyCase>& info) const {
        return "BodyTest_" + std::to_string(info.index);
    }
};

std::vector<HeaderCase> cases = {
    {"GET / HTTP/1.1\r\n\r\n",
        "GET", "/", "HTTP/1.1", {}},
    {"POST /x HTTP/1.1\r\nHost: a:8080\r\n\r\n",
        "POST", "/x", "HTTP/1.1", {{"host", "a:8080"}}},
    {"GET /path HTTP/1.1\r\nHost: localhost:8080\r\nUser-Agent: curl/8.0\r\n\r\n",
        "GET", "/path", "HTTP/1.1", {{"host", "localhost:8080"}, {"user-agent", "curl/8.0"}}},
};

bool checkParsedHeader(const std::vector<std::pair<std::string, std::string>>& expected, const std::unordered_map<std::string, std::string>& headers) {
    if (expected.size() != headers.size()) {
        return false;
    }
    for (const auto& [key, value] : expected) {
        if (headers.find(key) == headers.end() || headers.at(key) != value) {
            return false;
        }
    }
    return true;
}

HttpRequestParser::Result feedInChunks(HttpRequestParser& parser, const std::string& input, size_t chunk) {
    HttpRequestParser::Result r;
    for (size_t off = 0; off < input.size(); off += chunk) {
        size_t len = std::min(chunk, input.size() - off);
        r = parser.parse(input.data() + off, len);
    }
    return r;
}

TEST_P(ParserTest, ParserTest) {
    const auto& c = GetParam();

    HttpRequestParser parser;
    parser.parse(c.input.data(), c.input.size());

    EXPECT_EQ(parser.request().method, c.expected_method);
    EXPECT_EQ(parser.request().path, c.expected_path);
    EXPECT_EQ(parser.request().version, c.expected_version);
    EXPECT_TRUE(checkParsedHeader(c.expected_headers, parser.request().headers));
}

INSTANTIATE_TEST_SUITE_P(Normal, ParserTest, testing::ValuesIn(cases), CaseNameGenerator());

TEST_P(ChunkTest, ChunkTest) {
    const auto& c = GetParam();

    for (const HeaderCase& header : cases) {
        HttpRequestParser parser;

        HttpRequestParser::Result r = feedInChunks(parser, header.input, c);
        EXPECT_EQ(parser.request().method, header.expected_method);
        EXPECT_EQ(parser.request().path, header.expected_path);
        EXPECT_EQ(parser.request().version, header.expected_version);
        EXPECT_TRUE(checkParsedHeader(header.expected_headers, parser.request().headers));
        EXPECT_EQ(HttpRequestParser::Result::Ok, r);
    }

}

INSTANTIATE_TEST_SUITE_P(Normal, ChunkTest, testing::Values(1, 2, 3, 4, 5), ChunkNameGenerator());

std::vector<ErrorCase> errors = {
    {"GET  HTTP/1.1\r\n", "EmptyToken"},
    {"GET /index.html HTTP/1.1 loss\r\n", "ThreeSP"},
    {"GET/index.htmlHTTP/1.1\r\n", "NoSP"},
    {"GET / FOO\r\n\r\n", "WrongVersion"},
    {"GET / HTTP/1.1\r\nHost localhost\r\n\r\n", "NoColonInHeader"},
    {"GET / HTTP/1.1\n", "bareLF"},
};

TEST_P(ErrorTest, ErrorTest) {
    const auto& c = GetParam();
    HttpRequestParser parser;
    HttpRequestParser::Result r = parser.parse(c.input.data(), c.input.size());

    EXPECT_EQ(HttpRequestParser::Result::Error, r);
}

INSTANTIATE_TEST_SUITE_P(Error, ErrorTest, testing::ValuesIn(errors), ErrorNameGenerator());

TEST(HttpRequestParserTest, ErrorLatch) {
    HttpRequestParser parser;

    const char* bad = "GET / HTTP/1.1\n";
    EXPECT_TRUE(parser.parse(bad, std::strlen(bad)) == HttpRequestParser::Result::Error);

    const char* good = "GET / HTTP/1.1\r\n\r\n";
    EXPECT_TRUE(parser.parse(good, std::strlen(good)) == HttpRequestParser::Result::Error);
}

std::vector<BodyCase> body_cases = {
    {"GET / HTTP/1.1\r\ncontent-Length: 10\r\n\r\n1234567890", "1234567890", HttpRequestParser::Result::Ok},
    {"POST /index HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "", HttpRequestParser::Result::Ok},
    {"GET /path HTTP/1.1\r\n\r\n", "", HttpRequestParser::Result::Ok},
    {"POST /path/1 HTTP/1.1\r\nContent-length: abc\r\n\r\n", "", HttpRequestParser::Result::Error},
    {"GET /index/1 HTTP/1.1\r\nContent-Length: 20\r\n\r\nabc", "abc", HttpRequestParser::Result::Incomplete},
    {"POST /over HTTP/1.1\r\ncontent-length: 5\r\n\r\nhelloEXTRA", "hello", HttpRequestParser::Result::Ok},
    {"GET /dupCL1 HTTP/1.1\r\nContent-Length: 1\r\ncontent-length: 1\r\n\r\na", "a", HttpRequestParser::Result::Ok},
    {"POST /dupCL2 HTTP/1.1\r\nContent-Length: 1\r\ncontent-length: 2\r\n\r\nab", "", HttpRequestParser::Result::Error},
    {"POST /dupCL3 HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab", "", HttpRequestParser::Result::Error},
    {"GET /dupCookie HTTP/1.1\r\nCookie: a=1\r\nCookie: b=2\r\n\r\n", "", HttpRequestParser::Result::Ok}
};

TEST_P(BodyTest, BodyTest) {
    const auto& c = GetParam();
    HttpRequestParser parser;

    HttpRequestParser::Result r = parser.parse(c.input.data(), c.input.size());

    EXPECT_EQ(parser.request().body, c.expected_body);
    EXPECT_EQ(r, c.expected_result);
    parser.reset();

    r = feedInChunks(parser, c.input, 1);

    EXPECT_EQ(parser.request().body, c.expected_body);
    EXPECT_EQ(r, c.expected_result);
    parser.reset();
}

INSTANTIATE_TEST_SUITE_P(Body, BodyTest, testing::ValuesIn(body_cases), BodyNameGenerator());
