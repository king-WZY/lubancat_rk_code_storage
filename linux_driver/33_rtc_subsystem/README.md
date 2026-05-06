# rtc_subsystem

运行`make`命令后，将会有一个模块：

* rtc_pcf8563.ko

加载驱动程序和内核调试信息：

1. rtc_pcf8563.ko

```bash
# insmod rtc_pcf8563.ko

   [   38.297596] rtc-pcf8563 3-0051: registered as rtc0
   [   38.298545] rtc-pcf8563 3-0051: setting system clock to 2026-05-06T06:34:20 UTC (1778049260)
   [   38.299485] rtc-pcf8563 3-0051: PCF8563 RTC probed successfully

#如果系统有网络，重启ntp服务可网络校准时间

   sudo systemctl restart ntp

#如果系统无网络，可通过date命令手动设置当前时间

   sudo date -s "2026-05-06 14:30:30"

#系统时间同步到硬件rtc

   sudo hwclock -w

#硬件rtc同步到系统

    sudo hwclock -s

#查看rtc0的日期时间

   cat /sys/class/rtc/rtc0/date

#信息打印如下

   2026-05-06

#查看rtc0的小时、分钟、秒时间

   cat /sys/class/rtc/rtc0/time

#信息打印如下

   06:34:51

#通过date命令查询本地时间

   date

#信息打印如下

   2026年 05月 06日 星期三 14:34:55 CST

#通过date命令查询系统时间，UTC（世界协调时间）

   date -u

#信息打印如下

   2026年 05月 06日 星期三 06:34:57 UTC
```