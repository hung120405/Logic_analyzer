/*
 * =============================================================================
 * main.c — ESP32 Firmware (ESP-IDF v5.x) cho PicoScope Logic Analyzer
 * =============================================================================
 *
 * Kiến trúc:
 *   ┌─────────────┐  WebSocket   ┌─────────────────────────┐
 *   │  Trình duyệt │ ◄──────────► │      ESP32 (file này)   │
 *   │  (HTML/JS)   │             │                         │
 *   └─────────────┘             │  WiFi AP: PicoScope_WiFi │
 *                               │  HTTP :80  → serve HTML  │
 *                               │  WS   :80/ws → data/cmd  │
 *                               │  SPI Master → Pico 2     │
 *                               │  GPIO4 Trigger → GP8     │
 *                               └─────────────────────────┘
 *                                         │ SPI
 *                               ┌─────────▼──────────┐
 *                               │       Pico 2        │
 *                               │  (20MHz capture)    │
 *                               └─────────────────────┘
 *
 * Luồng hoạt động:
 *   1. ESP32 bật WiFi AP "PicoScope_WiFi"
 *   2. ESP32 bật HTTP + WebSocket server ở port 80
 *   3. Người dùng kết nối WiFi → mở http://192.168.4.1
 *   4. ESP32 serve file PicoScope_LA.html từ SPIFFS
 *   5. HTML kết nối WebSocket ws://192.168.4.1/ws
 *   6. Người dùng bấm "Start Capture" → HTML gửi "START_CAPTURE"
 *   7. ESP32 nhận → kéo GPIO4 HIGH → Pico bắt trigger → capture 20ms
 *   8. Pico nén RLE → gửi qua SPI → ESP32 nhận → gửi WebSocket → HTML vẽ
 *
 * Phần cứng:
 *   GPIO 4  → GP8 Pico   (Trigger, dây thẳng)
 *   GPIO 18 → Pico SPI SCK  (qua IC 74LVC8T245)  ← chú ý: đây là SPI DATA
 *   GPIO 19 → Pico SPI MISO (nhận RLE data từ Pico)
 *   GPIO 23 → Pico SPI MOSI
 *   GPIO 5  → Pico SPI CS
 * =============================================================================
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_mac.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "PicoScope";

/* ─── Cấu hình WiFi AP ──────────────────────────────────────────────────── */
#define WIFI_SSID       "PicoScope_WiFi"
#define WIFI_PASS       "picoscope123"
#define WIFI_CHANNEL    6
#define WIFI_MAX_STA    4

/* ─── Chân GPIO ─────────────────────────────────────────────────────────── */
#define TRIGGER_GPIO    4

/* ─── Cấu hình SPI (ESP32 = Master, Pico = Slave) ──────────────────────── */
#define SPI_HOST        SPI2_HOST   // VSPI
#define PIN_SCK         18
#define PIN_MOSI        23
#define PIN_MISO        19
#define PIN_CS          5
#define SPI_FREQ_HZ     (5 * 1000 * 1000)  // 5MHz — an toàn và ổn định

/* ─── Giao thức truyền với Pico ─────────────────────────────────────────── */
#define MARKER_START    0xAA    // Pico gửi byte này trước khi gửi RLE
#define MARKER_END      0xBB    // Pico gửi byte này sau khi gửi hết RLE

/* ─── Buffer ────────────────────────────────────────────────────────────── */
#define RLE_BUF_SIZE    (120 * 1024)    // 120KB — cấp phát động trên Heap
static uint8_t *s_rle_buf = NULL;
static size_t  s_rle_len = 0;

/* ─── Trạng thái WebSocket ─────────────────────────────────────────────── */
static httpd_handle_t s_server = NULL;
static int            s_ws_fd  = -1;

/* ─── SPI Device handle ───────────────────────────────────────────────────── */
static spi_device_handle_t s_spi_dev;

/* ==========================================================================
 * TEST SIGNAL GENERATOR — Phát I2C + UART + GPIO giả lập trong 20ms capture
 * Các chân này độc lập hoàn toàn với SPI data link (GPIO18/19/23/5)
 * ========================================================================== */
#define TST_I2C_SCL   22   /* → IC A1 → B1 → Pico GP0 = CH0 */
#define TST_I2C_SDA   21   /* → IC A2 → B2 → Pico GP1 = CH1 */
#define TST_UART_TX   17   /* → IC A3 → B3 → Pico GP2 = CH2 */
#define TST_UART_RX   16   /* → IC A8 → B8 → Pico GP7 = CH7 */

