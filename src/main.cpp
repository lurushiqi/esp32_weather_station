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
// ===================== 温湿度传感器引脚 =====================
#define DHT_PIN 25
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);
// ========== 雨量传感器 MH-RD 引脚 ==========
#define RAIN_AO_PIN 34
#define RAIN_DO_PIN 13
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
uint16_t co2 = 400;
uint16_t tvoc = 0;
float pressure = 0.0;
float altitude = 0.0;
// 雨量变量
int rainAdc = 0;
int rainDo = 0;
String rainLevel = "";
// 空气质量变量
String airQualityLevel = "";
// ===================== WiFi 状态 =====================
bool wifi_connected = false;
bool mdns_started = false;
char mqtt_buf[160]; // 缓冲区加大
uint32_t last_read = 0;
const uint32_t READ_INTERVAL = 5000;
// ===================== 函数声明 =====================
void reconnect_mqtt();
void read_sensors();
void handleJson();
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
  if (bme.begin())
  {
    bme_ok = true;
    Serial.println("✅ BME280 @0x76 初始化成功");
  }
  else
  {
    bme.setI2CAddress(0x77);
    if (bme.begin())
    {
      bme_ok = true;
      Serial.println("✅ BME280 @0x77 初始化成功");
    }
    else
    {
      Serial.println("❌ BME280 未找到");
    }
  }
  // SGP30
  sgp_ok = sgp.begin();
  if (sgp_ok)
  {
    sgp.IAQinit();
    Serial.println("✅ SGP30 初始化成功");
  }
  else
  {
    Serial.println("⚠️ 未检测到 SGP30");
  }
  dht.begin();
  pinMode(RAIN_DO_PIN, INPUT); // 雨量DO数字引脚初始化
  // WiFi 事件
  WiFi.onEvent(WiFiEvent);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  server.on("/", handleRoot);
  server.on("/json", handleJson);
  server.begin();
}
// ===================== LOOP =====================
void loop()
{
  server.handleClient();
  reconnect_mqtt();
  if (mqtt.connected())
    mqtt.loop();
  if (millis() - last_read >= READ_INTERVAL)
  {
    last_read = millis();
    read_sensors();
    // 串口打印，增加空气质量
    Serial.printf("温度:%.1f°C | 湿度:%.1f%% | CO2:%d | TVOC:%d | 气压:%.1f | 海拔:%.1f | 雨量ADC:%d | DO:%d | 雨量等级:%s | 空气质量:%s\n",
                  temperature, humidity, co2, tvoc, pressure, altitude, rainAdc, rainDo, rainLevel.c_str(), airQualityLevel.c_str());
    if (mqtt.connected() && wifi_connected)
    {
      // MQTT数据包：温度,湿度,CO2,TVOC,气压,海拔,雨量ADC,雨量等级,空气质量
      snprintf(mqtt_buf, sizeof(mqtt_buf), "%.2f,%.2f,%d,%d,%.1f,%.1f,%d,%s,%s",
               temperature, humidity, co2, tvoc, pressure, altitude, rainAdc, rainLevel.c_str(), airQualityLevel.c_str());
      mqtt.publish(MQTT_TOPIC, mqtt_buf);
      Serial.println("✅ 数据已发送至 MQTT");
    }
  }
}
// ===================== WiFi 事件回调 =====================
void WiFiEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    if (wifi_connected)
    {
      Serial.println("🔌 WiFi 已断开，等待自动重连...");
      wifi_connected = false;
      mdns_started = false;
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.print("✅ WiFi 重连成功！IP：");
    Serial.println(WiFi.localIP());
    wifi_connected = true;
    server.begin();
    MDNS.end();
    if (MDNS.begin("esp32weather"))
    {
      mdns_started = true;
      Serial.println("✅ MDNS 已启动：esp32weather.local");
    }
    mqtt.disconnect();
    break;
  default:
    break;
  }
}
// ===================== 网页面板 =====================
void handleRoot()
{
  String html = "";
  html += "<html lang='zh-CN'>";
  html += "<head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  // 【已经删掉meta自动刷新】
  html += "<title>🌤 气象监测站</title>";
  html += "<link rel='icon' href='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><text y=%22.9em%22 font-size=%2290%22>🌤</text></svg>'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js@3.9.1/dist/chart.min.js'></script>";
  html += "<style>";
  html += "*{margin:0;padding:0;box-sizing:border-box;font-family:'Microsoft YaHei',sans-serif;}";
  html += "body {background:#f0f7ff;color:#333;padding:15px 10px;}";
  html += ".page-wrap{max-width:1600px;margin:0 auto;display:grid;grid-template-columns:4fr 6fr;gap:24px;align-items:start;}";
  html += ".container {width:100%;}";
  html += ".card {background:#fff;border-radius:24px;padding:10px 38px;box-shadow:0 6px 26px rgba (45,138,255,0.09);}";
  html += ".ip-bar {background:#2d8aff;color:#fff;padding:12px 18px;border-radius:14px;text-align:center;font-size:15px;margin-top:28px;}";
  html += ".status-bar {text-align:center;font-size:12px;color:#666;margin:12px 0 0;}";
  html += ".title {text-align:center;font-size:28px;font-weight:bold;margin-bottom:20px;color:#2d8aff;letter-spacing:1px;}";
  html += ".item {padding:16px 0;border-bottom:1px solid #f3f6fc;}";
  html += ".item:last-child {border-bottom:none;padding-bottom:0;}";
  html += ".row-top {display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;}";
  html += ".item-left {display:flex;align-items:center;gap:12px;font-size:19px;}";
  html += ".item-right {font-size:21px;font-weight:bold;color:#222;display:flex;align-items:center;gap:9px;}";
  html += ".icon {font-size:26px;width:32px;text-align:center;}";
  html += ".desc {font-size:13px;color:#757983;padding-left:44px;}";
  html += ".tag {font-size:13px;padding:3px 8px;border-radius:7px;color:#fff;font-weight:normal;white-space:nowrap;}";
  html += ".tag-excel {background:#00C853;}";
  html += ".tag-good {background:#2196F3;}";
  html += ".tag-mid {background:#FF9800;}";
  html += ".tag-bad {background:#F44336;}";
  html += ".wifi-online {color:#00C853;font-weight:bold;}";
  html += ".wifi-offline {color:#F44336;font-weight:bold;}";
  html += ".mqtt-online {color:#00C853;font-weight:bold;}";
  html += ".mqtt-offline {color:#F44336;font-weight:bold;}";
  html += ".chart-col{display:flex;flex-direction:column;gap:14px;}";
  html += ".chart-box{background:#fff;border-radius:20px;padding:16px;height:320px;box-shadow:0 6px 26px rgba(45,138,255,0.09);}";
  html += "@media (max-width:1100px){.page-wrap{grid-template-columns:1fr;}}";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='page-wrap'>";
  html += "<div class='container'>";
  html += "<div class='card'>";
  html += "<div class='title'>🌤 智能气象监测站</div>";
  // 温度
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>🌡温度</div>";
  html += "<div class='item-right'><span id='tempVal'>"+String(temperature,1)+"</span> ℃<span id='tempTag' class='tag tag-excel'>适宜</span></div>";
  html += "</div>";
  html += "<div class='desc'>舒适参考：18～26℃</div>";
  html += "</div>";
  // 湿度
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>💧湿度</div>";
  html += "<div class='item-right'><span id='humVal'>"+String(humidity,1)+"</span> %<span id='humTag' class='tag tag-excel'>适宜</span></div>";
  html += "</div>";
  html += "<div class='desc'>舒适参考：40%～65% RH</div>";
  html += "</div>";
  // 雨量
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>🌧️雨量</div>";
  html += "<div class='item-right'><span id='rainVal'>"+rainLevel+"</span><span id='rainTag' class='tag tag-excel'>无雨</span></div>";
  html += "</div>";
  html += "<div class='desc'>传感器 AO 读数：<span id='rainAdcVal'>"+String(rainAdc)+"</span></div>";
  html += "</div>";
  // 空气质量
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>🌬️空气质量</div>";
  html += "<div class='item-right'><span id='airVal'>"+airQualityLevel+"</span><span id='airTag' class='tag tag-excel'>优</span></div>";
  html += "</div>";
  html += "<div class='desc'>依据 CO₂、TVOC 评估室内空气状况</div>";
  html += "</div>";
  // CO2
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>🫧CO₂</div>";
  html += "<div class='item-right'><span id='co2Val'>"+String(co2)+"</span> ppm<span id='co2Tag' class='tag tag-excel'>优</span></div>";
  html += "</div>";
  html += "<div class='desc'>正常参考：400～1000ppm</div>";
  html += "</div>";
  // TVOC
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>🧪TVOC 总挥发性有机气体</div>";
  html += "<div class='item-right'><span id='tvocVal'>"+String(tvoc)+"</span> ppb<span id='tvocTag' class='tag tag-excel'>优</span></div>";
  html += "</div>";
  html += "<div class='desc'>甲醛、苯、装修挥发气体总称，正常参考：0～100ppb</div>";
  html += "</div>";
  // 气压
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>🌪️气压</div>";
  html += "<div class='item-right'><span id='pressVal'>"+String(pressure,1)+"</span> hPa<span id='pressTag' class='tag tag-mid'>偏低</span></div>";
  html += "</div>";
  html += "<div class='desc'>标准常压参考：980～1030hPa</div>";
  html += "</div>";
  // 海拔
  html += "<div class='item'>";
  html += "<div class='row-top'>";
  html += "<div class='item-left'>⛰️海拔</div>";
  html += "<div class='item-right'><span id='altVal'>"+String(altitude,1)+"</span> m</div>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  html += "<div class='ip-bar'>IP："+ WiFi.localIP().toString() +"     域名：esp32weather.local</div>";
  html += "<div class='status-bar'>WiFi：<span id='wifiStatus'>";
  html += (wifi_connected?"已连接":"断开");
  html += "</span>   |  MQTT：<span id='mqttStatus'>";
  html += (mqtt.connected()?"已连接":"未连接");
  html += "</span></div>";
  html += "</div>";

  html += "<div class='chart-col'>";
  html += "<div class='chart-box'><canvas id='c1'></canvas></div>";
  html += "<div class='chart-box'><canvas id='c2'></canvas></div>";
  html += "</div>";
  html += "</div>";

  html += "<script>";
  html += "const MAX=20;";
  html += "const lbl=[],dTemp=[],dHum=[],dCO2=[],dTVOC=[];";
  html += "const optBase = {responsive:true,maintainAspectRatio:false,";
  html += "plugins:{legend:{position:'top',labels:{usePointStyle:true,pointStyle:'rect',font:{size:14}}}}};";

  html += "const c1=new Chart(document.getElementById('c1'),{";
  html += " type:'line',data:{labels:lbl,datasets:[";
  html += "{label:'温度 ℃',data:dTemp,borderColor:'#ff4444',backgroundColor:'rgba(255,68,68,.15)',borderWidth:2,pointRadius:4,tension:0.3,fill:true},";
  html += "{label:'湿度 %',data:dHum,borderColor:'#2196F3',backgroundColor:'rgba(33,150,243,.15)',borderWidth:2,pointRadius:4,tension:0.3,fill:true}";
  html += " ]},options:{...optBase,plugins:{...optBase.plugins,title:{display:true,text:'温湿度变化曲线',font:{size:20}}}}});";

  html += "const c2=new Chart(document.getElementById('c2'),{";
  html += " type:'line',data:{labels:lbl,datasets:[";
  html += "{label:'CO₂ ppm',data:dCO2,borderColor:'#00C853',backgroundColor:'rgba(0,200,83,.15)',borderWidth:2,pointRadius:4,tension:0.3,fill:true,yAxisID:'y'},";
  html += "{label:'TVOC ppb',data:dTVOC,borderColor:'#FF9800',backgroundColor:'rgba(255,152,0,.15)',borderWidth:2,pointRadius:4,tension:0.3,fill:true,yAxisID:'y1'}";
  html += " ]},options:{...optBase,plugins:{...optBase.plugins,title:{display:true,text:'气体浓度曲线',font:{size:20}}},";
  html += " scales:{ y:{position:'left',title:{display:true,text:'CO₂ ppm'}},";
  html += " y1:{position:'right',title:{display:true,text:'TVOC ppb'},grid:{drawOnChartArea:false}} }}});";

  html += "async function updateChart(){ ";
  html += " const res=await fetch('/json'); const d=await res.json(); ";
  html += " const now=new Date(); const timeStr=now.getHours()+\":\"+now.getMinutes()+\":\"+now.getSeconds();";
  html += "lbl.push(timeStr); dTemp.push(d.temp); dHum.push(d.hum); dCO2.push(d.co2); dTVOC.push(d.tvoc);";
  html += "if(lbl.length>MAX){lbl.shift();dTemp.shift();dHum.shift();dCO2.shift();dTVOC.shift();}";
  html += " c1.update();c2.update(); ";
  // 这里更新左侧卡片数值
  html += "document.getElementById('tempVal').innerText = d.temp;";
  html += "document.getElementById('humVal').innerText = d.hum;";
  html += "document.getElementById('rainVal').innerText = d.rain;";
  html += "document.getElementById('rainAdcVal').innerText = d.rainAdc;";
  html += "document.getElementById('co2Val').innerText = d.co2;";
  html += "document.getElementById('tvocVal').innerText = d.tvoc;";
  html += "document.getElementById('pressVal').innerText = d.press;";
  html += "document.getElementById('altVal').innerText = d.alt;";
  html += "document.getElementById('airVal').innerText = d.air;";
  html += "}";
  html += "updateChart(); setInterval(updateChart,2000);"; //2秒刷新一次
  html += "</script>";
  html += "</body></html>";
  server.send (200, "text/html", html);
}


