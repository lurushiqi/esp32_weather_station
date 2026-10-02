#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_SGP30.h>
#include <SparkFunBME280.h>
#include <ESPmDNS.h>
#include <time.h>       // NTP 网络对时（顶栏显示日期/时间/星期）

// ===================== 一、配置区（可修改） =====================

const char    *WIFI_SSID     = "iQOO";
const char    *WIFI_PASSWORD = "llllffff";
const char    *MQTT_BROKER   = "broker.emqx.io";
const uint16_t MQTT_PORT     = 1883;
const char    *MQTT_TOPIC    = "weather_station_data";   // 上行主题

// NTP 对时（顶栏日期时间用）。国内用阿里云的授时服务器，速度最快最稳。
const char    *NTP_SERVER_1  = "ntp.aliyun.com";
const char    *NTP_SERVER_2  = "cn.pool.ntp.org";
const long     NTP_GMT_SHIFT = 8 * 3600;   // 东八区 UTC+8
const int      NTP_DST_SHIFT = 0;          // 中国不实行夏令时

#define DHT_PIN     25    // DHT11 温湿度（单总线）
#define RAIN_AO_PIN 34    // 雨滴传感器模拟量（ADC1_CH6，0~4095）
#define RAIN_DO_PIN 13    // 雨滴传感器数字量
#define DHT_TYPE    DHT11

// ---- 当地地理参数（！！！换地方必须改，否则气压会被误判成'偏低'） ----
// 原理：BME280 测到的是【本站气压】（你所在位置的实际大气压），
//       海拔越高气压越低，近地面约每升高 8.3 米气压下降 1 hPa。
//       天气预报里说的 1010 hPa 是【海平面气压】，两者不是一个东西。
//       本站气压 ≈ 海平面气压 − 海拔 ÷ 8.3
//
// LOCAL_ALTITUDE   ：当地海拔（米）。查法：① 用手机指南针/海拔 App 直接读
//                     ② 反推：海拔 ≈ (当地海平面气压 − 本站气压) × 8.3
//                     ③ 搜"XX市 海拔"或问地理老师
// REF_SEA_PRESSURE ：海平面气压基准（hPa）。此处按【国际标准大气 ISA】取
//                     1013.25 hPa 固定值 —— 教学场景最通用、最好解释，
//                     不随天气波动，任何一台设备读数一致，便于同学对照。
//                     若想让海拔读数更贴近当天实况，可改为当天在天气 App
//                     查到的真实海平面气压（通常 1000~1025 hPa）。
//
// 【本机设定地点】北京市海淀区 · 北京实验学校（阜成路甲3号）
//   地处永定河冲积平原，海淀东部海拔约 45~55 m，取 50 m。
//   标准大气下 50 m 对应本站气压 ≈ 1007 hPa。
#define LOCAL_ALTITUDE     50.0     // 北京市海淀区北京实验学校，海拔约 50 m
#define REF_SEA_PRESSURE 1013.25    // 国际标准大气压（ISA 海平面），教学通用基准

#define READ_INTERVAL 5000   // 采样周期（毫秒）
#define HIST_LEN      120    // 历史点数：120 × 5 秒 ≈ 10 分钟

// ===================== 二、对象与全局状态 =====================

DHT            dht(DHT_PIN, DHT_TYPE);
BME280         bme;
Adafruit_SGP30 sgp;
WiFiClient     espClient;
PubSubClient   mqtt(espClient);
WebServer      server(80);

bool bme_ok = false;
bool sgp_ok = false;
bool dht_ok = false;

float    temperature = 0.0;
float    humidity    = 0.0;
uint16_t co2         = 400;
uint16_t tvoc        = 0;
float    pressure    = 0.0;
float    altitude    = 0.0;

int    rainAdc   = 0;
int    rainDo    = 0;
String rainLevel = "无雨";
String airQualityLevel = "传感器离线";

// 历史数据环形缓冲（曲线页与 CSV 导出使用）
float    histTemp[HIST_LEN];
float    histHum[HIST_LEN];
float    histCO2[HIST_LEN];
float    histTVOC[HIST_LEN];
float    histPress[HIST_LEN];
float    histAlt[HIST_LEN];
float    histRain[HIST_LEN];
uint32_t histTime[HIST_LEN];
uint16_t histCount = 0;
uint8_t  histIdx   = 0;

bool     wifi_connected = false;
uint32_t last_read      = 0;
uint32_t upCount        = 0;    // MQTT 成功上报条数
char     mqtt_buf[256];   // 9 字段报文最长约 70 字节，留足余量

// ===================== 三、函数声明 =====================
void   read_sensors();
void   push_history();
void   publish_data();
void   reconnect_mqtt();
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define WIFI_EVT_T arduino_event_id_t
#else
  #define WIFI_EVT_T WiFiEvent_t
#endif
void   WiFiEvent(WIFI_EVT_T event);
void   handleRoot();
void   handleJson();
void   handleChart();
void   handleHistory();
void   handleCSV();
String rainLevelFromAdc(int adc);
String airLevelFrom(uint16_t c, uint16_t tv);

// ===================== 四、初始化 =====================
void setup()
{
  Serial.begin(115200);
  Serial.println("\n===== 物联网气象站 · 检测端启动 =====");
  Serial.print("MAC 地址：");
  Serial.println(WiFi.macAddress());

  // 感知层初始化
  Wire.begin(21, 22);              // SDA=GPIO21, SCL=GPIO22
  pinMode(RAIN_DO_PIN, INPUT);
  dht.begin();

  // BME280：先试 0x76，再试 0x77
  bme.setI2CAddress(0x76);
  if (bme.begin()) {
    bme_ok = true;
    Serial.println("BME280 已连接 @0x76");
  } else {
    bme.setI2CAddress(0x77);
    if (bme.begin()) {
      bme_ok = true;
      Serial.println("BME280 已连接 @0x77");
    } else {
      Serial.println("未检测到 BME280，气压与海拔不可用");
    }
  }

  // 把海拔换算基准从库默认的 1013.25 hPa 改成当地实际海平面气压。
  // 不改的话，海平面气压每差 1 hPa，海拔就差约 8.3 米；
  // 天气变化能让海平面气压波动 ±30 hPa，对应海拔误差可达 ±250 米。
  if (bme_ok)
  {
    bme.setReferencePressure(REF_SEA_PRESSURE * 100.0);   // 注意：库里单位是 Pa
    Serial.printf("当地参数：海拔 %.0f m，海平面气压基准 %.0f hPa\n",
                  LOCAL_ALTITUDE, REF_SEA_PRESSURE);
    Serial.printf("据此推算本站气压正常范围约 %.0f ~ %.0f hPa\n",
                  REF_SEA_PRESSURE - LOCAL_ALTITUDE / 8.3 - 20.0,
                  REF_SEA_PRESSURE - LOCAL_ALTITUDE / 8.3 + 20.0);
  }

  // SGP30
  sgp_ok = sgp.begin();
  if (sgp_ok) {
    sgp.IAQinit();
    Serial.println("SGP30 已连接（首次使用需通电约 12 小时完成基线校准）");
  } else {
    Serial.println("未检测到 SGP30，CO₂ 与 TVOC 不可用");
  }

  // 网络层：WiFi
  WiFi.onEvent(WiFiEvent);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // 网络对时：顶栏要显示"日期 + 时分秒 + 星期"，必须先把系统时间对准。
  // 联网成功后由 NTP 服务器授时，之后由 ESP32 内部 RTC 自己走时。
  configTime(NTP_GMT_SHIFT, NTP_DST_SHIFT, NTP_SERVER_1, NTP_SERVER_2);

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);

  // 应用层：网页服务
  server.on("/",        handleRoot);
  server.on("/json",    handleJson);
  server.on("/chart",   handleChart);
  server.on("/history", handleHistory);
  server.on("/csv",     handleCSV);
  server.begin();
  Serial.println("网页服务已启动");
}

