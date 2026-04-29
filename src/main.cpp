#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_SGP30.h>
#include <SparkFunBME280.h>
#include <ESPmDNS.h>

// ===================== WiFi 信息 =====================
const char *WIFI_SSID = "iQOO";
const char *WIFI_PASSWORD = "llllffff";

// ===================== MQTT 配置 =====================
const char *MQTT_BROKER = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char *MQTT_TOPIC = "weather_station_data";

// ===================== 传感器引脚 =====================
#define DHT_PIN 25
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

#define AIR_QUALITY_PIN 34
#define AIR_ALARM_PIN 13

// ===================== BME280 =====================
BME280 bme;
bool bme_ok = false;

// ===================== SGP30 =====================
Adafruit_SGP30 sgp;
bool sgp_ok = false;

WiFiClient espClient;
PubSubClient mqtt(espClient);
WebServer server(80);

// ===================== 气象数据 =====================
float temperature = 0.0;
float humidity = 0.0;
int air_quality = 0;
uint16_t co2 = 400;
uint16_t tvoc = 0;
float pressure = 0.0;
float altitude = 0.0;
String air_level = "";

// ===================== WiFi 状态 =====================
bool wifi_connected = false;
bool mdns_started = false;

char mqtt_buf[128];
uint32_t last_read = 0;
const uint32_t READ_INTERVAL = 5000;

// ===================== 函数声明 =====================
void reconnect_mqtt();
void read_sensors();
void handleRoot();
void WiFiEvent(WiFiEvent_t event);

// ===================== SETUP =====================
void setup()
{
  Serial.begin(115200);
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("🔥 ESP32 无线气象站启动");

  Wire.begin(21, 22);

  // BME280
  bme.setI2CAddress(0x76);
  if (bme.begin()) {
    bme_ok = true;
    Serial.println("✅ BME280 @0x76 初始化成功");
  } else {
    bme.setI2CAddress(0x77);
    if (bme.begin()) {
      bme_ok = true;
      Serial.println("✅ BME280 @0x77 初始化成功");
    } else {
      Serial.println("❌ BME280 未找到");
    }
  }

  // SGP30
  sgp_ok = sgp.begin();
  if (sgp_ok) {
    sgp.IAQinit();
    Serial.println("✅ SGP30 初始化成功");
  } else {
    Serial.println("⚠️ 未检测到 SGP30");
  }

  dht.begin();
  pinMode(AIR_ALARM_PIN, INPUT);

  // WiFi 事件（安全重连核心）
  WiFi.onEvent(WiFiEvent);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  server.on("/", handleRoot);
  server.begin();
}

// ===================== LOOP =====================
void loop()
{
  server.handleClient();
  reconnect_mqtt();
  if (mqtt.connected()) mqtt.loop();

  if (millis() - last_read >= READ_INTERVAL) {
    last_read = millis();
    read_sensors();

    Serial.printf("温度:%.1f°C | 湿度:%.1f%% | 空气质量:%d | CO2:%d | TVOC:%d | 气压:%.1f | 海拔:%.1f\n",
      temperature, humidity, air_quality, co2, tvoc, pressure, altitude);

    if (mqtt.connected() && wifi_connected) {
      snprintf(mqtt_buf, sizeof(mqtt_buf), "%.2f,%.2f,%d,%d,%d,%.1f,%.1f",
        temperature, humidity, air_quality, co2, tvoc, pressure, altitude);
      mqtt.publish(MQTT_TOPIC, mqtt_buf);
      Serial.println("✅ 数据已发送至 MQTT");
    }
  }
}

// ===================== 【只修这里】WiFi 事件回调 =====================
// ===================== 【修复：WiFi重连后恢复网页 + MQTT立刻重连】 =====================
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (wifi_connected) {
        Serial.println("🔌 WiFi 已断开，等待自动重连...");
        wifi_connected = false;
        mdns_started = false;
      }
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("✅ WiFi 重连成功！IP：");
      Serial.println(WiFi.localIP());
      wifi_connected = true;

      // 🔥 修复 1：重启网页服务器（必须加）
      server.begin();

      // 🔥 修复 2：重启域名（必须加）
      MDNS.end();
      if (MDNS.begin("esp32weather")) {
        mdns_started = true;
        Serial.println("✅ MDNS 已启动：esp32weather.local");
        
      }

      // 🔥 修复 3：MQTT 立刻强制重连（必须加）
      mqtt.disconnect();
      break;

    default:
      break;
  }
}

