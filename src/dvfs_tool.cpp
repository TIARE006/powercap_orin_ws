// dvfs_tool.cpp
// Build: g++ -O2 -std=c++17 dvfs_tool.cpp -o dvfs_tool
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstdio>

namespace fs = std::filesystem;

static volatile std::sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

// ---------- low-level sysfs I/O ----------
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

static bool write_text(const std::string& path, const std::string& val) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    std::string v = val;
    if (v.empty() || v.back() != '\n') v.push_back('\n');
    ssize_t n = ::write(fd, v.c_str(), v.size());
    ::close(fd);
    return n == (ssize_t)v.size();
}

static bool exists(const std::string& p) { return fs::exists(p); }

static std::vector<std::string> list_dirs(const std::string& root) {
    std::vector<std::string> out;
    if (!fs::exists(root)) return out;
    for (auto const& e : fs::directory_iterator(root)) {
        if (e.is_directory()) out.push_back(e.path().string());
    }
    return out;
}

// ---------- discovery ----------
static std::optional<std::string> find_cpu_policy_dir() {
    const std::string a = "/sys/devices/system/cpu/cpufreq";
    if (exists(a)) {
        for (auto& d : list_dirs(a)) {
            if (fs::path(d).filename().string().rfind("policy", 0) == 0 &&
                exists(d + "/scaling_cur_freq")) {
                return d;
            }
        }
    }
    const std::string b = "/sys/devices/system/cpu/cpu0/cpufreq";
    if (exists(b) && exists(b + "/scaling_cur_freq")) return b;
    return std::nullopt;
}

static std::optional<std::string> find_gpu_devfreq_dir() {
    const std::string root = "/sys/class/devfreq";
    if (!exists(root)) return std::nullopt;

    auto is_blacklisted = [](const std::string& name) {
        return (name.find("nvjpg") != std::string::npos) ||
               (name.find("nvenc") != std::string::npos) ||
               (name.find("nvdec") != std::string::npos) ||
               (name.find("vic")   != std::string::npos) ||
               (name.find("se")    != std::string::npos);
    };

    // Prefer obvious GPU-like names
    for (auto& d : list_dirs(root)) {
        std::string name = fs::path(d).filename().string();
        if (is_blacklisted(name)) continue;
        if ((name.find("ga10b") != std::string::npos || name.find("gpu") != std::string::npos) &&
            exists(d + "/cur_freq") && exists(d + "/available_frequencies")) {
            return d;
        }
    }
    // Fallback
    for (auto& d : list_dirs(root)) {
        std::string name = fs::path(d).filename().string();
        if (is_blacklisted(name)) continue;
        if (exists(d + "/cur_freq") && exists(d + "/available_frequencies")) return d;
    }
    return std::nullopt;
}

static std::optional<std::string> find_thermal_zone_by_keywords(const std::vector<std::string>& kws) {
    const std::string tzroot = "/sys/class/thermal";
    if (!exists(tzroot)) return std::nullopt;
    for (auto const& e : fs::directory_iterator(tzroot)) {
        if (!e.is_directory()) continue;
        auto name = e.path().filename().string();
        if (name.find("thermal_zone") == std::string::npos) continue;
        auto type = read_text(e.path().string() + "/type");
        if (!type) continue;
        for (auto& kw : kws) {
            if (type->find(kw) != std::string::npos) return e.path().string();
        }
    }
    return std::nullopt;
}

static void print_kv(const std::string& k, const std::optional<std::string>& v) {
    std::cout << k << ": " << (v ? *v : "<N/A>") << "\n";
}

// ---------- tiny arg parsing ----------
static std::optional<std::string> get_flag(int argc, char** argv, const std::string& flag) {
    for (int i = 0; i + 1 < argc; ++i) {
        if (argv[i] == flag) return std::string(argv[i + 1]);
    }
    return std::nullopt;
}
static bool has_flag(int argc, char** argv, const std::string& flag) {
    for (int i = 0; i < argc; ++i) if (argv[i] == flag) return true;
    return false;
}

