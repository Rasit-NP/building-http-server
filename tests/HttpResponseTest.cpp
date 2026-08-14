# include <string>
# include <gtest/gtest.h>
# include "http/HttpResponse.h"

struct ResponseCase {
    int status;
    std::string body;
    std::string expected;
};

class ResponseTest : public testing::TestWithParam<ResponseCase> {
protected:
    HttpResponse res;
};

struct ResponseNameGenerator {
    std::string operator()(
        const testing::TestParamInfo<ResponseCase>& info) const {
        return "ResponseTest_" + std::to_string(info.param.status);
    }
};

std::vector<ResponseCase> ResponseCases = {
    {200, "Success",
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Date: Fri, 14 Aug 2026 06:53:31 GMT\r\n"
        "Content-Length: 7\r\n"
        "\r\n"
        "Success"},
    {400, "Bad Request",
        "HTTP/1.1 400 Bad Request\r\n"
        "Connection: close\r\n"
        "Date: Fri, 14 Aug 2026 06:53:31 GMT\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Bad Request"}
};

INSTANTIATE_TEST_SUITE_P(Normal, ResponseTest, testing::ValuesIn(ResponseCases), ResponseNameGenerator());

TEST_P(ResponseTest, ResponseTest) {
    const auto& c = GetParam();

    res.status_code = c.status;
    res.headers.emplace_back("Connection", "close");
    res.body = c.body;

    EXPECT_EQ(res.serialize(1786690411), c.expected);
    EXPECT_EQ(res.serialize(), res.serialize(std::time(nullptr)));
}