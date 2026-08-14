# include "http/StaticFileHandler.h"
# include <gtest/gtest.h>
# include <filesystem>
# include <fstream>
# include <atomic>

namespace fs = std::filesystem;

std::string unique_name() {
    static std::atomic<int> counter{0};
    return "sfh test_" + std::to_string(::getpid()) + "_" +  std::to_string(counter.fetch_add(1));
}

struct StaticFileHandlerCase {
    std::string file_name;
    std::string file_content;
};

struct MIMETestCase {
    std::string file_name;
    std::string file_ext;
    std::string expected_type;
};

class StaticFileHandlerTester {
protected:
    fs::path base_;
    fs::path root_;
    void SetUpRoot() {
        base_ = fs::temp_directory_path() / unique_name();
        root_ = base_ / unique_name();
        fs::create_directories(root_);
    }

    void TearDownRoot() {
        fs::remove_all(base_);
    }

    void write_file(const std::string& rel, const std::string& content) {
        std::ofstream ofs(root_ / rel, std::ios::binary);
        ofs << content;
    }

    HttpResponse get(const std::string& target) {
        StaticFileHandler handler(root_);
        HttpRequest req;
        req.method = "GET";
        req.path = target;
        return handler.handle(req);
    }
};

class StaticFileHandlerTest
    : public StaticFileHandlerTester,
      public ::testing::TestWithParam<StaticFileHandlerCase> {
protected:
    void SetUp() override { SetUpRoot(); }
    void TearDown() override { TearDownRoot(); }
};

class MIMETest
    : public StaticFileHandlerTester,
      public ::testing::TestWithParam<MIMETestCase> {
protected:
    void SetUp() override { SetUpRoot(); }
    void TearDown() override { TearDownRoot(); }
};

struct HandlerTestNameGenerator {
    std::string operator() (
        const testing::TestParamInfo<StaticFileHandlerCase>& info) const {
        std::string res = "StaticFileHandlerTest_";
        for (const char c : info.param.file_name) {
            if (c == '.')
                res.push_back('_');
            else
                res.push_back(c);
        }
        return res;
    }
};

struct MIMETestNameGenerator {
    std::string operator()(
        const testing::TestParamInfo<MIMETestCase>& info) const {
        return "MIMETest_" + info.param.file_ext;
    }
};

std::vector<StaticFileHandlerCase> handler_cases_200 = {
    {"hello.txt", "hello world"},
    {"index.html", "<html></html>"},
    {"empty.txt", ""},
};

TEST_P(StaticFileHandlerTest, StaticFileHandlerTest) {
    const auto& [name, content] = GetParam();

    write_file(name, content);
    HttpResponse res = get("/" + name);

    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.body, content);
}

INSTANTIATE_TEST_SUITE_P(200, StaticFileHandlerTest, testing::ValuesIn(handler_cases_200), HandlerTestNameGenerator());

TEST_F(StaticFileHandlerTest, MissingFileReturns404) {
    HttpResponse res = get("/nope.txt");
    EXPECT_EQ(res.status_code, 404);
}

TEST_F(StaticFileHandlerTest, DirectoryTargetReturns404) {
    fs::create_directories(root_ / "subdir");
    HttpResponse res = get("/subdir");
    EXPECT_EQ(res.status_code, 404);
}

TEST_F(StaticFileHandlerTest, TraversalReturns404) {
    fs::create_directories(base_ / "secret");
    {
        std::ofstream ofs(root_.parent_path() / "secret" / "secret.txt");
        ofs << "SECRET";
    }
    HttpResponse res = get("/../secret/secret.txt");
    EXPECT_EQ(res.status_code, 404);
    EXPECT_NE(res.body, "SECRET");
}

std::vector<MIMETestCase> MIMETestCases = {
    {"html", "html", "text/html"},
    {"css", "css", "text/css"},
    {"js", "js", "application/javascript"},
    {"png", "png", "image/png"},
    {"jpg", "jpg", "image/jpeg"},
    {"jpeg", "jpeg", "image/jpeg"},
    {"ico", "ico", "image/x-icon"},
    {"gif", "gif", "application/octet-stream"}
};
TEST_P(MIMETest, MIMETestCase) {
    const auto& c = GetParam();
    std::string names[2];
    names[0] = c.file_name + '.' + c.file_ext;
    names[1] = c.file_name + '.';
    for (const auto ch : c.file_ext) {
        if (ch >= 'a' && ch <= 'z')
            names[1].push_back(ch - 32);
        else
            names[1].push_back(ch);
    }
    for (const std::string& name : names) {
        write_file(name, "");

        HttpResponse res = get("/" + name);
        std::string val;
        for (const auto& [key, value] : res.headers) {
            if (key == "Content-Type")
                val = value;
        }
        EXPECT_EQ(res.status_code, 200);
        EXPECT_EQ(res.body, "");
        EXPECT_EQ(val, c.expected_type);
    }
}


INSTANTIATE_TEST_SUITE_P(MIMETest, MIMETest, testing::ValuesIn(MIMETestCases), MIMETestNameGenerator());
