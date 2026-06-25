/*
 * =============================================================================
 * esp_firmware.ino — ESP32 Web Server cho Logic Analyzer
 * =============================================================================
 * Vai trò: Cầu nối giữa Pico 2 (SPI Slave) và trình duyệt (WebSocket/WiFi)
 *
 * Luồng hoạt động:
 *   1. ESP32 khởi động WiFi Access Point "PicoScope_WiFi"
 *   2. Trình duyệt kết nối WiFi, mở http://192.168.4.1 → nhận file HTML
 *   3. HTML kết nối WebSocket ws://192.168.4.1:81
 *   4. Khi người dùng nhấn "Start Capture" trên HTML:
 *      a. HTML gửi WebSocket message: "START_CAPTURE"
 *      b. ESP32 nhận → kéo GPIO4 (Trigger) lên HIGH
 *      c. Pico 2 bắt sườn lên GP8 → capture 20ms dữ liệu → nén RLE
 *      d. Pico 2 gửi RLE qua SPI sang ESP32 (bắt đầu bằng 0xAA, kết thúc 0xBB)
 *      e. ESP32 nhận xong → đẩy cục RLE qua WebSocket về HTML
 *      f. HTML giải nén → decode UART/I2C/SPI → vẽ waveform
 *
 * Phần cứng:
 *   ESP32 ─── WiFi ──► Laptop/Điện thoại (mở HTML)
 *   ESP32 ─── SPI  ──► Pico 2 (nhận RLE)
 *   ESP32 ─── GPIO4 ── GP8 Pico 2 (Trigger)
 *
 * SPI Pins (ESP32 là Master, Pico là Slave):
 *   GPIO 18 (SCK)  → GP4 Pico (nhớ: đây là chân SPI DATA, khác với chân đo!)
 *   GPIO 23 (MOSI) → GP5 Pico
 *   GPIO 19 (MISO) → GP6 Pico  ← Pico gửi dữ liệu về qua chân này
 *   GPIO 5  (CS)   → GP7 Pico
 *
 * Trigger:
 *   GPIO 4 → GP8 Pico (dây thẳng, không qua IC)
 *
 * Thư viện cần cài (Arduino IDE → Library Manager):
 *   - "WebSockets" by Markus Sattler (tìm "WebSocketsServer")
 *   - "ESPAsyncWebServer" (tùy chọn nếu dùng async)
 * =============================================================================
 */

#include <WiFi.h>
#include <WebSocketsServer.h>
#include <SPI.h>
#include <SPIFFS.h>

// ─── CẤU HÌNH WiFi ────────────────────────────────────────────────────────────
const char* AP_SSID     = "PicoScope_WiFi";
const char* AP_PASSWORD = "picoscope123";   // Tối thiểu 8 ký tự. Để "" nếu muốn open

// ─── CHÂN PHẦN CỨNG ───────────────────────────────────────────────────────────
#define TRIGGER_PIN  4    // GPIO4 → GP8 Pico (xung trigger khởi động capture)
#define SPI_MOSI     23
#define SPI_MISO     19
#define SPI_SCK      18
#define SPI_CS       5

// ─── SPI ──────────────────────────────────────────────────────────────────────
#define SPI_FREQ     10000000   // 10 MHz — an toàn cho truyền RLE từ Pico
#define SPI_MARKER_START  0xAA // Byte đặc biệt Pico gửi để báo bắt đầu data
#define SPI_MARKER_END    0xBB // Byte đặc biệt Pico gửi để báo kết thúc data
#define RLE_BUFFER_SIZE   (200 * 1024) // 200KB buffer nhận RLE từ Pico

// ─── WebSocket ────────────────────────────────────────────────────────────────
WebSocketsServer webSocket(81);  // WebSocket chạy ở port 81
WiFiServer       httpServer(80); // HTTP server ở port 80 để serve file HTML

// ─── BUFFER ───────────────────────────────────────────────────────────────────
uint8_t rleBuffer[RLE_BUFFER_SIZE];
uint32_t rleLength = 0;

// ─── TRẠNG THÁI ───────────────────────────────────────────────────────────────
bool isCapturing   = false;
int  wsClientNum   = -1;   // Số thứ tự client WebSocket đang kết nối

