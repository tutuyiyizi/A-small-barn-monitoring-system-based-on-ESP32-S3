# ESP32-S3-N16R8 谷仓环境监测系统接线方案

## 1. 接线前注意事项

- 所有传感器和ESP32-S3必须共地，即所有`GND`相连。
- 本方案中的I2C模块统一使用`3.3V`供电，禁止将I2C信号线接到5V。
- ESP32-S3 GPIO只能承受3.3V电平，不能直接接5V信号。
- 以下GPIO分配避开了常见的启动相关引脚和USB引脚；最终以实际ESP32-S3-N16R8开发板引脚标识为准。

## 2. ESP32-S3引脚总表

| ESP32-S3引脚 | 连接设备 | 功能 |
|---|---|---|
| 3V3 | SHT30、SGP30、BH1750 | 传感器3.3V供电 |
| GND | 所有模块 | 公共地 |
| GPIO8 | SHT30、SGP30、BH1750的SDA | I2C数据线 |
| GPIO9 | SHT30、SGP30、BH1750的SCL | I2C时钟线 |
| GPIO4 | DS18B20 DATA | 单总线温度数据 |
| GPIO5 | MC-38门磁 | 门状态输入 |

## 3. SHT30温湿度模块

| SHT30引脚 | 接到ESP32-S3 |
|---|---|
| VCC / VIN | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |

SHT30通常I2C地址为`0x44`，部分模块通过地址焊盘配置为`0x45`。

## 4. SGP30气体传感器模块

| SGP30引脚 | 接到ESP32-S3 |
|---|---|
| VCC / VIN | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |

SGP30与SHT30共用I2C总线，常见地址为`0x58`。它输出TVOC和eCO2空气质量趋势值；其中eCO2是估算指标，不是精准CO2浓度。

## 5. GY-302 BH1750光照模块

| GY-302引脚 | 接到ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |
| ADDR | 悬空或接GND |

`ADDR`悬空或接GND时，BH1750常见地址为`0x23`；接3.3V时通常为`0x5C`。本项目只用一个BH1750，建议保持悬空或接GND。

## 6. DS18B20防水温度传感器

| DS18B20线色或引脚 | 接到ESP32-S3 |
|---|---|
| 红线 / VDD | 3V3 |
| 黑线 / GND | GND |
| 黄线、白线或DATA | GPIO4 |

在`GPIO4`与`3V3`之间接一个`4.7k欧`电阻作为上拉电阻。

```text
3V3 ----+------------------ DS18B20 VDD
        |
       4.7k欧
        |
GPIO4 --+------------------ DS18B20 DATA

GND ----------------------- DS18B20 GND
```

不同厂家DS18B20探头的DATA线颜色可能不同。第一次上电前先确认商品说明或用万用表核对，不能只依赖线色。

## 7. MC-38门磁开关

MC-38是无源干接点，只需要两根线。

| MC-38任意一根线 | 接到ESP32-S3 |
|---|---|
| 线1 | GPIO5 |
| 线2 | GND |

程序中把GPIO5配置为`INPUT_PULLUP`。磁铁远离或靠近时的高低电平与MC-38的常开/常闭型号有关，通电后用串口打印状态确认一次即可。

## 8. I2C总线连接示意

```text
ESP32-S3 GPIO8  ---------- SHT30 SDA
                 |-------- SGP30 SDA
                 |-------- BH1750 SDA

ESP32-S3 GPIO9  ---------- SHT30 SCL
                 |-------- SGP30 SCL
                 |-------- BH1750 SCL

ESP32-S3 3V3    ---------- 三个I2C模块的VCC
ESP32-S3 GND    ---------- 三个I2C模块的GND
```

如果三个模块都有I2C上拉电阻，短线原型通常可以正常工作；PCB定稿时建议只保留一组有效上拉电阻，推荐4.7k欧上拉到3.3V。

## 9. 后续扩展接线预留

| 设备 | ESP32-S3引脚 | 接线说明 |
|---|---|---|
| OLED SSD1306 | GPIO8、GPIO9 | 与三个I2C传感器共用SDA/SCL；VCC接3V3，GND接GND |
| 有源蜂鸣器 | GPIO7 | GPIO7接信号端；用户已确认低电平触发，选择3.3V兼容模块 |
| 绿LED | GPIO15 | GPIO15经220欧电阻接LED正极，LED负极接GND |
| 红LED | GPIO16 | GPIO16经220欧电阻接LED正极，LED负极接GND |

GPIO6 未分配，无需连接负载。

## 10. 上电检查顺序

1. 先只连接ESP32-S3并确认可下载程序。
2. 连接SHT30、SGP30、BH1750，运行I2C扫描程序，确认地址为三个不同值。
3. 连接DS18B20和4.7k欧上拉电阻，确认读取温度正常。
4. 连接MC-38，确认开门和关门时GPIO5状态改变。
5. 最后再接OLED和蜂鸣器。
## 11. 第二个 OLED I2C 模块（J7）

在现有 OLED 接口 J5 的基础上，增加第二个四针 OLED 接口，编号为 J7。J7 与 J5 以及 SHT30、SGP30、BH1750 共用同一条 I2C 总线，接线如下：

| J7 引脚 | 接到 ESP32-S3 | 说明 |
|---|---|---|
| GND | GND | 公共地 |
| VCC | 3V3 | 只使用 3.3V 供电 |
| SCL | GPIO9 | I2C 时钟线，与 J5 的 SCL 并联 |
| SDA | GPIO8 | I2C 数据线，与 J5 的 SDA 并联 |

```text
GPIO8 (SDA) ---------- J5 OLED SDA
       |-------------- J7 OLED SDA

GPIO9 (SCL) ---------- J5 OLED SCL
       |-------------- J7 OLED SCL

3V3 ------------------ J5 OLED VCC
       |-------------- J7 OLED VCC

GND ------------------ J5 OLED GND
       |-------------- J7 OLED GND
```

两个 OLED 必须设置为不同的 I2C 地址，否则会发生总线地址冲突。常见 SSD1306 OLED 地址为 `0x3C`，部分模块可通过电阻焊盘改为 `0x3D`。建议将 J5 设置为 `0x3C`、J7 设置为 `0x3D`，上电后用 I2C 扫描程序确认。

J7 的接口针序固定为从左到右：`GND / VCC / SCL / SDA`。制作 PCB 或接线端子时，必须在丝印和实际插头方向上保持这个顺序。
### J7 OLED GPIO 连接

- SDA → GPIO8
- SCL → GPIO9
- VCC → 3V3
- GND → GND
