#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

const char* WIFI_SSID     = "Xiaomi_twt";
const char* WIFI_PASSWORD = "qiuqiyuan189";

#define NB_SERIAL Serial
const long NB_BAUD = 115200;

const int US_TRIG_PIN = D1; // HC-SR04 Trig 接 D1
const int US_ECHO_PIN = D2; // HC-SR04 Echo 接 D2
const int RELAY_PIN = D5;   // 继电器控制引脚（HIGH = ON）

ESP8266WebServer server(80);
String serialLog = "";
String networkStatus = "正在检测网络..."; 

// 异步任务标志位
volatile bool triggerDiagnose = false;
volatile bool triggerActivate = false;
volatile bool triggerSend     = false;
String mockDataToSend = "Hello_BC260Y_Test"; // 默认模拟数据

void readSerial() {
  while (NB_SERIAL.available()) {
    char c = NB_SERIAL.read();
    serialLog += c;
    if (serialLog.length() > 8000) serialLog = serialLog.substring(4000);
  }
}

void clearSerial() {
  while (NB_SERIAL.available()) {
    NB_SERIAL.read();
  }
}

void smartDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    server.handleClient(); 
    readSerial();          
    delay(20);             
  }
}

bool waitForModuleReady(int timeoutMs = 10000) {
  unsigned long start = millis();
  String response = "";
  while (millis() - start < (unsigned long)timeoutMs) {
    server.handleClient();
    while (NB_SERIAL.available()) {
      char c = NB_SERIAL.read();
      response += c;
      if (response.indexOf("RDY") != -1 || response.indexOf("OK") != -1 || response.indexOf("+CPIN: READY") != -1) {
        serialLog += "[INFO] 模块已准备好\n";
        return true;
      }
    }
    delay(50);
  }
  return false;
}

void at(String cmd, int waitTime = 500) {
  serialLog += "\n[TX] " + cmd + "\n";
  NB_SERIAL.println(cmd);
  smartDelay(waitTime); 
}

String waitForURC(String key, int timeoutMs = 30000) {
  unsigned long start = millis();
  while (millis() - start < (unsigned long)timeoutMs) {
    server.handleClient(); 
    readSerial();
    int idx = serialLog.indexOf(key);
    if (idx != -1) {
      int lineEnd = serialLog.indexOf('\n', idx);
      if (lineEnd == -1) lineEnd = serialLog.length();
      return serialLog.substring(idx, lineEnd);
    }
    delay(50);
  }
  return "";
}

// --- 自动化核心业务逻辑 ---

// 自适应网络检查：开机自动运行，无需手动点击
void autoCheckNetwork() {
  serialLog += "\n===== [系统开机] 正在自动检索移动网络 =====\n";
  at("AT+QCGDEFCONT=\"IP\",\"CMNBIOT\"");
  smartDelay(500);
  at("AT+CGATT?");
  at("AT+CGPADDR");
  
  if (serialLog.indexOf("+CGPADDR: 0,") != -1 || serialLog.indexOf("+CGPADDR: 1,") != -1) {
    networkStatus = "<span style='color:#43a047'>● 移动网络已自动就绪 (Context 0)</span>";
    serialLog += "\n[系统提示] 检测到基站已分配 IP，网络自动激活成功！可直接发送数据。\n";
  } else {
    networkStatus = "<span style='color:#e53935'>● 网络未就绪，请尝试点击诊断或检查卡片</span>";
  }
}

void diagnose() {
  serialLog = "===== 深度诊断 (移动卡) =====\n";
  at("AT"); 
  at("AT+CPIN?"); 
  at("AT+CSQ"); 
  at("AT+CEREG?"); 
  at("AT+CGATT?"); 
  at("AT+CGPADDR");
  
  if (serialLog.indexOf("+CGPADDR: 0,") != -1) {
    networkStatus = "<span style='color:#43a047'>● 移动网络已自动就绪 (Context 0)</span>";
  }
}

void activate() {
  serialLog = "===== 手动激活移动网络 =====\n";
  at("AT+QCGDEFCONT=\"IP\",\"CMNBIOT\"");
  smartDelay(500);
  at("AT+CGACT=1,1", 4000); // 尝试激活通道1作为备用
  at("AT+CGPADDR");
}

void sendData() {
  serialLog = "===== 建立连接 (使用 Context 0) =====\n";
  at("AT+QSCLK=0"); 
  at("AT+QICFG=\"dataformat\",0,0");
  at("AT+QICLOSE=0");
  
  at("AT+QIOPEN=0,0,\"UDP\",\"129.211.174.144\",8888", 200);
  
  String urc = waitForURC("+QIOPEN:", 30000);
  if (urc.length() > 0) {
    serialLog += "[URC] " + urc + "\n";
    int c = urc.lastIndexOf(',');
    if (c != -1) {
      String res = urc.substring(c+1);
      res.trim();
      if (res == "0") {
        // 动态计算数据的字节长度
        int dataLen = mockDataToSend.length();
        String sendCmd = "AT+QISEND=0," + String(dataLen) + ",\"" + mockDataToSend + "\"";
        at(sendCmd, 5000);
        if (serialLog.indexOf("SEND OK") != -1) {
          serialLog += "[OK] 模拟数据 [" + mockDataToSend + "] 发送成功！\n";
        } else {
          serialLog += "[FAIL] 数据未能成功发送。\n";
        }
        return;
      }
    }
    serialLog += "[FAIL] QIOPEN 返回了错误代码\n";
  } else {
    serialLog += "[TIMEOUT] 未收到 +QIOPEN URC。\n";
  }
}