// ===================== 五、主循环 =====================
void loop()
{
  server.handleClient();
  reconnect_mqtt();
  if (mqtt.connected()) mqtt.loop();

  if (millis() - last_read >= READ_INTERVAL)
  {
    last_read = millis();
    read_sensors();
    push_history();
    publish_data();
  }
}

// ===================== 六、读取传感器 =====================
void read_sensors()
{
  // DHT11 温湿度
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    dht_ok = false;
    humidity = 0; temperature = 0;
  } else {
    dht_ok = true;
    humidity = h; temperature = t;
  }

  // 雨滴传感器：水越多 → 导电越强 → 电压越低 → ADC 读数越小
  rainAdc = analogRead(RAIN_AO_PIN);
  rainDo  = digitalRead(RAIN_DO_PIN);
  // 注意：MH-RD 干燥时 AO 接近 4095，浸满水也只降到 600 左右。
  // 读到接近 0 基本是断线/没接好，必须挡掉，否则会误报"大雨"。
  if      (rainAdc < 30)   rainLevel = "无雨";   // 断线保护：按无雨处理
  else if (rainAdc > 3500) rainLevel = "无雨";
  else if (rainAdc > 2500) rainLevel = "小雨";
  else if (rainAdc > 1500) rainLevel = "中雨";
  else                     rainLevel = "大雨";

  // SGP30 气体传感器
  if (sgp_ok)
  {
    sgp.IAQmeasure();
    co2  = sgp.eCO2;
    tvoc = sgp.TVOC;
    if      (co2 <= 600  && tvoc <= 50)  airQualityLevel = "优";
    else if (co2 <= 1000 && tvoc <= 100) airQualityLevel = "良";
    else if (co2 <= 1500 && tvoc <= 200) airQualityLevel = "中";
    else                                 airQualityLevel = "差";
  }

  // BME280：原始单位为 Pa，除以 100 得到 hPa
  if (bme_ok)
  {
    pressure = bme.readFloatPressure() / 100.0;
    altitude = bme.readFloatAltitudeMeters();
  }

  Serial.printf("%.1f℃ %.1f%% CO2=%d TVOC=%d %.1fhPa %.1fm 雨量ADC=%d DO=%d (%s) 空气%s\n",
                temperature, humidity, co2, tvoc, pressure, altitude,
                rainAdc, rainDo, rainLevel.c_str(), airQualityLevel.c_str());
}

// ===================== 七、MQTT 上报 =====================
void publish_data()
{
  // 报文格式：温度,湿度,CO2,TVOC,气压,海拔,雨量ADC,雨量等级,空气质量
  snprintf(mqtt_buf, sizeof(mqtt_buf), "%.2f,%.2f,%d,%d,%.1f,%.1f,%d,%s,%s",
           temperature, humidity, co2, tvoc, pressure, altitude,
           rainAdc, rainLevel.c_str(), airQualityLevel.c_str());

  if (mqtt.connected() && wifi_connected)
  {
    if (mqtt.publish(MQTT_TOPIC, mqtt_buf)) upCount++;
  }
}

// ===================== 八、历史缓存 =====================
void push_history()
{
  histTemp[histIdx]  = temperature;
  histHum[histIdx]   = humidity;
  histCO2[histIdx]   = (float)co2;
  histTVOC[histIdx]  = (float)tvoc;
  histPress[histIdx] = pressure;
  histAlt[histIdx]   = altitude;
  histRain[histIdx]  = (float)rainAdc;
  histTime[histIdx]  = millis() / 1000;
  histIdx = (histIdx + 1) % HIST_LEN;
  if (histCount < HIST_LEN) histCount++;
}

