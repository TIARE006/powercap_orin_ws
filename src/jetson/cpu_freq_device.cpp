#include "jetson/cpu_freq_device.hpp"
#include "common/sysfs_io.hpp"
#include <filesystem>

namespace fs = std::filesystem;

namespace jetson {

CpuFreqDevice::CpuFreqDevice(std::string policy_dir)
    : policy_dir_(std::move(policy_dir)) {}

std::optional<CpuFreqDevice> CpuFreqDevice::discover() {
    const std::string root = "/sys/devices/system/cpu/cpufreq";
    if (common::SysfsIO::exists(root)) {
        for (const auto& d : common::SysfsIO::list_dirs(root)) {
            auto name = fs::path(d).filename().string();
            if (name.rfind("policy", 0) == 0 &&
                common::SysfsIO::exists(d + "/scaling_cur_freq")) {
                return CpuFreqDevice(d);
            }
        }
    }

    const std::string fallback = "/sys/devices/system/cpu/cpu0/cpufreq";
    if (common::SysfsIO::exists(fallback) &&
        common::SysfsIO::exists(fallback + "/scaling_cur_freq")) {
        return CpuFreqDevice(fallback);
    }

    return std::nullopt;
}

CpuState CpuFreqDevice::read_state() const {
    CpuState s;
    s.governor   = common::SysfsIO::read_text(policy_dir_ + "/scaling_governor").value_or("");
    s.cur_khz    = common::SysfsIO::read_long(policy_dir_ + "/scaling_cur_freq").value_or(-1);
    s.min_khz    = common::SysfsIO::read_long(policy_dir_ + "/scaling_min_freq").value_or(-1);
    s.max_khz    = common::SysfsIO::read_long(policy_dir_ + "/scaling_max_freq").value_or(-1);
    s.hw_min_khz = common::SysfsIO::read_long(policy_dir_ + "/cpuinfo_min_freq").value_or(-1);
    s.hw_max_khz = common::SysfsIO::read_long(policy_dir_ + "/cpuinfo_max_freq").value_or(-1);
    return s;
}

bool CpuFreqDevice::set_frequency(long khz) const {
    const std::string v = std::to_string(khz);
    bool ok1 = common::SysfsIO::write_text(policy_dir_ + "/scaling_min_freq", v);
    bool ok2 = common::SysfsIO::write_text(policy_dir_ + "/scaling_max_freq", v);
    return ok1 && ok2;
}

bool CpuFreqDevice::unlock() const {
    auto min_khz = common::SysfsIO::read_text(policy_dir_ + "/cpuinfo_min_freq");
    auto max_khz = common::SysfsIO::read_text(policy_dir_ + "/cpuinfo_max_freq");
    if (!min_khz || !max_khz) return false;

    bool ok1 = common::SysfsIO::write_text(policy_dir_ + "/scaling_min_freq", *min_khz);
    bool ok2 = common::SysfsIO::write_text(policy_dir_ + "/scaling_max_freq", *max_khz);
    return ok1 && ok2;
}

} // namespace jetson