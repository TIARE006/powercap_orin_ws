#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/sort.h>
#include <linux/hrtimer.h>

#define INA260_REG_CURRENT      0x01
#define INA260_REG_VOLTAGE      0x02
#define INA260_REG_POWER        0x03
#define INA260_REG_MANUFACTURER 0xFE
#define INA260_REG_DIE_ID       0xFF

#define INA260_MANUFACTURER_ID  0x5449

#define INA260_ADDR_MIN         0x40
#define INA260_ADDR_MAX         0x4F

/*
 * bus_num = -1 means auto-detect I2C bus.
 * i2c_addr = -1 means auto-detect INA260 address.
 */
static int bus_num = -1;
module_param(bus_num, int, 0644);
MODULE_PARM_DESC(bus_num, "I2C bus number. Use -1 for auto-detect.");

static int i2c_addr = -1;
module_param(i2c_addr, int, 0644);
MODULE_PARM_DESC(i2c_addr, "INA260 I2C address. Use -1 for auto-detect.");

static unsigned int period_ms = 10;
module_param(period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "Runtime monitoring period in milliseconds");

static unsigned int max_bus_num = 31;
module_param(max_bus_num, uint, 0644);
MODULE_PARM_DESC(max_bus_num, "Maximum I2C bus number to scan during auto-detect");

static bool skip_internal_buses = true;
module_param(skip_internal_buses, bool, 0644);
MODULE_PARM_DESC(skip_internal_buses, "Skip clearly internal I2C adapters such as BPMP");

static unsigned long skip_bus_mask = 0;
module_param(skip_bus_mask, ulong, 0644);
MODULE_PARM_DESC(skip_bus_mask, "Bitmask of I2C bus numbers to skip during auto-detect");

static struct work_struct monitor_work;
static struct hrtimer monitor_timer;
static struct i2c_adapter *ina260_adapter;
static struct i2c_client *ina260_client;

static struct kobject *runtime_monitor_kobj;
static DEFINE_MUTEX(sample_lock);

static int latest_current_ma;
static int latest_voltage_mv;
static int latest_power_mw;
static unsigned long latest_jiffies;
static int latest_read_status;

static int selected_bus_num = -1;
static int selected_i2c_addr = -1;
static int auto_detect_used = 0;

static DEFINE_MUTEX(limit_lock);

static unsigned int limit_default_samples = 50000;
module_param(limit_default_samples, uint, 0644);
MODULE_PARM_DESC(limit_default_samples, "Default number of samples for kernel-side sampling-limit test");

static unsigned int limit_full_last_samples;
static unsigned int limit_full_ok;
static unsigned int limit_full_fail;
static u64 limit_full_total_us;
static u32 limit_full_mean_us;
static u32 limit_full_min_us;
static u32 limit_full_p50_us;
static u32 limit_full_p90_us;
static u32 limit_full_p99_us;
static u32 limit_full_max_us;
static u32 limit_full_rate_hz;

static unsigned int limit_power_only_samples;
static unsigned int limit_power_only_ok;
static unsigned int limit_power_only_fail;
static u64 limit_power_only_total_us;
static u32 limit_power_only_mean_us;
static u32 limit_power_only_min_us;
static u32 limit_power_only_p50_us;
static u32 limit_power_only_p90_us;
static u32 limit_power_only_p99_us;
static u32 limit_power_only_max_us;
static u32 limit_power_only_rate_hz;


static int ina260_read_reg16_raw(struct i2c_client *client, u8 reg, u16 *val, bool verbose)
{
    int ret;
    u8 buf[2];

    ret = i2c_smbus_read_i2c_block_data(client, reg, 2, buf);
    if (ret < 0) {
        if (verbose)
            pr_err("runtime_monitor: failed to read reg 0x%02x, ret=%d\n", reg, ret);
        return ret;
    }

    if (ret != 2) {
        if (verbose)
            pr_err("runtime_monitor: short read reg 0x%02x, ret=%d\n", reg, ret);
        return -EIO;
    }

    *val = ((u16)buf[0] << 8) | buf[1];
    return 0;
}

static int ina260_read_reg16(struct i2c_client *client, u8 reg, u16 *val)
{
    return ina260_read_reg16_raw(client, reg, val, true);
}