// ===================== 九、网页看板主页 =====================
// 面向课堂广播场景设计（极域电子教室屏幕广播）：
//   - 一屏自包含：学生端被锁定，无法点击或滚动，不交互也要看懂全部信息
//   - 每张卡片标注参考范围，学生无需预先知道"638ppm 是否正常"
//   - 顶部建议句与表情随实测数据动态变化
//   - 点击卡片进入该变量的独立曲线页（由教师在教师机上操作）
// 布局：动态建议条 → 4 张主卡 → 3 张次卡 → 页脚
// ==============================================================
void handleRoot()
{
  // 本站气压的正常范围 = 当地海平面气压 − 海拔压降，再留 ±20 hPa 给天气波动
  float pressMid = REF_SEA_PRESSURE - LOCAL_ALTITUDE / 8.3;
  float pressLo  = pressMid - 20.0;
  float pressHi  = pressMid + 20.0;

  String html = "";
  html.reserve(11000);

  html += "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>智能气象监测站</title>";
  html += "<link rel='icon' href='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><text y=%22.9em%22 font-size=%2290%22>&#127780;</text></svg>'>";
  html += "<style>";
  html += "*{margin:0;padding:0;box-sizing:border-box;font-family:'Microsoft YaHei',sans-serif;}";
  html += "html,body{height:100%;overflow:hidden;}";
  // 竖向撑满整屏：body 高度锁死 100vh，用 grid 五行(页头/建议/主卡/次卡/页脚)分配空间。
  // 主卡行 1.35fr、次卡行 1fr —— 主卡略高，视觉重心在上，符合"重点数据大字"的投影需求。
  html += "body{background:#eef4fb;color:#2c3440;padding:12px 14px;height:100vh;display:grid;";
  html += "grid-template-rows:auto auto 1fr 1fr auto;gap:12px;}";
  html += ".hdrOuter{position:relative;}";
  html += ".adv{margin:0;}";
  html += ".main{margin:0;}";
  html += ".minor{margin:0;}";
  html += ".ft{margin:0;}";


  // ---------- 页头：时间条 + 运行状态 ----------
  html += ".hdr{display:flex;align-items:center;gap:14px;flex-wrap:wrap;}";
  html += ".hdr .lg{font-size:40px;line-height:1;}";
  html += ".hdr h1{font-size:34px;font-weight:bold;color:#1b2735;letter-spacing:1px;}";
  html += ".hdr .rt{margin-left:auto;font-size:17px;color:#8a94a6;}";
  html += ".hdr .rt b{color:#2d8aff;font-size:19px;}";
  // 顶栏中间的日期 / 时间 / 星期（绝对居中，不受左右内容影响）
  html += ".clk{position:absolute;left:0;right:0;top:0;height:100%;display:flex;align-items:center;justify-content:center;gap:16px;pointer-events:none;}";
  html += ".clk .dt{font-size:24px;font-weight:bold;color:#1b2735;letter-spacing:1px;}";
  html += ".clk .tm{font-size:30px;font-weight:bold;color:#2d8aff;letter-spacing:2px;font-variant-numeric:tabular-nums;}";
  html += ".clk .wk{font-size:24px;font-weight:bold;color:#1b2735;letter-spacing:1px;}";
  html += ".hdrOuter{position:relative;}";

  // ---------- 顶部动态建议条（整行居中） ----------
  html += ".adv{color:#fff;border-radius:18px;padding:14px 26px;display:flex;align-items:center;justify-content:center;gap:18px;text-align:center;}";
  html += ".adv .em{font-size:52px;line-height:1;}";
  html += ".adv .tx{font-size:28px;font-weight:bold;line-height:1.4;}";
  html += ".adv .seg{font-size:20px;opacity:.95;margin-top:5px;}";
  html += ".adv-g{background:linear-gradient(120deg,#38d17a,#00a849);}";
  html += ".adv-o{background:linear-gradient(120deg,#ffb74d,#f57c00);}";
  html += ".adv-r{background:linear-gradient(120deg,#ff8a80,#e53935);}";

  // ---------- 主卡：温度、湿度、CO₂、雨量 ----------
  // 结构：顶部 标题+图标 / 中间 数值组(整体垂直居中) / 底部 参考区间。
  // 数值组用 flex:1 + justify-content:center 居中，避免卡片变高后数值被推到底部、中间空一大片。
  html += ".main{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;min-height:0;height:100%;}";
  html += ".big{border-radius:18px;padding:16px 20px;color:#fff;cursor:pointer;box-shadow:0 4px 16px rgba(45,138,255,.10);display:flex;flex-direction:column;overflow:hidden;}";
  html += ".big .hd{display:flex;align-items:center;justify-content:space-between;flex:0 0 auto;}";
  html += ".big .nm{font-size:38px;font-weight:bold;opacity:.97;}";
  html += ".big .em{font-size:52px;}";
  html += ".big .mid{flex:1 1 auto;display:flex;flex-direction:column;justify-content:center;align-items:flex-start;min-height:0;padding:6px 0;}";
  html += ".big .vv{font-size:100px;font-weight:bold;line-height:1;letter-spacing:-4px;}";
  html += ".big .uu{font-size:30px;font-weight:normal;opacity:.85;margin-left:5px;}";
  html += ".big .tg{display:inline-block;align-self:flex-start;margin-top:14px;background:rgba(255,255,255,.3);padding:6px 26px;border-radius:26px;font-size:28px;font-weight:bold;}";
  html += ".big .rng{font-size:24px;opacity:.95;margin-top:11px;flex:0 0 auto;}";
  html += ".bg-green{background:linear-gradient(135deg,#38d17a,#00a849);}";
  html += ".bg-blue{background:linear-gradient(135deg,#5aa9ff,#1a75e0);}";
  html += ".bg-orange{background:linear-gradient(135deg,#ffb74d,#f57c00);}";
  html += ".bg-red{background:linear-gradient(135deg,#ff8a80,#e53935);}";
  html += ".bg-gray{background:linear-gradient(135deg,#b0bec5,#78909c);}";

  // ---------- 次卡：TVOC、气压、海拔（白底 + 顶部色条） ----------
  // 与主卡同构：标题在顶，数值组居中，标签+参考范围贴底。
  html += ".minor{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;min-height:0;height:100%;}";
  html += ".sm{background:#fff;border-radius:17px;padding:14px 20px;cursor:pointer;display:flex;flex-direction:column;overflow:hidden;";
  html += "border-top:5px solid #c8d4e4;box-shadow:0 3px 14px rgba(45,138,255,.08);";
  html += "transition:transform .12s,box-shadow .12s;}";
  html += ".sm:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(45,138,255,.16);}";
  html += ".sm .hd{display:flex;align-items:center;justify-content:space-between;flex:0 0 auto;}";
  html += ".sm .nm{font-size:38px;font-weight:bold;color:#4a5563;letter-spacing:.5px;}";
  html += ".sm .ic{font-size:52px;line-height:1;}";
  html += ".sm .mid{flex:1 1 auto;display:flex;flex-direction:column;justify-content:center;align-items:flex-start;min-height:0;padding:10px 0 0;}";
  html += ".sm .vv{font-size:84px;font-weight:bold;color:#2c3440;line-height:1;}";
  html += ".sm .uu{font-size:26px;font-weight:normal;color:#8a94a6;margin-left:6px;}";
  html += ".sm .rw{margin-top:14px;display:flex;align-items:center;gap:11px;flex-wrap:wrap;flex:0 0 auto;}";
  html += ".tg{font-size:26px;font-weight:bold;color:#fff;padding:6px 24px;border-radius:24px;}";
  html += ".tg-g{background:#00C853;}.tg-b{background:#2196F3;}.tg-o{background:#FF9800;}";
  html += ".tg-r{background:#F44336;}.tg-n{background:#90a4ae;}";
  html += ".sm .rng{font-size:23px;color:#6b7688;background:#f2f6fb;padding:6px 18px;border-radius:24px;}";
  html += ".sl-green{border-top-color:#00C853;}.sl-blue{border-top-color:#2196F3;}";
  html += ".sl-orange{border-top-color:#FF9800;}.sl-red{border-top-color:#F44336;}.sl-gray{border-top-color:#c8d4e4;}";

  // ---------- 页脚 ----------
  html += ".ft{background:#fff;border-radius:14px;padding:11px 20px;display:flex;";
  html += "justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px;";
  html += "box-shadow:0 2px 10px rgba(45,138,255,.06);font-size:18px;color:#5a6675;}";
  html += ".ft .lf{display:flex;align-items:center;gap:16px;flex-wrap:wrap;}";
  html += ".ft b{color:#2c3440;}";
  html += ".ft .sp{color:#d5dce6;}";
  html += ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:7px;vertical-align:middle;}";
  html += ".d-on{background:#00C853;box-shadow:0 0 0 3px rgba(0,200,83,.18);}";
  html += ".d-off{background:#e53935;box-shadow:0 0 0 3px rgba(229,57,53,.18);}";
  html += ".ft .hint{color:#a3adbb;}";
  html += ".ft a{color:#2d8aff;text-decoration:none;font-weight:bold;margin-left:16px;font-size:18px;}";

  // ---------- 数值刷新反馈 ----------
  html += "@keyframes pop{0%{transform:scale(1)}45%{transform:scale(1.04)}100%{transform:scale(1)}}";
  html += ".pulse{animation:pop .5s ease;}";
  html += "@media(min-width:1800px){.adv .tx{font-size:46px;} .adv .em{font-size:68px;} .adv .seg{font-size:22px;} .big .vv{font-size:104px;} .sm .vv{font-size:86px;} .big .uu{font-size:30px;} .sm .uu{font-size:28px;} .big .nm{font-size:34px;} .sm .nm{font-size:34px;} .big .em{font-size:52px;} .sm .ic{font-size:52px;} .big .rng{font-size:24px;} .sm .rng{font-size:22px;} .big .tg{font-size:28px;} .tg{font-size:26px;}}";
  html += "@media(max-width:1700px){.big .vv{font-size:68px;} .sm .vv{font-size:58px;} .big .uu{font-size:22px;} .sm .uu{font-size:20px;} .big .nm{font-size:32px;} .sm .nm{font-size:32px;} .big .em{font-size:42px;} .sm .ic{font-size:42px;} .big .rng{font-size:18px;} .sm .rng{font-size:17px;} .big .tg{font-size:21px;padding:4px 18px;margin-top:8px;} .tg{font-size:20px;padding:4px 16px;} .big .mid,.sm .mid{padding:0;} .big .rng{margin-top:6px;} .sm .rw{margin-top:8px;}}";
  // ---------- 平板 / 手机竖屏（≤1000px）----------
  // 关键：解除 100vh 锁定，改回自适应高度 + 可滚动；
  //      顶栏时间条由"绝对居中悬浮"改回静态流，避免盖住标题；卡片转两列。
  html += "@media(max-width:1000px){";
  html += "body{height:auto;min-height:100vh;overflow:auto;display:block;}";
  html += ".hdrOuter{position:static;}";
  html += ".clk{position:static;height:auto;justify-content:center;gap:12px;margin-top:8px;}";
  html += ".clk .dt{font-size:18px;} .clk .tm{font-size:22px;} .clk .wk{font-size:18px;}";
  html += ".hdr h1{font-size:26px;} .hdr .lg{font-size:32px;}";
  html += ".main{grid-template-columns:repeat(2,1fr);height:auto;margin-top:12px;}";
  html += ".minor{grid-template-columns:repeat(2,1fr);height:auto;margin-top:12px;}";
  html += ".big,.sm{min-height:170px;}";
  html += ".adv{margin-top:12px;min-height:0;padding:12px 18px;}";
  html += ".adv .em{font-size:40px;} .adv .tx{font-size:24px;} .adv .seg{font-size:16px;}";
  html += ".big .vv{font-size:56px;} .sm .vv{font-size:46px;}";
  html += ".big .uu{font-size:20px;} .sm .uu{font-size:18px;}";
  html += ".big .nm{font-size:20px;} .sm .nm{font-size:20px;}";
  html += ".big .em{font-size:30px;} .sm .ic{font-size:30px;}";
  html += ".big .rng{font-size:14px;} .sm .rng{font-size:13px;}";
  html += ".big .tg{font-size:16px;padding:3px 13px;margin-top:8px;} .tg{font-size:15px;padding:3px 12px;}";
  html += ".ft{margin-top:12px;font-size:14px;}";
  html += "}";

  // ---------- 手机（≤620px）：单列 ----------
  html += "@media(max-width:620px){";
  html += "body{padding:10px;}";
  html += ".main{grid-template-columns:1fr;} .minor{grid-template-columns:1fr;}";
  html += ".big,.sm{min-height:150px;padding:14px 16px;}";
  html += ".hdr .lg{font-size:28px;} .hdr h1{font-size:22px;} .hdr .rt{font-size:13px;}";
  html += ".clk{gap:9px;margin-top:6px;} .clk .dt{font-size:15px;} .clk .tm{font-size:19px;} .clk .wk{font-size:15px;}";
  html += ".adv{flex-direction:column;gap:8px;padding:14px 16px;}";
  html += ".adv .em{font-size:36px;} .adv .tx{font-size:20px;} .adv .seg{font-size:14px;}";
  html += ".big .vv{font-size:58px;} .sm .vv{font-size:50px;}";
  html += ".big .nm{font-size:18px;} .sm .nm{font-size:18px;}";
  html += ".big .rng{font-size:13px;} .sm .rng{font-size:12.5px;} .sm .rw{gap:7px;}";
  html += ".ft{font-size:12.5px;padding:10px 14px;gap:8px;}";
  html += ".ft .lf{gap:10px;} .ft .hint{display:none;}";
  html += "}";
  html += "</style></head><body>";

  // ---------- 页头（时间条绝对居中，标题在左、状态在右） ----------
  html += "<div class='hdrOuter'><div class='hdr'>";
  html += "<div class='lg'>🌤️</div>";
  html += "<div><h1>智能气象监测站</h1></div>";
  html += "<div class='rt'>已运行 <b id='fUp'>--</b>　已上报 <b id='nUp'>0</b> 条</div>";
  html += "</div>";
  html += "<div class='clk'><span class='dt' id='cDate'>----/--/--</span>";
  html += "<span class='tm' id='cTime'>--:--:--</span>";
  html += "<span class='wk' id='cWeek'>星期-</span></div></div>";

  // ---------- 动态建议条（整行居中：表情 + 主句，副句另起一行居中） ----------
  html += "<div class='adv adv-g' id='advBar'>";
  html += "<div class='em' id='aEm'>😊</div>";
  html += "<div><div class='tx' id='aTx'>正在获取数据…</div>";
  html += "<div class='seg' id='aSeg'>--</div></div></div>";

  // ---------- 主卡 ----------
  html += "<div class='main'>";
  html += "<div class='big bg-gray' id='cTemp' onclick='go(\"temp\")'>";
  html += "<div class='hd'><span class='nm'>温度</span><span class='em' id='eTemp'>🌡</span></div>";
  html += "<div class='mid'><div class='vv'><span id='vTemp'>--</span><span class='uu'>℃</span></div>";
  html += "<div class='tg' id='tTemp'>--</div></div>";
  html += "<div class='rng'>舒适区间 18~26 ℃</div></div>";

  html += "<div class='big bg-gray' id='cHum' onclick='go(\"hum\")'>";
  html += "<div class='hd'><span class='nm'>湿度</span><span class='em' id='eHum'>💧</span></div>";
  html += "<div class='mid'><div class='vv'><span id='vHum'>--</span><span class='uu'>%RH</span></div>";
  html += "<div class='tg' id='tHum'>--</div></div>";
  html += "<div class='rng'>舒适区间 40~65 %RH</div></div>";

  html += "<div class='big bg-gray' id='cCO2' onclick='go(\"co2\")'>";
  html += "<div class='hd'><span class='nm'>CO₂ 浓度</span><span class='em' id='eCO2'>💨</span></div>";
  html += "<div class='mid'><div class='vv'><span id='vCO2'>--</span><span class='uu'>ppm</span></div>";
  html += "<div class='tg' id='tCO2'>--</div></div>";
  html += "<div class='rng'>正常参考 400~1000 ppm（来自人的呼吸）</div></div>";

  html += "<div class='big bg-gray' id='cRain' onclick='go(\"rain\")'>";
  html += "<div class='hd'><span class='nm'>雨量</span><span class='em' id='eRain'>☀️</span></div>";
  html += "<div class='mid'><div class='vv'><span id='vRain'>--</span></div>";
  html += "<div class='tg' id='tRain'>--</div></div>";
  html += "<div class='rng' id='rRain'>AO 读数越小代表雨越大</div></div>";
  html += "</div>";

  // ---------- 次卡 ----------
  html += "<div class='minor'>";
  html += "<div class='sm sl-gray' id='cTVOC' onclick='go(\"tvoc\")'>";
  html += "<div class='hd'><span class='nm'>TVOC</span><span class='ic'>🧪</span></div>";
  html += "<div class='mid'><div class='vv'><span id='vTVOC'>--</span><span class='uu'>ppb</span></div></div>";
  html += "<div class='rw'><span class='tg tg-n' id='tTVOC'>--</span>";
  html += "<span class='rng'>正常参考 0~100 ppb（装修挥发物）</span></div></div>";

  html += "<div class='sm sl-gray' id='cPress' onclick='go(\"press\")'>";
  html += "<div class='hd'><span class='nm'>气压</span><span class='ic'>🌪️</span></div>";
  html += "<div class='mid'><div class='vv'><span id='vPress'>--</span><span class='uu'>hPa</span></div></div>";
  html += "<div class='rw'><span class='tg tg-n' id='tPress'>--</span>";
  html += "<span class='rng'>本站常压 " + String(pressLo, 0) + "~" + String(pressHi, 0)
       + " hPa（校本部海拔 " + String(LOCAL_ALTITUDE, 0) + " m 折算）</span></div></div>";

  html += "<div class='sm sl-gray' id='cAlt' onclick='go(\"alt\")'>";
  html += "<div class='hd'><span class='nm'>海拔</span><span class='ic'>⛰️</span></div>";
  html += "<div class='mid'><div class='vv'><span id='vAlt'>--</span><span class='uu'>m</span></div></div>";
  html += "<div class='rw'><span class='tg tg-n' id='tAlt'>--</span>";
  html += "<span class='rng' id='rAlt'>由本站气压按标准大气换算，会随天气波动</span></div></div>";
  html += "</div>";

  // ---------- 页脚：IP / 域名 / 连接状态（实时） ----------
  html += "<div class='ft'><div class='lf'>";
  html += "<span>IP：<b>" + WiFi.localIP().toString() + "</b></span>";
  html += "<span>域名：<b>esp32weather.local</b></span>";
  html += "<span class='sp'>|</span>";
  html += "<span><i class='dot d-off' id='dW'></i>WiFi：<b id='sW'>检测中</b></span>";
  html += "<span class='sp'>|</span>";
  html += "<span><i class='dot d-off' id='dM'></i>MQTT：<b id='sM'>检测中</b></span>";
  html += "<span class='hint'>点击卡片查看该变量的变化曲线</span>";
  html += "</div><div><a href='/csv'>导出 CSV</a></div></div>";
  // ---------- 脚本 ----------
  html += "<script>";
  html += "function S(id,v){const e=document.getElementById(id);if(e)e.innerText=v;}";
  html += "function C(id,c){const e=document.getElementById(id);if(e)e.className=c;}";
  html += "function pulse(id){const e=document.getElementById(id);if(!e)return;e.classList.add('pulse');setTimeout(function(){e.classList.remove('pulse');},520);}";
  html += "let P={};";

  // 分段评价
  html += "function segTemp(v){return v<10?'寒冷':(v<18?'偏凉':(v<=26?'适宜':(v<=32?'偏热':'炎热')));}";
  html += "function segAir(s){return s==='优'?'空气清新':(s==='良'?'空气良好':(s==='中'?'空气有点闷':(s==='差'?'空气浑浊':'传感器离线')));}";
  html += "function segRain(s){return s==='无雨'?'没有下雨':(s==='小雨'?'有小雨':(s==='中雨'?'正在下雨':'雨很大'));}";

  // 图标随数值变化
  html += "function icTemp(v){return v<18?'🥶':(v>26?'🥵':'🌡');}";
  html += "function icHum(v){return v<40?'🏜️':(v>65?'🌊':'💧');}";
  html += "function icCO2(v){return v<=1000?'💨':'🌫️';}";
  html += "function icRain(s){return s==='无雨'?'☀️':(s==='小雨'?'🌦️':(s==='中雨'?'🌧️':'⛈️'));}";

  // 顶部主句：取最严重的一项
  html += "function advice(d){";
  html += " if(d.air==='差')return['😷','空气比较浑浊，记得开窗通风','adv-r'];";
  html += " if(d.rain==='大雨')return['⛈️','外面雨不小，出门记得带伞','adv-o'];";
  html += " if(d.sen.dht&&d.temp<10)return['🥶','气温偏低，注意添件外套','adv-o'];";
  html += " if(d.sen.dht&&d.temp>32)return['🥵','天气炎热，记得多喝水','adv-o'];";
  html += " if(d.air==='中')return['😐','空气有点闷，建议开窗透透气','adv-o'];";
  html += " if(d.rain!=='无雨')return['🌦️','有雨，出门记得带伞','adv-o'];";
  html += " return['😊','今天天气不错，适合户外活动','adv-g'];}";

  // 次卡统一设置：顶部色条 + 状态标签
  html += "function smSt(id,tid,ok,cls,txt){";
  html += " C(id,'sm '+(ok?cls:'sl-gray'));";
  html += " S(tid,ok?txt:'无数据');";
  html += " C(tid,'tg '+(ok?(cls==='sl-green'?'tg-g':(cls==='sl-blue'?'tg-b':(cls==='sl-orange'?'tg-o':'tg-r'))):'tg-n'));}";

  html += "async function update(){";
  html += " let d;";
  html += " try{const r=await fetch('/json');d=await r.json();}catch(e){return;}";
  // 顶栏日期 / 时间 / 星期（由 ESP32 的 NTP 授时提供，精确到秒）
  html += " S('cDate',d.date);S('cTime',d.time);S('cWeek',d.week);";
  html += " S('fUp',Math.floor(d.uptime/60)+'分'+String(d.uptime%60).padStart(2,'0')+'秒');";
  html += " S('nUp',d.up);";

  // 连接状态：真实取自 ESP32 上报的 wifi / mqtt 字段
  html += " const w=d.wifi===1,m=d.mqtt===1;";
  html += " S('sW',w?'已连接':'未连接');C('dW',w?'dot d-on':'dot d-off');";
  html += " S('sM',m?'已连接':'未连接');C('dM',m?'dot d-on':'dot d-off');";

  // 顶部建议条
  html += " const a=advice(d);S('aEm',a[0]);S('aTx',a[1]);C('advBar','adv '+a[2]);";
  html += " const ts=d.sen.dht?('温度'+segTemp(d.temp)):'温度未知';";
  html += " S('aSeg',ts+' · '+segAir(d.air)+' · '+segRain(d.rain));";

  // 温度
  html += " if(d.sen.dht){";
  html += "  if(P.temp!==undefined&&P.temp!==d.temp)pulse('cTemp');";
  html += "  S('vTemp',d.temp);S('eTemp',icTemp(d.temp));";
  html += "  C('cTemp','big '+(d.temp<18?'bg-blue':(d.temp>26?'bg-orange':'bg-green')));";
  html += "  S('tTemp',d.temp<18?'偏低':(d.temp>26?'偏高':'适宜'));}";
  html += " else{S('vTemp','--');C('cTemp','big bg-gray');S('tTemp','无数据');}";

  // 湿度
  html += " if(d.sen.dht){";
  html += "  if(P.hum!==undefined&&P.hum!==d.hum)pulse('cHum');";
  html += "  S('vHum',d.hum);S('eHum',icHum(d.hum));";
  html += "  C('cHum','big '+(d.hum<40?'bg-orange':(d.hum>65?'bg-blue':'bg-green')));";
  html += "  S('tHum',d.hum<40?'干燥':(d.hum>65?'潮湿':'适宜'));}";
  html += " else{S('vHum','--');C('cHum','big bg-gray');S('tHum','无数据');}";

  // CO₂
  html += " if(d.sen.sgp){";
  html += "  if(P.co2!==undefined&&P.co2!==d.co2)pulse('cCO2');";
  html += "  S('vCO2',d.co2);S('eCO2',icCO2(d.co2));";
  html += "  C('cCO2','big '+(d.co2<=600?'bg-green':(d.co2<=1000?'bg-blue':(d.co2<=1500?'bg-orange':'bg-red'))));";
  html += "  S('tCO2',d.co2<=600?'优':(d.co2<=1000?'良':(d.co2<=1500?'偏高':'过高')));}";
  html += " else{S('vCO2','--');C('cCO2','big bg-gray');S('tCO2','无数据');}";

  // 雨量
  html += " S('vRain',d.rain);S('eRain',icRain(d.rain));S('rRain','AO 读数 '+d.rainAdc+'（越小雨越大）');";
  html += " C('cRain','big '+(d.rain==='无雨'?'bg-green':(d.rain==='小雨'?'bg-blue':(d.rain==='中雨'?'bg-orange':'bg-red'))));";
  html += " S('tRain',d.rain==='无雨'?'放心出门':(d.rain==='小雨'?'留意天气':(d.rain==='中雨'?'记得带伞':'雨很大')));";

  // TVOC
  html += " if(d.sen.sgp){S('vTVOC',d.tvoc);";
  html += "  smSt('cTVOC','tTVOC',true,(d.tvoc<=50?'sl-green':(d.tvoc<=100?'sl-blue':(d.tvoc<=200?'sl-orange':'sl-red'))),";
  html += "       (d.tvoc<=50?'清新':(d.tvoc<=100?'正常':(d.tvoc<=200?'偏高':'过高'))));}";
  html += " else{S('vTVOC','--');smSt('cTVOC','tTVOC',false,'sl-gray','');}";

  // 气压
  html += " if(d.sen.bme){S('vPress',d.press);";
  html += "  smSt('cPress','tPress',true,(d.press<" + String(pressLo, 0)
       + "?'sl-orange':(d.press>" + String(pressHi, 0) + "?'sl-orange':'sl-green')),";
  html += "       (d.press<" + String(pressLo, 0) + "?'偏低':(d.press>"
       + String(pressHi, 0) + "?'偏高':'正常')));}";
  html += " else{S('vPress','--');smSt('cPress','tPress',false,'sl-gray','');}";

  // 海拔
  // 注意：拼接顺序是 "JS字符串片段" + C++变量 + "JS字符串片段"，
  //       C++ 变量两侧必须各有一个 JS 引号，否则拼出来是 '+1013 hPa）' 这种非法 JS
  html += " if(d.sen.bme){S('vAlt',d.alt);smSt('cAlt','tAlt',true,'sl-blue','换算值');";
  html += "  S('rAlt','由本站气压 '+d.press+' hPa 换算（基准 "
       + String(REF_SEA_PRESSURE, 0) + " hPa）');}";
  html += " else{S('vAlt','--');smSt('cAlt','tAlt',false,'sl-gray','');S('rAlt','由本站气压按标准大气换算，会随天气波动');}";

  html += " P=d;}";
  html += "function go(t){location='/chart?type='+t;}";
  // 页面已经默认按投影大字排版，不再需要点击切换；为避免闪烁，刷新周期 1 秒
  html += "update();setInterval(update,1000);";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