// =============================================================================
// XỬ LÝ SỰ KIỆN WEBSOCKET
// =============================================================================
void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsClientNum = clientNum;
            Serial.printf("[WS] Client #%u kết nối từ %s\n", clientNum,
                          webSocket.remoteIP(clientNum).toString().c_str());
            // Gửi thông báo chào mừng
            webSocket.sendTXT(clientNum, "{\"type\":\"status\",\"msg\":\"connected\",\"device\":\"PicoScope_LA\"}");
            break;

        case WStype_DISCONNECTED:
            Serial.printf("[WS] Client #%u ngắt kết nối\n", clientNum);
            if (wsClientNum == clientNum) wsClientNum = -1;
            break;

        case WStype_TEXT:
            // Nhận lệnh từ HTML
            handleCommand(clientNum, (char*)payload);
            break;

        case WStype_BIN:
            // Không dùng binary từ client
            break;

        case WStype_ERROR:
            Serial.printf("[WS] Lỗi client #%u\n", clientNum);
            break;

        default:
            break;
    }
}

// =============================================================================
// XỬ LÝ LỆNH TỪ HTML
// =============================================================================
void handleCommand(uint8_t clientNum, const char* cmd) {
    Serial.printf("[CMD] Nhận lệnh: %s\n", cmd);

    if (strcmp(cmd, "START_CAPTURE") == 0) {
        if (isCapturing) {
            webSocket.sendTXT(clientNum, "{\"type\":\"error\",\"msg\":\"Đang capture, vui lòng chờ\"}");
            return;
        }
        // Bắt đầu capture
        startCapture(clientNum);
    }
    else if (strcmp(cmd, "STOP") == 0) {
        // Người dùng ấn Stop (chưa cần xử lý phức tạp)
        isCapturing = false;
        webSocket.sendTXT(clientNum, "{\"type\":\"status\",\"msg\":\"stopped\"}");
    }
    else if (strncmp(cmd, "SET_BAUD:", 9) == 0) {
        // Ví dụ: "SET_BAUD:9600" — để sau này mở rộng
        int baud = atoi(cmd + 9);
        Serial.printf("[CMD] Set baud = %d\n", baud);
        // TODO: lưu lại và gửi sang Pico nếu cần
    }
}

// =============================================================================
// THỰC HIỆN CAPTURE — Kéo trigger → nhận SPI từ Pico → gửi WebSocket
// =============================================================================
void startCapture(uint8_t clientNum) {
    isCapturing = true;
    rleLength   = 0;

    // Thông báo cho HTML biết đang bắt đầu
    webSocket.sendTXT(clientNum, "{\"type\":\"status\",\"msg\":\"capturing\"}");
    Serial.println("[CAPTURE] Kéo Trigger lên HIGH...");

    // 1. Kéo GPIO4 (Trigger) lên HIGH → Pico bắt sườn lên ở GP8 → bắt đầu capture
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(100);   // 100µs >> thời gian PIO cần (vài ns)

    // 2. Chờ Pico gửi marker bắt đầu (0xAA) qua SPI
    Serial.println("[CAPTURE] Chờ Pico gửi dữ liệu SPI...");
    SPI.beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(SPI_CS, LOW);

    // Chờ byte 0xAA (start marker) với timeout 6 giây
    bool gotStart = false;
    unsigned long t0 = millis();
    while (millis() - t0 < 6000) {
        uint8_t b = SPI.transfer(0x00);
        if (b == SPI_MARKER_START) {
            gotStart = true;
            break;
        }
    }

    if (!gotStart) {
        // Timeout — báo lỗi về HTML
        digitalWrite(SPI_CS, HIGH);
        SPI.endTransaction();
        digitalWrite(TRIGGER_PIN, LOW);
        isCapturing = false;
        webSocket.sendTXT(clientNum, "{\"type\":\"error\",\"msg\":\"Timeout: Pico không phản hồi. Kiểm tra dây SPI và Trigger.\"}");
        Serial.println("[CAPTURE] TIMEOUT chờ Pico!");
        return;
    }

    // 3. Kéo Trigger xuống LOW sau khi Pico đã bắt đầu gửi
    digitalWrite(TRIGGER_PIN, LOW);
    Serial.println("[CAPTURE] Nhận RLE từ Pico...");

    // 4. Đọc từng byte RLE cho đến khi gặp 0xBB (end marker)
    while (rleLength < RLE_BUFFER_SIZE) {
        uint8_t b = SPI.transfer(0x00);
        if (b == SPI_MARKER_END) break;  // Pico gửi xong
        rleBuffer[rleLength++] = b;
    }

    digitalWrite(SPI_CS, HIGH);
    SPI.endTransaction();

    Serial.printf("[CAPTURE] Nhận xong %lu byte RLE.\n", rleLength);

    // 5. Gửi cục RLE về HTML qua WebSocket (binary frame)
    // Prepend header JSON nhỏ, theo sau là binary data
    // HTML sẽ nhận theo 2 message: 1 TEXT header + 1 BIN data
    char header[128];
    snprintf(header, sizeof(header),
             "{\"type\":\"data_rle\",\"length\":%lu,\"sampleRate\":20000000,\"samples\":400000}",
             rleLength);
    webSocket.sendTXT(clientNum, header);

    // Gửi binary RLE data
    webSocket.sendBIN(clientNum, rleBuffer, rleLength);

    Serial.println("[CAPTURE] Đã gửi dữ liệu về HTML qua WebSocket.");
    isCapturing = false;
}