static int ina260_read_all_from_client(struct i2c_client *client,
                                       int *current_ma,
                                       int *voltage_mv,
                                       int *power_mw)
{
    int ret;
    u16 raw_current;
    u16 raw_voltage;
    u16 raw_power;

    ret = ina260_read_reg16(client, INA260_REG_CURRENT, &raw_current);
    if (ret)
        return ret;

    ret = ina260_read_reg16(client, INA260_REG_VOLTAGE, &raw_voltage);
    if (ret)
        return ret;

    ret = ina260_read_reg16(client, INA260_REG_POWER, &raw_power);
    if (ret)
        return ret;

    /*
     * INA260 units:
     * current LSB = 1.25 mA
     * voltage LSB = 1.25 mV
     * power   LSB = 10 mW
     */
    *current_ma = ((int)raw_current * 125) / 100;
    *voltage_mv = ((int)raw_voltage * 125) / 100;
    *power_mw   = ((int)raw_power) * 10;

    return 0;
}

static int ina260_read_all(int *current_ma, int *voltage_mv, int *power_mw)
{
    return ina260_read_all_from_client(ina260_client, current_ma, voltage_mv, power_mw);
}

static bool runtime_monitor_adapter_should_skip(struct i2c_adapter *adapter, int bus)
{
    const char *name;
    const char *devname;

    if (!adapter)
        return true;

    name = adapter->name;
    devname = dev_name(&adapter->dev);

    /*
     * User-configurable skip mask.
     * Example: skip_bus_mask=0x30 skips bus 4 and bus 5.
     */
    if (bus >= 0 && bus < BITS_PER_LONG) {
        if (skip_bus_mask & BIT(bus)) {
            pr_info("runtime_monitor: skip adapter i2c-%d by skip_bus_mask=0x%lx name=%s\n",
                    bus, skip_bus_mask, name ? name : "unknown");
            return true;
        }
    }

    /*
     * Generic internal-adapter filter.
     * Do not hard-code board-specific bus numbers here.
     */
    if (skip_internal_buses && name) {
        if (strnstr(name, "BPMP", strlen(name)) ||
            strnstr(name, "bpmp", strlen(name))) {
            pr_info("runtime_monitor: skip internal adapter i2c-%d name=%s\n",
                    bus, name);
            return true;
        }
    }

    if (skip_internal_buses && devname) {
        if (strnstr(devname, "BPMP", strlen(devname)) ||
            strnstr(devname, "bpmp", strlen(devname))) {
            pr_info("runtime_monitor: skip internal adapter i2c-%d dev=%s\n",
                    bus, devname);
            return true;
        }
    }

    return false;
}

static bool ina260_candidate_valid(struct i2c_client *client)
{
    int ret;
    u16 manufacturer_id = 0;
    u16 die_id = 0;
    int current_ma = 0;
    int voltage_mv = 0;
    int power_mw = 0;

    ret = ina260_read_reg16_raw(client, INA260_REG_MANUFACTURER, &manufacturer_id, false);
    if (ret)
        return false;

    if (manufacturer_id != INA260_MANUFACTURER_ID)
        return false;

    /*
     * Die ID is useful for debugging, but do not hard-fail on it because
     * revision bits may vary.
     */
    ret = ina260_read_reg16_raw(client, INA260_REG_DIE_ID, &die_id, false);
    if (ret)
        die_id = 0;

    ret = ina260_read_all_from_client(client, &current_ma, &voltage_mv, &power_mw);
    if (ret)
        return false;

    pr_info("runtime_monitor: INA260 candidate valid: manufacturer=0x%04x die=0x%04x power=%d mW voltage=%d mV current=%d mA\n",
            manufacturer_id, die_id, power_mw, voltage_mv, current_ma);

    return true;
}

static int runtime_monitor_try_bind(int candidate_bus, int candidate_addr)
{
    struct i2c_adapter *adapter;
    struct i2c_client *client;

    adapter = i2c_get_adapter(candidate_bus);
    if (!adapter)
        return -ENODEV;

    client = i2c_new_dummy_device(adapter, candidate_addr);
    if (IS_ERR(client)) {
        int err = PTR_ERR(client);
        i2c_put_adapter(adapter);
        return err;
    }

    if (!ina260_candidate_valid(client)) {
        i2c_unregister_device(client);
        i2c_put_adapter(adapter);
        return -ENODEV;
    }

    ina260_adapter = adapter;
    ina260_client = client;
    selected_bus_num = candidate_bus;
    selected_i2c_addr = candidate_addr;

    pr_info("runtime_monitor: selected INA260 at i2c-%d addr=0x%02x\n",
            selected_bus_num, selected_i2c_addr);

    return 0;
}

