#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/spi.h"       // BẮT BUỘC THÊM: Thư viện điều khiển SPI
#include "logic_analyzer.pio.h"

// --- CẤU HÌNH THÔNG SỐ ĐO LƯỜNG ---
// Chân đầu tiên: GP0 (B8→GP0, B7→GP1, ..., B1→GP7)
// Tương ứng IC: A8→B8→GP0 ... A1→B1→GP7 | Arduino: Pin9→GP0 ... Pin2→GP7
#define CAPTURE_PIN_BASE 0
#define CAPTURE_SAMPLES 400000  // 400K mẫu = 16ms tại 25MHz (an toàn RAM: 400KB/520KB)

// --- CHẾ ĐỘ HOẠT ĐỘNG (DUAL-MODE) ---
#define MODE_USB 1
#define MODE_WIFI 2

// ⚠️ ĐỔI SỐ NÀY THÀNH MODE_WIFI (2) KHI NÀO MANG ĐI BẢO VỆ DÙNG ESP32
// 🔧 HIỆN TẠI: Dùng MODE_USB để test qua PuTTY. Khi bảo vệ thì đổi lại MODE_WIFI.
int current_mode = MODE_USB;

// --- CẤU HÌNH CHÂN SPI NỐI SANG ESP32 ---
#define SPI_PORT spi0
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19

// Bộ đệm RAM 100KB
uint32_t capture_buffer[CAPTURE_SAMPLES / 4]; 
int dma_chan;

// --- HÀM XỬ LÝ: VỪA NÉN RLE VỪA ĐIỀU HƯỚNG DỮ LIỆU ---
void compress_and_send_data() {
    // 1. Gửi cờ báo hiệu bắt đầu
    if (current_mode == MODE_USB) {
        printf("START_DATA_RLE\n");
    } else {
        // Gửi 1 byte đặc biệt (0xAA) qua SPI để đánh thức ESP32
        uint8_t start_marker = 0xAA; 
        spi_write_blocking(SPI_PORT, &start_marker, 1);
    }

    uint8_t *raw_data = (uint8_t *)capture_buffer; 
    uint8_t current_val = raw_data[0];
    uint32_t run_count = 1;
    
    // 2. Thuật toán nén RLE
    for (uint32_t i = 1; i < CAPTURE_SAMPLES; i++) {
        if (raw_data[i] == current_val && run_count < 65535) {
            run_count++; 
        } else {
            // Đã tìm thấy sự thay đổi tín hiệu -> Phân luồng gửi đi
            if (current_mode == MODE_USB) {
                // Đẩy thẳng ra USB
                fwrite(&current_val, 1, 1, stdout);
                fwrite(&run_count, 2, 1, stdout);
            } else {
                // Đẩy qua dây SPI sang ESP32
                spi_write_blocking(SPI_PORT, &current_val, 1);
                uint8_t count_bytes[2] = {run_count & 0xFF, (run_count >> 8) & 0xFF};
                spi_write_blocking(SPI_PORT, count_bytes, 2);
            }
            
            // Đặt lại biến đếm
            current_val = raw_data[i];
            run_count = 1;
        }
    }
    
    // 3. Gửi gói cuối cùng và cờ kết thúc
    if (current_mode == MODE_USB) {
        fwrite(&current_val, 1, 1, stdout);
        fwrite(&run_count, 2, 1, stdout);
        printf("\nEND_DATA_RLE\n");
    } else {
        spi_write_blocking(SPI_PORT, &current_val, 1);
        uint8_t count_bytes[2] = {run_count & 0xFF, (run_count >> 8) & 0xFF};
        spi_write_blocking(SPI_PORT, count_bytes, 2);
        
        uint8_t end_marker = 0xBB; // Gửi byte 0xBB để ESP32 biết đã xong
        spi_write_blocking(SPI_PORT, &end_marker, 1);
    }
}