// ===================== 网页（完全不动） =====================
// ===================== 网页（已修改：WiFi/MQTT同行+放后面+小字） =====================
void handleRoot() {
  String html = "<!DOCTYPE html>";
  html += "<html lang='zh-CN'>";
  html += "<head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<meta http-equiv='refresh' content='2'>";
  
  html += "<title>🌤 气象监测站</title>";
  html += "<link rel='icon' href='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><text y=%22.9em%22 font-size=%2290%22>🌤</text></svg>'>";
  
  html += "<style>";
  html += "*{margin:0;padding:0;box-sizing:border-box;font-family:'Microsoft YaHei',sans-serif;}";
  html += "body{background:#f0f7ff;color:#333;}";
  html += ".container{max-width:500px;margin:30px auto;padding:0 20px;}";
  html += ".card{background:#fff;border-radius:20px;padding:25px 30px;box-shadow:0 8px 30px rgba(0,80,200,0.08);margin-bottom:20px;}";
  html += ".ip-bar{background:#2d8aff;color:#fff;padding:10px 18px;border-radius:12px;text-align:center;font-size:15px;margin-bottom:10px;}";
  html += ".status-bar{text-align:center;font-size:12px;color:#666;margin-bottom:20px;}"; // 小字状态条
  html += ".title{text-align:center;font-size:26px;font-weight:bold;margin-bottom:25px;color:#2d8aff;}";
  html += ".item{display:flex;align-items:center;justify-content:space-between;padding:14px 0;border-bottom:1px solid #f0f0f0;}";
  html += ".item:last-child{border-bottom:none;}";
  html += ".item-left{display:flex;align-items:center;gap:12px;font-size:18px;}";
  html += ".item-right{font-size:20px;font-weight:bold;color:#222;}";
  html += ".icon{font-size:24px;width:30px;text-align:center;}";
  html += ".wifi-online{color:#00C853;font-weight:bold;}";
  html += ".wifi-offline{color:#F44336;font-weight:bold;}";
  html += ".mqtt-online{color:#00C853;font-weight:bold;}";
  html += ".mqtt-offline{color:#F44336;font-weight:bold;}";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='container'>";
  html += "<div class='card'>";

  html += "<div class='title'>🌤 智能气象监测站</div>";

  html += "<div class='item'><div class='item-left'><span class='icon'>🌡</span><span>温度</span></div><div class='item-right'>" + String(temperature,1) + " ℃</div></div>";
  html += "<div class='item'><div class='item-left'><span class='icon'>💧</span><span>湿度</span></div><div class='item-right'>" + String(humidity,1) + " %</div></div>";
  html += "<div class='item'><div class='item-left'><span class='icon'>🍃</span><span>空气质量</span></div><div class='item-right'>" + air_level + "</div></div>";
  html += "<div class='item'><div class='item-left'><span class='icon'>🫧</span><span>CO₂</span></div><div class='item-right'>" + String(co2) + " ppm</div></div>";
  html += "<div class='item'><div class='item-left'><span class='icon'>🧪</span><span>TVOC</span></div><div class='item-right'>" + String(tvoc) + " ppb</div></div>";
  html += "<div class='item'><div class='item-left'><span class='icon'>🌪️</span><span>气压</span></div><div class='item-right'>" + String(pressure,1) + " hPa</div></div>";
  html += "<div class='item'><div class='item-left'><span class='icon'>⛰️</span><span>海拔</span></div><div class='item-right'>" + String(altitude,1) + " m</div></div>";

// IP + 域名
  html += "<div class='ip-bar'>IP：" + WiFi.localIP().toString() + "  &nbsp;&nbsp; 域名：esp32weather.local</div>";

  // 🔥 WiFi + MQTT 同行小字（已放在这里）
  html += "<div class='status-bar'>";
  html += "WiFi：";
  html += wifi_connected ? "<span class='wifi-online'>已连接</span>" : "<span class='wifi-offline'>断开</span>";
  html += " &nbsp;&nbsp;|&nbsp;&nbsp;";
  html += "MQTT：";
  html += mqtt.connected() ? "<span class='mqtt-online'>已连接</span>" : "<span class='mqtt-offline'>未连接</span>";
  html += "</div>";
  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}
// ===================== 读取传感器（完全不动） =====================
void read_sensors()
{
  humidity = dht.readHumidity();
  temperature = dht.readTemperature();
  if (isnan(humidity)) humidity = 0;
  if (isnan(temperature)) temperature = 0;

  air_quality = analogRead(AIR_QUALITY_PIN);
  if (air_quality >= 0 && air_quality <= 2500) {
    air_level = "优";
  } else if (air_quality >= 2501 && air_quality <= 3000) {
    air_level = "良";
  } else if (air_quality >= 3001 && air_quality <= 3500) {
    air_level = "中";
  } else if (air_quality >= 3501 && air_quality <= 4000) {
    air_level = "差";
  } else {
    air_level = "严重污染";
  }

  if (sgp_ok) {
    sgp.IAQmeasure();
    co2 = sgp.eCO2;
    tvoc = sgp.TVOC;
  }

  if (bme_ok) {
    pressure = bme.readFloatPressure() / 100.0;
    altitude = bme.readFloatAltitudeMeters();
  }
}

// ===================== MQTT 重连（完全不动） =====================
void reconnect_mqtt()
{
  if (!wifi_connected) return;
  if (mqtt.connected()) return;

  static unsigned long last_retry = 0;
  if (millis() - last_retry < 3000) return;
  last_retry = millis();

  char client_id[32];
  snprintf(client_id, 32, "ESP32_WEATHER_%d", random(1000, 9999));

  Serial.print("正在重连 MQTT...");
  if (mqtt.connect(client_id)) {
    Serial.println("✅ MQTT 重连成功！");
  } else {
    Serial.print("❌ MQTT 失败：");
    Serial.println(mqtt.state());
  }
}