/* Khởi tạo 4 chân GPIO test như output */
static void test_gpio_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<TST_I2C_SCL)|(1ULL<<TST_I2C_SDA)|
                        (1ULL<<TST_UART_TX)|(1ULL<<TST_UART_RX),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    /* Idle: I2C = HIGH, UART = HIGH (idle mark) */
    gpio_set_level(TST_I2C_SCL, 1);
    gpio_set_level(TST_I2C_SDA, 1);
    gpio_set_level(TST_UART_TX, 1);
    gpio_set_level(TST_UART_RX, 1);
}

/* Phát 1 byte UART 8N1 @ 115200 bâud bằng bit-bang */
static void uart_bb_byte(int pin, uint8_t byte) {
    const int bit_us = 9;   /* 1 / 115200 ≈ 8.7µs → 9µs */
    gpio_set_level(pin, 0); /* Start bit */
    esp_rom_delay_us(bit_us);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(pin, (byte >> i) & 1);
        esp_rom_delay_us(bit_us);
    }
    gpio_set_level(pin, 1); /* Stop bit */
    esp_rom_delay_us(bit_us);
}

/* Phát 1 cặp xung I2C (SCL clock, SDA data) */
static void i2c_bb_bit(int scl, int sda, int val) {
    gpio_set_level(sda, val);
    esp_rom_delay_us(10);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(20);
    gpio_set_level(scl, 0);
    esp_rom_delay_us(10);
}

/* Phát một giao dịch I2C rút gọn: START + địa chỉ + 1 byte + STOP */
static void i2c_bb_transaction(int scl, int sda, uint8_t addr, uint8_t data) {
    /* START: SDA ↓ khi SCL HIGH */
    gpio_set_level(sda, 1); gpio_set_level(scl, 1); esp_rom_delay_us(10);
    gpio_set_level(sda, 0); esp_rom_delay_us(10);
    gpio_set_level(scl, 0); esp_rom_delay_us(10);
    /* Địa chỉ 7 bit + W(0) */
    uint8_t addr_byte = (addr << 1) & 0xFE;
    for (int i = 7; i >= 0; i--) i2c_bb_bit(scl, sda, (addr_byte >> i) & 1);
    /* ACK (giả định slave ACK, SDA LOW) */
    gpio_set_level(sda, 0); gpio_set_level(scl, 1); esp_rom_delay_us(20);
    gpio_set_level(scl, 0); esp_rom_delay_us(10);
    /* Data byte */
    for (int i = 7; i >= 0; i--) i2c_bb_bit(scl, sda, (data >> i) & 1);
    /* ACK */
    gpio_set_level(sda, 0); gpio_set_level(scl, 1); esp_rom_delay_us(20);
    gpio_set_level(scl, 0); esp_rom_delay_us(10);
    /* STOP: SDA ↑ khi SCL HIGH */
    gpio_set_level(sda, 0); gpio_set_level(scl, 1); esp_rom_delay_us(10);
    gpio_set_level(sda, 1); esp_rom_delay_us(20);
}

/* Task phát tín hiệu test trong 18ms, rồi tự xóa */
static void test_signal_task(void *arg) {
    ESP_LOGI("TEST", "Bắt đầu phát tín hiệu test (18ms)...");
    /* UART TX: gửi chuỗi "Hi!" */
    const char *msg = "Hi!";
    for (int k = 0; msg[k]; k++) uart_bb_byte(TST_UART_TX, (uint8_t)msg[k]);
    esp_rom_delay_us(500);
    /* UART RX giả: gửi "OK" */
    const char *ack = "OK";
    for (int k = 0; ack[k]; k++) uart_bb_byte(TST_UART_RX, (uint8_t)ack[k]);
    esp_rom_delay_us(500);
    /* I2C: 3 giao dịch với địa chỉ 0x48, dữ liệu khác nhau */
    i2c_bb_transaction(TST_I2C_SCL, TST_I2C_SDA, 0x48, 0xAB);
    esp_rom_delay_us(200);
    i2c_bb_transaction(TST_I2C_SCL, TST_I2C_SDA, 0x48, 0xCD);
    esp_rom_delay_us(200);
    i2c_bb_transaction(TST_I2C_SCL, TST_I2C_SDA, 0x48, 0xEF);
    /* Reset về idle */
    gpio_set_level(TST_I2C_SCL, 1); gpio_set_level(TST_I2C_SDA, 1);
    gpio_set_level(TST_UART_TX, 1); gpio_set_level(TST_UART_RX, 1);
    ESP_LOGI("TEST", "Phát xong tín hiệu test.");
    vTaskDelete(NULL);
}