// --- HÀM CHẨN ĐOÁN: In hex 32 byte đầu để kiểm tra PIO có bắt được tín hiệu không ---
void print_diagnostic(uint8_t *data, uint32_t len) {
    printf("\n=== CHAN DOAN: 32 BYTE DAU TIEN (HEX) ===\n");
    printf("(0x00 = khong co tin hieu / GND, 0xFF = 3.3V, gia tri khac = co xung)\n");
    uint32_t show = len > 32 ? 32 : len;
    for (uint32_t i = 0; i < show; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
    
    // Đếm số byte khác 0 để xem có tín hiệu không
    uint32_t nonzero = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (data[i] != 0x00 && data[i] != 0xFF) nonzero++;
    }
    printf("So byte co xung (khac 0x00 va 0xFF): %lu / %lu\n", nonzero, len);
    printf("=========================================\n\n");
}

int main() {
    stdio_init_all();
    sleep_ms(2000); // Bắt buộc: Chờ USB kết nối với máy tính (2 giây)
    
    // --- KHỞI TẠO PHẦN CỨNG DỰA THEO CHẾ ĐỘ ---
    if (current_mode == MODE_WIFI) {
        // Mở cổng SPI với tốc độ 10 MHz
        spi_init(SPI_PORT, 10000000);
        gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
        gpio_set_function(PIN_CS,   GPIO_FUNC_SPI);
        gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
        gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    }

   while (true) {
        printf("=== LOGIC ANALYZER 25 MHz ===\n");
        printf("Dang chay o che do: %s\n", current_mode == MODE_USB ? "CÁP USB" : "WIFI/SPI");
        printf("Nhan 'c' tren ban phim de bat dau do...\n\n");
        
        // Đợi phím 'c' trong 2.000.000 micro-giây (2 giây)
        // Lệnh này không làm treo mạch như getchar() bình thường
        int cmd = getchar_timeout_us(2000000); 
        
        if (cmd == 'c') {
            printf("\nBat dau!\n");
            break; // Thoát vòng lặp chào hỏi để vào thuật toán đo chính
        }
    }


    // --- KHỞI TẠO PIO ---
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &logic_analyzer_program);
    
    float clk_div = 150000000.0 / 25000000.0; // Ép PIO chạy đúng 25MHz
    logic_analyzer_program_init(pio, sm, offset, CAPTURE_PIN_BASE, clk_div);

    // --- KHỞI TẠO DMA ---
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dma_cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, false);
    channel_config_set_write_increment(&dma_cfg, true);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio, sm, false));

    dma_channel_configure(dma_chan, &dma_cfg, capture_buffer, &pio->rxf[sm], CAPTURE_SAMPLES / 4, false);

    // --- VÒNG LẶP CHÍNH ---
    while (true) {
        printf("\nNhan 'c' tren ban phim de bat dau do...\n");
        // Dùng timeout 0 = chờ vô hạn nhưng không block hoàn toàn như getchar()
        // Thực ra getchar_timeout_us(0) = polling mode. Dùng giá trị lớn để chờ input.
        int c = getchar_timeout_us(10000000); // Chờ tối đa 10 giây
        
        if (c == 'c') {
            printf("Dang hut du lieu 25MB/s...\n");
            
            // Xóa rác và kích hoạt DMA
            pio_sm_clear_fifos(pio, sm);
            dma_channel_set_write_addr(dma_chan, capture_buffer, true);
            dma_channel_wait_for_finish_blocking(dma_chan); // Đứng chờ chụp xong
            
            printf("Chup xong! Dang phan tich...\n");
            
            // === THONG KE NHANH: Dem so lan tin hieu thay doi ===
            uint8_t *raw = (uint8_t *)capture_buffer;
            uint32_t transitions = 0;
            for (uint32_t i = 1; i < CAPTURE_SAMPLES; i++) {
                if (raw[i] != raw[i-1]) transitions++;
            }
            // In 8 byte dau tien de kiem tra
            printf("  8 byte dau (HEX): ");
            for (int i = 0; i < 8; i++) printf("%02X ", raw[i]);
            printf("\n");
            printf("  So lan doi tin hieu: %lu / 100000 mau\n", transitions);
            if (transitions == 0)
                printf("  => CANH BAO: Tin hieu phang! Kiem tra day cap.\n");
            else
                printf("  => CO TIN HIEU. Dang nen RLE...\n");
            // =====================================================
            
            compress_and_send_data();
        }
    }
    
    return 0;
}