// ===================== 十、数据接口 =====================
void handleJson()
{
  // 取系统本地时间（NTP 已对准东八区）
  struct tm ti;
  bool tok = getLocalTime(&ti, 0);
  char dateBuf[16] = "----------";
  char timeBuf[12] = "--:--:--";
  const char *weekBuf = "---";
  static const char *WK[7] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
  if (tok)
  {
    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    if (ti.tm_wday >= 0 && ti.tm_wday < 7) weekBuf = WK[ti.tm_wday];
  }

  String j = "{";
  j += "\"date\":\""  + String(dateBuf) + "\"";
  j += ",\"time\":\""  + String(timeBuf) + "\"";
  j += ",\"week\":\""  + String(weekBuf) + "\"";
  j += ",\"temp\":"    + String(temperature, 1);
  j += ",\"hum\":"     + String(humidity, 1);
  j += ",\"co2\":"     + String(co2);
  j += ",\"tvoc\":"    + String(tvoc);
  j += ",\"press\":"   + String(pressure, 1);
  j += ",\"alt\":"     + String(altitude, 1);
  j += ",\"rainAdc\":" + String(rainAdc);
  j += ",\"rainDo\":"  + String(rainDo);
  j += ",\"rain\":\""  + rainLevel + "\"";
  j += ",\"air\":\""   + airQualityLevel + "\"";
  j += ",\"sen\":{\"dht\":" + String(dht_ok ? 1 : 0);
  j += ",\"bme\":"          + String(bme_ok ? 1 : 0);
  j += ",\"sgp\":"          + String(sgp_ok ? 1 : 0) + "}";
  j += ",\"wifi\":"   + String(wifi_connected ? 1 : 0);
  j += ",\"mqtt\":"   + String(mqtt.connected() ? 1 : 0);
  j += ",\"up\":"     + String(upCount);
  j += ",\"uptime\":" + String(millis() / 1000);
  j += "}";
  server.send(200, "application/json", j);
}