/* =========================================================================
 * WIFI — Khởi động Access Point
 * ========================================================================= */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Client kết nối: MAC " MACSTR, MAC2STR(ev->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "Client ngắt kết nối: MAC " MACSTR, MAC2STR(ev->mac));
    }
}

static void wifi_ap_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid           = WIFI_SSID,
            .password       = WIFI_PASS,
            .channel        = WIFI_CHANNEL,
            .max_connection = WIFI_MAX_STA,
            .authmode       = strlen(WIFI_PASS) > 0 ? WIFI_AUTH_WPA2_PSK
                                                    : WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, WIFI_SSID, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, WIFI_PASS, sizeof(wifi_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP '%s' đã bật. IP mặc định: 192.168.4.1", WIFI_SSID);
}

/* =========================================================================
 * SPIFFS — Mount partition để đọc file HTML
 * ========================================================================= */
static void spiffs_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "spiffs",
        .max_files              = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount thất bại: %s", esp_err_to_name(ret));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS OK — Tổng: %u KB, Đã dùng: %u KB",
             (unsigned)(total/1024), (unsigned)(used/1024));
}

/* =========================================================================
 * SPI — Khởi động SPI Master
 * ========================================================================= */
static void spi_master_init(void) {
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = RLE_BUF_SIZE + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .mode           = 0,                // CPOL=0, CPHA=0
        .clock_speed_hz = SPI_FREQ_HZ,
        .spics_io_num   = PIN_CS,
        .queue_size     = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST, &dev_cfg, &s_spi_dev));
    ESP_LOGI(TAG, "SPI Master OK — SCK=%d MOSI=%d MISO=%d CS=%d @ %dMHz",
             PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS, SPI_FREQ_HZ/1000000);
}

/* =========================================================================
 * SPI — Nhận toàn bộ RLE data từ Pico 2
 * ========================================================================= */
static esp_err_t spi_receive_rle(size_t *out_len) {
    *out_len = 0;
    ESP_LOGI(TAG, "Kéo Trigger (GPIO%d) lên HIGH...", TRIGGER_GPIO);
    gpio_set_level(TRIGGER_GPIO, 1);

    /* ─── Phát tín hiệu test INLINE trong 15ms (trong cửa sổ capture của Pico) ───
     * KHÔNG dùng task riêng vì FreeRTOS scheduler không đảm bảo timing.
     * Pico capture 20ms → còn 5ms dự phòng trước khi bắt đầu SPI receive.
     * Lặp phát UART + I2C liên tục cho đến khi đủ 15ms. */
    ESP_LOGI(TAG, "[TEST] Phát tín hiệu test 15ms...");
    int64_t t_start = esp_timer_get_time();
    int burst = 0;
    while (esp_timer_get_time() - t_start < 15000) {  /* 15ms */
        /* UART TX: gửi 'A','B','C' */
        uart_bb_byte(TST_UART_TX, 'A' + (burst % 3));
        /* UART RX giả: gửi 'X' */
        uart_bb_byte(TST_UART_RX, 'X');
        /* I2C transaction: addr=0x48, data thay đổi mỗi burst */
        i2c_bb_transaction(TST_I2C_SCL, TST_I2C_SDA, 0x48, (uint8_t)(0xA0 + burst));
        burst++;
    }
    gpio_set_level(TST_I2C_SCL, 1); gpio_set_level(TST_I2C_SDA, 1);
    gpio_set_level(TST_UART_TX, 1); gpio_set_level(TST_UART_RX, 1);
    ESP_LOGI(TAG, "[TEST] Xong. %d burst trong 15ms.", burst);

    /* ─── Chờ byte MARKER_START (0xAA) ─── */
    ESP_LOGI(TAG, "Chờ Pico gửi marker 0xAA qua SPI...");
    bool got_start = false;
    uint32_t timeout_ms = 6000;
    uint32_t elapsed    = 0;

    while (elapsed < timeout_ms) {
        uint8_t rx = 0;
        spi_transaction_t t = {
            .length    = 8,
            .rx_buffer = &rx,
        };
        ESP_ERROR_CHECK(spi_device_transmit(s_spi_dev, &t));

        if (rx == MARKER_START) {
            got_start = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }

    gpio_set_level(TRIGGER_GPIO, 0);    // Kéo Trigger xuống

    if (!got_start) {
        ESP_LOGE(TAG, "TIMEOUT %lu ms — Không nhận được 0xAA từ Pico!", timeout_ms);
        ESP_LOGE(TAG, "Kiểm tra: 1) Dây GPIO4→GP8  2) Dây SPI  3) GND chung");
        return ESP_ERR_TIMEOUT;
    }

    /* ─── Đọc từng byte RLE đến khi gặp MARKER_END (0xBB) ─── */
    ESP_LOGI(TAG, "Nhận RLE data từ Pico...");
    size_t idx = 0;
    while (idx < RLE_BUF_SIZE) {
        uint8_t rx = 0;
        spi_transaction_t t = {
            .length    = 8,
            .rx_buffer = &rx,
        };
        ESP_ERROR_CHECK(spi_device_transmit(s_spi_dev, &t));

        if (rx == MARKER_END) break;
        s_rle_buf[idx++] = rx;
    }

    *out_len = idx;
    ESP_LOGI(TAG, "Nhận xong: %u byte RLE.", (unsigned)idx);
    return ESP_OK;
}

/* =========================================================================
 * WebSocket — Gửi frame đồng bộ từ trong ws_handler (cách chính thức)
 * Ghi chú: httpd_ws_send_frame() chỉ được gọi từ trong handler (cùng httpd task)
 * ========================================================================= */
static esp_err_t ws_send_sync(httpd_req_t *req, const char *text) {
    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len     = strlen(text),
        .fragmented = false,
        .final   = true,
    };
    return httpd_ws_send_frame(req, &frame);
}

