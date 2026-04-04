#include "jetson/fan_device.hpp"
#include "common/sysfs_io.hpp"
#include <filesystem>

namespace fs = std::filesystem;

namespace jetson {

FanDevice::FanDevice(std::string cooling_dir, std::string pwm_path)
    : cooling_dir_(std::move(cooling_dir)), pwm_path_(std::move(pwm_path)) {}

std::optional<FanDevice> FanDevice::discover() {
    const std::string root = "/sys/class/thermal";
    if (!common::SysfsIO::exists(root)) return std::nullopt;

    for (const auto& d : common::SysfsIO::list_dirs(root)) {
        auto base = fs::path(d).filename().string();
        if (base.rfind("cooling_device", 0) != 0) continue;

        auto type = common::SysfsIO::read_text(d + "/type");
        if (type && type->find("pwm-fan") != std::string::npos) {
            return FanDevice(d, "/sys/devices/platform/pwm-fan/hwmon/hwmon0/pwm1");
        }
    }
    return std::nullopt;
}

FanState FanDevice::read_state() const {
    FanState s;
    s.cur_state = common::SysfsIO::read_long(cooling_dir_ + "/cur_state").value_or(-1);
    s.max_state = common::SysfsIO::read_long(cooling_dir_ + "/max_state").value_or(-1);
    s.pwm = common::SysfsIO::read_long(pwm_path_).value_or(-1);
    return s;
}

} // namespace jetson