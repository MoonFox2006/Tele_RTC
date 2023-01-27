#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>

constexpr uint8_t BTN_PIN = 0;
constexpr bool BTN_LEVEL = LOW;

constexpr uint8_t LED_PIN = 23;
constexpr bool LED_LEVEL = LOW;

const char WIFI_SSID[] = "YOUR_SSID";
const char WIFI_PSWD[] = "YOUR_PSWD";

const char NTP_SERVER[] = "pool.ntp.org";
constexpr int8_t NTP_TZ = 3; // Your time zone

const char TELE_KEY[] = "YOUR_TELE_BOT_KEY";
constexpr int32_t TELE_CHAT = 0; // YOUR_TELE_GROUP_CHAT_ID

RTC_DATA_ATTR uint32_t sleepDuration = 0;
RTC_DATA_ATTR int32_t teleMsgId = 0;

static bool wifiConnect(uint32_t timeout = 30000) {
  uint32_t time;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PSWD);
  time = millis();
  while ((! WiFi.isConnected()) && (millis() - time < timeout)) {
    digitalWrite(LED_PIN, LED_LEVEL);
    delay(25);
    digitalWrite(LED_PIN, ! LED_LEVEL);
    delay(500 - 25);
    Serial.print('.');
  }
  if (WiFi.isConnected()) {
    Serial.print(" OK (IP ");
    Serial.print(WiFi.localIP());
    Serial.println(')');
    return true;
  } else {
    WiFi.disconnect(true);
    Serial.println(" FAIL!");
    return false;
  }
}

static uint32_t ntpUpdate(uint32_t timeout = 1000, uint8_t repeat = 1) {
  constexpr uint16_t LOCAL_PORT = 55123;

  WiFiUDP udp;

  if (udp.begin(LOCAL_PORT)) {
    do {
      uint8_t buffer[48];

      memset(buffer, 0, sizeof(buffer));
      // Initialize values needed to form NTP request
      buffer[0] = 0B11100011; // LI, Version, Mode
      buffer[1] = 0; // Stratum, or type of clock
      buffer[2] = 6; // Polling Interval
      buffer[3] = 0xEC; // Peer Clock Precision
      // 8 bytes of zero for Root Delay & Root Dispersion
      buffer[12] = 49;
      buffer[13] = 0x4E;
      buffer[14] = 49;
      buffer[15] = 52;
      // all NTP fields have been given values, now
      // you can send a packet requesting a timestamp
      if (udp.beginPacket(NTP_SERVER, 123) && (udp.write(buffer, sizeof(buffer)) == sizeof(buffer)) && udp.endPacket()) {
        uint32_t time = millis();
        int cb;

        while ((! (cb = udp.parsePacket())) && (millis() - time < timeout)) {
          delay(1);
        }
        if (cb) {
          // We've received a packet, read the data from it
          if (udp.read(buffer, sizeof(buffer)) == sizeof(buffer)) { // read the packet into the buffer
            // the timestamp starts at byte 40 of the received packet and is four bytes,
            // or two words, long. First, esxtract the two words:
            time = (((uint32_t)buffer[40] << 24) | ((uint32_t)buffer[41] << 16) | ((uint32_t)buffer[42] << 8) | buffer[43]) - 2208988800UL;
            time += NTP_TZ * 3600;
            return time;
          }
        }
      }
    } while (repeat--);
  }
  return 0;
}

static void printTime(uint32_t time) {
  Serial.printf("%u (%02u:%02u:%02u)\n", time, (time % 86400) / 3600, (time % 3600) / 60, time % 60);
}

static bool getMessageId(const char *answer) {
  char *str = strstr(answer, "\"message_id\":");

  if (str) {
    int32_t num = 0;
    bool minus = false;

    str += 13; // strlen("\"message_id\":")
    if (*str == '-') {
      minus = true;
      ++str;
    }
    while ((*str >= '0') && (*str <= '9')) {
      num = num * 10 + (*str++ - '0');
    }
    if (minus)
      num = -num;
    teleMsgId = num;
    return true;
  }
  return false;
}

