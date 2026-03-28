#!/usr/bin/env python3
from jtop import jtop
import time

def main():
    try:
        with jtop() as jetson:
            while jetson.ok():
                s = jetson.stats

                power_tot = s.get("Power TOT", "")
                power_cpu_gpu_cv = s.get("Power VDD_CPU_GPU_CV", "")
                power_soc = s.get("Power VDD_SOC", "")

                print(
                    f"{power_tot},"
                    f"{power_cpu_gpu_cv},"
                    f"{power_soc}",
                    flush=True
                )

                time.sleep(0.2)   # 5 Hz
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()