/* =========================================================================
 * HTTP Handler — Serve file HTML từ SPIFFS
 * ========================================================================= */
static esp_err_t html_handler(httpd_req_t *req) {
    const char *path = "/spiffs/PicoScope_LA.html";
    FILE *f = fopen(path, "r");

    if (!f) {
        ESP_LOGE(TAG, "Không tìm thấy %s. Hãy chạy 'idf.py flash' để upload SPIFFS.", path);
        const char *err = "<h2>Lỗi: PicoScope_LA.html chưa được flash vào SPIFFS!</h2>"
                          "<p>Chạy: <code>idf.py -p /dev/ttyUSB0 flash</code></p>";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, err, strlen(err));
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* Stream file theo chunk 4KB để không tràn RAM */
    static char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
            ESP_LOGE(TAG, "Lỗi khi gửi chunk HTML.");
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // Kết thúc chunked response
    return ESP_OK;
}

/* =========================================================================
 * WebSocket Handler — Nhận lệnh từ HTML
 * ========================================================================= */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        s_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "[WS] Client kết nối (fd=%d)", s_ws_fd);
        return ESP_OK;
    }

    /* Nhận frame từ client */
    httpd_ws_frame_t rx = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &rx, 0);
    if (ret != ESP_OK || rx.len == 0) return ret;

    uint8_t *buf = calloc(1, rx.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    rx.payload = buf;
    ret = httpd_ws_recv_frame(req, &rx, rx.len);
    if (ret != ESP_OK) { free(buf); return ret; }

    buf[rx.len] = '\0';
    ESP_LOGI(TAG, "[WS] Nhận lệnh: %s", buf);

    if (strcmp((char *)buf, "START_CAPTURE") == 0) {
        /* Gửi ACK ngay (vẫn trong httpd context) */
        ws_send_sync(req, "{\"type\":\"status\",\"msg\":\"capturing\"}");

        ESP_LOGI(TAG, "[CAPTURE] Bắt đầu...");

        /* Thực hiện capture SPI ngay trong handler (~40ms, chấp nhận được) */
        esp_err_t spi_ret = spi_receive_rle(&s_rle_len);

        if (spi_ret != ESP_OK) {
            ws_send_sync(req, "{\"type\":\"error\",\"msg\":\"Timeout: Pico không phản hồi. Kiểm tra dây SPI và Trigger GPIO4->GP8.\"}");
            free(buf);
            return ESP_OK;
        }

        /* Xây dựng JSON hex để gửi */
        size_t hex_sz = s_rle_len * 2 + 128;
        char *hbuf = malloc(hex_sz);
        if (!hbuf) { 
            ws_send_sync(req, "{\"type\":\"error\",\"msg\":\"OOM\"}"); 
            free(buf);
            return ESP_OK; 
        }

        int pfx = snprintf(hbuf, hex_sz,
            "{\"type\":\"data_hex\",\"samples\":400000,\"sampleRate\":20000000,\"hex\":\"");
        for (size_t i = 0; i < s_rle_len; i++) {
            hbuf[pfx + i*2]     = "0123456789abcdef"[s_rle_buf[i] >> 4];
            hbuf[pfx + i*2 + 1] = "0123456789abcdef"[s_rle_buf[i] & 0xf];
        }
        hbuf[pfx + s_rle_len*2]     = '"';
        hbuf[pfx + s_rle_len*2 + 1] = '}';
        hbuf[pfx + s_rle_len*2 + 2] = '\0';

        esp_err_t send_ret = ws_send_sync(req, hbuf);
        free(hbuf);

        if (send_ret == ESP_OK) {
            ESP_LOGI(TAG, "[CAPTURE] Đã gửi %u byte RLE về browser.", (unsigned)s_rle_len);
        } else {
            ESP_LOGE(TAG, "[CAPTURE] Gửi thất bại: %s", esp_err_to_name(send_ret));
        }
        ws_send_sync(req, "{\"type\":\"status\",\"msg\":\"idle\"}");
    }

    free(buf);
    return ESP_OK;
}