// ===================== 十一、单变量曲线页 =====================
// 一次只呈现一个变量，便于集中观察趋势。
// 使用内置 canvas 绘图，不依赖外部 CDN，教室断网也能正常显示。
// ==============================================================
void handleChart()
{
  String t = server.hasArg("type") ? server.arg("type") : String("temp");

  String name, unit, color, rgb;
  float ymin = 0, ymax = 100;
  if      (t == "temp")  { name = "温度";        unit = "℃";   color = "#ff4444"; rgb = "255,68,68";  ymin = 0;   ymax = 50;   }
  else if (t == "hum")   { name = "湿度";        unit = "%";    color = "#2196F3"; rgb = "33,150,243"; ymin = 0;   ymax = 100;  }
  else if (t == "co2")   { name = "CO₂ 浓度";    unit = "ppm";  color = "#00C853"; rgb = "0,200,83";   ymin = 400; ymax = 1200; }
  else if (t == "tvoc")  { name = "TVOC 浓度";   unit = "ppb";  color = "#FF9800"; rgb = "255,152,0";  ymin = 0;   ymax = 300;  }
  else if (t == "press") { name = "气压";        unit = "hPa";  color = "#9C27B0"; rgb = "156,39,176";
                          ymin = REF_SEA_PRESSURE - LOCAL_ALTITUDE / 8.3 - 30;
                          ymax = REF_SEA_PRESSURE - LOCAL_ALTITUDE / 8.3 + 30; }
  else if (t == "alt")   { name = "海拔";        unit = "m";    color = "#795548"; rgb = "121,85,72";
                          ymin = LOCAL_ALTITUDE - 120; ymax = LOCAL_ALTITUDE + 120; }
  else if (t == "rain")  { name = "雨量 AO 读数"; unit = "";    color = "#0091EA"; rgb = "0,145,234";  ymin = 0;   ymax = 4095; }
  else { t = "temp"; name = "温度"; unit = "℃"; color = "#ff4444"; rgb = "255,68,68"; ymin = 0; ymax = 50; }

  String html = "";
  html.reserve(5200);
  html += "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + name + " 实时曲线</title>";
  html += "<style>";
  html += "*{margin:0;padding:0;box-sizing:border-box;font-family:'Microsoft YaHei',sans-serif;}";
  html += "body{background:#eef4fb;color:#2c3440;padding:16px;}";
  html += ".wrap{max-width:1000px;margin:0 auto;}";
  html += ".top{display:flex;align-items:center;gap:12px;margin-bottom:14px;flex-wrap:wrap;}";
  html += ".back{padding:9px 18px;border-radius:12px;background:#2d8aff;color:#fff;text-decoration:none;font-size:15px;font-weight:bold;}";
  html += ".tt{font-size:23px;font-weight:bold;color:#2d8aff;}";
  html += ".box{background:#fff;border-radius:20px;padding:14px;box-shadow:0 4px 18px rgba(45,138,255,.08);height:66vh;min-height:300px;}";
  html += ".cwrap{position:relative;width:100%;height:100%;}";
  html += "canvas{width:100%;height:100%;display:block;cursor:crosshair;}";
  html += ".cur{margin-top:14px;text-align:center;font-size:17px;color:#555;}";
  html += ".cur b{font-size:32px;vertical-align:middle;margin:0 6px;color:" + color + ";}";
  html += ".stat{margin-top:8px;text-align:center;font-size:15px;color:#6b7688;}";
  html += ".stat b{color:#2c3440;font-size:19px;margin:0 3px;}";
  html += ".tip{margin-top:10px;text-align:center;font-size:12.5px;color:#8a94a6;}";
  html += "</style></head><body><div class='wrap'>";
  html += "<div class='top'><a class='back' href='/'>返回看板</a>";
  // 小数位数：连续量保留 1 位，计数类取整
  int dec = 1;
  if (t == "co2" || t == "tvoc" || t == "rain") dec = 0;

  html += "<div class='tt'>" + name + " 实时曲线</div>";
  html += "<a class='back' style='background:#00C853' href='/csv'>导出 CSV</a></div>";
  html += "<div class='box'><div class='cwrap'><canvas id='cv'></canvas></div></div>";
  html += "<div class='cur'>当前 " + name + "：<b id='curVal'>--</b>" + unit + "</div>";
  // 注意：字符串拼接必须有一个 String 宿主（String 变量、String(...) 或 html 本身）。
  // 两个裸字符串字面量无法直接相加，若续行写成 加号后紧跟字面量，
  // 会报 invalid operands of types 'const char [n]' and 'const char [m]'。
  // 因此这里把统计行拆成独立的 += 语句，最稳妥。
  html += "<div class='stat'>最近 <b id='stN'>0</b> 个采样点　";
  html += "最低 <b id='stMin'>--</b>" + unit + "　";
  html += "最高 <b id='stMax'>--</b>" + unit + "　";
  html += "平均 <b id='stAvg'>--</b>" + unit + "</div>";
  html += "<div class='tip'>曲线上的数字即为该时刻的实测值；把鼠标移到曲线上（手机可直接触摸）可查看任意一点的数值与时间；采样周期 5 秒，保留最近 10 分钟</div>";
  html += "</div>";

  html += "<script>";
  html += "function S(id,v){const e=document.getElementById(id);if(e)e.innerText=v;}";
  html += "const TYPE='" + t + "';";
  html += "const KM={temp:'temp',hum:'hum',co2:'co2',tvoc:'tvoc',press:'press',alt:'alt',rain:'rainAdc'};";
  html += "const UNIT='" + unit + "';";
  html += "const cv=document.getElementById('cv'),ctx=cv.getContext('2d');";
  html += "let lbl=[],dat=[];const MAXN=200;";
  // 悬停/触摸用的全局量：hov=当前高亮下标，GX/GY/GT/GB=上一帧的坐标换算与边距
  html += "let hov=-1,GX=null,GY=null,GT=52,GB=34;";
  html += "const YMIN=" + String(ymin, 0) + ",YMAX=" + String(ymax, 0)
       + ",CLR='" + color + "',RGB='" + rgb + "',DEC=" + String(dec) + ";";
  html += "function fmt(ms){const d=new Date(ms);const p=function(n){return String(n).padStart(2,'0');};";
  html += "return p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());}";
  html += "function draw(){";
  html += " const r=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;";
  html += " cv.width=w*r;cv.height=h*r;ctx.setTransform(r,0,0,r,0,0);ctx.clearRect(0,0,w,h);";
  html += " if(!dat.length)return;";
  // Y 轴范围：以数据实际范围为准，上下各留 12% 余量，让曲线铺满画面、点与点拉开。
  // YMIN/YMAX 只在数据异常（全相等/无数据）时兜底。
  html += " let lo=Math.min.apply(null,dat),hi=Math.max.apply(null,dat);";
  html += " lo=Number(lo);hi=Number(hi);";
  html += " if(!isFinite(lo)||!isFinite(hi)){lo=YMIN;hi=YMAX;}";
  html += " let span=hi-lo;";
  html += " if(span<1e-6){span=Math.max(Math.abs(hi)*0.1,1);lo-=span/2;hi+=span/2;}";
  html += " const pad=Math.max(span*0.12,span<1?0.5:0.1);";
  html += " let mn=lo-pad,mx=hi+pad;";
  // 整数量的刻度对齐到整数，读起来更顺
  html += " if(DEC===0){mn=Math.floor(mn);mx=Math.ceil(mx);}";
  // 上边距加大到 38，给数值标签留出空间；右边距 28 防止最右侧标签被裁
  html += " const L=62,R=28,T=52,B=34;";
  html += " GT=T;GB=B;";
  html += " const X=function(i){return L+(w-L-R)*i/Math.max(1,dat.length-1);};";
  html += " const Y=function(v){return T+(h-T-B)*(1-(v-mn)/(mx-mn||1));};";
  html += " GX=X;GY=Y;";
  // 网格与 Y 轴刻度：跨度大取整、跨度小多留一位小数
  html += " ctx.font='13px sans-serif';ctx.textBaseline='middle';";
  html += " const ydec=(DEC===0&&mx-mn>20)?0:(mx-mn>60?0:(mx-mn>6?1:2));";
  html += " for(let i=0;i<=4;i++){const v=mn+(mx-mn)*i/4,y=Y(v);";
  html += "  ctx.strokeStyle='#eef2f8';ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(w-R,y);ctx.stroke();";
  html += "  ctx.fillStyle='#8a94a6';ctx.textAlign='right';ctx.fillText(v.toFixed(ydec),L-8,y);}";
  // 面积填充
  html += " ctx.beginPath();ctx.moveTo(X(0),Y(dat[0]));";
  html += " for(let i=1;i<dat.length;i++)ctx.lineTo(X(i),Y(dat[i]));";
  html += " ctx.lineTo(X(dat.length-1),h-B);ctx.lineTo(X(0),h-B);ctx.closePath();";
  html += " ctx.fillStyle='rgba('+RGB+',0.16)';ctx.fill();";
  // 曲线
  html += " ctx.beginPath();ctx.moveTo(X(0),Y(dat[0]));";
  html += " for(let i=1;i<dat.length;i++)ctx.lineTo(X(i),Y(dat[i]));";
  html += " ctx.strokeStyle=CLR;ctx.lineWidth=2.4;ctx.stroke();";
  // 数值标签：按画布宽度自动抽稀，保证标签不重叠
  html += " const plotW=Math.max(60,w-L-R);";
  html += " const maxLab=Math.max(2,Math.floor(plotW/56));";
  html += " const lstep=Math.max(1,Math.ceil(dat.length/maxLab));";
  html += " ctx.font='bold 13px sans-serif';ctx.textAlign='center';ctx.textBaseline='bottom';";
  html += " for(let i=0;i<dat.length;i+=lstep){";
  html += "  const x=X(i),y=Y(dat[i]),txt=Number(dat[i]).toFixed(DEC);";
  html += "  let yy=(y-9<16)?(y+22):(y-9);";
  html += "  if(yy<12)yy=12;if(yy>h-8)yy=h-8;";
  html += "  ctx.lineWidth=3.5;ctx.strokeStyle='#ffffff';ctx.strokeText(txt,x,yy);";
  html += "  ctx.fillStyle='#334155';ctx.fillText(txt,x,yy);}";
  // 数据点
  html += " ctx.fillStyle=CLR;";
  html += " for(let i=0;i<dat.length;i+=lstep){ctx.beginPath();ctx.arc(X(i),Y(dat[i]),4,0,7);ctx.fill();}";
  // 最新点：始终显示且高亮（白底描边 + 大字号数值）
  html += " const li=dat.length-1,lx=X(li),ly=Y(dat[li]);";
  html += " let ly2=(ly-14<16)?(ly+26):(ly-14);";
  html += " if(ly2<14)ly2=14;if(ly2>h-8)ly2=h-8;";
  html += " ctx.font='bold 16px sans-serif';ctx.textAlign='center';ctx.textBaseline='bottom';";
  html += " const lt=Number(dat[li]).toFixed(DEC);";
  html += " ctx.lineWidth=4.5;ctx.strokeStyle='#ffffff';ctx.strokeText(lt,lx,ly2);";
  html += " ctx.fillStyle=CLR;ctx.fillText(lt,lx,ly2);";
  html += " ctx.fillStyle='#ffffff';ctx.beginPath();ctx.arc(lx,ly,7,0,7);ctx.fill();";
  html += " ctx.fillStyle=CLR;ctx.beginPath();ctx.arc(lx,ly,5,0,7);ctx.fill();";
  // X 轴时间标签：按画布宽度决定密度，避免小屏挤成一团
  html += " ctx.font='12px sans-serif';ctx.fillStyle='#8a94a6';ctx.textAlign='center';ctx.textBaseline='middle';";
  html += " const maxXT=Math.max(2,Math.floor((w-L-R)/76));";
  html += " const step=Math.max(1,Math.ceil(dat.length/maxXT));";
  html += " for(let i=0;i<dat.length;i+=step){ctx.fillText(lbl[i],X(i),h-B+16);}";
  html += " if(hov>=0&&hov<dat.length){";
  html += "  const hx=X(hov),hy=Y(dat[hov]);";
  html += "  ctx.save();";
  html += "  ctx.strokeStyle='rgba('+RGB+',0.6)';ctx.lineWidth=1.5;ctx.setLineDash([5,4]);";
  html += "  ctx.beginPath();ctx.moveTo(hx,T);ctx.lineTo(hx,h-B);ctx.stroke();ctx.setLineDash([]);";
  html += "  ctx.fillStyle='#ffffff';ctx.beginPath();ctx.arc(hx,hy,8,0,7);ctx.fill();";
  html += "  ctx.fillStyle=CLR;ctx.beginPath();ctx.arc(hx,hy,6,0,7);ctx.fill();";
  html += "  const un=UNIT?(' '+UNIT):'';";
  html += "  const t1=Number(dat[hov]).toFixed(DEC)+un,t2=lbl[hov]||'';";
  html += "  ctx.font='bold 14px sans-serif';";
  html += "  const bw=Math.max(ctx.measureText(t1).width,ctx.measureText(t2).width)+18,bh=44;";
  html += "  let bx=hx+12;if(bx+bw>w-4)bx=hx-12-bw;if(bx<4)bx=4;";
  // 气泡优先放曲线右上方，空间不够就换到左下，最后再兜底贴边
  html += "  let by=hy-bh-14,dy=hy+14;";
  html += "  if(by<4)by=dy;";
  html += "  if(by+bh>h-B-2)by=hy-bh-14;";
  html += "  if(by<4)by=4;if(by+bh>h-4)by=h-4-bh;";
  html += "  rr(bx,by,bw,bh,8);ctx.fillStyle='rgba(255,255,255,0.97)';ctx.fill();";
  html += "  ctx.strokeStyle=CLR;ctx.lineWidth=1.6;ctx.stroke();";
  html += "  ctx.textAlign='left';ctx.textBaseline='middle';";
  html += "  ctx.fillStyle=CLR;ctx.font='bold 14px sans-serif';ctx.fillText(t1,bx+9,by+15);";
  html += "  ctx.fillStyle='#8a94a6';ctx.font='12px sans-serif';ctx.fillText(t2,bx+9,by+31);";
  html += "  ctx.restore();}";
  html += "}";
  // 圆角矩形（不依赖新版 Canvas 的 roundRect，兼容老浏览器）
  html += "function rr(x,y,w,h,r){ctx.beginPath();ctx.moveTo(x+r,y);ctx.lineTo(x+w-r,y);";
  html += " ctx.quadraticCurveTo(x+w,y,x+w,y+r);ctx.lineTo(x+w,y+h-r);";
  html += " ctx.quadraticCurveTo(x+w,y+h,x+w-r,y+h);ctx.lineTo(x+r,y+h);";
  html += " ctx.quadraticCurveTo(x,y+h,x,y+h-r);ctx.lineTo(x,y+r);";
  html += " ctx.quadraticCurveTo(x,y,x+r,y);ctx.closePath();}";
  // 找到离指针最近的采样点并重绘
  html += "function setHover(px){if(!dat.length||!GX)return;let b=-1,bd=1e9;";
  html += " for(let i=0;i<dat.length;i++){const d=Math.abs(GX(i)-px);if(d<bd){bd=d;b=i;}}";
  html += " if(b!==hov){hov=b;draw();}}";
  html += "function atX(e){const r=cv.getBoundingClientRect();return e.clientX-r.left;}";
  html += "cv.addEventListener('mousemove',function(e){setHover(atX(e));});";
  html += "cv.addEventListener('mouseleave',function(){if(hov!==-1){hov=-1;draw();}});";
  html += "cv.addEventListener('touchstart',function(e){setHover(e.touches[0].clientX-cv.getBoundingClientRect().left);},{passive:true});";
  html += "cv.addEventListener('touchmove',function(e){setHover(e.touches[0].clientX-cv.getBoundingClientRect().left);},{passive:true});";
  html += "async function loadHist(){const r=await fetch('/history?type='+TYPE);const h=await r.json();";
  html += " const now=Date.now();for(let i=0;i<h.t.length;i++){lbl.push(fmt(now-(h.now-h.t[i])*1000));dat.push(h.v[i]);}draw();}";
  html += "async function tick(){const r=await fetch('/json');const d=await r.json();";
  html += " const v=d[KM[TYPE]];lbl.push(fmt(Date.now()));dat.push(v);";
  html += " if(lbl.length>MAXN){lbl.shift();dat.shift();}";
  html += " document.getElementById('curVal').innerText=(typeof v==='number')?v.toFixed(DEC):v;";
  // 底部统计：采样点数 / 最低 / 最高 / 平均
  html += " if(dat.length){let s=0,mi=dat[0],ma=dat[0];";
  html += "  for(let i=0;i<dat.length;i++){s+=Number(dat[i]);if(Number(dat[i])<mi)mi=Number(dat[i]);if(Number(dat[i])>ma)ma=Number(dat[i]);}";
  html += "  S('stN',dat.length);S('stMin',mi.toFixed(DEC));";
  html += "  S('stMax',ma.toFixed(DEC));S('stAvg',(s/dat.length).toFixed(DEC));}";
  html += " draw();}";
  html += "loadHist();tick();setInterval(tick,2000);";
  html += "window.addEventListener('resize',draw);";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

// ===================== 十二、历史数据接口 =====================
void handleHistory()
{
  String t = server.hasArg("type") ? server.arg("type") : String("temp");

  String j = "{\"now\":" + String(millis() / 1000) + ",\"t\":[";
  for (uint16_t i = 0; i < histCount; i++)
  {
    uint8_t k = (uint8_t)((histCount < HIST_LEN) ? i : (histIdx + i) % HIST_LEN);
    if (i) j += ",";
    j += String(histTime[k]);
  }
  j += "],\"v\":[";
  for (uint16_t i = 0; i < histCount; i++)
  {
    uint8_t k = (uint8_t)((histCount < HIST_LEN) ? i : (histIdx + i) % HIST_LEN);
    if (i) j += ",";
    float v = 0;
    if      (t == "temp")  v = histTemp[k];
    else if (t == "hum")   v = histHum[k];
    else if (t == "co2")   v = histCO2[k];
    else if (t == "tvoc")  v = histTVOC[k];
    else if (t == "press") v = histPress[k];
    else if (t == "alt")   v = histAlt[k];
    else if (t == "rain")  v = histRain[k];
    j += String(v, 1);
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// ===================== 十三、CSV 导出 =====================
String rainLevelFromAdc(int adc)
{
  if (adc > 3500) return "无雨";
  if (adc > 2500) return "小雨";
  if (adc > 1500) return "中雨";
  return "大雨";
}
String airLevelFrom(uint16_t c, uint16_t tv)
{
  if (c <= 600  && tv <= 50)  return "优";
  if (c <= 1000 && tv <= 100) return "良";
  if (c <= 1500 && tv <= 200) return "中";
  return "差";
}

void handleCSV()
{
  server.sendHeader("Content-Disposition", "attachment; filename=weather_data.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  server.sendContent("\xEF\xBB\xBF");   // UTF-8 BOM，避免 Excel 打开中文表头乱码
  server.sendContent("序号,运行时间(秒),温度(℃),湿度(%),CO2(ppm),TVOC(ppb),气压(hPa),海拔(m),雨量ADC,雨量等级,空气质量\n");

  char line[170];
  for (uint16_t i = 0; i < histCount; i++)
  {
    uint8_t k = (uint8_t)((histCount < HIST_LEN) ? i : (histIdx + i) % HIST_LEN);
    snprintf(line, sizeof(line), "%u,%lu,%.1f,%.1f,%.0f,%.0f,%.1f,%.1f,%.0f,%s,%s\n",
             i + 1, (unsigned long)histTime[k],
             histTemp[k], histHum[k], histCO2[k], histTVOC[k],
             histPress[k], histAlt[k], histRain[k],
             rainLevelFromAdc((int)histRain[k]).c_str(),
             airLevelFrom((uint16_t)histCO2[k], (uint16_t)histTVOC[k]).c_str());
    server.sendContent(line);
  }
  server.sendContent("");
}

// ===================== 十四、WiFi 事件回调 =====================
void WiFiEvent(WIFI_EVT_T event)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    if (wifi_connected)
    {
      Serial.println("WiFi 已断开，等待自动重连");
      wifi_connected = false;
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.print("WiFi 已连接，IP：");
    Serial.println(WiFi.localIP());
    wifi_connected = true;
    MDNS.end();
    if (MDNS.begin("esp32weather"))
    {
      Serial.println("mDNS 已启动：esp32weather.local");
    }
    mqtt.disconnect();
    break;
  default:
    break;
  }
}

// ===================== 十五、MQTT 连接维持 =====================
void reconnect_mqtt()
{
  if (!wifi_connected) return;
  if (mqtt.connected()) return;

  static unsigned long last_retry = 0;
  if (millis() - last_retry < 3000) return;
  last_retry = millis();

  char client_id[32];
  // 用芯片 MAC 生成固定 clientId：重连时 Broker 认得是同一台设备，
  // 不会像 random() 那样每次都换身份、反复踢掉旧会话
  uint64_t mac = ESP.getEfuseMac();
  snprintf(client_id, sizeof(client_id), "ESP32WS_%04X%04X",
           (uint16_t)(mac >> 32), (uint16_t)(mac & 0xFFFF));
  Serial.print("正在连接 MQTT 服务器...");
  if (mqtt.connect(client_id))
  {
    Serial.println("成功");
  }
  else
  {
    Serial.print("失败，错误码：");
    Serial.println(mqtt.state());
  }
}