#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/bcd.h>
#include <linux/of.h>

#define PCF8563_REG_ST1     0x00        /* 控制和状态寄存器1地址，用于控制RTC基础工作状态 */

#define PCF8563_REG_ST2     0x01        /* 控制和状态寄存器2地址，用于中断控制、闹钟标志位管理 */
#define PCF8563_BIT_AIE     (1 << 1)    /* 闹钟中断使能位，置1表示开启闹钟中断功能 */
#define PCF8563_BIT_AF      (1 << 3)    /* 闹钟触发标志位，置1表示闹钟时间已到 */
#define PCF8563_BITS_ST2_N  (7 << 5)    /* 控制和状态寄存器2保留位掩码，用于清零无效位 */

#define PCF8563_REG_SC      0x02        /* 秒寄存器地址，存储当前时间秒值，BCD码格式 */
#define PCF8563_REG_MN      0x03        /* 分寄存器地址，存储当前时间分值，BCD码格式 */
#define PCF8563_REG_HR      0x04        /* 时寄存器地址，存储当前时间时值，BCD码格式 */
#define PCF8563_REG_DM      0x05        /* 日期寄存器地址，存储当前日期，BCD码格式 */
#define PCF8563_REG_DW      0x06        /* 星期寄存器地址，存储当前星期，BCD码格式 */
#define PCF8563_REG_MO      0x07        /* 世纪标志和月份寄存器，存储当前月份，BCD码格式，含世纪位 */
#define PCF8563_REG_YR      0x08        /* 年寄存器地址，存储当前年份低2位，BCD码格式 */
#define PCF8563_REG_AMN     0x09        /* 闹钟分寄存器地址，存储闹钟触发分值，BCD码格式 */
#define PCF8563_REG_TMRC    0x0E        /* 定时器控制寄存器地址，用于配置内部定时器工作模式 */

#define PCF8563_TMRC_1_60   3           /* 定时器分频配置，1/60Hz最低功耗模式 */

#define PCF8563_SC_LV       0x80        /* 低压检测标志位，置1表示RTC供电异常，时间不可靠 */
#define PCF8563_MO_C        0x80        /* 世纪标志位，用于区分19xx/20xx年份 */

/* PCF8563驱动私有数据结构体 */
struct pcf8563_data {
    struct i2c_client *client;  /* I2C客户端句柄，指向I2C从设备实例 */
    struct regmap *regmap;      /* Regmap寄存器映射句柄，用于寄存器读写 */
    struct rtc_device *rtc;     /* RTC设备句柄，注册到内核RTC子系统 */
    int c_polarity;             /* 世纪位极性标记，判断19xx/20xx年份规则 */
    int voltage_low;            /* 低压状态标记，1表示检测到供电异常 */
};

/* Regmap配置参数 */
static const struct regmap_config pcf8563_regmap_config = {
    .reg_bits = 8,                  /* 寄存器地址位宽：8位 */
    .val_bits = 8,                  /* 寄存器数据位宽：8位 */
    .max_register = 0x0F,           /* 最大可访问寄存器地址 */
    .cache_type = REGCACHE_NONE,    /* 关闭寄存器缓存，保证实时性 */
};

/* 闹钟中断模式配置函数：设置闹钟中断使能/禁用，并清除闹钟标志位 */
static int pcf8563_set_alarm_mode(struct pcf8563_data *data, bool on)
{
    /* 临时存储寄存器读取值 */
    unsigned int val;
    /* 函数返回值 */
    int ret;

    /* 读取状态寄存器2的值 */
    ret = regmap_read(data->regmap, PCF8563_REG_ST2, &val);
    if (ret)
        return ret;

    /* 判断是否需要开启闹钟中断 */
    if (on)
        val |= PCF8563_BIT_AIE;     /* 开启闹钟中断使能位 */
    else
        val &= ~PCF8563_BIT_AIE;    /* 关闭闹钟中断使能位 */

    val &= ~(PCF8563_BIT_AF | PCF8563_BITS_ST2_N);      /* 清除闹钟标志位与保留位 */

    /* 写入配置后的寄存器值并返回结果 */
    return regmap_write(data->regmap, PCF8563_REG_ST2, val);
}

