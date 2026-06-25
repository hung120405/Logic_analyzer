#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/spi.h"
#include "logic_analyzer.pio.h"

// =============================================================================
// CẤU HÌNH THÔNG SỐ ĐO LƯỜNG
// =============================================================================
// 8 kênh ĐO (qua IC SN74LVC8T245, A→B direction):
//   GP0 (CH0) = I2C SCL   ← ESP32 GPIO22 → IC A1 → IC B1 → GP0
//   GP1 (CH1) = I2C SDA   ← ESP32 GPIO21 → IC A2 → IC B2 → GP1
//   GP2 (CH2) = UART2 TX  ← ESP32 GPIO17 → IC A3 → IC B3 → GP2
//   GP3 (CH3) = SPI MOSI  ← ESP32 GPIO23 → IC A4 → IC B4 → GP3
//   GP4 (CH4) = SPI MISO  ← ESP32 GPIO19 → IC A5 → IC B5 → GP4
//   GP5 (CH5) = SPI SCK   ← ESP32 GPIO18 → IC A6 → IC B6 → GP5
//   GP6 (CH6) = SPI CS    ← ESP32 GPIO5  → IC A7 → IC B7 → GP6
//   GP7 (CH7) = UART2 RX  ← ESP32 GPIO16 → IC A8 → IC B8 → GP7
//
// SPI DATA LINK (dây thẳng trực tiếp, không qua IC):
//   GP16 ← ESP32 GPIO23 (MOSI→Pico RX)   [Pico GP16 = SPI0_RX]
//   GP17 ← ESP32 GPIO5  (CS)              [Pico GP17 = SPI0_CSn]
//   GP18 ← ESP32 GPIO18 (SCK)             [Pico GP18 = SPI0_SCK]
//   GP19 → ESP32 GPIO19 (MISO←Pico TX)   [Pico GP19 = SPI0_TX]
//   GP8  ← ESP32 GPIO4  (TRIGGER, dây thẳng)
#define CAPTURE_PIN_BASE 0           // GP0 là chân đo đầu tiên
#define TRIGGER_PIN      8           // GP8: chân Trigger riêng biệt
#define CAPTURE_SAMPLES  400000      // 400K mẫu = 20ms @ 20MHz (dùng 400KB/520KB RAM)
#define TRIGGER_TIMEOUT_MS 5000      // Timeout chờ trigger: 5 giây

// =============================================================================
// CHẾ ĐỘ HOẠT ĐỘNG
// =============================================================================
#define MODE_USB  1
#define MODE_WIFI 2

// ⚠️ Đổi thành MODE_WIFI khi mang đi bảo vệ
int current_mode = MODE_WIFI;

// --- CẤU HÌNH CHÂN SPI NỐI SANG ESP32 (khi dùng MODE_WIFI) ---
#define SPI_PORT spi0
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19

// Bộ đệm capture: 400K mẫu 8-bit, đóng gói thành uint32_t (4 mẫu/word)
uint32_t capture_buffer[CAPTURE_SAMPLES / 4];
int dma_chan;

// =============================================================================
// HÀM NÉN RLE VÀ GỬI DỮ LIỆU
// =============================================================================
void compress_and_send_data() {
    if (current_mode == MODE_USB) {
        printf("START_DATA_RLE\n");
    } else {
        uint8_t start_marker = 0xAA;
        spi_write_blocking(SPI_PORT, &start_marker, 1);
    }

    uint8_t *raw_data = (uint8_t *)capture_buffer;
    uint8_t current_val = raw_data[0];
    uint32_t run_count = 1;

    for (uint32_t i = 1; i < CAPTURE_SAMPLES; i++) {
        if (raw_data[i] == current_val && run_count < 65535) {
            run_count++;
        } else {
            if (current_mode == MODE_USB) {
                fwrite(&current_val, 1, 1, stdout);
                fwrite(&run_count, 2, 1, stdout);
            } else {
                spi_write_blocking(SPI_PORT, &current_val, 1);
                uint8_t count_bytes[2] = {run_count & 0xFF, (run_count >> 8) & 0xFF};
                spi_write_blocking(SPI_PORT, count_bytes, 2);
            }
            current_val = raw_data[i];
            run_count = 1;
        }
    }

    // Gói cuối
    if (current_mode == MODE_USB) {
        fwrite(&current_val, 1, 1, stdout);
        fwrite(&run_count, 2, 1, stdout);
        printf("\nEND_DATA_RLE\n");
    } else {
        spi_write_blocking(SPI_PORT, &current_val, 1);
        uint8_t count_bytes[2] = {run_count & 0xFF, (run_count >> 8) & 0xFF};
        spi_write_blocking(SPI_PORT, count_bytes, 2);
        uint8_t end_marker = 0xBB;
        spi_write_blocking(SPI_PORT, &end_marker, 1);
    }
}