static void usage() {
    std::cout <<
R"(Usage:
  dvfs_tool probe
  dvfs_tool log   --out <csv> --period_ms <ms> [--watch] [--watch_ms <ms>]

  # Writes are locked by default (dry-run). Add --apply to actually write sysfs:
  sudo dvfs_tool set    --cpu_khz <kHz> --gpu_hz <Hz> [--apply]
  sudo dvfs_tool unlock [--apply]

Examples:
  dvfs_tool probe
  dvfs_tool log --out logs/run.csv --period_ms 100
  dvfs_tool log --out logs/run.csv --period_ms 100 --watch --watch_ms 200

  # Dry-run (prints what would be written)
  sudo dvfs_tool set --cpu_khz 1344000 --gpu_hz 918000000
  sudo dvfs_tool unlock

  # Actually apply
  sudo dvfs_tool set --cpu_khz 1344000 --gpu_hz 918000000 --apply
  sudo dvfs_tool unlock --apply
)";
}

// ---------- watch-like formatting ----------
static std::string fmt_temp_C_1dp(const std::optional<std::string>& temp_mC) {
    if (!temp_mC || temp_mC->empty()) return "NA";
    try {
        long long mc = std::stoll(*temp_mC);
        double c = mc / 1000.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", c);
        return std::string(buf);
    } catch (...) {
        return "NA";
    }
}

static void print_watch_block(
    bool& initialized,
    const std::optional<std::string>& gpu_cur,
    const std::optional<std::string>& gpu_min,
    const std::optional<std::string>& gpu_max,
    const std::optional<std::string>& gpu_gov,
    const std::optional<std::string>& t_cpu,
    const std::optional<std::string>& t_gpu,
    const std::optional<std::string>& t_soc0,
    const std::optional<std::string>& t_soc1,
    const std::optional<std::string>& t_soc2,
    const std::optional<std::string>& t_tj
) {
    // Reserve 2 lines on first call
    if (!initialized) {
        std::cerr << "\n\n";
        initialized = true;
    } else {
        // Move cursor up 2 lines
        std::cerr << "\033[2A";
    }

    auto v = [](const std::optional<std::string>& x) { return x ? *x : std::string("NA"); };

    // Line 1
    std::cerr << "\033[2K\r"
              << "GPUfreq: cur=" << v(gpu_cur)
              << " min=" << v(gpu_min)
              << " max=" << v(gpu_max)
              << " gov=" << v(gpu_gov)
              << "\n";

    // Line 2
    std::cerr << "\033[2K\r"
              << "Temps: CPU " << fmt_temp_C_1dp(t_cpu) << "C"
              << " | GPU "  << fmt_temp_C_1dp(t_gpu)  << "C"
              << " | SOC0 " << fmt_temp_C_1dp(t_soc0) << "C"
              << " | SOC1 " << fmt_temp_C_1dp(t_soc1) << "C"
              << " | SOC2 " << fmt_temp_C_1dp(t_soc2) << "C"
              << " | TJ "   << fmt_temp_C_1dp(t_tj)   << "C"
              << "\n";

    std::cerr << std::flush;
}

// ---------- subcommands ----------
static int cmd_probe() {
    std::cout << "=== dvfs_tool probe ===\n";

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
        print_kv("scaling_available_frequencies(kHz)", read_text(*cpu_dir + "/scaling_available_frequencies"));
        print_kv("cpuinfo_min_freq(kHz)", read_text(*cpu_dir + "/cpuinfo_min_freq"));
        print_kv("cpuinfo_max_freq(kHz)", read_text(*cpu_dir + "/cpuinfo_max_freq"));
    }

    auto gpu_dir = find_gpu_devfreq_dir();
    std::cout << "\n[GPU devfreq]\n";
    if (!gpu_dir) {
        std::cout << "gpu devfreq dir not found under /sys/class/devfreq.\n";
    } else {
        std::cout << "dir: " << *gpu_dir << "\n";
        print_kv("cur_freq(Hz)", read_text(*gpu_dir + "/cur_freq"));
        print_kv("min_freq(Hz)", read_text(*gpu_dir + "/min_freq"));
        print_kv("max_freq(Hz)", read_text(*gpu_dir + "/max_freq"));
        print_kv("available_frequencies(Hz)", read_text(*gpu_dir + "/available_frequencies"));
        print_kv("governor", read_text(*gpu_dir + "/governor"));
    }

    std::cout << "\n[Temps (thermal_zone, first ~12)]\n";
    const std::string tzroot = "/sys/class/thermal";
    if (exists(tzroot)) {
        int shown = 0;
        for (auto& d : list_dirs(tzroot)) {
            if (fs::path(d).filename().string().find("thermal_zone") == std::string::npos) continue;
            auto type = read_text(d + "/type");
            auto temp = read_text(d + "/temp");
            if (type && temp) {
                std::cout << fs::path(d).filename().string() << "  type=" << *type
                          << "  temp=" << *temp << "\n";
                if (++shown >= 12) break;
            }
        }
    } else {
        std::cout << "No /sys/class/thermal\n";
    }

    return 0;
}