float measureDistanceCm() {
  digitalWrite(US_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(US_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(US_TRIG_PIN, LOW);

  unsigned long duration = pulseIn(US_ECHO_PIN, HIGH, 30000);
  if (duration == 0) {
    return -1.0;
  }
  return duration * 0.0343 / 2.0;
}


// --- 网页 HTML ---
void handleRoot() {
  // 极简页面：仅包含一个复选框用于控制继电器
  String html = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>继电器</title>
  <style>html,body{height:100%;margin:0;display:flex;align-items:center;justify-content:center;background:#fff}input[type=checkbox]{transform:scale(1.6);width:22px;height:22px}</style>
</head>
<body>
  <input id="relay" type="checkbox" aria-label="继电器">
  <script>
    const cb=document.getElementById('relay');
    cb.addEventListener('change',()=>{fetch(`/relay?state=${cb.checked?'on':'off'}`).catch(()=>{});});
    window.addEventListener('load',async()=>{try{const r=await fetch('/relay');if(r.ok){const j=await r.json();cb.checked= j && (j.state==='on' || j.state===1 || j.state===true);} }catch(e){} }
    );
  </script>
</body>
</html>)HTML";
  server.send(200, "text/html", html);
}

void setup() {
  NB_SERIAL.begin(NB_BAUD);
  Serial.println();
  Serial.println("===== ESP8266 启动 =====");
  Serial.print("串口波特率: ");
  Serial.println(NB_BAUD);
  delay(2000);
  clearSerial();
  Serial.println("等待外部模块准备...");
  if (waitForModuleReady(12000)) {
    Serial.println("外部模块已准备好");
  } else {
    Serial.println("外部模块准备超时");
  }
  Serial.print("连接 Wi-Fi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.println("Wi-Fi 已连接");

  server.on("/", handleRoot);
  
  // 仅获取最新日志文本的异步接口（供前端网页 AJAX 调用）
  server.on("/getLog", [](){ server.send(200, "text/plain", serialLog); });
  
  server.on("/diagnose", [](){ triggerDiagnose = true; server.send(200, "text/plain", "OK"); });
  server.on("/activate", [](){ triggerActivate = true; server.send(200, "text/plain", "OK"); });
  
  // 接收前端模拟数据输入的接口
  server.on("/sendData", [](){ 
    if (server.hasArg("text")) {
      mockDataToSend = server.arg("text");
    }
    triggerSend = true;     
    server.send(200, "text/plain", "OK"); 
  });
  
  server.on("/distance", [](){
    float d = measureDistanceCm();
    if (d < 0) {
      server.send(200, "text/plain", "距离超时或无回波");
    } else {
      server.send(200, "text/plain", String(d, 1) + " cm");
    }
  });
  
  server.on("/cmd", [](){
    if (server.hasArg("val")) at(server.arg("val"), 1000);
    server.send(200, "text/plain", "OK");
  });

  // 继电器控制接口： GET /relay -> 返回 {"state":"on"|"off"}
  //                   GET /relay?state=on|off -> 设置继电器并返回当前状态
  server.on("/relay", [](){
    if (server.hasArg("state")){
      String s = server.arg("state");
      s.toLowerCase();
      if (s == "on" || s == "1") digitalWrite(RELAY_PIN, HIGH);
      else digitalWrite(RELAY_PIN, LOW);
    }
    String state = (digitalRead(RELAY_PIN) == HIGH) ? "on" : "off";
    String resp = "{";
    resp += "\"state\":\"" + state + "\"}";
    server.send(200, "application/json", resp);
  });

  server.begin();

  // 初始化传感器与继电器引脚
  pinMode(US_TRIG_PIN, OUTPUT);
  digitalWrite(US_TRIG_PIN, LOW);
  pinMode(US_ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // 默认关闭继电器

  // 无 I2C 传感器初始化（仅控制继电器）
  
  // 【核心改变】开机在网络栈准备好后，自动做一次网络扫描附着，不需要用户手动点
  autoCheckNetwork();
}

void loop() {
  server.handleClient();
  readSerial();
  
  if (triggerDiagnose) { triggerDiagnose = false; diagnose(); }
  if (triggerActivate) { triggerActivate = false; activate(); }
  if (triggerSend)     { triggerSend = false;     sendData(); }
}