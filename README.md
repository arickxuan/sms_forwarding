# 低成本短信转发器

> 本项目修改自linuxdo大佬的[项目](https://github.com/chenxuuu/sms_forwarding

本项目仅用于接收短信与进行保号相关功能。  
多卡控制、通话、拨号、开放接口、eSIM卡管理等功能永远不会支持，请勿提出相关需求。

![后台页面演示](assets/esim.png)

本项目旨在使用低成本的硬件设备，实现短信的自动转发功能，支持多种推送方式同时启用。

> 视频教程：[B站视频](https://www.bilibili.com/video/BV1cSmABYEiX)

<img src="assets/photo.png" width="200" />

## 功能

- 支持使用通用AT指令与模块进行通信
- 开启后支持通过WEB界面配置短信转发参数、查询当前状态
- **支持多达5个推送通道同时启用**，每个通道可独立配置
- 支持将收到的短信转发到指定的邮箱
- 支持通过WEB界面主动发送短信，以便消耗余额
- 支持通过WEB界面进行Ping测试，以极低的成本消耗余额
- 支持长短信自动合并（30秒超时）
- 支持管理员短信远程发送短信和重启设备
- 支持eSIM卡管理功能

## 推送通道支持

支持以下7种推送方式，可同时启用多个通道：

| 推送方式 | 说明 | 需要配置 |
|---------|------|---------|
| **POST JSON** | 通用HTTP POST | URL |
| **Bark** | iOS推送服务 | Bark服务器URL |
| **飞书机器人** | 自定义通知 | Webhook URL |

### 推送格式说明

- **POST JSON**: `{"sender":"发送者号码","message":"短信内容","timestamp":"时间戳"}`
- **Bark**: `{"title":"发送者号码","body":"短信内容"}`
- **飞书机器人**: 文本消息格式，支持加签验证

|状态信息|主动ping|
|-|-|
|![](assets/status.png)|![](assets/ping.png)|

## 硬件搭配

如果希望自行焊接硬件，参考下面的硬件搭配。

- ESP32C3开发板
- ML307A开发板
- 4G FPC天线

就不推荐某某鲸了，过程不太愉快，解决不了问题就说我焊短路了，把模块烧了。气死我了，果断退货。
其实是usb-tll兼容问题，直连树莓派就好了
自行某宝搜索型号即可

## 硬件连接

ESP32C3 与 ML307A 通过串口（UART）连接，接线如下：

```
┌───────────────────────────────────────────────┐
|                                               |
|   ESP32C3 Super Mini      ML307R-DC核心板     |
| ┌───────────────────┐    ┌─────────────────┐ |
└─┼─ GPIO5 (MODEM_EN) │    │                 │ |
  │       GPIO3 (TX) ─┼───►│ RX              │ |
  │                   │    │             EN ─┼─┘
  │       GPIO4 (RX) ◄┼────┤ TX              │ 
  │                   │    │                 │ 
  │              GND ─┼────┤ GND             │ 
  │                   │    │                 │ 
  │               5V ─┼────┤ VCC (5V)        |
  │                   │    │                 │
  └───────────────────┘    └─────────────────┘
                           │                 │
                           │  SIM卡槽        │
                           │  (插入Nano SIM) │
                           │                 │
                           │  天线接口       │
                           │  (连接4G天线)   │
                           └─────────────────┘
```
改变接线方式，核心板不再和en短接而是和esp32c3的GPIO5连接，使模块能够被控制上下电(代码也同步改动)。
可通过USB连接ESP32C3进行编程和供电，正常工作时，ESP32C3的虚拟串口数据将直接被转发到ML307A，方便调试。

## 软件组成

- ESP32C3运行自己的`Arduino`固件，负责连接WiFi和接收ML307R-DC发送过来的短信数据，然后转发到指定HTTP接口或邮箱
- ML307A运行默认的AT固件，不用动



需要在`Arduino IDE`中单独安装这些库：

其他开发板管理器地址：
https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json

开发板管理器 esp32

lib：
- **ReadyMail** by Mobizt
- **pdulib** by David Henry

需要在`Arduino IDE`中安装ESP32开发板支持，参考[官方文档](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)，版型选`MakerGO ESP32 C3 SuperMini`。
