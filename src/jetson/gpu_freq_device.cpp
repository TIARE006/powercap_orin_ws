#include "jetson/gpu_freq_device.hpp"
#include "common/sysfs_io.hpp"
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace jetson {

GpuFreqDevice::GpuFreqDevice(std::string devfreq_dir)
    : devfreq_dir_(std::move(devfreq_dir)) {}

std::optional<GpuFreqDevice> GpuFreqDevice::discover() {
    const std::string root = "/sys/class/devfreq";
    if (!common::SysfsIO::exists(root)) return std::nullopt;

    auto is_blacklisted = [](const std::string& name) {
        return name.find("nvjpg") != std::string::npos ||
               name.find("nvenc") != std::string::npos ||
               name.find("nvdec") != std::string::npos ||
               name.find("vic")   != std::string::npos ||
               name.find("se")    != std::string::npos;
    };

    for (const auto& d : common::SysfsIO::list_dirs(root)) {
        auto name = fs::path(d).filename().string();
        if (is_blacklisted(name)) continue;
        if ((name.find("gpu") != std::string::npos || name.find("ga10b") != std::string::npos) &&
            common::SysfsIO::exists(d + "/cur_freq") &&
            common::SysfsIO::exists(d + "/available_frequencies")) {
            return GpuFreqDevice(d);
        }
    }

    for (const auto& d : common::SysfsIO::list_dirs(root)) {
        auto name = fs::path(d).filename().string();
        if (is_blacklisted(name)) continue;
        if (common::SysfsIO::exists(d + "/cur_freq") &&
            common::SysfsIO::exists(d + "/available_frequencies")) {
            return GpuFreqDevice(d);
        }
    }

    return std::nullopt;
}

std::vector<long long> GpuFreqDevice::parse_available_frequencies(const std::string& s) {
    std::vector<long long> out;
    std::istringstream iss(s);
    long long x;
    while (iss >> x) out.push_back(x);
    return out;
}

GpuState GpuFreqDevice::read_state() const {
    GpuState s;
    s.governor = common::SysfsIO::read_text(devfreq_dir_ + "/governor").value_or("");
    s.cur_hz   = common::SysfsIO::read_long_long(devfreq_dir_ + "/cur_freq").value_or(-1);
    s.min_hz   = common::SysfsIO::read_long_long(devfreq_dir_ + "/min_freq").value_or(-1);
    s.max_hz   = common::SysfsIO::read_long_long(devfreq_dir_ + "/max_freq").value_or(-1);

    auto af = common::SysfsIO::read_text(devfreq_dir_ + "/available_frequencies");
    if (af) s.available_hz = parse_available_frequencies(*af);

    return s;
}

bool GpuFreqDevice::set_frequency(long long hz) const {
    const std::string v = std::to_string(hz);
    bool ok1 = common::SysfsIO::write_text(devfreq_dir_ + "/min_freq", v);
    bool ok2 = common::SysfsIO::write_text(devfreq_dir_ + "/max_freq", v);
    return ok1 && ok2;
}

bool GpuFreqDevice::unlock() const {
    auto s = read_state();
    if (s.available_hz.empty()) return false;

    const auto min_hz = std::to_string(s.available_hz.front());
    const auto max_hz = std::to_string(s.available_hz.back());

    bool ok1 = common::SysfsIO::write_text(devfreq_dir_ + "/min_freq", min_hz);
    bool ok2 = common::SysfsIO::write_text(devfreq_dir_ + "/max_freq", max_hz);

    bool ok3 = true;
    if (common::SysfsIO::exists(devfreq_dir_ + "/governor")) {
        ok3 = common::SysfsIO::write_text(devfreq_dir_ + "/governor", "nvhost_podgov");
    }

    return ok1 && ok2 && ok3;
}

} // namespace jetson