static int runtime_monitor_detect_device(void)
{
    int ret;
    int bus_start;
    int bus_end;
    int addr_start;
    int addr_end;
    int bus;
    int addr;

    if (bus_num >= 0) {
        bus_start = bus_num;
        bus_end = bus_num;
    } else {
        bus_start = 0;
        bus_end = max_bus_num;
        auto_detect_used = 1;
    }

    if (i2c_addr >= 0) {
        addr_start = i2c_addr;
        addr_end = i2c_addr;
    } else {
        addr_start = INA260_ADDR_MIN;
        addr_end = INA260_ADDR_MAX;
        auto_detect_used = 1;
    }

    pr_info("runtime_monitor: detecting INA260, bus range=%d..%d addr range=0x%02x..0x%02x\n",
            bus_start, bus_end, addr_start, addr_end);

    for (bus = bus_start; bus <= bus_end; bus++) {
        struct i2c_adapter *adapter;

        adapter = i2c_get_adapter(bus);
        if (!adapter)
            continue;

        pr_info("runtime_monitor: scan adapter i2c-%d name=%s dev=%s\n",
                bus,
                adapter->name ? adapter->name : "unknown",
                dev_name(&adapter->dev) ? dev_name(&adapter->dev) : "unknown");

        if (runtime_monitor_adapter_should_skip(adapter, bus)) {
            i2c_put_adapter(adapter);
            continue;
        }

        i2c_put_adapter(adapter);

        for (addr = addr_start; addr <= addr_end; addr++) {
            ret = runtime_monitor_try_bind(bus, addr);
            if (ret == 0)
                return 0;
        }
    }

    pr_err("runtime_monitor: failed to auto-detect INA260\n");
    return -ENODEV;
}


