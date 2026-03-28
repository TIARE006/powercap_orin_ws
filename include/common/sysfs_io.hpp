#pragma once
#include <optional>
#include <string>
#include <vector>

namespace common {

class SysfsIO {
public:
    static std::optional<std::string> read_text(const std::string& path);
    static bool write_text(const std::string& path, const std::string& value);
    static bool exists(const std::string& path);
    static std::vector<std::string> list_dirs(const std::string& root);

    static std::optional<long> read_long(const std::string& path);
    static std::optional<long long> read_long_long(const std::string& path);
};

} // namespace common