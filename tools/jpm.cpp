#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

const std::string SYSFS_DIR = "/sys/kernel/runtime_monitor";
const std::string MODULE_NAME = "runtime_monitor";

void handle_signal(int) {
    stop_requested = 1;
}

std::string get_home_dir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        return ".";
    }
    return std::string(home);
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

int run_command(const std::string& cmd) {
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        return ret;
    }
    return 0;
}

bool path_exists(const std::string& path) {
    std::ifstream fin(path);
    return fin.good();
}

bool module_loaded() {
    std::ifstream fin("/proc/modules");
    if (!fin.is_open()) {
        return false;
    }

    std::string name;
    while (fin >> name) {
        std::string rest;
        std::getline(fin, rest);
        if (name == MODULE_NAME) {
            return true;
        }
    }
    return false;
}

int read_int_file(const std::string& path) {
    std::ifstream fin(path);
    if (!fin.is_open()) {
        throw std::runtime_error("failed to open " + path);
    }

    int value = 0;
    fin >> value;

    if (!fin.good() && !fin.eof()) {
        throw std::runtime_error("failed to read integer from " + path);
    }

    return value;
}

double now_wall_sec() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto us = duration_cast<microseconds>(now.time_since_epoch()).count();
    return static_cast<double>(us) / 1e6;
}

struct MonitorConfig {
    std::string module_path = get_home_dir() + "/runtime_monitor.ko";
    std::string output = "runtime_power_log.csv";

    int bus_num = -1;
    int i2c_addr = -1;
    int period_ms = 10;
    int i2c_rate_hz = 400000;
    std::string skip_bus_mask = "0";

    double interval_ms = 10.0;
    double duration_sec = 10.0;

    bool quiet = false;
    bool keep_loaded = false;
    bool no_load = false;
};

void print_usage() {
    std::cout
        << "Usage:\n"
        << "  jpm monitor [options]\n\n"
        << "Options:\n"
        << "  --module <path>          Path to runtime_monitor.ko\n"
        << "  --output <file>          Output CSV path\n"
        << "  --duration <sec>         Logging duration in seconds\n"
        << "  --interval-ms <ms>       User-space logging interval in milliseconds (default: 10)\n"
        << "  --period-ms <ms>         Kernel sampling period in milliseconds (default: 10)\n"
        << "  --i2c-rate-hz <Hz>       I2C bus rate before logging (default: 400000; 0 disables)\n"
        << "  --bus-num <n>            I2C bus number, -1 for auto-detect\n"
        << "  --i2c-addr <n>           I2C address, -1 for auto-detect, e.g. 0x40\n"
        << "  --skip-bus-mask <mask>   Bitmask of I2C buses to skip, e.g. 0x20\n"
        << "  --quiet                  Do not print each sample\n"
        << "  --keep-loaded            Do not unload kernel module after logging\n"
        << "  --no-load                Do not load/unload module; assume it is already loaded\n"
        << "  -h, --help               Show this help\n\n"
        << "Examples:\n"
        << "  jpm monitor --duration 10 --output log.csv\n"
        << "  jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20\n"
        << "  jpm monitor --bus-num 7 --i2c-addr 0x40 --duration 10\n";
}

std::string require_value(int& i, int argc, char** argv, const std::string& opt) {
    if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + opt);
    }
    return argv[++i];
}

int parse_int_auto_base(const std::string& s) {
    size_t pos = 0;
    int value = std::stoi(s, &pos, 0);
    if (pos != s.size()) {
        throw std::runtime_error("invalid integer: " + s);
    }
    return value;
}

MonitorConfig parse_monitor_args(int argc, char** argv) {
    MonitorConfig cfg;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--module") {
            cfg.module_path = require_value(i, argc, argv, arg);
        } else if (arg == "--output") {
            cfg.output = require_value(i, argc, argv, arg);
        } else if (arg == "--duration") {
            cfg.duration_sec = std::stod(require_value(i, argc, argv, arg));
        } else if (arg == "--interval-ms") {
            cfg.interval_ms = std::stod(require_value(i, argc, argv, arg));
        } else if (arg == "--period-ms") {
            cfg.period_ms = parse_int_auto_base(require_value(i, argc, argv, arg));
        } else if (arg == "--i2c-rate-hz") {
            cfg.i2c_rate_hz = parse_int_auto_base(require_value(i, argc, argv, arg));
        } else if (arg == "--bus-num") {
            cfg.bus_num = parse_int_auto_base(require_value(i, argc, argv, arg));
        } else if (arg == "--i2c-addr") {
            cfg.i2c_addr = parse_int_auto_base(require_value(i, argc, argv, arg));
        } else if (arg == "--skip-bus-mask") {
            cfg.skip_bus_mask = require_value(i, argc, argv, arg);
        } else if (arg == "--quiet") {
            cfg.quiet = true;
        } else if (arg == "--keep-loaded") {
            cfg.keep_loaded = true;
        } else if (arg == "--no-load") {
            cfg.no_load = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (cfg.interval_ms <= 0.0) {
        throw std::runtime_error("interval-ms must be positive");
    }

    if (cfg.duration_sec <= 0.0) {
        throw std::runtime_error("duration must be positive");
    }

    if (cfg.period_ms < 10) {
        throw std::runtime_error("period-ms must be >= 10");
    }

    if (cfg.i2c_rate_hz < 0) {
        throw std::runtime_error("i2c-rate-hz must be >= 0");
    }

    return cfg;
}

