#include "jetson/thermal_sensor.hpp"
#include "common/sysfs_io.hpp"
#include <filesystem>

namespace fs = std::filesystem;

namespace jetson {

ThermalSensor::ThermalSensor(std::string name, std::string zone_dir)
    : name_(std::move(name)), zone_dir_(std::move(zone_dir)) {}

std::vector<ThermalSensor> ThermalSensor::discover_all() {
    std::vector<ThermalSensor> out;
    const std::string root = "/sys/class/thermal";
    if (!common::SysfsIO::exists(root)) return out;

    for (const auto& d : common::SysfsIO::list_dirs(root)) {
        auto base = fs::path(d).filename().string();
        if (base.find("thermal_zone") == std::string::npos) continue;

        auto type = common::SysfsIO::read_text(d + "/type");
        if (!type) continue;

        out.emplace_back(*type, d);
    }
    return out;
}

ThermalReading ThermalSensor::read() const {
    ThermalReading r;
    r.name = name_;
    r.temp_mc = common::SysfsIO::read_long(zone_dir_ + "/temp").value_or(-1);
    return r;
}

} // namespace jetson