static int cmd_set(int argc, char** argv) {
    auto cpu_khz = get_flag(argc, argv, "--cpu_khz");
    auto gpu_hz  = get_flag(argc, argv, "--gpu_hz");
    bool apply   = has_flag(argc, argv, "--apply");

    if (!cpu_khz || !gpu_hz) {
        std::cerr << "set requires --cpu_khz and --gpu_hz\n";
        return 2;
    }

    auto cpu_dir = find_cpu_policy_dir();
    auto gpu_dir = find_gpu_devfreq_dir();
    if (!cpu_dir || !gpu_dir) {
        std::cerr << "Failed to discover cpu/gpu sysfs dirs. Run: dvfs_tool probe\n";
        return 3;
    }

    const std::string cpu_min_p = *cpu_dir + "/scaling_min_freq";
    const std::string cpu_max_p = *cpu_dir + "/scaling_max_freq";
    const std::string gpu_min_p = *gpu_dir + "/min_freq";
    const std::string gpu_max_p = *gpu_dir + "/max_freq";

    std::cout << "CPU dir: " << *cpu_dir << "\n";
    std::cout << "GPU dir: " << *gpu_dir << "\n";
    std::cout << "Will write:\n";
    std::cout << "  " << cpu_min_p << " = " << *cpu_khz << "\n";
    std::cout << "  " << cpu_max_p << " = " << *cpu_khz << "\n";
    std::cout << "  " << gpu_min_p << " = " << *gpu_hz  << "\n";
    std::cout << "  " << gpu_max_p << " = " << *gpu_hz  << "\n";

    if (!apply) {
        std::cout << "Dry-run (no sysfs writes). Add --apply to actually write.\n";
        return 0;
    }

    bool ok_cpu1 = write_text(cpu_min_p, *cpu_khz);
    bool ok_cpu2 = write_text(cpu_max_p, *cpu_khz);
    bool ok_gpu1 = write_text(gpu_min_p, *gpu_hz);
    bool ok_gpu2 = write_text(gpu_max_p, *gpu_hz);

    std::cout << "Applied.\n";
    std::cout << "CPU write min/max: " << ok_cpu1 << "," << ok_cpu2 << "\n";
    std::cout << "GPU write min/max: " << ok_gpu1 << "," << ok_gpu2 << "\n";

    print_kv("CPU cur(kHz)", read_text(*cpu_dir + "/scaling_cur_freq"));
    print_kv("GPU cur(Hz)",  read_text(*gpu_dir + "/cur_freq"));
    print_kv("GPU min/max",  read_text(*gpu_dir + "/min_freq").value_or("<N/A>") + std::string("/") +
                             read_text(*gpu_dir + "/max_freq").value_or("<N/A>"));

    return (ok_cpu1 && ok_cpu2 && ok_gpu1 && ok_gpu2) ? 0 : 4;
}

