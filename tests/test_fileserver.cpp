#include "fileserver.h"
#include "test_helpers.h"

#include <fstream>
#include <sys/stat.h>
#include <filesystem>

// --- Test fixture: create/clean temp files ---

static const std::string TEST_DIR = "/tmp/http_fileserver_test";

static void cleanTestFiles() {
    std::filesystem::remove_all(TEST_DIR);
}

static void createTestFiles() {
    cleanTestFiles();
    mkdir(TEST_DIR.c_str(), 0755);

    {
        std::ofstream f(TEST_DIR + "/index.html");
        f << "<html><body>Hello</body></html>";
    }
    {
        std::ofstream f(TEST_DIR + "/app.js");
        f << "console.log('hello');";
    }
    {
        std::ofstream f(TEST_DIR + "/style.css");
        f << "body { color: red; }";
    }
    {
        std::ofstream f(TEST_DIR + "/data.json");
        f << "{\"key\":\"value\"}";
    }
    {
        std::ofstream f(TEST_DIR + "/icon.ico", std::ios::binary);
        f << "\x00\x00\x01\x00";
    }
    {
        std::ofstream f(TEST_DIR + "/image.png", std::ios::binary);
        std::string pngHeader = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
        f << pngHeader;
    }
    {
        std::ofstream f(TEST_DIR + "/drawing.svg");
        f << "<svg></svg>";
    }
    {
        std::ofstream f(TEST_DIR + "/readme.txt");
        f << "plain text file";
    }
}

// ============================================================
// FileServer tests
// ============================================================

static constexpr int FS_PORT = 53200;

TEST(test_fs_serve_html) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("index.html");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/index.html");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("<html><body>Hello</body></html>"));
    ASSERT_EQ(getContentType(resp), std::string("text/html"));

    server.stop();
}

TEST(test_fs_serve_js) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("app.js");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/app.js");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("console.log('hello');"));
    ASSERT_EQ(getContentType(resp), std::string("text/javascript"));

    server.stop();
}

TEST(test_fs_serve_css) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("style.css");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/style.css");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("body { color: red; }"));
    ASSERT_EQ(getContentType(resp), std::string("text/css"));

    server.stop();
}

TEST(test_fs_serve_json) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("data.json");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/data.json");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("{\"key\":\"value\"}"));
    ASSERT_EQ(getContentType(resp), std::string("application/json"));

    server.stop();
}

TEST(test_fs_serve_png) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("image.png");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/image.png");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getContentType(resp), std::string("image/png"));
    auto body = getResponseBody(resp);
    ASSERT_EQ((int)body.size(), 8);
    ASSERT_EQ((uint8_t)body[0], 0x89);
    ASSERT_EQ(body[1], 'P');

    server.stop();
}

TEST(test_fs_serve_svg) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("drawing.svg");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/drawing.svg");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("<svg></svg>"));
    ASSERT_EQ(getContentType(resp), std::string("image/svg+xml"));

    server.stop();
}

TEST(test_fs_serve_ico) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("icon.ico");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/icon.ico");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getContentType(resp), std::string("image/x-icon"));

    server.stop();
}

TEST(test_fs_unknown_extension) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("readme.txt");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/readme.txt");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("plain text file"));
    ASSERT_EQ(getContentType(resp), std::string("application/octet-stream"));

    server.stop();
}

TEST(test_fs_index_auto_root) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("index.html");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("<html><body>Hello</body></html>"));
    ASSERT_EQ(getContentType(resp), std::string("text/html"));

    server.stop();
}

TEST(test_fs_unregistered_404) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add("index.html");
    server.start();
    waitForServer(FS_PORT);

    auto resp = httpGet(FS_PORT, "/nonexistent.html");
    ASSERT_EQ(getResponseCode(resp), 404);

    server.stop();
}

TEST(test_fs_reject_slash) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    bool threw = false;
    try {
        files.add("sub/file.html");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT(threw);
}

TEST(test_fs_reject_dotdot) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    bool threw = false;
    try {
        files.add("../escape.html");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT(threw);
}

TEST(test_fs_missing_file_throws) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    bool threw = false;
    try {
        files.add("doesnotexist.html");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT(threw);
}

TEST(test_fs_batch_add) {
    Http::Server server(FS_PORT);
    Http::FileServer files(server, TEST_DIR);
    files.add({"index.html", "app.js", "style.css"});
    server.start();
    waitForServer(FS_PORT);

    auto resp1 = httpGet(FS_PORT, "/index.html");
    ASSERT_EQ(getResponseCode(resp1), 200);
    ASSERT_EQ(getContentType(resp1), std::string("text/html"));

    auto resp2 = httpGet(FS_PORT, "/app.js");
    ASSERT_EQ(getResponseCode(resp2), 200);
    ASSERT_EQ(getContentType(resp2), std::string("text/javascript"));

    auto resp3 = httpGet(FS_PORT, "/style.css");
    ASSERT_EQ(getResponseCode(resp3), 200);
    ASSERT_EQ(getContentType(resp3), std::string("text/css"));

    server.stop();
}

// ============================================================

int main() {
    createTestFiles();

    std::cout << "=== FileServer tests ===" << std::endl;
    RUN(test_fs_serve_html);
    RUN(test_fs_serve_js);
    RUN(test_fs_serve_css);
    RUN(test_fs_serve_json);
    RUN(test_fs_serve_png);
    RUN(test_fs_serve_svg);
    RUN(test_fs_serve_ico);
    RUN(test_fs_unknown_extension);
    RUN(test_fs_index_auto_root);
    RUN(test_fs_unregistered_404);
    RUN(test_fs_reject_slash);
    RUN(test_fs_reject_dotdot);
    RUN(test_fs_missing_file_throws);
    RUN(test_fs_batch_add);

    cleanTestFiles();

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;

    return failed > 0 ? 1 : 0;
}
