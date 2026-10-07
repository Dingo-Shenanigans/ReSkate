// What the Linux server's self-update accepts from a release: launcher.json's "server_linux"
// entry, and the list of files in the tarball.
#include "Server/server_release.h"
#include <iostream>
#include <stdexcept>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
template <class Call> bool throws(Call &&call) {
    try {
        call();
    } catch (const std::exception &) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace dingosdk::server;
    const std::string sha(64, 'a'), exe(64, 'b');
    const auto assets = release_assets(
        R"({"tag_name":"v1.2.0","assets":[{"name":"launcher.json","browser_download_url":"https://github.com/o/r/releases/download/v1.2.0/launcher.json"},)"
        R"({"name":"ReSkateServer-Linux-1.2.0.tar.gz","browser_download_url":"https://github.com/o/r/releases/download/v1.2.0/ReSkateServer-Linux-1.2.0.tar.gz"}]})");
    check(assets.size() == 2 && assets.contains("launcher.json"), "A release's assets were not read");
    check(throws([] { release_assets(R"({"message":"API rate limit exceeded"})"); }), "An answer that is not a release was accepted");

    const auto entry = [&](std::string url, std::string hash, std::string exe_hash, std::string size, std::string version = "1.2.0") {
        return R"({"schema":1,"server":{"url":"asset:ReSkateServer-1.2.0.zip"},"server_linux":{"version":")" + version + R"(","url":")" + url +
               R"(","sha256":")" + hash + R"(","size":)" + size + R"(,"exe_sha256":")" + exe_hash + R"("}})";
    };
    const auto good = parse_linux_release(entry("asset:ReSkateServer-Linux-1.2.0.tar.gz", sha, exe, "31000000"), assets);
    check(good && good->version == "1.2.0" && good->size == 31000000 && good->sha256 == sha && good->exe_sha256 == exe &&
              good->url == "https://github.com/o/r/releases/download/v1.2.0/ReSkateServer-Linux-1.2.0.tar.gz",
          "A Linux release was not read, or its asset was not resolved");
    const auto direct = parse_linux_release(entry("https://example.com/server.tar.gz", sha, exe, "1"), {});
    check(direct && direct->url == "https://example.com/server.tar.gz", "A direct HTTPS link was not kept");
    // A release from before Linux servers updated themselves has no entry: nothing to do.
    check(!parse_linux_release(R"({"schema":1,"server":{"url":"asset:ReSkateServer-1.1.3.zip"}})", assets), "A release with no Linux server gave one");
    for (const auto &wrong : {entry("asset:missing.tar.gz", sha, exe, "1"), entry("http://example.com/s.tar.gz", sha, exe, "1"),
                              entry("https://example.com/s.tar.gz --output /etc/passwd", sha, exe, "1"),
                              entry("https://example.com/s.tar.gz", std::string(64, 'A'), exe, "1"),
                              entry("https://example.com/s.tar.gz", sha, "abc", "1"), entry("https://example.com/s.tar.gz", sha, exe, "0"),
                              entry("https://example.com/s.tar.gz", sha, exe, "999999999999"),
                              entry("https://example.com/s.tar.gz", sha, exe, "-5"), entry("https://example.com/s.tar.gz", sha, exe, "1", "1.2; rm"),
                              std::string(R"({"server_linux":"yes"})"), std::string("[]")})
        check(throws([&] { parse_linux_release(wrong, assets); }), ("A wrong Linux release was accepted: " + wrong).c_str());

    check(https_url("https://github.com/o/r/releases/download/v1/a.tar.gz?x=1&y=%20") && !https_url("https://") &&
              !https_url("file:///etc/passwd") && !https_url("https://a b") && !https_url("https://a\nb") && !https_url("-o x"),
          "Only plain HTTPS links may be handed to curl");

    check(checked_archive_listing("ReSkateServer-Linux-1.2.0/\nReSkateServer-Linux-1.2.0/ReSkateServer\n"
                                  "ReSkateServer-Linux-1.2.0/licenses/\nReSkateServer-Linux-1.2.0/licenses/zstd-LICENSE.txt\n") ==
              "ReSkateServer-Linux-1.2.0",
          "A release tarball's listing was refused");
    check(checked_archive_listing("./pkg/\r\n./pkg/ReSkateServer\r\n") == "pkg", "A listing written with ./ was refused");
    for (const char *wrong : {"", "pkg/\n", "pkg/ReSkateServer\nother/file\n", "pkg/a\n/etc/passwd\n", "pkg/a\npkg/../../etc/passwd\n",
                              "pkg/a\npkg//b\n", "pkg/a\npkg\\b\n", "../a\n../b\n", "pkg/a\n./\n"})
        check(throws([&] { checked_archive_listing(wrong); }), "A tarball that is not one plain folder was accepted");

    if (!failures) std::cout << "server release: ok\n";
    return failures ? 1 : 0;
}
