#include "jetson/jtop_reader.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace jetson {

namespace {
uint64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}
}

JtopReader::~JtopReader() {
    stop();
}

bool JtopReader::start(const std::string& cmd) {
    if (running_) return true;

    pipe_ = popen(cmd.c_str(), "r");
    if (!pipe_) return false;

    running_ = true;
    thread_ = std::thread(&JtopReader::reader_loop, this);
    return true;
}

void JtopReader::stop() {
    if (!running_) return;

    running_ = false;

    if (pipe_) {
        pclose(pipe_);
        pipe_ = nullptr;
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

JtopSample JtopReader::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void JtopReader::reader_loop() {
    char buf[512];

    while (running_ && pipe_) {
        if (!fgets(buf, sizeof(buf), pipe_)) {
            break;
        }

        std::string line(buf);
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }

        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, ',')) {
            fields.push_back(item);
        }

        if (fields.size() < 3) {
            continue;
        }

        JtopSample s;
        s.valid = true;
        s.host_ts_ns = now_ns();
        s.power_tot_mw = fields[0];
        s.power_vdd_cpu_gpu_cv_mw = fields[1];
        s.power_vdd_soc_mw = fields[2];

        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = std::move(s);
        }
    }

    running_ = false;
}

} // namespace jetson