// ===================== 读取传感器 =====================
void read_sensors()
{
  humidity = dht.readHumidity();
  temperature = dht.readTemperature();
  if (isnan(humidity))
    humidity = 0;
  if (isnan(temperature))
    temperature = 0;
  // 读取雨量传感器
  rainAdc = analogRead(RAIN_AO_PIN);
  rainDo = digitalRead(RAIN_DO_PIN);
  if (rainAdc > 3500)
  {
    rainLevel = "无雨";
  }
  else if (rainAdc > 2500)
  {
    rainLevel = "小雨";
  }
  else if (rainAdc > 1500)
  {
    rainLevel = "中雨";
  }
  else
  {
    rainLevel = "大雨";
  }
  if (sgp_ok)
  {
    sgp.IAQmeasure();
    co2 = sgp.eCO2;
    tvoc = sgp.TVOC;
    // 空气质量判定
    if (co2 <= 600 && tvoc <= 50)
    {
      airQualityLevel = "优";
    }
    else if (co2 <= 1000 && tvoc <= 100)
    {
      airQualityLevel = "良";
    }
    else if (co2 <= 1500 && tvoc <= 200)
    {
      airQualityLevel = "中";
    }
    else
    {
      airQualityLevel = "差";
    }
  }
  else
  {
    airQualityLevel = "传感器离线";
  }
  if (bme_ok)
  {
    pressure = bme.readFloatPressure() / 100.0;
    altitude = bme.readFloatAltitudeMeters();
  }
}
void handleJson(){
  String json = "{";
  json += "\"temp\":" + String(temperature,1);
  json += ",\"hum\":" + String(humidity,1);
  json += ",\"rain\":\"" + rainLevel + "\"";
  json += ",\"rainAdc\":" + String(rainAdc);
  json += ",\"co2\":" + String(co2);
  json += ",\"tvoc\":" + String(tvoc);
  json += ",\"press\":" + String(pressure,1);
  json += ",\"alt\":" + String(altitude,1);
  json += ",\"air\":\"" + airQualityLevel + "\"";
  json += "}";
  server.send(200,"application/json",json);
}


// ===================== MQTT 重连 =====================
void reconnect_mqtt()
{
  if (!wifi_connected)
    return;
  if (mqtt.connected())
    return;
  static unsigned long last_retry = 0;
  if (millis() - last_retry < 3000)
    return;
  last_retry = millis();
  char client_id[32];
  snprintf(client_id, 32, "ESP32_WEATHER_%d", random(1000, 9999));
  Serial.print("正在重连 MQTT...");
  if (mqtt.connect(client_id))
  {
    Serial.println("✅ MQTT 重连成功！");
  }
  else
  {
    Serial.print("❌ MQTT 失败：");
    Serial.println(mqtt.state());
  }
}