/* 闹钟中断处理函数：响应闹钟触发事件 */
static irqreturn_t pcf8563_irq(int irq, void *dev_id)
{
    /* 获取I2C客户端设备实例 */
    struct i2c_client *client = dev_id;
    /* 获取驱动私有数据 */
    struct pcf8563_data *data = i2c_get_clientdata(client);
    /* 存储状态寄存器2的值 */
    unsigned int st2;

    /* 读取状态寄存器2，判断是否成功 */
    if (regmap_read(data->regmap, PCF8563_REG_ST2, &st2))
        return IRQ_NONE;

    /* 判断闹钟标志位是否未触发 */
    if (!(st2 & PCF8563_BIT_AF))
        return IRQ_NONE;

    /* 向RTC子系统上报闹钟中断事件 */
    rtc_update_irq(data->rtc, 1, RTC_IRQF | RTC_AF);

    /* 重新使能闹钟中断，等待下一次触发 */
    pcf8563_set_alarm_mode(data, 1);

    /* 中断处理完成，返回已处理 */
    return IRQ_HANDLED;
}

/* 读取RTC当前时间函数：从硬件读取时间，转换为内核标准rtc_time格式 */
static int pcf8563_get_time(struct device *dev, struct rtc_time *tm)
{   
    /* 从设备指针转换为I2C客户端指针 */
    struct i2c_client *client = to_i2c_client(dev);
    /* 获取驱动私有数据 */
    struct pcf8563_data *data = i2c_get_clientdata(client);
    /* 批量存储寄存器读取数据 */
    u8 buf[9];
    /* 函数返回值 */
    int ret;

    /* 批量读取前9个寄存器数据 */
    ret = regmap_bulk_read(data->regmap, PCF8563_REG_ST1, buf, 9);
    if (ret)
        return ret;

    /* 判断是否检测到低压标志 */
    if (buf[PCF8563_REG_SC] & PCF8563_SC_LV) {
        data->voltage_low = 1;                          /* 标记低压状态为异常 */
        dev_err(dev, "low voltage, time invalid!\n");   /* 打印时间无效错误日志 */
        return -EINVAL;
    }

    tm->tm_sec  = bcd2bin(buf[PCF8563_REG_SC] & 0x7F);      /* 秒BCD码转二进制，屏蔽低压位 */
    tm->tm_min  = bcd2bin(buf[PCF8563_REG_MN] & 0x7F);      /* 分BCD码转二进制，屏蔽无效位 */
    tm->tm_hour = bcd2bin(buf[PCF8563_REG_HR] & 0x3F);      /* 时BCD码转二进制，屏蔽无效位 */
    tm->tm_mday = bcd2bin(buf[PCF8563_REG_DM] & 0x3F);      /* 日期BCD码转二进制，屏蔽无效位 */
    tm->tm_wday = buf[PCF8563_REG_DW] & 0x07;               /* 获取星期值，屏蔽无效位 */
    tm->tm_mon  = bcd2bin(buf[PCF8563_REG_MO] & 0x1F) - 1;  /* 月BCD码转二进制，系统月份从0开始 */
    tm->tm_year = bcd2bin(buf[PCF8563_REG_YR]);             /* 年BCD码转二进制 */

    if (tm->tm_year < 70)       /* 判断年份是否小于70，对应2070年分界 */
        tm->tm_year += 100;     /* 年份偏移100，适配1970-2069时间范围 */

    /* 根据世纪位设置极性标记，判断19xx/20xx年份，PCF8563_MO_C的值为1000 0000，因此判断的是世纪标志和月份寄存器的位号7：世纪标志位 */
    data->c_polarity = (buf[PCF8563_REG_MO] & PCF8563_MO_C)
        ? (tm->tm_year >= 100)
        : (tm->tm_year < 100);

    return 0;
}