/* =========================================================================
 * HTTP Server — Khởi động với handler HTML và WebSocket
 * ========================================================================= */
static void http_server_start(void) {
    httpd_config_t config      = HTTPD_DEFAULT_CONFIG();
    config.server_port         = 80;
    config.max_open_sockets    = 7;
    config.lru_purge_enable    = true;
    config.stack_size          = 8192; // Tăng stack để ws_handler có thể xử lý capture inline

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    /* URI /  → trả file HTML */
    httpd_uri_t uri_html = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = html_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &uri_html);

    /* URI /ws → WebSocket */
    httpd_uri_t uri_ws = {
        .uri            = "/ws",
        .method         = HTTP_GET,
        .handler        = ws_handler,
        .user_ctx       = NULL,
        .is_websocket   = true,
    };
    httpd_register_uri_handler(s_server, &uri_ws);

    ESP_LOGI(TAG, "HTTP Server OK — port 80");
    ESP_LOGI(TAG, "WebSocket OK   — ws://192.168.4.1/ws");
}

/* =========================================================================
 * GPIO — Khởi động chân Trigger
 * ========================================================================= */
static void trigger_gpio_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(TRIGGER_GPIO, 0);  // Mặc định LOW
    ESP_LOGI(TAG, "Trigger GPIO%d OK (mặc định LOW)", TRIGGER_GPIO);
}

/* =========================================================================
 * app_main
 * ========================================================================= */
void app_main(void) {
    ESP_LOGI(TAG, "=== PicoScope Logic Analyzer — ESP32 (ESP-IDF v5.x) ===");

    /* 1. Cấp phát buffer RLE động trên heap (120KB) để tránh tràn DRAM */
    s_rle_buf = (uint8_t *)malloc(RLE_BUF_SIZE);
    if (!s_rle_buf) {
        ESP_LOGE(TAG, "KHÔNG ĐỦ RAM để cấp phát 120KB cho RLE Buffer!");
        return;
    }

    /* 2. NVS (bắt buộc trước WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. GPIO Trigger */
    trigger_gpio_init();

    /* 3. SPI Master */
    spi_master_init();

    /* 4. SPIFFS */
    spiffs_init();

    /* 5. WiFi AP */
    wifi_ap_init();

    /* 6. Khởi tạo chân phát tín hiệu test (GPIO21/22/16/17) */
    test_gpio_init();

    /* 7. HTTP + WebSocket Server */
    http_server_start();

    /* ─── In hướng dẫn ─── */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Hệ thống sẵn sàng!");
    ESP_LOGI(TAG, "  1. Kết nối WiFi: %s", WIFI_SSID);
    ESP_LOGI(TAG, "  2. Mở trình duyệt: http://192.168.4.1");
    ESP_LOGI(TAG, "  3. Nhấn 'Start Capture' trên giao diện");
    ESP_LOGI(TAG, "========================================");
}
