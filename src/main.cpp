#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

// 配置信息
#define USER_ACCOUNT 1258878
#define USER_PASSWD "aaaa"

const char* WIFI_SSID = "vivo X100s";
const char* WIFI_PASS = "56nauhcd";

const char* LOGIN_URL = "https://api.meatsuger.top/api/user/login";
const char* DATA_URL = "https://api.meatsuger.top/api/data/a5c83f/Data";

// 全局变量
String authorizationToken = "";

// 函数声明
bool connectToWiFi();
bool authenticateUser();
bool sendSensorData();
String createSensorData();
String extractTokenFromHeader(const String& setCookieHeader);
String extractTokenFromBody(const String& jsonBody);
void deepSleepIfPossible();

void setup() {
  Serial.begin(115200);
  Serial.println("设备启动中...");

  // 连接 WiFi
  if (!connectToWiFi()) {
    Serial.println("WiFi 连接失败,重启设备...");
    ESP.restart();
    return;
  }

  // 用户认证
  if (!authenticateUser()) {
    Serial.println("用户认证失败,重启设备...");
    ESP.restart();
    return;
  }

  Serial.println("系统初始化完成");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 连接断开,尝试重新连接...");
    if (!connectToWiFi() || !authenticateUser()) {
      Serial.println("重新连接失败,重启设备...");
      ESP.restart();
      return;
    }
  }

  // 发送传感器数据
  if (!sendSensorData()) {
    Serial.println("数据发送失败");
  }

  delay(3000); // 3秒间隔
}

bool connectToWiFi() {
  Serial.print("正在连接 Wi-Fi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > 15000) { // 15秒超时
      Serial.println("\nWiFi 连接超时");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi 连接成功");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool authenticateUser() {
  Serial.println("开始用户认证...");
  
  WiFiClientSecure client;
  HTTPClient http;
  
  // 配置 HTTPS 客户端（根据需要进行证书验证）
  client.setInsecure(); // 跳过证书验证,生产环境建议使用证书
  
  // 构建登录 URL
  String loginUrl = String(LOGIN_URL) + "?account=" + String(USER_ACCOUNT) + 
                   "&passwd=" + USER_PASSWD;
  
  if (!http.begin(client, loginUrl)) {
    Serial.println("无法连接到登录服务器");
    return false;
  }
  
  http.setTimeout(10000);
  int httpCode = http.POST("");
  
  bool authSuccess = false;
  
  if (httpCode == HTTP_CODE_OK) {
    Serial.println("登录请求成功");
    
    // 尝试从响应头获取 token
    String setCookieHeader = http.header("Set-Cookie");
    if (!setCookieHeader.isEmpty()) {
      authorizationToken = extractTokenFromHeader(setCookieHeader);
      if (!authorizationToken.isEmpty()) {
        Serial.println("从响应头获取 token 成功");
        authSuccess = true;
      }
    }
    
    // 如果头部没有 token,尝试从响应体获取
    if (!authSuccess) {
      String responseBody = http.getString();
      authorizationToken = extractTokenFromBody(responseBody);
      if (!authorizationToken.isEmpty()) {
        Serial.println("从响应体获取 token 成功");
        authSuccess = true;
      }
    }
  } else {
    Serial.print("登录失败,HTTP 代码: ");
    Serial.println(httpCode);
    if (httpCode < 0) {
      Serial.print("错误信息: ");
      Serial.println(http.errorToString(httpCode));
    }
  }
  
  http.end();
  
  if (authSuccess) {
    Serial.print("认证 token: ");
    Serial.println(authorizationToken);
  } else {
    Serial.println("认证失败:无法获取 token");
  }
  
  return authSuccess;
}

bool sendSensorData() {
  if (authorizationToken.isEmpty()) {
    Serial.println("未认证,无法发送数据");
    return false;
  }
  
  WiFiClientSecure client;
  HTTPClient http;
  
  client.setInsecure();
  
  if (!http.begin(client, DATA_URL)) {
    Serial.println("无法连接到数据服务器");
    return false;
  }
  
  // 设置请求头
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", authorizationToken);
  http.setTimeout(10000);
  
  String jsonData = createSensorData();
  Serial.println("发送传感器数据:");
  Serial.println(jsonData);
  
  int httpCode = http.POST(jsonData);
  bool success = false;
  
  if (httpCode > 0) {
    Serial.print("HTTP 响应代码: ");
    Serial.println(httpCode);
    
    String response = http.getString();
    Serial.print("服务器响应: ");
    Serial.println(response);
    
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      Serial.println("数据发送成功");
      success = true;
    } else {
      Serial.println("数据发送失败,服务器返回错误");
      
      // 如果是认证错误,清除 token
      if (httpCode == HTTP_CODE_UNAUTHORIZED) {
        Serial.println("Token 已过期,需要重新认证");
        authorizationToken = "";
      }
    }
  } else {
    Serial.print("请求失败,错误代码: ");
    Serial.println(httpCode);
    Serial.print("错误信息: ");
    Serial.println(http.errorToString(httpCode));
  }
  
  http.end();
  return success;
}

String createSensorData() {
  DynamicJsonDocument doc(1024);
  JsonArray sensors = doc.createNestedArray("sensors");
  
  // 内部温度传感器
  JsonObject internalTemp = sensors.createNestedObject();
  internalTemp["name"] = "tempuaatre";
  internalTemp["type"] = "temp";
  internalTemp["value"] = roundf(temperatureRead() * 100) / 100.0f;
  
  // 可以添加更多传感器数据
  /*
  JsonObject humidity = sensors.createNestedObject();
  humidity["name"] = "humidity";
  humidity["type"] = "humidity";
  humidity["value"] = 68.5;
  */
  
  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

String extractTokenFromHeader(const String& setCookieHeader) {
  int authStart = setCookieHeader.indexOf("Authorization=");
  if (authStart == -1) return "";
  
  int valueStart = authStart + strlen("Authorization=");
  int valueEnd = setCookieHeader.indexOf(';', valueStart);
  if (valueEnd == -1) valueEnd = setCookieHeader.length();
  
  return setCookieHeader.substring(valueStart, valueEnd);
}

String extractTokenFromBody(const String& jsonBody) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, jsonBody);
  
  if (error) {
    Serial.print("JSON 解析错误: ");
    Serial.println(error.c_str());
    return "";
  }
  
  // 根据实际 API 响应结构调整
  if (doc.containsKey("data")) {
    JsonObject data = doc["data"];
    if (data.containsKey("tokenValue")) {
      return data["tokenValue"].as<String>();
    }
  }
  
  return "";
}