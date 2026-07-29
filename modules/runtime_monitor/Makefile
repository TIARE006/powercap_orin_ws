obj-m += runtime_monitor.o

KDIR ?= /data/jetson_power_monitor/l4t/Linux_for_Tegra/source/kernel/kernel-jammy-src
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-tegra modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-tegra clean