// =============================================================================
// HÀM CAPTURE CÓ TIMEOUT — Thay thế dma_channel_wait_for_finish_blocking()
// Lý do: Hàm blocking sẽ treo CPU vĩnh viễn nếu Trigger không đến.
//        Hàm này cho phép CPU thoát sau TRIGGER_TIMEOUT_MS và báo lỗi rõ ràng.
// =============================================================================
bool capture_with_timeout() {
    // Xóa FIFO cũ, kích hoạt DMA
    pio_sm_clear_fifos(pio0, 1); // SM1 là data capture
    dma_channel_set_write_addr(dma_chan, capture_buffer, true);

    printf("Dang cho xung Trigger tren GP8... (timeout %d giay)\n",
           TRIGGER_TIMEOUT_MS / 1000);

    // Polling loop: CPU không bị treo, luôn kiểm tra timeout
    uint32_t start_ms = to_ms_since_boot(get_absolute_time());

    while (dma_channel_is_busy(dma_chan)) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_ms;

        if (elapsed > TRIGGER_TIMEOUT_MS) {
            // HẾT TIMEOUT: hủy DMA, báo lỗi, trả về false để vòng lặp ngoài xử lý
            dma_channel_abort(dma_chan);
            printf("[TIMEOUT] Khong nhan duoc Trigger sau %lu ms!\n", elapsed);
            printf("  Kiem tra:\n");
            printf("  1. Day noi: ESP32 GPIO4 -> Pico GP8\n");
            printf("  2. ESP32 co dang chay va phat xung trigger (GPIO4 HIGH) khong?\n");
            printf("  3. GND chung giua ESP32 va Pico?\n");
            return false;
        }

        tight_loop_contents(); // Giữ CPU hoạt động, tránh WDT
    }

    return true; // Capture thành công
}