static int cmd_unlock(int argc, char** argv) {
    bool apply = has_flag(argc, argv, "--apply");

    auto cpu_dir = find_cpu_policy_dir();
    auto gpu_dir = find_gpu_devfreq_dir();
    if (!cpu_dir || !gpu_dir) {
        std::cerr << "Failed to discover cpu/gpu sysfs dirs. Run: dvfs_tool probe\n";
        return 3;
    }

    auto cmin = read_text(*cpu_dir + "/cpuinfo_min_freq");
    auto cmax = read_text(*cpu_dir + "/cpuinfo_max_freq");

    const std::string cpu_min_p = *cpu_dir + "/scaling_min_freq";
    const std::string cpu_max_p = *cpu_dir + "/scaling_max_freq";
    const std::string gpu_min_p = *gpu_dir + "/min_freq";
    const std::string gpu_max_p = *gpu_dir + "/max_freq";
    const std::string gov_p     = *gpu_dir + "/governor";

    // Pick GPU max default from available_frequencies (last token), fallback to "infinite"
    std::string gpu_max_default = "9223372036854775807";
    if (auto af = read_text(*gpu_dir + "/available_frequencies"); af && !af->empty()) {
        std::string s = *af;
        while (!s.empty() && (s.back()==' ' || s.back()=='\t' || s.back()=='\n' || s.back()=='\r')) s.pop_back();
        size_t pos = s.find_last_of(" \t");
        gpu_max_default = (pos == std::string::npos) ? s : s.substr(pos + 1);
        if (gpu_max_default.empty()) gpu_max_default = "9223372036854775807";
    }

    std::cout << "CPU dir: " << *cpu_dir << "\n";
    std::cout << "GPU dir: " << *gpu_dir << "\n";
    std::cout << "Will write:\n";
    if (cmin) std::cout << "  " << cpu_min_p << " = " << *cmin << " (cpuinfo_min_freq)\n";
    else      std::cout << "  " << cpu_min_p << " = <skip> (cpuinfo_min_freq missing)\n";
    if (cmax) std::cout << "  " << cpu_max_p << " = " << *cmax << " (cpuinfo_max_freq)\n";
    else      std::cout << "  " << cpu_max_p << " = <skip> (cpuinfo_max_freq missing)\n";

    std::cout << "  " << gpu_min_p << " = 0\n";
    std::cout << "  " << gpu_max_p << " = " << gpu_max_default
              << " (from available_frequencies if present)\n";
    if (exists(gov_p)) std::cout << "  " << gov_p << " = nvhost_podgov\n";
    else               std::cout << "  " << gov_p << " = <skip> (no governor file)\n";

    if (!apply) {
        std::cout << "Dry-run (no sysfs writes). Add --apply to actually write.\n";
        return 0;
    }

    bool ok_cpu1 = cmin ? write_text(cpu_min_p, *cmin) : true;
    bool ok_cpu2 = cmax ? write_text(cpu_max_p, *cmax) : true;

    bool ok_gpu1 = write_text(gpu_min_p, "0");
    bool ok_gpu2 = write_text(gpu_max_p, gpu_max_default);
    bool ok_gov  = true;
    if (exists(gov_p)) ok_gov = write_text(gov_p, "nvhost_podgov");

    std::cout << "Applied.\n";
    std::cout << "CPU unlock ok: " << ok_cpu1 << "," << ok_cpu2 << "\n";
    std::cout << "GPU unlock ok: " << ok_gpu1 << "," << ok_gpu2 << " governor_ok=" << ok_gov << "\n";
    return (ok_cpu1 && ok_cpu2 && ok_gpu1 && ok_gpu2 && ok_gov) ? 0 : 4;
}

