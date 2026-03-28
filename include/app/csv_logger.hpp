#pragma once
#include "jetson/types.hpp"
#include <fstream>
#include <string>

namespace app {

class CsvLogger {
public:
    explicit CsvLogger(const std::string& path);

    bool ok() const;
    void write_header();
    void write_sample(const jetson::PlatformSample& s);

private:
    static long find_temp(const jetson::ThermalState& t, const std::string& key);

    std::ofstream ofs_;
};

} // namespace app