// =============================================================================
// HTTP SERVER — Phục vụ file HTML khi trình duyệt mở 192.168.4.1
// =============================================================================
void handleHttpClient(WiFiClient client) {
    // Đọc request
    String request = "";
    while (client.available()) {
        char c = client.read();
        request += c;
        if (request.endsWith("\r\n\r\n")) break;
    }

    // Chỉ serve "/" hoặc "/index.html"
    bool serveHtml = request.startsWith("GET / ") || request.startsWith("GET /index.html ");

    if (serveHtml) {
        // Đọc file HTML từ SPIFFS (flash của ESP32)
        if (SPIFFS.exists("/PicoScope_LA.html")) {
            File f = SPIFFS.open("/PicoScope_LA.html", "r");
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: text/html; charset=utf-8");
            client.println("Connection: close");
            client.println();
            // Stream file theo từng chunk để không tràn RAM
            uint8_t buf[512];
            while (f.available()) {
                int n = f.readBytes((char*)buf, sizeof(buf));
                client.write(buf, n);
            }
            f.close();
        } else {
            // File chưa được upload → báo lỗi
            client.println("HTTP/1.1 404 Not Found");
            client.println("Content-Type: text/html");
            client.println();
            client.println("<h2>PicoScope_LA.html chưa được upload lên SPIFFS!</h2>");
            client.println("<p>Dùng Arduino IDE: Tools → ESP32 Sketch Data Upload để upload thư mục /data/</p>");
        }
    } else {
        client.println("HTTP/1.1 404 Not Found");
        client.println();
    }
    client.stop();
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== PicoScope Logic Analyzer — ESP32 Firmware ===");

    // 1. Khởi động SPIFFS (để đọc file HTML từ flash)
    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] Lỗi! Không mount được SPIFFS.");
    } else {
        Serial.println("[SPIFFS] OK.");
    }

    // 2. Cấu hình GPIO Trigger
    pinMode(TRIGGER_PIN, OUTPUT);
    digitalWrite(TRIGGER_PIN, LOW);   // Mặc định LOW

    // 3. Khởi động SPI (ESP32 làm MASTER, Pico làm SLAVE)
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS);
    pinMode(SPI_CS, OUTPUT);
    digitalWrite(SPI_CS, HIGH);       // CS mặc định HIGH (inactive)
    Serial.println("[SPI] Master khởi động OK.");

    // 4. Tạo WiFi Access Point
    WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) > 0 ? AP_PASSWORD : nullptr);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WiFi] AP '%s' đã bật. IP: %s\n", AP_SSID, ip.toString().c_str());
    Serial.printf("[WiFi] Mở trình duyệt, truy cập: http://%s\n", ip.toString().c_str());

    // 5. Khởi động HTTP Server
    httpServer.begin();
    Serial.println("[HTTP] Server đang chạy ở port 80.");

    // 6. Khởi động WebSocket Server
    webSocket.begin();
    webSocket.onEvent(onWebSocketEvent);
    Serial.println("[WS] WebSocket Server đang chạy ở port 81.");

    Serial.println("\n=== Sẵn sàng! ===");
    Serial.println("1. Kết nối WiFi: " + String(AP_SSID));
    Serial.println("2. Mở trình duyệt: http://" + ip.toString());
    Serial.println("3. Nhấn 'Start Capture' trên giao diện web.\n");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    // Xử lý WebSocket events
    webSocket.loop();

    // Xử lý HTTP client (serve file HTML)
    WiFiClient client = httpServer.available();
    if (client) {
        handleHttpClient(client);
    }
}