/* 设置RTC硬件时间函数：将内核标准时间写入硬件，同步RTC时钟 */
static int pcf8563_set_time(struct device *dev, struct rtc_time *tm)
{
    /* 从设备指针转换为I2C客户端指针 */
    struct i2c_client *client = to_i2c_client(dev);
    /* 获取驱动私有数据 */
    struct pcf8563_data *data = i2c_get_clientdata(client);
    /* 存储时间寄存器写入数据 */
    u8 buf[7];

    buf[0] = bin2bcd(tm->tm_sec);        /* 秒二进制转BCD码，准备写入 */
    buf[1] = bin2bcd(tm->tm_min);        /* 分二进制转BCD码，准备写入 */
    buf[2] = bin2bcd(tm->tm_hour);       /* 时二进制转BCD码，准备写入 */
    buf[3] = bin2bcd(tm->tm_mday);       /* 日期二进制转BCD码，准备写入 */
    buf[4] = tm->tm_wday & 0x07;         /* 星期纯二进制，直接写入 */
    buf[5] = bin2bcd(tm->tm_mon + 1);    /* 月份+1后转BCD码，硬件月份从1开始 */
    buf[6] = bin2bcd(tm->tm_year % 100); /* 年份取低2位转BCD码 */

    /* 根据世纪位极性回填世纪标志位 */
    if (data->c_polarity ? (tm->tm_year >= 100) : (tm->tm_year < 100))
        buf[5] |= PCF8563_MO_C; /* 置位世纪标志位，
                                 * buf[5]是世纪标志和月份寄存器，PCF8563_MO_C的值为1000 0000
                                 * 因此设置的是世纪标志和月份寄存器的位号7：世纪标志位 */

    /* 批量写入时间寄存器并返回结果 */
    return regmap_bulk_write(data->regmap, PCF8563_REG_SC, buf, 7);
}

/* 读取闹钟配置函数：从硬件读取闹钟时间与使能状态 */
static int pcf8563_read_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
    /* 从设备指针转换为I2C客户端指针 */
    struct i2c_client *client = to_i2c_client(dev);
    /* 获取驱动私有数据 */
    struct pcf8563_data *data = i2c_get_clientdata(client);
    /* 批量存储闹钟寄存器读取数据 */
    u8 buf[4];
    /* 存储状态寄存器2的值 */
    unsigned int st2;

    /* 批量读取4个闹钟寄存器 */
    if (regmap_bulk_read(data->regmap, PCF8563_REG_AMN, buf, 4))
        return -EIO;

    alm->time.tm_sec  = 0;                          /* PCF8563不支持秒级闹钟，固定为0 */
    alm->time.tm_min  = bcd2bin(buf[0] & 0x7F);     /* 闹钟分BCD码转二进制 */
    alm->time.tm_hour = bcd2bin(buf[1] & 0x3F);     /* 闹钟时BCD码转二进制 */
    alm->time.tm_mday = bcd2bin(buf[2] & 0x3F);     /* 闹钟日期BCD码转二进制 */
    alm->time.tm_wday = buf[3] & 0x07;             /* 闹钟星期是二进制，不需要转换 */

    /* 读取状态寄存器2 */
    if (regmap_read(data->regmap, PCF8563_REG_ST2, &st2))
        return -EIO;

    alm->enabled = !!(st2 & PCF8563_BIT_AIE);   /* 获取闹钟中断使能状态 */
    alm->pending = !!(st2 & PCF8563_BIT_AF);    /* 获取闹钟触发标志状态 */

    return 0;
}

/* 设置闹钟配置函数：将闹钟时间写入硬件，配置闹钟触发条件 */
static int pcf8563_set_alarm(struct device *dev, struct rtc_wkalrm *alm)
{   
    /* 从设备指针转换为I2C客户端指针 */
    struct i2c_client *client = to_i2c_client(dev);
    /* 获取驱动私有数据 */
    struct pcf8563_data *data = i2c_get_clientdata(client);
    /* 存储闹钟寄存器写入数据 */
    u8 buf[4];

    /* 判断是否设置秒级闹钟 */
    if (alm->time.tm_sec) {
        time64_t t = rtc_tm_to_time64(&alm->time);  /* 转换闹钟时间为时间戳 */
        t += 60 - alm->time.tm_sec;                 /* 向上取整到最近一分钟 */
        rtc_time64_to_tm(t, &alm->time);            /* 转换回rtc_time格式 */
    }

    buf[0] = bin2bcd(alm->time.tm_min);     /* 闹钟分二进制转BCD码 */
    buf[1] = bin2bcd(alm->time.tm_hour);    /* 闹钟时二进制转BCD码 */
    buf[2] = bin2bcd(alm->time.tm_mday);    /* 闹钟日期二进制转BCD码 */
    buf[3] = alm->time.tm_wday & 0x07;      /* 闹钟星期值，屏蔽无效位 */

    /* 批量写入闹钟寄存器 */
    if (regmap_bulk_write(data->regmap, PCF8563_REG_AMN, buf, 4))
        return -EIO;

    /* 配置闹钟中断使能状态并返回结果 */
    return pcf8563_set_alarm_mode(data, alm->enabled);
}

