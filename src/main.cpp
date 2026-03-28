#include "app/csv_logger.hpp"
#include "app/watch_renderer.hpp"
#include "jetson/jetson_platform.hpp"
#include "jetson/jtop_reader.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <thread>

static volatile std::sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

static void usage() {
    std::cout <<
R"(Usage:
  dvfs_tool probe
  dvfs_tool log --out <csv> --period_ms <ms> [--watch]
  sudo dvfs_tool set --cpu_khz <kHz> --gpu_hz <Hz>
  sudo dvfs_tool unlock
)";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 0;
    }

    std::string cmd = argv[1];
    auto platform = jetson::JetsonPlatform::discover();
    if (!platform) {
        std::cerr << "Failed to discover Jetson platform\n";
        return 1;
    }

    if (cmd == "probe") {
        auto s = platform->sample();
        std::cout << "CPU cur: " << s.cpu.cur_khz << " kHz\n";
        std::cout << "GPU cur: " << s.gpu.cur_hz << " Hz\n";
        std::cout << "Thermal zones: " << s.thermal.zones.size() << "\n";
        return 0;
    }

    if (cmd == "set") {
        long cpu_khz = -1;
        long long gpu_hz = -1;
        for (int i = 2; i + 1 < argc; ++i) {
            std::string a = argv[i];
            if (a == "--cpu_khz") cpu_khz = std::stol(argv[++i]);
            else if (a == "--gpu_hz") gpu_hz = std::stoll(argv[++i]);
        }

        if (cpu_khz <= 0 || gpu_hz <= 0) {
            std::cerr << "Need --cpu_khz and --gpu_hz\n";
            return 2;
        }

        return platform->set_dvfs({cpu_khz, gpu_hz}) ? 0 : 3;
    }

    if (cmd == "unlock") {
        return platform->unlock_dvfs() ? 0 : 3;
    }

    if (cmd == "log") {
        std::signal(SIGINT, on_sigint);

        std::string out = "logs/run.csv";
        int period_ms = 100;
        bool watch = false;

        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--out" && i + 1 < argc) out = argv[++i];
            else if (a == "--period_ms" && i + 1 < argc) period_ms = std::stoi(argv[++i]);
            else if (a == "--watch") watch = true;
        }

        std::filesystem::path out_path(out);
        if (out_path.has_parent_path()) {
            std::filesystem::create_directories(out_path.parent_path());
        } else {
            std::filesystem::create_directories("logs");
        }

        platform->start_monitors(period_ms);

        jetson::JtopReader jtop_reader;
        if (!jtop_reader.start("python3 /home/yuanyang/powercap_orin_ws/scripts/jtop_probe.py")) {
            std::cerr << "Warning: failed to start jtop reader, continuing without jtop\n";
        }

        app::CsvLogger logger(out);
        if (!logger.ok()) {
            std::cerr << "Failed to open log file: " << out << "\n";
            jtop_reader.stop();
            platform->stop_monitors();
            return 4;
        }
        logger.write_header();

        app::WatchRenderer renderer;
        auto next = std::chrono::steady_clock::now();

        while (!g_stop) {
            next += std::chrono::milliseconds(period_ms);

            auto s = platform->sample();
            auto jt = jtop_reader.latest();

            logger.write_sample(s, jt);

            if (watch) renderer.render(s, jt);

            std::this_thread::sleep_until(next);
        }

        jtop_reader.stop();
        platform->stop_monitors();
        return 0;
    }

    usage();
    return 2;
}