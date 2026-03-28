#include "common/sysfs_io.hpp"
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace common {

std::optional<std::string> SysfsIO::read_text(const std::string& path) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == EAGAIN) { ::usleep(1000); continue; }
            return std::nullopt;
        }

        char buf[4096];
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        int e = errno;
        ::close(fd);

        if (n < 0) {
            if (e == EAGAIN) { ::usleep(1000); continue; }
            return std::nullopt;
        }

        buf[n] = '\0';
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
        return s;
    }
    return std::nullopt;
}

bool SysfsIO::write_text(const std::string& path, const std::string& value) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;

    std::string v = value;
    if (v.empty() || v.back() != '\n') v.push_back('\n');

    ssize_t n = ::write(fd, v.c_str(), v.size());
    ::close(fd);
    return n == static_cast<ssize_t>(v.size());
}

bool SysfsIO::exists(const std::string& path) {
    return fs::exists(path);
}

std::vector<std::string> SysfsIO::list_dirs(const std::string& root) {
    std::vector<std::string> out;
    if (!fs::exists(root)) return out;
    for (const auto& e : fs::directory_iterator(root)) {
        if (e.is_directory()) out.push_back(e.path().string());
    }
    return out;
}

std::optional<long> SysfsIO::read_long(const std::string& path) {
    auto s = read_text(path);
    if (!s) return std::nullopt;
    try { return std::stol(*s); } catch (...) { return std::nullopt; }
}

std::optional<long long> SysfsIO::read_long_long(const std::string& path) {
    auto s = read_text(path);
    if (!s) return std::nullopt;
    try { return std::stoll(*s); } catch (...) { return std::nullopt; }
}

} // namespace common