static bool teleSend(uint32_t rtcTime, uint32_t ntpTime) {
  WiFiClientSecure *tcp;
  HTTPClient *https;
  bool result = false;

  tcp = new WiFiClientSecure();
  if (tcp) {
    tcp->setInsecure();
    https = new HTTPClient();
    if (https) {
      char msg[256];
      String answer;

      snprintf(msg, sizeof(msg), "https://api.telegram.org/bot%s/", TELE_KEY);
      Serial.print("Connecting to Telegram BOT API... ");
      if (https->begin(*tcp, msg)) {
        Serial.println("OK");
        https->addHeader("Content-Type", "application/json");
        https->addHeader("Connection", "close");
        if (teleMsgId)
          sprintf(msg, "{\"method\":\"editMessageText\",\"chat_id\":%d,\"message_id\":%d", TELE_CHAT, teleMsgId);
        else
          sprintf(msg, "{\"method\":\"sendMessage\",\"chat_id\":%d", TELE_CHAT);
        sprintf(&msg[strlen(msg)], ",\"text\":\"RTC time: %u\nNTP time: %u\nSleep duration: %u sec.\"}",
          rtcTime, ntpTime, sleepDuration / 1000);
//        Serial.printf("Sending BOT message \"%s\" ", msg);
        Serial.print("Sending BOT message ");
        result = https->POST(msg) == 200;
        if (result)
          Serial.println("OK");
        else
          Serial.println("FAIL!");
        answer = https->getString();
//        Serial.println(answer);
        if (result) {
          if (getMessageId(answer.c_str())) {
            Serial.printf("Message Id: %d\n", teleMsgId);
          }
        } else {
          if (teleMsgId) {
            sprintf(msg, "{\"method\":\"sendMessage\",\"chat_id\":%d,\"text\":\"RTC time: %u\nNTP time: %u\nSleep duration: %u sec.\"}",
              TELE_CHAT, rtcTime, ntpTime, sleepDuration / 1000);
//            Serial.printf("Resending BOT message \"%s\" ", msg);
            Serial.print("Resending BOT message ");
            result = https->POST(msg) == 200;
            if (result)
              Serial.println("OK");
            else
              Serial.println("FAIL!");
            answer = https->getString();
//            Serial.println(answer);
            if (result) {
              if (getMessageId(answer.c_str())) {
                Serial.printf("Message Id: %d\n", teleMsgId);
              }
            }
          }
        }
        https->end();
      } else
        Serial.println("FAIL!");
      delete https;
    }
    delete tcp;
  }
  return result;
}

void setup() {
  uint32_t brownOutReg;

  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, ! LED_LEVEL);

  brownOutReg = READ_PERI_REG(RTC_CNTL_BROWN_OUT_REG);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector
  if (wifiConnect()) {
    timeval val;
    uint32_t time;

    gettimeofday(&val, nullptr);
    Serial.print("RTC time: ");
    printTime((uint32_t)val.tv_sec);

    Serial.printf("Updating time from NTP server \"%s\" ", NTP_SERVER);
    if ((time = ntpUpdate(1000, 3)) != 0) {
      Serial.print("OK\n"
        "NTP time: ");
      printTime(time);
      if (val.tv_sec < 24 * 3600) {
        val.tv_sec = time;
        val.tv_usec = 0;
        settimeofday(&val, NULL);
        Serial.print("RTC time updated\n");
      } else {
        Serial.printf("Time difference: %d sec.\n", (int32_t)(time - (uint32_t)val.tv_sec));
      }
      teleSend((uint32_t)val.tv_sec, time);
    } else
      Serial.println("FAIL!");
    WiFi.disconnect(true);
  }

  if (sleepDuration == 0)
    sleepDuration = 300000; // 5 min.
  else if (sleepDuration == 300000)
    sleepDuration = 600000; // 10 min.
  else if (sleepDuration == 600000)
    sleepDuration = 900000; // 15 min.
  else if (sleepDuration == 900000)
    sleepDuration = 1800000; // 30 min.
  else if (sleepDuration == 1800000)
    sleepDuration = 3600000; // 1 hour
  else if (sleepDuration == 3600000)
    sleepDuration = 7200000; // 2 hours
  else if (sleepDuration == 7200000)
    sleepDuration = 14400000; // 4 hours
  else if (sleepDuration == 14400000)
    sleepDuration = 28800000; // 8 hours
  else if (sleepDuration == 28800000)
    sleepDuration = 43200000; // 12 hours
  else
    sleepDuration = 86400000; // 24 hours
  Serial.printf("Going to sleep for %u sec.\n", sleepDuration / 1000);
  Serial.flush();

  esp_deep_sleep_disable_rom_logging();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepDuration * 1000);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, brownOutReg); // Restore brownout detector
  esp_deep_sleep_start();
}

void loop() {}
