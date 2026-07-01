#include "globals.h"
#include "wifi_config.h"
#include "config.h"
#include "web_handlers.h"
#include "web_handlers.h"
#include "modem.h"
#include "web_handlers.h"
#include "push.h"
#include "web_handlers.h"
#include "sms_process.h"
#include "web_handlers.h"
#include "esim.h"
#include <esp_task_wdt.h>
#include <esp_idf_version.h>

static void initTaskWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 90000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_init(&wdtConfig);
  if (err == ESP_ERR_INVALID_STATE) {
    err = esp_task_wdt_reconfigure(&wdtConfig);
  }
#else
  esp_err_t err = esp_task_wdt_init(90, true);
#endif
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_add(NULL);
    logCaptureLn(String("任务看门狗已启用: 90s"));
  } else {
    logCaptureLn(String("任务看门狗启用失败: ") + String((int)err));
  }
}

static void registerRoutes() {
  const char* collectHeaders[] = { "If-None-Match" };
  server.collectHeaders(collectHeaders, 1);

  server.on("/", handleRoot);
  server.on("/api/config", handleApiConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleRoot);
  server.on("/sms", handleRoot);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/query", handleQuery);
  server.on("/flight", handleFlightMode);
  server.on("/at", handleATCommand);
  server.on("/log", handleLog);
  server.on("/modem", handleModem);
  server.on("/wifi", handleWifi);
  server.on("/wifisave", HTTP_POST, handleWifiSave);
  server.on("/esim", handleESim);
  server.onNotFound(handleNotFound);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  // 缩短初始化延时，WiFi连接会处理自己的超时
  delay(200);
  initTaskWatchdog();
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  configValid = isConfigValid();
  registerRoutes();

  bool wifiConnected = false;
  for (int attempt = 1; attempt <= 3 && !wifiConnected; attempt++) {
    logCaptureLn(String("WiFi连接尝试 ") + String(attempt) + "/3");
    wifiConnected = connectConfiguredWiFi(20000);
  }
  if (!wifiConnected) {
    logCaptureLn(String("⚠️ WiFi连接失败，进入AP应急配网模式"));
    startProvisioningAP();
  }

  server.begin();
  logCaptureLn(String("HTTP服务器已启动"));

  // ---- NTP 时间同步 ----
  if (WiFi.status() == WL_CONNECTED) {
    logCaptureLn(String("正在同步NTP时间..."));
    configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
    int ntpRetry = 0;
    while (time(nullptr) < 100000 && ntpRetry < 100) {
      delay(1);
      server.handleClient();
      ntpRetry++;
    }
    if (time(nullptr) >= 100000) {
      timeSynced = true;
      logCaptureLn(String("NTP时间同步成功"));
      time_t now = time(nullptr);
      logCapture(String("当前UTC时间戳: "));
      logCaptureLn(String(now));
    } else {
      logCaptureLn(String("NTP时间同步失败，将使用设备时间"));
    }
  } else {
    logCaptureLn(String("WiFi未连接，跳过NTP时间同步"));
  }

  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, config.ledEnabled ? LOW : HIGH);

  // ---- 启动通知（网页已可用，发邮件不会影响用户访问） ----
  if (configValid && WiFi.status() == WL_CONNECTED) {
    logCaptureLn(String("配置有效，发送启动通知..."));
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }



  // ---- 模组初始化（较慢，但网页已可访问） ----
  modemInit();

  // ---- eSIM初始化 ----
  logCaptureLn(String("初始化eSIM..."));
  if (esimInit()) {
    logCaptureLn(String("eSIM初始化成功"));
    char eid[40];
    if (esimGetEID(eid, sizeof(eid))) {
      logCapture(String("EID: "));
      logCaptureLn(eid);
    }
  } else {
    logCaptureLn(String("eSIM初始化失败或未检测到eUICC芯片"));
  }

}

void loop() {
  esp_task_wdt_reset();
  if (provisioningMode) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  if (!provisioningMode && WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiReconnect = 0;
    if (millis() - lastWifiReconnect >= 60000) {
      lastWifiReconnect = millis();
      logCaptureLn(String("WiFi断开，后台尝试重连"));
      connectConfiguredWiFi(10000);
    }
  }
  if (!configValid) {
    if (millis() - lastPrintTime >= 1000) {
      lastPrintTime = millis();
      logCaptureLn(String("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数"));
    }
  }
  checkConcatTimeout();
  handleSerialConsole();
  checkSerial1URC();
}