static int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static int cmd_log(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);

    std::string out = get_flag(argc, argv, "--out").value_or("run.csv");
    int period_ms = 100;
    if (auto p = get_flag(argc, argv, "--period_ms")) period_ms = std::stoi(*p);
    if (period_ms <= 0) period_ms = 100;

    bool watch_mode = has_flag(argc, argv, "--watch");
    int watch_ms = 200;
    if (auto w = get_flag(argc, argv, "--watch_ms")) watch_ms = std::stoi(*w);
    if (watch_ms <= 0) watch_ms = 200;

    auto cpu_dir = find_cpu_policy_dir();
    auto gpu_dir = find_gpu_devfreq_dir();
    if (!cpu_dir || !gpu_dir) {
        std::cerr << "Failed to discover cpu/gpu sysfs dirs. Run: dvfs_tool probe\n";
        return 3;
    }

    // Temperatures: try to match commonly-used names on Jetson.
    auto tz_cpu  = find_thermal_zone_by_keywords({"CPU-therm","cpu"});
    auto tz_gpu  = find_thermal_zone_by_keywords({"GPU-therm","gpu","ga10b"});
    auto tz_soc0 = find_thermal_zone_by_keywords({"soc0-thermal","SOC0","soc0"});
    auto tz_soc1 = find_thermal_zone_by_keywords({"soc1-thermal","SOC1","soc1"});
    auto tz_soc2 = find_thermal_zone_by_keywords({"soc2-thermal","SOC2","soc2"});
    auto tz_tj   = find_thermal_zone_by_keywords({"tj-thermal","TJ","tj"});

    std::ofstream ofs(out);
    if (!ofs) {
        std::cerr << "Failed to open: " << out << "\n";
        return 1;
    }

    ofs << "ts_ns,dt_ns,cpu_khz,cpu_min_khz,cpu_max_khz,"
           "gpu_hz,gpu_min_hz,gpu_max_hz,gpu_governor,"
           "temp_cpu_mC,temp_gpu_mC,temp_soc0_mC,temp_soc1_mC,temp_soc2_mC,temp_tj_mC\n";
    ofs.flush();

    if (!watch_mode) {
        std::cerr << "Logging to " << out << " period=" << period_ms << "ms\n";
        std::cerr << "cpu_dir=" << *cpu_dir << "\n";
        std::cerr << "gpu_dir=" << *gpu_dir << "\n";
        std::cerr << "tz_cpu="  << (tz_cpu  ? *tz_cpu  : "NOT_FOUND") << "\n";
        std::cerr << "tz_gpu="  << (tz_gpu  ? *tz_gpu  : "NOT_FOUND") << "\n";
        std::cerr << "tz_soc0=" << (tz_soc0 ? *tz_soc0 : "NOT_FOUND") << "\n";
        std::cerr << "tz_soc1=" << (tz_soc1 ? *tz_soc1 : "NOT_FOUND") << "\n";
        std::cerr << "tz_soc2=" << (tz_soc2 ? *tz_soc2 : "NOT_FOUND") << "\n";
        std::cerr << "tz_tj="   << (tz_tj   ? *tz_tj   : "NOT_FOUND") << "\n";
    } else {
        std::cerr << "Logging to " << out << " period=" << period_ms << "ms (watch)\n";
    }

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    int line_cnt = 0;
    int64_t prev_ts = 0;
    int64_t last_watch_ns = 0;
    bool watch_initialized = false;

    while (!g_stop) {
        next += std::chrono::milliseconds(period_ms);

        const int64_t ts = now_ns();
        const int64_t dt = (prev_ts == 0) ? 0 : (ts - prev_ts);
        prev_ts = ts;

        auto cpu_khz = read_text(*cpu_dir + "/scaling_cur_freq");
        auto cpu_min = read_text(*cpu_dir + "/scaling_min_freq");
        auto cpu_max = read_text(*cpu_dir + "/scaling_max_freq");

        auto gpu_hz  = read_text(*gpu_dir + "/cur_freq");
        auto gpu_min = read_text(*gpu_dir + "/min_freq");
        auto gpu_max = read_text(*gpu_dir + "/max_freq");
        auto gov     = read_text(*gpu_dir + "/governor");

        auto t_cpu  = tz_cpu  ? read_text(*tz_cpu  + "/temp") : std::nullopt;
        auto t_gpu  = tz_gpu  ? read_text(*tz_gpu  + "/temp") : std::nullopt;
        auto t_soc0 = tz_soc0 ? read_text(*tz_soc0 + "/temp") : std::nullopt;
        auto t_soc1 = tz_soc1 ? read_text(*tz_soc1 + "/temp") : std::nullopt;
        auto t_soc2 = tz_soc2 ? read_text(*tz_soc2 + "/temp") : std::nullopt;
        auto t_tj   = tz_tj   ? read_text(*tz_tj   + "/temp") : std::nullopt;

        // CSV
        ofs << ts << "," << dt << ","
            << (cpu_khz ? *cpu_khz : "") << "," << (cpu_min ? *cpu_min : "") << "," << (cpu_max ? *cpu_max : "") << ","
            << (gpu_hz  ? *gpu_hz  : "") << "," << (gpu_min ? *gpu_min : "") << "," << (gpu_max ? *gpu_max : "") << ","
            << (gov ? *gov : "") << ","
            << (t_cpu  ? *t_cpu  : "") << "," << (t_gpu  ? *t_gpu  : "") << ","
            << (t_soc0 ? *t_soc0 : "") << "," << (t_soc1 ? *t_soc1 : "") << "," << (t_soc2 ? *t_soc2 : "") << ","
            << (t_tj   ? *t_tj   : "") << "\n";

        // watch-like terminal status (throttled)
        if (watch_mode) {
            if (last_watch_ns == 0 || (ts - last_watch_ns) >= (int64_t)watch_ms * 1000000LL) {
                last_watch_ns = ts;
                print_watch_block(
                    watch_initialized,
                    gpu_hz, gpu_min, gpu_max, gov,
                    t_cpu, t_gpu, t_soc0, t_soc1, t_soc2, t_tj
                );
            }
        }

        if (++line_cnt % 10 == 0) ofs.flush();
        std::this_thread::sleep_until(next);
    }

    ofs.flush();
    if (watch_mode) std::cerr << "\n";
    std::cerr << "Stopped.\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
        usage();
        return 0;
    }

    std::string cmd = argv[1];
    if (cmd == "probe")  return cmd_probe();
    if (cmd == "set")    return cmd_set(argc, argv);
    if (cmd == "unlock") return cmd_unlock(argc, argv);
    if (cmd == "log")    return cmd_log(argc, argv);

    std::cerr << "Unknown subcommand: " << cmd << "\n";
    usage();
    return 1;
}