static int limit_cmp_u32(const void *a, const void *b)
{
    u32 va = *(const u32 *)a;
    u32 vb = *(const u32 *)b;

    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

static u32 limit_percentile(const u32 *v, unsigned int n, unsigned int p)
{
    unsigned int idx;

    if (!v || n == 0)
        return 0;

    idx = (u64)(n - 1) * p / 100;
    return v[idx];
}

static int runtime_monitor_run_full_limit(unsigned int samples)
{
    u32 *dt_us;
    unsigned int i;
    unsigned int ok = 0;
    unsigned int fail = 0;
    u64 total_us = 0;
    u32 min_us = U32_MAX;
    u32 max_us = 0;
    int ret;
    int current_ma = 0;
    int voltage_mv = 0;
    int power_mw = 0;

    if (samples == 0)
        samples = limit_default_samples;

    if (samples < 10)
        return -EINVAL;

    if (samples > 1000000)
        samples = 1000000;

    dt_us = vmalloc_array(samples, sizeof(u32));
    if (!dt_us)
        return -ENOMEM;

    /*
     * Pause periodic sampling while the benchmark is running so the benchmark
     * measures the INA260 read path without competing with monitor_tick().
     */
    hrtimer_cancel(&monitor_timer);
    cancel_work_sync(&monitor_work);

    pr_info("runtime_monitor: full-read limit test start, samples=%u\n", samples);

    for (i = 0; i < samples; i++) {
        ktime_t t0;
        ktime_t t1;
        s64 delta_ns;
        u32 delta_us;

        t0 = ktime_get();
        ret = ina260_read_all(&current_ma, &voltage_mv, &power_mw);
        t1 = ktime_get();

        if (ret == 0) {
            delta_ns = ktime_to_ns(ktime_sub(t1, t0));
            if (delta_ns < 0)
                delta_ns = 0;

            delta_us = (u32)div64_s64(delta_ns + 999, 1000);

            dt_us[ok++] = delta_us;
            total_us += delta_us;

            if (delta_us < min_us)
                min_us = delta_us;
            if (delta_us > max_us)
                max_us = delta_us;
        } else {
            fail++;
        }

        if (need_resched())
            cond_resched();
    }

    if (ok > 0)
        sort(dt_us, ok, sizeof(u32), limit_cmp_u32, NULL);

    mutex_lock(&limit_lock);

    limit_full_last_samples = samples;
    limit_full_ok = ok;
    limit_full_fail = fail;
    limit_full_total_us = total_us;

    if (ok > 0) {
        limit_full_mean_us = (u32)div64_u64(total_us, ok);
        limit_full_min_us = min_us;
        limit_full_p50_us = limit_percentile(dt_us, ok, 50);
        limit_full_p90_us = limit_percentile(dt_us, ok, 90);
        limit_full_p99_us = limit_percentile(dt_us, ok, 99);
        limit_full_max_us = max_us;

        if (limit_full_mean_us > 0)
            limit_full_rate_hz = 1000000U / limit_full_mean_us;
        else
            limit_full_rate_hz = 0;
    } else {
        limit_full_mean_us = 0;
        limit_full_min_us = 0;
        limit_full_p50_us = 0;
        limit_full_p90_us = 0;
        limit_full_p99_us = 0;
        limit_full_max_us = 0;
        limit_full_rate_hz = 0;
    }

    mutex_unlock(&limit_lock);

    pr_info("runtime_monitor: full-read limit test done, ok=%u fail=%u mean=%u us p99=%u us max=%u us rate=%u Hz\n",
            limit_full_ok, limit_full_fail, limit_full_mean_us,
            limit_full_p99_us, limit_full_max_us, limit_full_rate_hz);

    vfree(dt_us);

    hrtimer_start(&monitor_timer, ms_to_ktime(period_ms), HRTIMER_MODE_REL);

    return 0;
}


static int runtime_monitor_run_power_only_limit(unsigned int samples)
{
    u32 *dt_us;
    unsigned int i;
    unsigned int ok = 0;
    unsigned int fail = 0;
    u64 total_us = 0;
    u32 min_us = U32_MAX;
    u32 max_us = 0;
    int ret;
    u16 raw_power = 0;

    if (samples == 0)
        samples = limit_default_samples;

    if (samples < 10)
        return -EINVAL;

    if (samples > 1000000)
        samples = 1000000;

    dt_us = vmalloc_array(samples, sizeof(u32));
    if (!dt_us)
        return -ENOMEM;

    /*
     * Pause periodic sampling while the benchmark is running.
     * This measures the kernel-side single-register INA260 power read path.
     */
    hrtimer_cancel(&monitor_timer);
    cancel_work_sync(&monitor_work);

    pr_info("runtime_monitor: power-only full-read limit test start, samples=%u\n", samples);

    for (i = 0; i < samples; i++) {
        ktime_t t0;
        ktime_t t1;
        s64 delta_ns;
        u32 delta_us;

        t0 = ktime_get();
        ret = ina260_read_reg16(ina260_client, INA260_REG_POWER, &raw_power);
        t1 = ktime_get();

        if (ret == 0) {
            delta_ns = ktime_to_ns(ktime_sub(t1, t0));
            if (delta_ns < 0)
                delta_ns = 0;

            delta_us = (u32)div64_s64(delta_ns + 999, 1000);

            dt_us[ok++] = delta_us;
            total_us += delta_us;

            if (delta_us < min_us)
                min_us = delta_us;
            if (delta_us > max_us)
                max_us = delta_us;
        } else {
            fail++;
        }

        if (need_resched())
            cond_resched();
    }

    if (ok > 0)
        sort(dt_us, ok, sizeof(u32), limit_cmp_u32, NULL);

    mutex_lock(&limit_lock);

    limit_power_only_samples = samples;
    limit_power_only_ok = ok;
    limit_power_only_fail = fail;
    limit_power_only_total_us = total_us;

    if (ok > 0) {
        limit_power_only_mean_us = (u32)div64_u64(total_us, ok);
        limit_power_only_min_us = min_us;
        limit_power_only_p50_us = limit_percentile(dt_us, ok, 50);
        limit_power_only_p90_us = limit_percentile(dt_us, ok, 90);
        limit_power_only_p99_us = limit_percentile(dt_us, ok, 99);
        limit_power_only_max_us = max_us;

        if (limit_power_only_mean_us > 0)
            limit_power_only_rate_hz = 1000000U / limit_power_only_mean_us;
        else
            limit_power_only_rate_hz = 0;
    } else {
        limit_power_only_mean_us = 0;
        limit_power_only_min_us = 0;
        limit_power_only_p50_us = 0;
        limit_power_only_p90_us = 0;
        limit_power_only_p99_us = 0;
        limit_power_only_max_us = 0;
        limit_power_only_rate_hz = 0;
    }

    mutex_unlock(&limit_lock);

    pr_info("runtime_monitor: power-only full-read limit test done, ok=%u fail=%u mean=%u us p99=%u us max=%u us rate=%u Hz\n",
            limit_power_only_ok,
            limit_power_only_fail,
            limit_power_only_mean_us,
            limit_power_only_p99_us,
            limit_power_only_max_us,
            limit_power_only_rate_hz);

    vfree(dt_us);

    hrtimer_start(&monitor_timer, ms_to_ktime(period_ms), HRTIMER_MODE_REL);

    return 0;
}

static void monitor_tick(struct work_struct *work)
{
    int ret;
    int current_ma = 0;
    int voltage_mv = 0;
    int power_mw = 0;

    ret = ina260_read_all(&current_ma, &voltage_mv, &power_mw);

    mutex_lock(&sample_lock);
    latest_read_status = ret;
    if (ret == 0) {
        latest_current_ma = current_ma;
        latest_voltage_mv = voltage_mv;
        latest_power_mw = power_mw;
        latest_jiffies = jiffies;
    }
    mutex_unlock(&sample_lock);

    if (ret) {
        pr_err("runtime_monitor: INA260 read failed, ret=%d\n", ret);
    }

}

static enum hrtimer_restart monitor_timer_tick(struct hrtimer *timer)
{
    queue_work(system_highpri_wq, &monitor_work);
    hrtimer_forward_now(timer, ms_to_ktime(READ_ONCE(period_ms)));
    return HRTIMER_RESTART;
}



static ssize_t limit_power_only_run_store(struct kobject *kobj,
                                             struct kobj_attribute *attr,
                                             const char *buf,
                                             size_t count)
{
    unsigned int samples;
    int ret;

    ret = kstrtouint(buf, 0, &samples);
    if (ret)
        return ret;

    ret = runtime_monitor_run_power_only_limit(samples);
    if (ret)
        return ret;

    return count;
}

static ssize_t limit_power_only_samples_show(struct kobject *kobj,
                                                struct kobj_attribute *attr,
                                                char *buf)
{
    unsigned int value;

    mutex_lock(&limit_lock);
    value = limit_power_only_samples;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_ok_show(struct kobject *kobj,
                                           struct kobj_attribute *attr,
                                           char *buf)
{
    unsigned int value;

    mutex_lock(&limit_lock);
    value = limit_power_only_ok;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_fail_show(struct kobject *kobj,
                                             struct kobj_attribute *attr,
                                             char *buf)
{
    unsigned int value;

    mutex_lock(&limit_lock);
    value = limit_power_only_fail;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_mean_us_show(struct kobject *kobj,
                                                struct kobj_attribute *attr,
                                                char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_power_only_mean_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_min_us_show(struct kobject *kobj,
                                               struct kobj_attribute *attr,
                                               char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_power_only_min_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_p50_us_show(struct kobject *kobj,
                                               struct kobj_attribute *attr,
                                               char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_power_only_p50_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_p90_us_show(struct kobject *kobj,
                                               struct kobj_attribute *attr,
                                               char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_power_only_p90_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_p99_us_show(struct kobject *kobj,
                                               struct kobj_attribute *attr,
                                               char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_power_only_p99_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_max_us_show(struct kobject *kobj,
                                               struct kobj_attribute *attr,
                                               char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_power_only_max_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_power_only_rate_hz_show(struct kobject *kobj,
                                                struct kobj_attribute *attr,
                                                char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_power_only_rate_hz;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_run_store(struct kobject *kobj,
                                   struct kobj_attribute *attr,
                                   const char *buf,
                                   size_t count)
{
    unsigned int samples;
    int ret;

    ret = kstrtouint(buf, 0, &samples);
    if (ret)
        return ret;

    ret = runtime_monitor_run_full_limit(samples);
    if (ret)
        return ret;

    return count;
}

static ssize_t limit_full_samples_show(struct kobject *kobj,
                                      struct kobj_attribute *attr,
                                      char *buf)
{
    unsigned int value;

    mutex_lock(&limit_lock);
    value = limit_full_last_samples;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_ok_show(struct kobject *kobj,
                                 struct kobj_attribute *attr,
                                 char *buf)
{
    unsigned int value;

    mutex_lock(&limit_lock);
    value = limit_full_ok;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_fail_show(struct kobject *kobj,
                                   struct kobj_attribute *attr,
                                   char *buf)
{
    unsigned int value;

    mutex_lock(&limit_lock);
    value = limit_full_fail;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_mean_us_show(struct kobject *kobj,
                                      struct kobj_attribute *attr,
                                      char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_full_mean_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_min_us_show(struct kobject *kobj,
                                     struct kobj_attribute *attr,
                                     char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_full_min_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_p50_us_show(struct kobject *kobj,
                                     struct kobj_attribute *attr,
                                     char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_full_p50_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_p90_us_show(struct kobject *kobj,
                                     struct kobj_attribute *attr,
                                     char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_full_p90_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_p99_us_show(struct kobject *kobj,
                                     struct kobj_attribute *attr,
                                     char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_full_p99_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_max_us_show(struct kobject *kobj,
                                     struct kobj_attribute *attr,
                                     char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_full_max_us;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t limit_full_rate_hz_show(struct kobject *kobj,
                                      struct kobj_attribute *attr,
                                      char *buf)
{
    u32 value;

    mutex_lock(&limit_lock);
    value = limit_full_rate_hz;
    mutex_unlock(&limit_lock);

    return sysfs_emit(buf, "%u\n", value);
}

static ssize_t power_mw_show(struct kobject *kobj,
                             struct kobj_attribute *attr,
                             char *buf)
{
    int value;

    mutex_lock(&sample_lock);
    value = latest_power_mw;
    mutex_unlock(&sample_lock);

    return sysfs_emit(buf, "%d\n", value);
}

static ssize_t voltage_mv_show(struct kobject *kobj,
                               struct kobj_attribute *attr,
                               char *buf)
{
    int value;

    mutex_lock(&sample_lock);
    value = latest_voltage_mv;
    mutex_unlock(&sample_lock);

    return sysfs_emit(buf, "%d\n", value);
}

static ssize_t current_ma_show(struct kobject *kobj,
                               struct kobj_attribute *attr,
                               char *buf)
{
    int value;

    mutex_lock(&sample_lock);
    value = latest_current_ma;
    mutex_unlock(&sample_lock);

    return sysfs_emit(buf, "%d\n", value);
}

static ssize_t read_status_show(struct kobject *kobj,
                                struct kobj_attribute *attr,
                                char *buf)
{
    int value;

    mutex_lock(&sample_lock);
    value = latest_read_status;
    mutex_unlock(&sample_lock);

    return sysfs_emit(buf, "%d\n", value);
}

static ssize_t sample_age_ms_show(struct kobject *kobj,
                                  struct kobj_attribute *attr,
                                  char *buf)
{
    unsigned long age_ms;

    mutex_lock(&sample_lock);
    age_ms = jiffies_to_msecs(jiffies - latest_jiffies);
    mutex_unlock(&sample_lock);

    return sysfs_emit(buf, "%lu\n", age_ms);
}

static ssize_t period_ms_show(struct kobject *kobj,
                              struct kobj_attribute *attr,
                              char *buf)
{
    return sysfs_emit(buf, "%u\n", period_ms);
}

static ssize_t period_ms_store(struct kobject *kobj,
                               struct kobj_attribute *attr,
                               const char *buf,
                               size_t count)
{
    unsigned int new_period;
    int ret;

    ret = kstrtouint(buf, 10, &new_period);
    if (ret)
        return ret;

    if (new_period < 10)
        return -EINVAL;

    period_ms = new_period;
    return count;
}

static ssize_t selected_bus_show(struct kobject *kobj,
                                 struct kobj_attribute *attr,
                                 char *buf)
{
    return sysfs_emit(buf, "%d\n", selected_bus_num);
}

static ssize_t selected_i2c_addr_show(struct kobject *kobj,
                                      struct kobj_attribute *attr,
                                      char *buf)
{
    return sysfs_emit(buf, "0x%02x\n", selected_i2c_addr);
}

static ssize_t auto_detect_used_show(struct kobject *kobj,
                                     struct kobj_attribute *attr,
                                     char *buf)
{
    return sysfs_emit(buf, "%d\n", auto_detect_used);
}

static ssize_t skip_bus_mask_show(struct kobject *kobj,
                                  struct kobj_attribute *attr,
                                  char *buf)
{
    return sysfs_emit(buf, "0x%lx\n", skip_bus_mask);
}



static struct kobj_attribute limit_power_only_run_attr =
    __ATTR_WO(limit_power_only_run);

static struct kobj_attribute limit_power_only_samples_attr =
    __ATTR_RO(limit_power_only_samples);

static struct kobj_attribute limit_power_only_ok_attr =
    __ATTR_RO(limit_power_only_ok);

static struct kobj_attribute limit_power_only_fail_attr =
    __ATTR_RO(limit_power_only_fail);

static struct kobj_attribute limit_power_only_mean_us_attr =
    __ATTR_RO(limit_power_only_mean_us);

static struct kobj_attribute limit_power_only_min_us_attr =
    __ATTR_RO(limit_power_only_min_us);

static struct kobj_attribute limit_power_only_p50_us_attr =
    __ATTR_RO(limit_power_only_p50_us);

static struct kobj_attribute limit_power_only_p90_us_attr =
    __ATTR_RO(limit_power_only_p90_us);

static struct kobj_attribute limit_power_only_p99_us_attr =
    __ATTR_RO(limit_power_only_p99_us);

static struct kobj_attribute limit_power_only_max_us_attr =
    __ATTR_RO(limit_power_only_max_us);

static struct kobj_attribute limit_power_only_rate_hz_attr =
    __ATTR_RO(limit_power_only_rate_hz);

static struct kobj_attribute limit_full_run_attr =
    __ATTR_WO(limit_full_run);

static struct kobj_attribute limit_full_samples_attr =
    __ATTR_RO(limit_full_samples);

static struct kobj_attribute limit_full_ok_attr =
    __ATTR_RO(limit_full_ok);

static struct kobj_attribute limit_full_fail_attr =
    __ATTR_RO(limit_full_fail);

static struct kobj_attribute limit_full_mean_us_attr =
    __ATTR_RO(limit_full_mean_us);

static struct kobj_attribute limit_full_min_us_attr =
    __ATTR_RO(limit_full_min_us);

static struct kobj_attribute limit_full_p50_us_attr =
    __ATTR_RO(limit_full_p50_us);

static struct kobj_attribute limit_full_p90_us_attr =
    __ATTR_RO(limit_full_p90_us);

static struct kobj_attribute limit_full_p99_us_attr =
    __ATTR_RO(limit_full_p99_us);

static struct kobj_attribute limit_full_max_us_attr =
    __ATTR_RO(limit_full_max_us);

static struct kobj_attribute limit_full_rate_hz_attr =
    __ATTR_RO(limit_full_rate_hz);

static struct kobj_attribute power_mw_attr =
    __ATTR_RO(power_mw);

static struct kobj_attribute voltage_mv_attr =
    __ATTR_RO(voltage_mv);

static struct kobj_attribute current_ma_attr =
    __ATTR_RO(current_ma);

static struct kobj_attribute read_status_attr =
    __ATTR_RO(read_status);

static struct kobj_attribute sample_age_ms_attr =
    __ATTR_RO(sample_age_ms);

static struct kobj_attribute period_ms_attr =
    __ATTR(period_ms, 0644, period_ms_show, period_ms_store);

static struct kobj_attribute selected_bus_attr =
    __ATTR_RO(selected_bus);

static struct kobj_attribute selected_i2c_addr_attr =
    __ATTR_RO(selected_i2c_addr);

static struct kobj_attribute auto_detect_used_attr =
    __ATTR_RO(auto_detect_used);

static struct kobj_attribute skip_bus_mask_attr =
    __ATTR_RO(skip_bus_mask);

static struct attribute *runtime_monitor_attrs[] = {
    &limit_power_only_run_attr.attr,
    &limit_power_only_samples_attr.attr,
    &limit_power_only_ok_attr.attr,
    &limit_power_only_fail_attr.attr,
    &limit_power_only_mean_us_attr.attr,
    &limit_power_only_min_us_attr.attr,
    &limit_power_only_p50_us_attr.attr,
    &limit_power_only_p90_us_attr.attr,
    &limit_power_only_p99_us_attr.attr,
    &limit_power_only_max_us_attr.attr,
    &limit_power_only_rate_hz_attr.attr,
    &limit_full_run_attr.attr,
    &limit_full_samples_attr.attr,
    &limit_full_ok_attr.attr,
    &limit_full_fail_attr.attr,
    &limit_full_mean_us_attr.attr,
    &limit_full_min_us_attr.attr,
    &limit_full_p50_us_attr.attr,
    &limit_full_p90_us_attr.attr,
    &limit_full_p99_us_attr.attr,
    &limit_full_max_us_attr.attr,
    &limit_full_rate_hz_attr.attr,
    &power_mw_attr.attr,
    &voltage_mv_attr.attr,
    &current_ma_attr.attr,
    &read_status_attr.attr,
    &sample_age_ms_attr.attr,
    &period_ms_attr.attr,
    &selected_bus_attr.attr,
    &selected_i2c_addr_attr.attr,
    &auto_detect_used_attr.attr,
    &skip_bus_mask_attr.attr,
    NULL,
};

static const struct attribute_group runtime_monitor_attr_group = {
    .attrs = runtime_monitor_attrs,
};

static int runtime_monitor_create_sysfs(void)
{
    int ret;

    runtime_monitor_kobj = kobject_create_and_add("runtime_monitor", kernel_kobj);
    if (!runtime_monitor_kobj)
        return -ENOMEM;

    ret = sysfs_create_group(runtime_monitor_kobj, &runtime_monitor_attr_group);
    if (ret) {
        kobject_put(runtime_monitor_kobj);
        runtime_monitor_kobj = NULL;
        return ret;
    }

    return 0;
}

static void runtime_monitor_remove_sysfs(void)
{
    if (runtime_monitor_kobj) {
        sysfs_remove_group(runtime_monitor_kobj, &runtime_monitor_attr_group);
        kobject_put(runtime_monitor_kobj);
        runtime_monitor_kobj = NULL;
    }
}

static int __init runtime_monitor_init(void)
{
    int ret;

    pr_info("runtime_monitor: module loaded\n");
    pr_info("runtime_monitor: requested bus_num=%d i2c_addr=%d period_ms=%u max_bus_num=%u skip_internal_buses=%d skip_bus_mask=0x%lx\n",
            bus_num, i2c_addr, period_ms, max_bus_num, skip_internal_buses, skip_bus_mask);

    ret = runtime_monitor_detect_device();
    if (ret) {
        pr_err("runtime_monitor: device detection failed, ret=%d\n", ret);
        return ret;
    }

    ret = runtime_monitor_create_sysfs();
    if (ret) {
        pr_err("runtime_monitor: failed to create sysfs, ret=%d\n", ret);
        if (ina260_client) {
            i2c_unregister_device(ina260_client);
            ina260_client = NULL;
        }
        if (ina260_adapter) {
            i2c_put_adapter(ina260_adapter);
            ina260_adapter = NULL;
        }
        return ret;
    }

    mutex_lock(&sample_lock);
    latest_current_ma = 0;
    latest_voltage_mv = 0;
    latest_power_mw = 0;
    latest_read_status = -EAGAIN;
    latest_jiffies = jiffies;
    mutex_unlock(&sample_lock);

    mutex_lock(&limit_lock);
    limit_full_last_samples = 0;
    limit_full_ok = 0;
    limit_full_fail = 0;
    limit_full_total_us = 0;
    limit_full_mean_us = 0;
    limit_full_min_us = 0;
    limit_full_p50_us = 0;
    limit_full_p90_us = 0;
    limit_full_p99_us = 0;
    limit_full_max_us = 0;
    limit_full_rate_hz = 0;
    limit_power_only_samples = 0;
    limit_power_only_ok = 0;
    limit_power_only_fail = 0;
    limit_power_only_total_us = 0;
    limit_power_only_mean_us = 0;
    limit_power_only_min_us = 0;
    limit_power_only_p50_us = 0;
    limit_power_only_p90_us = 0;
    limit_power_only_p99_us = 0;
    limit_power_only_max_us = 0;
    limit_power_only_rate_hz = 0;
    mutex_unlock(&limit_lock);

    INIT_WORK(&monitor_work, monitor_tick);
    hrtimer_init(&monitor_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    monitor_timer.function = monitor_timer_tick;
    queue_work(system_highpri_wq, &monitor_work);
    hrtimer_start(&monitor_timer, ms_to_ktime(period_ms), HRTIMER_MODE_REL);

    return 0;
}

static void __exit runtime_monitor_exit(void)
{
    hrtimer_cancel(&monitor_timer);
    cancel_work_sync(&monitor_work);

    if (ina260_client) {
        i2c_unregister_device(ina260_client);
        ina260_client = NULL;
    }

    if (ina260_adapter) {
        i2c_put_adapter(ina260_adapter);
        ina260_adapter = NULL;
    }

    runtime_monitor_remove_sysfs();

    pr_info("runtime_monitor: module unloaded\n");
}

module_init(runtime_monitor_init);
module_exit(runtime_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tiare");
MODULE_DESCRIPTION("Auto-detecting high-resolution runtime power monitoring module for Jetson embedded GPUs");
