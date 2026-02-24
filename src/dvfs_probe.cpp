#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

namespace fs = std::filesystem;

static std::optional<std::string> read_text(const std::string& path) {
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
        while (!s.empty() && (s.back()=='\n' || s.back()=='\r' || s.back()==' ' || s.back()=='\t')) s.pop_back();
        return s;
    }
    return std::nullopt;
}

static bool exists(const std::string& p) { return fs::exists(p); }

static std::vector<std::string> glob_dirs(const std::string& root) {
    std::vector<std::string> out;
    if (!fs::exists(root)) return out;
    for (auto const& e : fs::directory_iterator(root)) {
        if (e.is_directory()) out.push_back(e.path().string());
    }
    return out;
}

static std::optional<std::string> find_cpu_policy_dir() {
    // Try common locations:
    // /sys/devices/system/cpu/cpufreq/policy0
    // /sys/devices/system/cpu/cpu0/cpufreq
    const std::string a = "/sys/devices/system/cpu/cpufreq";
    if (exists(a)) {
        for (auto& d : glob_dirs(a)) {
            if (d.find("policy") != std::string::npos) return d;
        }
    }
    const std::string b = "/sys/devices/system/cpu/cpu0/cpufreq";
    if (exists(b)) return b;
    return std::nullopt;
}

static std::optional<std::string> find_gpu_devfreq_dir() {
    const std::string root = "/sys/class/devfreq";
    if (!exists(root)) return std::nullopt;

    auto is_blacklisted = [](const std::string& name) {
        // Not GPU: multimedia accelerators
        return (name.find("nvjpg") != std::string::npos) ||
               (name.find("nvenc") != std::string::npos) ||
               (name.find("nvdec") != std::string::npos) ||
               (name.find("vic")   != std::string::npos) ||
               (name.find("se")    != std::string::npos);
    };

    // Pass 1: prefer ga10b / gpu-like names
    for (auto& d : glob_dirs(root)) {
        std::string name = fs::path(d).filename().string();
        if (is_blacklisted(name)) continue;
        if ((name.find("ga10b") != std::string::npos || name.find("gpu") != std::string::npos) &&
            exists(d + "/cur_freq") && exists(d + "/available_frequencies")) {
            return d;
        }
    }

    // Pass 2: any devfreq with cur_freq + available_frequencies, excluding blacklist
    for (auto& d : glob_dirs(root)) {
        std::string name = fs::path(d).filename().string();
        if (is_blacklisted(name)) continue;
        if (exists(d + "/cur_freq") && exists(d + "/available_frequencies")) return d;
    }

    return std::nullopt;
}

static void print_kv(const std::string& k, const std::optional<std::string>& v) {
    std::cout << k << ": " << (v ? *v : "<N/A>") << "\n";
}

int main() {
    std::cout << "=== dvfs_probe ===\n";

    // CPU
    auto cpu_dir = find_cpu_policy_dir();
    std::cout << "\n[CPU cpufreq]\n";
    if (!cpu_dir) {
        std::cout << "cpu cpufreq dir not found.\n";
    } else {
        std::cout << "dir: " << *cpu_dir << "\n";
        print_kv("scaling_governor", read_text(*cpu_dir + "/scaling_governor"));
        print_kv("scaling_cur_freq(kHz)", read_text(*cpu_dir + "/scaling_cur_freq"));
        print_kv("scaling_min_freq(kHz)", read_text(*cpu_dir + "/scaling_min_freq"));
        print_kv("scaling_max_freq(kHz)", read_text(*cpu_dir + "/scaling_max_freq"));
        print_kv("available_frequencies(kHz)", read_text(*cpu_dir + "/scaling_available_frequencies"));
        // Some platforms use cpuinfo_* files
        print_kv("cpuinfo_min_freq(kHz)", read_text(*cpu_dir + "/cpuinfo_min_freq"));
        print_kv("cpuinfo_max_freq(kHz)", read_text(*cpu_dir + "/cpuinfo_max_freq"));
    }

    // GPU
    auto gpu_dir = find_gpu_devfreq_dir();
    std::cout << "\n[GPU devfreq]\n";
    if (!gpu_dir) {
        std::cout << "gpu devfreq dir not found under /sys/class/devfreq.\n";
        std::cout << "Try: ls /sys/class/devfreq\n";
    } else {
        std::cout << "dir: " << *gpu_dir << "\n";
        print_kv("cur_freq(Hz)", read_text(*gpu_dir + "/cur_freq"));
        print_kv("min_freq(Hz)", read_text(*gpu_dir + "/min_freq"));
        print_kv("max_freq(Hz)", read_text(*gpu_dir + "/max_freq"));
        print_kv("available_frequencies(Hz)", read_text(*gpu_dir + "/available_frequencies"));
        // governor exists on some devfreq devices
        print_kv("governor", read_text(*gpu_dir + "/governor"));
    }

    // Temps (optional)
    std::cout << "\n[Temps (thermal_zone)]\n";
    const std::string tzroot = "/sys/class/thermal";
    if (exists(tzroot)) {
        int shown = 0;
        for (auto& d : glob_dirs(tzroot)) {
            if (fs::path(d).filename().string().find("thermal_zone") == std::string::npos) continue;
            auto type = read_text(d + "/type");
            auto temp = read_text(d + "/temp"); // usually milli-C
            if (type && temp) {
                std::cout << fs::path(d).filename().string() << "  type=" << *type
                          << "  temp=" << *temp << "\n";
                if (++shown >= 12) break; // don't spam
            }
        }
    } else {
        std::cout << "No /sys/class/thermal\n";
    }

    std::cout << "\nDone.\n";
    return 0;
}