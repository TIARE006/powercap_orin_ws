#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace fs = std::filesystem;
static volatile std::sig_atomic_t g_stop = 0;

static void on_sigint(int) { g_stop = 1; }

static std::optional<std::string> read_text(const std::string& path) {
    try {
        std::ifstream ifs(path);
        if (!ifs) return std::nullopt;
        std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        while (!s.empty() && (s.back()=='\n' || s.back()=='\r' || s.back()==' ' || s.back()=='\t')) s.pop_back();
        return s;
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<std::string> find_thermal_zone_of(const std::string& keyword) {
    const std::string tzroot = "/sys/class/thermal";
    if (!fs::exists(tzroot)) return std::nullopt;
    for (auto const& e : fs::directory_iterator(tzroot)) {
        if (!e.is_directory()) continue;
        auto name = e.path().filename().string();
        if (name.find("thermal_zone") == std::string::npos) continue;
        auto type = read_text(e.path().string() + "/type");
        if (type && type->find(keyword) != std::string::npos) return e.path().string();
    }
    return std::nullopt;
}

static int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static std::string val_or_empty(const std::optional<std::string>& v) { return v ? *v : ""; }

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);

    int period_ms = 100;
    std::string out_csv = "../logs/run.csv";
    if (argc >= 2) out_csv = argv[1];
    if (argc >= 3) period_ms = std::stoi(argv[2]);

    // Fixed paths from your probe
    const std::string cpu_dir = "/sys/devices/system/cpu/cpufreq/policy4";
    const std::string gpu_dir = "/sys/class/devfreq/17000000.gpu";

    // thermal zones by type string
    auto tz_cpu = find_thermal_zone_of("cpu-thermal");
    auto tz_gpu = find_thermal_zone_of("gpu-thermal");

    std::ofstream ofs(out_csv);
    if (!ofs) {
        std::cerr << "Failed to open output: " << out_csv << "\n";
        return 1;
    }

    ofs << "ts_ns,cpu_khz,cpu_min_khz,cpu_max_khz,gpu_hz,gpu_min_hz,gpu_max_hz,"
           "temp_cpu_mC,temp_gpu_mC,gpu_governor\n";
    ofs.flush();

    std::cout << "Logging to " << out_csv << " period=" << period_ms << "ms\n";
    std::cout << "Ctrl+C to stop.\n";

    while (!g_stop) {
        const int64_t ts = now_ns();

        auto cpu_khz = read_text(cpu_dir + "/scaling_cur_freq");
        auto cpu_min = read_text(cpu_dir + "/scaling_min_freq");
        auto cpu_max = read_text(cpu_dir + "/scaling_max_freq");

        auto gpu_hz  = read_text(gpu_dir + "/cur_freq");
        auto gpu_min = read_text(gpu_dir + "/min_freq");
        auto gpu_max = read_text(gpu_dir + "/max_freq");
        auto gov     = read_text(gpu_dir + "/governor");

        std::string t_cpu = tz_cpu ? val_or_empty(read_text(*tz_cpu + "/temp")) : "";
        std::string t_gpu = tz_gpu ? val_or_empty(read_text(*tz_gpu + "/temp")) : "";

        ofs << ts << ","
            << val_or_empty(cpu_khz) << "," << val_or_empty(cpu_min) << "," << val_or_empty(cpu_max) << ","
            << val_or_empty(gpu_hz)  << "," << val_or_empty(gpu_min) << "," << val_or_empty(gpu_max) << ","
            << t_cpu << "," << t_gpu << ","
            << val_or_empty(gov) << "\n";
        ofs.flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
    }

    std::cout << "Stopped.\n";
    return 0;
}