void unload_module_if_loaded() {
    if (module_loaded()) {
        std::cout << "[jpm] unloading " << MODULE_NAME << "\n";
        run_command("sudo rmmod " + MODULE_NAME);
    }
}

bool configure_i2c_rate(int bus_num, int rate_hz) {
    if (rate_hz == 0) {
        std::cout << "[jpm] I2C rate change disabled\n";
        return true;
    }

    if (bus_num < 0) {
        std::cerr << "[jpm] warning: cannot set I2C rate before the bus is known\n";
        return false;
    }

    const std::string rate_path =
        "/sys/class/i2c-adapter/i2c-" + std::to_string(bus_num) + "/bus_clk_rate";

    if (!path_exists(rate_path)) {
        std::cerr << "[jpm] warning: this platform does not expose " << rate_path
                  << "; keeping the platform default I2C rate\n";
        return false;
    }

    std::ostringstream cmd;
    cmd << "echo " << rate_hz << " | sudo tee " << shell_quote(rate_path)
        << " >/dev/null";

    if (run_command(cmd.str()) != 0) {
        std::cerr << "[jpm] warning: failed to set I2C bus " << bus_num
                  << " to " << rate_hz << " Hz; keeping the current rate\n";
        return false;
    }

    int actual_rate = 0;
    try {
        actual_rate = read_int_file(rate_path);
    } catch (...) {
        std::cerr << "[jpm] warning: I2C rate was written but could not be read back\n";
        return true;
    }

    std::cout << "[jpm] I2C bus " << bus_num << " rate=" << actual_rate << " Hz\n";
    if (actual_rate != rate_hz) {
        std::cerr << "[jpm] warning: requested " << rate_hz
                  << " Hz but the platform reports " << actual_rate << " Hz\n";
    }
    return true;
}

void load_module(const MonitorConfig& cfg) {
    if (cfg.no_load) {
        std::cout << "[jpm] --no-load specified; using existing module\n";
        return;
    }

    if (module_loaded()) {
        std::cout << "[jpm] " << MODULE_NAME << " already loaded; unloading first\n";
        unload_module_if_loaded();
    }

    std::ostringstream cmd;
    cmd << "sudo insmod " << shell_quote(cfg.module_path)
        << " bus_num=" << cfg.bus_num
        << " i2c_addr=" << cfg.i2c_addr
        << " period_ms=" << cfg.period_ms
        << " skip_bus_mask=" << cfg.skip_bus_mask;

    std::cout << "[jpm] loading module: " << cfg.module_path << "\n";
    std::cout << "[jpm] bus_num=" << cfg.bus_num
              << " i2c_addr=" << cfg.i2c_addr
              << " period_ms=" << cfg.period_ms
              << " skip_bus_mask=" << cfg.skip_bus_mask << "\n";

    int ret = run_command(cmd.str());
    if (ret != 0) {
        throw std::runtime_error("failed to load kernel module");
    }
}