/* 闹钟中断使能/禁用控制：统一控制闹钟中断的开启与关闭 */
static int pcf8563_alarm_irq_enable(struct device *dev, unsigned int enable)
{
    /* 从设备指针转换为I2C客户端指针 */
    struct i2c_client *client = to_i2c_client(dev);
    /* 获取驱动私有数据 */
    struct pcf8563_data *data = i2c_get_clientdata(client);

    /* 调用闹钟模式配置函数，设置中断使能 */
    return pcf8563_set_alarm_mode(data, !!enable);
}

/* RTC子系统操作接口集合 */
static const struct rtc_class_ops pcf8563_rtc_ops = {
    .read_time      = pcf8563_get_time,             /* 绑定时间读取回调函数 */
    .set_time       = pcf8563_set_time,             /* 绑定时间设置回调函数 */
    .read_alarm     = pcf8563_read_alarm,           /* 绑定闹钟读取回调函数 */
    .set_alarm      = pcf8563_set_alarm,            /* 绑定闹钟设置回调函数 */
    .alarm_irq_enable = pcf8563_alarm_irq_enable,   /* 绑定闹钟中断使能回调函数 */
};


/* probe函数：I2C设备匹配成功后执行，初始化硬件与驱动资源 */
static int pcf8563_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    /* 驱动私有数据指针 */
    struct pcf8563_data *data;
    /* 函数返回值 */
    int ret;

    /* 动态分配私有数据内存，自动释放 */
    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    /* 绑定I2C客户端实例到私有数据 */
    data->client = client;
    /* 将私有数据挂载到I2C客户端 */
    i2c_set_clientdata(client, data);

    /* 初始化Regmap寄存器映射 */
    data->regmap = devm_regmap_init_i2c(client, &pcf8563_regmap_config);
    if (IS_ERR(data->regmap)) {
        dev_err(&client->dev, "regmap init failed\n");
        return PTR_ERR(data->regmap);
    }

    /* 配置定时器为最低功耗模式 */
    ret = regmap_write(data->regmap, PCF8563_REG_TMRC, PCF8563_TMRC_1_60);
    if (ret)
        return ret;

    /* 清除所有中断标志位，禁用中断 */
    ret = regmap_write(data->regmap, PCF8563_REG_ST2, 0x00);
    if (ret)
        return ret;

    /* 向内核RTC子系统注册RTC设备 */
    data->rtc = devm_rtc_device_register(&client->dev, "pcf8563", &pcf8563_rtc_ops, THIS_MODULE);
    if (IS_ERR(data->rtc))
        return PTR_ERR(data->rtc);

    /* 判断设备树是否配置中断引脚 */
    if (client->irq > 0) {
        /* 申请线程化中断，绑定闹钟中断服务函数 */
        ret = devm_request_threaded_irq(&client->dev, client->irq,
                        NULL, pcf8563_irq,
                        IRQF_TRIGGER_LOW | IRQF_ONESHOT,
                        "pcf8563", client);
        if (ret) {
            dev_err(&client->dev, "irq request failed\n");
            return ret;
        }
    }

    /* 设置设备支持休眠唤醒功能 */
    device_set_wakeup_capable(&client->dev, true);

    dev_info(&client->dev, "PCF8563 RTC probed successfully\n");
    return 0;
}

/* 定义设备树匹配表 */
static const struct of_device_id pcf8563_of_match[] = {
    { .compatible = "fire,pcf8563" },
    { }
};

/* 声明设备树匹配表，供内核识别 */
MODULE_DEVICE_TABLE(of, pcf8563_of_match);


/* 定义i2c总线设备结构体 */
static struct i2c_driver pcf8563_driver = {
    .probe = pcf8563_probe,
    .driver = {
        .name = "rtc-pcf8563",
        .of_match_table = pcf8563_of_match,
    },
};

/* 注册/注销I2C驱动，简化模块入口/出口函数 */
module_i2c_driver(pcf8563_driver);

MODULE_AUTHOR("embedfire <embedfire@embedfire.com>");
MODULE_DESCRIPTION("rtc-pcf8563 module");
MODULE_LICENSE("GPL");