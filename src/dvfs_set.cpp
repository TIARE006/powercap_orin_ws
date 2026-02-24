#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

static bool write_text(const std::string& path, const std::string& val) {
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << val;
    return ofs.good();
}

static std::optional<std::string> read_text(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back()=='\n' || s.back()=='\r' || s.back()==' ' || s.back()=='\t')) s.pop_back();
    return s;
}

int main(int argc, char** argv) {
    // usage:
    //   sudo ./dvfs_set <cpu_khz> <gpu_hz>
    // Example:
    //   sudo ./dvfs_set 1344000 918000000
    if (argc < 3) {
        std::cerr << "Usage: sudo dvfs_set <cpu_khz> <gpu_hz>\n";
        return 1;
    }
    const std::string cpu_khz = argv[1];
    const std::string gpu_hz  = argv[2];

    const std::string cpu_dir = "/sys/devices/system/cpu/cpufreq/policy4";
    const std::string gpu_dir = "/sys/class/devfreq/17000000.gpu";

    // CPU: clamp min/max
    bool ok_cpu1 = write_text(cpu_dir + "/scaling_min_freq", cpu_khz);
    bool ok_cpu2 = write_text(cpu_dir + "/scaling_max_freq", cpu_khz);

    // GPU: clamp min/max (works with nvhost_podgov to “pin”)
    bool ok_gpu1 = write_text(gpu_dir + "/min_freq", gpu_hz);
    bool ok_gpu2 = write_text(gpu_dir + "/max_freq", gpu_hz);

    std::cout << "CPU write min/max: " << ok_cpu1 << "," << ok_cpu2 << "\n";
    std::cout << "GPU write min/max: " << ok_gpu1 << "," << ok_gpu2 << "\n";

    std::cout << "CPU cur(kHz)=" << (read_text(cpu_dir + "/scaling_cur_freq").value_or("NA")) << "\n";
    std::cout << "GPU cur(Hz)="  << (read_text(gpu_dir + "/cur_freq").value_or("NA")) << "\n";
    std::cout << "GPU min/max="  << read_text(gpu_dir + "/min_freq").value_or("NA")
              << "/" << read_text(gpu_dir + "/max_freq").value_or("NA") << "\n";
    return 0;
}