void wait_for_sysfs() {
    const std::string power_path = SYSFS_DIR + "/power_mw";

    for (int i = 0; i < 50; ++i) {
        if (path_exists(power_path)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    throw std::runtime_error(SYSFS_DIR + " did not appear");
}

void print_sysfs_info() {
    std::cout << "[jpm] sysfs: " << SYSFS_DIR << "\n";

    try {
        std::ifstream bus(SYSFS_DIR + "/selected_bus");
        std::ifstream addr(SYSFS_DIR + "/selected_i2c_addr");
        std::ifstream mask(SYSFS_DIR + "/skip_bus_mask");

        std::string bus_s, addr_s, mask_s;
        std::getline(bus, bus_s);
        std::getline(addr, addr_s);
        std::getline(mask, mask_s);

        if (!bus_s.empty()) {
            std::cout << "[jpm] selected_bus=" << bus_s << "\n";
        }
        if (!addr_s.empty()) {
            std::cout << "[jpm] selected_i2c_addr=" << addr_s << "\n";
        }
        if (!mask_s.empty()) {
            std::cout << "[jpm] skip_bus_mask=" << mask_s << "\n";
        }
    } catch (...) {
    }
}

void log_csv(const MonitorConfig& cfg) {
    const std::string power_path = SYSFS_DIR + "/power_mw";
    const std::string voltage_path = SYSFS_DIR + "/voltage_mv";
    const std::string current_path = SYSFS_DIR + "/current_ma";
    const std::string status_path = SYSFS_DIR + "/read_status";
    const std::string age_path = SYSFS_DIR + "/sample_age_ms";

    std::ofstream csv(cfg.output);
    if (!csv.is_open()) {
        throw std::runtime_error("failed to open output file: " + cfg.output);
    }

    csv << "wall_time_sec,elapsed_ms,power_mw,voltage_mv,current_ma,read_status,sample_age_ms\n";

    std::cout << "[jpm] logging to " << cfg.output << "\n";
    std::cout << "[jpm] interval_ms=" << cfg.interval_ms
              << " duration_sec=" << cfg.duration_sec << "\n";

    using clock = std::chrono::steady_clock;
    using duration_d = std::chrono::duration<double>;

    const auto start = clock::now();
    auto next_sample = start;
    const auto interval = std::chrono::duration<double, std::milli>(cfg.interval_ms);

    while (!stop_requested) {
        const auto now = clock::now();
        const double elapsed_sec = duration_d(now - start).count();

        if (elapsed_sec > cfg.duration_sec) {
            break;
        }

        const int power_mw = read_int_file(power_path);
        const int voltage_mv = read_int_file(voltage_path);
        const int current_ma = read_int_file(current_path);
        const int read_status = read_int_file(status_path);
        const int sample_age_ms = read_int_file(age_path);

        csv << std::fixed << std::setprecision(6) << now_wall_sec() << ","
            << std::setprecision(3) << elapsed_sec * 1000.0 << ","
            << power_mw << ","
            << voltage_mv << ","
            << current_ma << ","
            << read_status << ","
            << sample_age_ms << "\n";
        csv.flush();

        if (!cfg.quiet) {
            std::cout << "t=" << std::setw(8) << std::fixed << std::setprecision(3)
                      << elapsed_sec << "s "
                      << "power=" << std::setw(6) << power_mw << " mW "
                      << "voltage=" << std::setw(6) << voltage_mv << " mV "
                      << "current=" << std::setw(5) << current_ma << " mA "
                      << "status=" << std::setw(3) << read_status << " "
                      << "age=" << std::setw(4) << sample_age_ms << " ms\n";
        }

        next_sample += std::chrono::duration_cast<clock::duration>(interval);
        std::this_thread::sleep_until(next_sample);
    }

    if (stop_requested) {
        std::cout << "\n[jpm] interrupted by user\n";
    }

    std::cout << "[jpm] logging finished\n";
}

int monitor_main(int argc, char** argv) {
    MonitorConfig cfg = parse_monitor_args(argc, argv);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        load_module(cfg);
        wait_for_sysfs();

        // Let the module identify the adapter at the platform's boot-time
        // rate, then change that selected adapter before logging begins. Some
        // Tegra I2C drivers apply bus_clk_rate on the next transfer; changing
        // it before INA260 detection can make that first transfer fail.
        if (cfg.i2c_rate_hz != 0) {
            const int selected_bus = read_int_file(SYSFS_DIR + "/selected_bus");
            configure_i2c_rate(selected_bus, cfg.i2c_rate_hz);
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.period_ms * 2));
        }

        print_sysfs_info();
        log_csv(cfg);
    } catch (...) {
        if (!cfg.no_load && !cfg.keep_loaded) {
            unload_module_if_loaded();
        }
        throw;
    }

    if (!cfg.no_load && !cfg.keep_loaded) {
        unload_module_if_loaded();
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 1;
        }

        std::string cmd = argv[1];

        if (cmd == "monitor") {
            return monitor_main(argc, argv);
        }

        if (cmd == "-h" || cmd == "--help") {
            print_usage();
            return 0;
        }

        throw std::runtime_error("unknown command: " + cmd);

    } catch (const std::exception& e) {
        std::cerr << "[jpm] error: " << e.what() << "\n";
        return 1;
    }
}
