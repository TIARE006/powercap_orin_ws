#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/i2c-dev.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t g_stop = 0;

void handle_sigint(int) {
    g_stop = 1;
}

class I2CDevice {
public:
    I2CDevice(const std::string& dev_path, int addr) : fd_(-1), addr_(addr) {
        fd_ = open(dev_path.c_str(), O_RDWR);
        if (fd_ < 0) {
            throw std::runtime_error("Failed to open " + dev_path + ": " + std::strerror(errno));
        }
        if (ioctl(fd_, I2C_SLAVE, addr_) < 0) {
            close(fd_);
            throw std::runtime_error("Failed to set I2C slave address: " + std::string(std::strerror(errno)));
        }
    }

    ~I2CDevice() {
        if (fd_ >= 0) close(fd_);
    }

    uint16_t read_reg16_be(uint8_t reg) {
        if (write(fd_, &reg, 1) != 1) {
            throw std::runtime_error("Failed to write register address");
        }

        uint8_t buf[2] = {0, 0};
        if (read(fd_, buf, 2) != 2) {
            throw std::runtime_error("Failed to read register data");
        }

        return static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    }

private:
    int fd_;
    int addr_;
};

std::string now_string() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

double epoch_seconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_sigint);

    const std::string i2c_dev = "/dev/i2c-7";
    const int addr = 0x40;

    const uint8_t REG_CURRENT = 0x01;
    const uint8_t REG_BUS_VOLTAGE = 0x02;
    const uint8_t REG_POWER = 0x03;

    const int period_ms = 1000;
    const std::string csv_path = "../logs/ina260.csv";

    try {
        I2CDevice dev(i2c_dev, addr);

        std::ofstream csv(csv_path);
        if (!csv.is_open()) {
            throw std::runtime_error("Failed to open CSV file: " + csv_path);
        }

        csv << "wall_time,epoch_s,current_a,voltage_v,power_w\n";
        csv << std::fixed << std::setprecision(6);

        std::cout << "Logging INA260 from " << i2c_dev
                  << " addr=0x" << std::hex << addr << std::dec
                  << " -> " << csv_path << "\n";
        std::cout << "Press Ctrl+C to stop.\n";

        while (!g_stop) {
            const uint16_t raw_current = dev.read_reg16_be(REG_CURRENT);
            const uint16_t raw_voltage = dev.read_reg16_be(REG_BUS_VOLTAGE);
            const uint16_t raw_power   = dev.read_reg16_be(REG_POWER);

            const double current_a = (raw_current * 1.25) / 1000.0;
            const double voltage_v = (raw_voltage * 1.25) / 1000.0;
            const double power_w   = (raw_power * 10.0) / 1000.0;

            const std::string wall = now_string();
            const double epoch = epoch_seconds();

            csv << wall << "," << epoch << ","
                << current_a << "," << voltage_v << "," << power_w << "\n";
            csv.flush();

            std::cout << wall
                      << "  I=" << std::fixed << std::setprecision(3) << current_a << " A"
                      << "  V=" << voltage_v << " V"
                      << "  P=" << power_w << " W\n";

            std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        }

        std::cout << "\nStopped.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}