// =============================================================================
// HÀM CHẨN ĐOÁN
// =============================================================================
void print_diagnostic(uint8_t *data, uint32_t len) {
    printf("\n=== CHAN DOAN: 32 BYTE DAU TIEN (HEX) ===\n");
    printf("(0x00=GND, 0xFF=3.3V, gia tri khac=co xung)\n");
    uint32_t show = len > 32 ? 32 : len;
    for (uint32_t i = 0; i < show; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");

    uint32_t nonzero = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (data[i] != 0x00 && data[i] != 0xFF) nonzero++;
    }
    printf("So byte co xung (khac 0x00 va 0xFF): %lu / %lu\n", nonzero, len);
    printf("=========================================\n\n");
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    stdio_init_all();
    sleep_ms(2000); // Chờ USB ổn định

    // --- KHỞI TẠO SPI (chỉ trong MODE_WIFI, TRƯỚC khi PIO để tránh xung đột pin) ---
    // QUAN TRỌNG: Đấu dây trực tiếp ESP32 → Pico (không qua IC):
    //   ESP32 GPIO18 (SCK)  → Pico GP18 (SPI0_SCK)
    //   ESP32 GPIO23 (MOSI) → Pico GP16 (SPI0_RX — Pico nhận từ Master)
    //   ESP32 GPIO19 (MISO) → Pico GP19 (SPI0_TX — Pico gửi về Master)
    //   ESP32 GPIO5  (CS)   → Pico GP17 (SPI0_CSn)
    //   ESP32 GPIO4  (TRIG) → Pico GP8  (dây thẳng)
    if (current_mode == MODE_WIFI) {
        spi_init(SPI_PORT, 10000000); // 10MHz (bị bỏ qua trong slave mode, master quyết định)
        spi_set_slave(SPI_PORT, true); // ← QUAN TRỌNG: Pico là SLAVE, ESP32 là MASTER
        gpio_set_function(PIN_MISO, GPIO_FUNC_SPI); // GP16 = SPI0_RX
        gpio_set_function(PIN_CS,   GPIO_FUNC_SPI); // GP17 = SPI0_CSn
        gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI); // GP18 = SPI0_SCK
        gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI); // GP19 = SPI0_TX
        printf("[WiFi] SPI Slave khoi tao xong tren GP16-GP19.\n");
        printf("[WiFi] Cho Trigger tu ESP32 (GPIO4 -> GP8)...\n");
    } else {
        // MODE_USB: Chờ lệnh 'c' từ terminal
        while (true) {
            printf("=== LOGIC ANALYZER 20 MHz — 8 KENH + HARDWARE TRIGGER ===\n");
            printf("Che do: USB (PuTTY/Serial Monitor)\n");
            printf("Nhan 'c' de bat dau do...\n\n");
            int cmd = getchar_timeout_us(2000000);
            if (cmd == 'c') {
                printf("OK! Chuyen sang che do do...\n");
                break;
            }
        }
    }

    // ==========================================================================
    // KHỞI TẠO 2 STATE MACHINE PIO (cả 2 chế độ đều cần)
    // ==========================================================================
    PIO pio = pio0;
    float clk_div = 150000000.0f / 20000000.0f; // 150MHz / 20MHz = 7.5

    uint offset0 = pio_add_program(pio, &trigger_watcher_program);
    trigger_watcher_program_init(pio, 0, offset0, TRIGGER_PIN);

    uint offset1 = pio_add_program(pio, &logic_analyzer_program);
    logic_analyzer_program_init(pio, 1, offset1, CAPTURE_PIN_BASE, clk_div);

    // ==========================================================================
    // KHỞI TẠO DMA — đọc từ FIFO của SM1 (rxf[1])
    // ==========================================================================
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dma_cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, false);
    channel_config_set_write_increment(&dma_cfg, true);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio, 1, false)); // SM1!

    dma_channel_configure(dma_chan, &dma_cfg,
                          capture_buffer,
                          &pio->rxf[1],
                          CAPTURE_SAMPLES / 4,
                          false); // Chưa bắt đầu

    printf("PIO + DMA khoi tao xong. San sang capture.\n");

    // ==========================================================================
    // VÒNG LẶP CHÍNH
    // ==========================================================================
    while (true) {
        if (current_mode == MODE_USB) {
            // USB mode: chờ lệnh 'c' từ terminal trước mỗi lần capture
            printf("\nNhan 'c' de bat dau mot lan capture moi...\n");
            int c = getchar_timeout_us(10000000);
            if (c != 'c') continue;
            printf("Bat dau! Kich hoat DMA, chay SM0+SM1...\n");
        } else {
            // WiFi mode: tự động chờ Trigger phần cứng từ ESP32
            printf("\n[WiFi] San sang. Cho Trigger (GPIO4 HIGH) tu ESP32...\n");
        }

        // Capture với timeout — CPU không bị treo
        bool ok = capture_with_timeout();

        if (!ok) {
            pio_sm_restart(pio, 0);
            pio_sm_restart(pio, 1);
            pio_sm_exec(pio, 1, pio_encode_jmp(offset1));
            printf("Da reset. Thu lai...\n");
            continue;
        }

        printf("Chup xong %d mau!\n", CAPTURE_SAMPLES);

        // Thống kê nhanh
        uint8_t *raw = (uint8_t *)capture_buffer;
        uint32_t transitions = 0;
        for (uint32_t i = 1; i < CAPTURE_SAMPLES; i++) {
            if (raw[i] != raw[i-1]) transitions++;
        }
        printf("  8 byte dau (HEX): ");
        for (int i = 0; i < 8; i++) printf("%02X ", raw[i]);
        printf("\n  So lan doi tin hieu: %lu / %d mau\n", transitions, CAPTURE_SAMPLES);
        if (transitions == 0)
            printf("  => CANH BAO: Tin hieu phang!\n");
        else
            printf("  => CO TIN HIEU! Dang nen va gui...\n");

        compress_and_send_data();

        // Reset SM1 về trạng thái chờ trigger cho lần tiếp theo
        pio_sm_exec(pio, 1, pio_encode_jmp(offset1));
    }

    return 0;
}
