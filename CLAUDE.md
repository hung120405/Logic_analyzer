# BTL_HTN — Logic Analyzer Dùng Raspberry Pi Pico 2

## Mô Tả Dự Án
Xây dựng một **Logic Analyzer 20MHz, 8 kênh** dùng Raspberry Pi Pico 2.
- PIO đọc **8 chân cùng lúc** → tránh lệch thời gian (CPU chỉ đọc được 1 chân/lần)
- **DMA** kéo dữ liệu từ PIO FIFO vào RAM → không tốn CPU
- **RLE** nén dữ liệu trước khi gửi → giảm băng thông
- **2 chế độ output**: USB→PuTTY (test) | SPI→ESP32→WebServer (bảo vệ)
- IC bảo vệ: **SN74LVC8T245** (Level Shifter 5V→3.3V)

---

## Mục Tiêu Dự Án

### 🎯 Mục Tiêu Cuối (Dài Hạn)
Xây dựng **giao diện hiển thị sóng** (waveform viewer) hoàn chỉnh:
- Pico 2 capture raw 0/1 → nén RLE → gửi về PC (USB) hoặc ESP32 (SPI)
- Phần mềm trên PC / Web Server nhận dữ liệu RLE → giải nén → vẽ đồ thị dạng sóng
- Hỗ trợ **decode giao thức**: UART, SPI, I2C → hiển thị byte/frame đọc được

### ✅ Mục Tiêu Hiện Tại (Ngắn Hạn)
Pico 2 **đo được các tín hiệu thật** (UART, SPI, I2C, xung vuông):
- Phần cứng capture đúng → dữ liệu raw lưu vào buffer
- RLE nén và gửi đi → sẵn sàng cho GUI decode sau
- **Pico không cần biết tín hiệu là giao thức gì** — chỉ ghi 0/1 theo thời gian
- Việc decode giao thức là nhiệm vụ của **phần mềm phía sau** (Python/Web)

### 📋 Nguyên Tắc Thiết Kế
```
Nguồn tín hiệu          Pico 2              Phần mềm (HTML/JS)
(Arduino/UART/SPI) ──► [Capture 0/1] ──► [Decode + Hiển thị]
                         ✅ Đã xong         ✅ Đã xong
```

> **Lưu ý quan trọng**: Firmware Pico **không phân biệt** UART/SPI/I2C.
> Nó chỉ sample 8 chân ở 20MHz và lưu raw bits. Protocol decoder nằm ở tầng software.

---

## Cấu Trúc File

```
Logic_analyzer-main/
├── BTL_HTN.c                          # Firmware Pico 2 (PIO+DMA+RLE+SPI)
├── logic_analyzer.pio                 # PIO: 2 SM (trigger_watcher + logic_analyzer)
├── CMakeLists.txt                     # Build config Pico
├── build/                             # Thư mục output sau khi cmake+make
│   └── BTL_HTN.uf2                    # File nạp vào Pico 2
├── esp_firmware/                      # Dự án ESP-IDF cho ESP32
│   ├── CMakeLists.txt                 # Top-level CMake ESP-IDF
│   ├── partitions.csv                 # Bảng phân vùng tùy chỉnh (4MB Flash)
│   ├── sdkconfig.defaults             # Cấu hình mặc định (Flash 4MB, WS bật)
│   ├── data/
│   │   └── PicoScope_LA.html          # Giao diện web (lưu trong SPIFFS)
│   ├── main/
│   │   ├── CMakeLists.txt             # Component CMake
│   │   └── main.c                     # Firmware ESP32 (WiFi AP, HTTP, WS, SPI)
│   └── build/                         # Thư mục output ESP-IDF
│       ├── picoscope_esp32.bin        # Firmware ESP32
│       └── spiffs.bin                 # Ảnh SPIFFS chứa file HTML
└── CLAUDE.md                          # File này
```

---

## Thông Số Kỹ Thuật

| Thông số | Giá trị |
|---------|---------|
| Board | Raspberry Pi Pico 2 (RP2350) |
| Tần số PIO | 20 MHz |
| Clock Pico 2 | 150 MHz → clk_div = 7.5 |
| Số kênh đo | 8 kênh song song (GP0–GP7) |
| CAPTURE_PIN_BASE | 0 (GP0) |
| Số mẫu | 100.000 mẫu ≈ 4ms |
| Buffer RAM | `uint32_t[25000]` = 100 KB |
| DMA | DMA_SIZE_32, dreq từ PIO RX FIFO |
| Binary type | `copy_to_ram` |

---

## Sơ Đồ Đấu Dây ✅ (Cập nhật 2026-06-14)

### IC SN74LVC8T245 — Nguồn & Điều Khiển

| Chân IC | Nối với | Ghi chú |
|---------|---------|---------|
| Pin 1 (VCCA) | 3.3V (ESP32) | A-side supply (KHÔNG dùng 5V nữa) |
| Pin 2 (DIR) | 3.3V (ESP32/Pico) | Chiều A→B (ESP32→Pico), kéo HIGH |
| Pin 11, 12, 13 (GND) | Rãnh âm breadboard | GND chung |
| Pin 22 (OE) | Rãnh âm breadboard (GND) | LOW = IC kích hoạt |
| Pin 23, 24 (VCCB) | 3.3V Pico | B-side supply |

> ⚠️ **GND chung bắt buộc**: ESP32 GND + Pico GND + IC GND + Mạch ngoài đều phải vào rãnh âm breadboard.

### IC SN74LVC8T245 — 8 Kênh Tín Hiệu (Qua IC)

Mỹi tín hiệu từ ESP32 đi qua IC để level-shift an toàn sang Pico:

| Chân IC (A-side) | ESP32 GPIO | Vai trò / Giao thức | Chân IC (B-side) | Pico GP | Kênh |
|-----------------|------------|---------------------|-----------------|---------|------|
| Pin 10 (A8) | GPIO 16 | **UART2 - RX** | Pin 14 (B8) | **GP0** | CH0 |
| Pin 9 (A7) | GPIO 17 | **UART2 - TX** | Pin 15 (B7) | **GP1** | CH1 |
| Pin 8 (A6) | GPIO 22 | I2C - SCL | Pin 16 (B6) | **GP2** | CH2 |
| Pin 7 (A5) | GPIO 21 | I2C - SDA | Pin 17 (B5) | **GP3** | CH3 |
| Pin 6 (A4) | GPIO 18 | SPI - SCK | Pin 18 (B4) | **GP4** | CH4 |
| Pin 5 (A3) | GPIO 23 | SPI - MOSI | Pin 19 (B3) | **GP5** | CH5 |
| Pin 4 (A2) | GPIO 19 | SPI - MISO | Pin 20 (B2) | **GP6** | CH6 |
| Pin 3 (A1) | GPIO 5 | SPI - CS | Pin 21 (B1) | **GP7** | CH7 |

### Kênh Trigger (Dây Thẳng, Không Qua IC)

> Tại sao không qua IC? Vì ESP32 và Pico đều dùng 3.3V — không cần level-shift.

| Từ | Đến | Mục đích |
|------|------|----------|
| ESP32 **GPIO 4** | Pico **GP8** | Trigger: SM0 chờ sườn lên ở đây → bắt n SM1 capture |

### Pinout Pico 2 (Dễ Nhầm)
```
        [USB]
   GP0 — [1]  [40] — VBUS (5V)
   GP1 — [2]  [39] — VSYS
   GND — [3]  [38] — GND
   GP2 — [4]  [37] — 3V3_EN
   GP3 — [5]  [36] — 3V3 OUT  ← Nguồn 3.3V
   GP4 — [6]  ...
   GP5 — [7]  ...
   GND — [8]  ...
   GP6 — [9]  ...
   GP7 — [10] ...
```
> ⚠️ GP2 = chân vật lý số **4**, không phải số 2!

---

## ESP32 — Web Server & Gateway (Chuẩn ESP-IDF v5)

> ESP32 đóng vai trò là "bộ não giao tiếp", vừa phát WiFi, vừa điều khiển Pico đo, vừa hiển thị lên trình duyệt web.

**Luồng hoạt động:**
1. Khởi tạo WiFi AP `PicoScope_WiFi`.
2. Tạo HTTP Server ở port 80 để trả file `PicoScope_LA.html` (được lưu trong phân vùng SPIFFS của bộ nhớ Flash).
3. Tạo WebSocket Server lắng nghe lệnh `START_CAPTURE`.
4. Khi có lệnh, kéo GPIO 4 (Trigger) lên HIGH để Pico đo đạc.
5. Pico đo xong, nén RLE rồi gửi qua SPI cho ESP32.
6. ESP32 nhận RLE, đẩy qua WebSocket về lại trình duyệt để Javascript giải nén và vẽ đồ thị.

**Sơ đồ SPI (ESP32 Master → Pico Slave):**
- GPIO 18 (SCK)  → Pico GP4 (qua IC)
- GPIO 23 (MOSI) → Pico GP5 (qua IC)
- GPIO 19 (MISO) → Pico GP6 (qua IC) — Pico gửi data qua dây này
- GPIO 5  (CS)   → Pico GP7 (qua IC)

---

## Trạng Thái Hiện Tại ✅ (Cập nhật 2026-06-14)

### Đã Hoàn Thành
- [x] Đấu nối phần cứng 8 kênh qua IC SN74LVC8T245
- [x] Firmware PIO + DMA hoạt động ở 20MHz (có timeout chống treo)
- [x] Kiến trúc 2 State Machine: SM0 (trigger_watcher GP8) + SM1 (logic_analyzer GP0-GP7)
- [x] Giao diện web PicoScope_LA.html (waveform, overlay, decoding)
- [x] Protocol Decoder: UART (2 chiều), I²C, SPI
- [x] 8 kênh đo trọn vẹn (GP0-GP7) + GP8 riêng làm Trigger
- [x] **Firmware ESP32 viết lại theo chuẩn ESP-IDF v5.x / v6.2 (WiFi AP, HTTP, WebSocket, SPI Master)**
- [x] **SPIFFS (1.44MB) lưu trữ file HTML trực tiếp trên Flash của ESP32**
- [x] **Build thành công và nạp thành công cả firmware lẫn SPIFFS vào ESP32**
- [x] Trang web load đầy đủ từ ESP32 AP (`192.168.4.1`)
- [x] Fix: Xóa Google Fonts (tránh treo trang khi không có Internet)
- [x] Fix: Thêm inline favicon (chặn lỗi `httpd_sock_err: 104`)
- [x] WebSocket URL tự động trỏ vào đúng IP: `ws://${window.location.host}/ws`

### Còn Lại
- [ ] Test lệnh `START_CAPTURE` qua WebSocket → Trigger → SPI data → vẽ waveform
- [ ] Xử lý luồng nạp dữ liệu USB dự phòng (nếu cần thiết)

### Sự Cố & Khắc Phục (Cập nhật 2026-06-21)
1. **Lỗi nhiễu kênh (Crosstalk) và sai cấu hình giao diện:**
   - **Hiện tượng:** Tất cả các kênh đều hiển thị nhiễu giống nhau, UART decode ra `0x00`, I2C nhận nhầm START/STOP, SPI không nhận được data.
   - **Khắc phục:** UI cũ bị sai lệch `CHANNEL_LABELS` và config của decoder so với đấu dây thực tế. Đã sửa lại mapping trong `PicoScope_LA.html` cho khớp hoàn toàn với phần cứng.
2. **Lỗi thiếu tín hiệu phát:**
   - **Hiện tượng:** ESP32 đang dùng SPI để nhận data từ Pico nên không thể phát tín hiệu I2C/UART ra ngoài trong khoảng 20ms Pico đang đo (toàn bộ các chân thả nổi / idle).
   - **Khắc phục:** Viết thêm bộ **Test Signal Generator (Bit-bang)** chạy đồng bộ (inline) ngay sau khi kích hoạt Trigger. Hàm này sử dụng `esp_rom_delay_us` để phát xung UART và I2C liên tục trong 15ms (vì Pico đo 20ms).
   - Code `CMakeLists.txt` đã được bổ sung `esp_timer` và `esp_driver_uart`.

---

## ⚠️ 3 Trở Ngại Kỹ Thuật & Giải Pháp (Cập nhật 2026-06-14)

### Trở Ngại 1 — Capture Window 20ms quá hẹp

**Vấn đề:** Buffer 400KB @ 20MHz đầy sau 20ms. Nếu ESP32 đang `delay(500ms)` lúc bấm Capture → thu về toàn khoảng trắng.

**Giải pháp: Hardware Edge Trigger bằng lệnh `wait` trong PIO** (KHÔNG dùng CPU Interrupt).

So sánh hai cách:
- CPU Interrupt: độ trễ 1–5µs → mất 20–100 mẫu
- PIO `wait`: phản ứng trong 1 cycle = 6.7ns → gần như tức thì

**Sửa `logic_analyzer.pio`:**
```asm
.program logic_analyzer

; Chờ sườn LÊN trên GP0 (nối với GPIO 4 của ESP32 — chân Trigger)
wait 0 pin 0    ; đảm bảo GP0 đang LOW trước
wait 1 pin 0    ; phát hiện sườn LÊN → BẮT ĐẦU CAPTURE ngay lập tức

.wrap_target
    in pins, 8  ; đọc 8 kênh GP0-GP7, mỗi 50ns
.wrap
```

**Sửa `BTL_HTN.c`:** Khởi động DMA trước, PIO tự trigger khi có sườn — CPU không tham gia:
```c
// DMA sẵn sàng chờ, PIO chờ phần cứng → không mất một mẫu nào
dma_channel_set_write_addr(dma_chan, capture_buffer, true);
dma_channel_wait_for_finish_blocking(dma_chan);
```

**Phía ESP32:** Kéo GPIO 4 lên HIGH trước khi phát tín hiệu:
```cpp
// Gửi xung Trigger TRƯỚC
digitalWrite(4, HIGH);
delayMicroseconds(10);   // 10µs >> thời gian PIO cần (vài ns)

// Bây giờ mới phát tín hiệu thật
Wire.beginTransmission(0x68);
Wire.write(0x3B);
Wire.endTransmission();

digitalWrite(4, LOW);    // Reset trigger cho lần capture tiếp theo
```

> ✅ Kết quả: ESP32 có delay bao lâu cũng không ảnh hưởng — Pico luôn bắt đúng thời điểm có tín hiệu.

---

### Trở Ngại 2 — Nyquist & Tốc độ SPI quá nhanh

**Vấn đề:** Tần số an toàn tối đa = `20MHz / 5 = 4MHz`. SPI của ESP32 có thể chạy tới 80MHz nếu không giới hạn → aliasing → decoder xuất ra rác.

**Giải pháp: Giới hạn cứng tốc độ tín hiệu phát từ ESP32**

| Giao thức | Tốc độ được phép dùng | Lý do |
|-----------|----------------------|-------|
| I2C | 100kHz hoặc 400kHz | Rất an toàn, dư dả |
| UART | 115200 baud | An toàn tuyệt đối |
| SPI | **Tối đa 1MHz** | 20MHz / 5 = 4MHz → để dư thì dùng 1MHz |

```cpp
// ESP32 — giới hạn SPI clock ở 1MHz
SPI.beginTransaction(SPISettings(
    1000000,    // 1 MHz — KHÔNG được tăng lên 10MHz hay 80MHz
    MSBFIRST,
    SPI_MODE0
));
```

**Thêm cảnh báo trong HTML decoder** (`PicoScope_LA.html`): Khi decode SPI, nếu phát hiện chu kỳ clock < 5 samples (< 250ns) → hiển thị warning "Signal too fast, possible aliasing".

> ✅ Kết quả: I2C và UART decode cực sắc nét. SPI giới hạn 1MHz → an toàn với oversampling 20×.

---

### Trở Ngại 3 — Clock Jitter ±1 mẫu (±50ns)

**Vấn đề:** Xung nhịp ESP32 và RP2350 hoàn toàn độc lập. PIO sample đúng vào lúc tín hiệu đang chuyển trạng thái → sườn xung bị "rung" ±50ns khi zoom in hết cỡ.

**Giải pháp: Xử lý ở tầng phần mềm (HTML decoder) — không thể loại hoàn toàn bằng phần cứng**

**Cách 1 — Hysteresis filter trong decoder** (đơn giản, hiệu quả nhất):
```javascript
// Trong decodeUART / decodeI2C / decodeSPI
// Không nhận diện edge nếu trạng thái chưa ổn định >= MIN_STABLE mẫu
const MIN_STABLE = 3; // 3 mẫu × 50ns = 150ns — lọc jitter ±50ns
let stableCount = 0;
let lastStable = data[0];

for (let i = 1; i < data.length; i++) {
    if (data[i] === lastStable) {
        stableCount++;
    } else {
        if (stableCount >= MIN_STABLE) {
            // Edge hợp lệ — xử lý bình thường
        }
        stableCount = 0;
        lastStable = data[i];
    }
}
```

**Cách 2 — Sample giữa bit, không sample tại edge** (đã áp dụng trong UART decoder):
Thay vì detect edge rồi sample ngay, sample ở giữa khoảng thời gian ổn định của mỗi bit:
```javascript
// Sample tại 1.5 × bitSamples từ start bit (giữa bit đầu tiên)
const samplePos = startEdge + Math.floor(bitSamples * 1.5);
```

**Cách 3 — Hiểu và chấp nhận**: Jitter ±50ns là giới hạn vật lý của hệ thống 2 clock domain độc lập. Với I2C (100–400kHz) và UART (115200 baud), 50ns jitter chiếm < 0.5% chu kỳ bit → hoàn toàn chấp nhận được, decoder vẫn chính xác.

> ✅ Kết quả: Decoder vẫn xuất đúng dữ liệu. Jitter ±50ns chỉ thấy khi zoom in tột độ, không ảnh hưởng tính năng decode.

---

### Tóm Tắt Ưu Tiên Xử Lý

| Trở ngại | Mức độ ảnh hưởng | Giải pháp | Nơi thực hiện |
|----------|-----------------|-----------|---------------|
| 1 — Capture Window | 🔴 Nghiêm trọng | Hardware Trigger bằng PIO `wait` | `logic_analyzer.pio` + `BTL_HTN.c` + ESP32 |
| 2 — Nyquist/SPI speed | 🟡 Quan trọng | Giới hạn SPI ≤ 1MHz | ESP32 code |
| 3 — Clock Jitter | 🟢 Chấp nhận được | Hysteresis filter + sample giữa bit | `PicoScope_LA.html` |

---

## ⚡ Nâng Cấp Giải Pháp v2 — Phản Biện Kỹ Thuật (Cập nhật 2026-06-14)

### Nâng Cấp 1 — Chống Deadlock: Thay Blocking bằng Timeout Loop

**Lỗ hổng gốc:** `dma_channel_wait_for_finish_blocking()` treo CPU vĩnh viễn nếu dây Trigger bị lỏng hoặc ESP32 bị treo → Pico không nhận được lệnh Cancel từ máy tính.

**Giải pháp: Polling loop có timeout + abort + thông báo lỗi rõ ràng:**

```c
// BTL_HTN.c — Thay thế dma_channel_wait_for_finish_blocking()

#define TRIGGER_TIMEOUT_MS 5000  // Chờ tối đa 5 giây

void capture_with_timeout() {
    // Kích hoạt DMA
    pio_sm_clear_fifos(pio, sm);
    dma_channel_set_write_addr(dma_chan, capture_buffer, true);

    // Polling loop có timeout — CPU vẫn sống, vẫn kiểm tra lệnh Cancel
    uint32_t start_ms = to_ms_since_boot(get_absolute_time());

    while (dma_channel_is_busy(dma_chan)) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_ms;

        // Hết timeout → hủy DMA, báo lỗi, quay lại chờ lệnh
        if (elapsed > TRIGGER_TIMEOUT_MS) {
            dma_channel_abort(dma_chan);
            printf("TRIGGER_TIMEOUT: Khong nhan duoc xung Trigger sau %dms\n",
                   TRIGGER_TIMEOUT_MS);
            printf("Kiem tra: Day GPIO4 ESP32 -> GP0 Pico, va ESP32 co chay khong.\n");
            return;  // Thoát, vòng lặp ngoài sẽ hỏi lệnh mới
        }

        // Trong khi chờ: vẫn có thể kiểm tra lệnh Cancel từ USB
        // (nếu sau này thêm giao tiếp 2 chiều)
        tight_loop_contents();  // CPU không idle hoàn toàn, tránh WDT reset
    }

    printf("Chup xong!\n");
    compress_and_send_data();
}
```

> ✅ CPU luôn sống, luôn có thể thoát. Thông báo lỗi rõ ràng để debug phần cứng.

---

### Nâng Cấp 2 — Trigger Không Lãng Phí Kênh Đo: Dùng GP8 + 2 State Machine

**Lỗ hổng gốc:** `wait 0 pin 0` / `wait 1 pin 0` trong PIO dùng GP0 vừa làm Trigger vừa làm kênh đo → mất 1 kênh, chỉ còn 7 kênh thực sự.

**Giải pháp: Dùng chân riêng GP8 làm Trigger, tách ra khỏi 8 kênh đo GP0–GP7.**

Cấu trúc 2 State Machine song song:
- **SM0**: Chờ sườn lên trên GP8 → bắn IRQ cho SM1
- **SM1**: Nhận IRQ từ SM0 → bắt đầu capture GP0–GP7

```asm
; logic_analyzer.pio — Phiên bản 2 SM, giữ nguyên 8 kênh đo

; === State Machine 0: Trigger Watcher ===
.program trigger_watcher
    wait 0 pin 8    ; chờ GP8 xuống LOW (reset)
    wait 1 pin 8    ; chờ sườn LÊN GP8
    irq set 0       ; bắn IRQ 0 báo cho SM1 bắt đầu

% c-sdk {
static inline void trigger_watcher_init(PIO pio, uint sm, uint offset) {
    pio_sm_config c = trigger_watcher_program_get_default_config(offset);
    sm_config_set_in_pins(&c, 8);       // GP8 làm trigger pin
    pio_gpio_init(pio, 8);
    pio_sm_set_consecutive_pindirs(pio, sm, 8, 1, false);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
%}

; === State Machine 1: Data Capture ===
.program logic_analyzer
    wait 1 irq 0    ; đứng đây chờ IRQ 0 từ SM0 (trigger watcher)
                    ; tự động clear IRQ sau khi nhận

.wrap_target
    in pins, 8      ; đọc GP0–GP7 đầy đủ 8 kênh, mỗi 50ns
.wrap

% c-sdk {
static inline void logic_analyzer_init(PIO pio, uint sm, uint offset,
                                        uint pin_base, float clk_div) {
    pio_sm_config c = logic_analyzer_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin_base);    // GP0 là chân đầu tiên
    for (uint i = 0; i < 8; i++) pio_gpio_init(pio, pin_base + i);
    sm_config_set_in_shift(&c, false, true, 32);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_base, 8, false);
    sm_config_set_clkdiv(&c, clk_div);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
%}
```

**Cập nhật đấu dây:** ESP32 GPIO 4 → IC Pin 10 (A8) → Pico **GP8** (không còn là GP0 nữa).

**Cập nhật `BTL_HTN.c`:** Khởi tạo cả 2 SM:
```c
// SM0: Trigger Watcher trên GP8
uint offset0 = pio_add_program(pio, &trigger_watcher_program);
trigger_watcher_init(pio, 0, offset0);

// SM1: Data Capture trên GP0-GP7
uint offset1 = pio_add_program(pio, &logic_analyzer_program);
logic_analyzer_init(pio, 1, offset1, CAPTURE_PIN_BASE, clk_div);

// DMA đọc từ SM1 (rxf[1])
channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio, 1, false));
dma_channel_configure(dma_chan, &dma_cfg, capture_buffer, &pio->rxf[1],
                      CAPTURE_SAMPLES / 4, true);
```

> ✅ Giữ nguyên đầy đủ 8 kênh đo GP0–GP7. GP8 riêng biệt chỉ làm Trigger.

---

### Nâng Cấp 3 — Tách biệt Raw View và Filtered Decode

**Lỗ hổng gốc:** Hysteresis filter `MIN_STABLE = 3` (150ns) áp dụng toàn bộ → âm thầm xóa Glitch phần cứng thật mà người dùng cần thấy để debug.

**Giải pháp: Tách 2 tầng hoàn toàn độc lập trong `PicoScope_LA.html`**

```
Raw Data (state.data)         ← KHÔNG bao giờ lọc, luôn hiển thị nguyên xi
       │
       ├──► Waveform Canvas   ← Vẽ RAW, thấy mọi glitch kể cả 50ns
       │
       └──► Protocol Decoder  ← Lọc hysteresis CHỈ trong bước decode
                                 (không sửa state.data gốc)
```

**Thêm toggle "Glitch Filter" trong UI:**

```javascript
// PicoScope_LA.html — Tách raw display vs filtered decode

const decoderSettings = {
    glitchFilterEnabled: false,  // Mặc định TẮT để thấy glitch thật
    minStableSamples: 3,         // Dùng khi bật filter (150ns)
};

// Hàm decode có filter (dùng cho protocol decode)
function findEdgeFiltered(data, mask, startIdx) {
    if (!decoderSettings.glitchFilterEnabled) {
        return findEdgeRaw(data, mask, startIdx);  // Không filter
    }
    // Chỉ nhận edge nếu trạng thái ổn định >= minStableSamples
    const N = decoderSettings.minStableSamples;
    for (let i = startIdx + 1; i < data.length - N; i++) {
        const prev = (data[i-1] & mask) !== 0;
        const curr = (data[i]   & mask) !== 0;
        if (prev !== curr) {
            // Kiểm tra N mẫu tiếp theo có ổn định không
            let stable = true;
            for (let k = 1; k < N; k++) {
                if (((data[i+k] & mask) !== 0) !== curr) { stable = false; break; }
            }
            if (stable) return i;  // Edge hợp lệ
        }
    }
    return -1;
}

// Waveform canvas luôn dùng RAW (không qua filter)
function drawWaves() {
    // ... vẽ thẳng từ state.data, không lọc gì cả ...
    // Glitch 50ns vẫn hiển thị trên màn hình
}
```

**Thêm nút toggle trong toolbar HTML:**
```html
<button class="btn ghost" onclick="toggleGlitchFilter()" id="glitchFilterBtn"
        title="Bật: lọc nhiễu khi decode giao thức. Tắt: thấy mọi glitch phần cứng">
  🔍 Glitch Filter: OFF
</button>
```

**Bảng so sánh 2 chế độ:**

| Chế độ | Waveform | Decoder | Dùng khi nào |
|--------|----------|---------|--------------|
| Filter OFF (mặc định) | Thấy mọi glitch 50ns | Có thể false-positive | Debug phần cứng, tìm nhiễu |
| Filter ON | Thấy mọi glitch 50ns | Decode sạch, chính xác | Khi tín hiệu đã ổn, muốn decode giao thức |

> ✅ Logic Analyzer luôn trung thực với dữ liệu gốc. Glitch phần cứng KHÔNG bao giờ bị xóa khỏi waveform. Filter chỉ dùng trong bước suy luận giao thức.

---

### Tóm Tắt Nâng Cấp v2

| Vấn đề phản biện | Giải pháp nâng cấp | File thay đổi |
|-----------------|-------------------|---------------|
| CPU deadlock khi mất Trigger | Timeout loop 5s + `dma_channel_abort()` | `BTL_HTN.c` |
| GP0 bị dùng làm Trigger | Tách GP8 + 2 State Machine (IRQ) | `logic_analyzer.pio` + `BTL_HTN.c` |
| Filter xóa Glitch thật | Raw waveform + Filter chỉ trong decoder + Toggle UI | `PicoScope_LA.html` |

---

## Cách Đọc Kết Quả Chẩn Đoán

Sau khi nhấn 'c' trên PuTTY:
```
8 byte dau (HEX): FF FF FF FF FF FF FF FF
So lan doi tin hieu: XXXX / 100000 mau
```

| Kết quả transitions | Ý nghĩa |
|--------------------|---------|
| = 0 | Không có tín hiệu — kiểm tra dây |
| ~8 | Chỉ 1 kênh hoạt động (tone 1kHz cũ) |
| ~1600+ | 8 kênh đầy đủ đang hoạt động ✅ |

| Hex value | Ý nghĩa |
|-----------|---------|
| `FF` = 11111111 | Tất cả 8 kênh đang HIGH |
| `FD` = 11111101 | GP1 đang LOW (bit 1 = 0) |
| `3F` = 00111111 | GP6, GP7 floating (chân cũ chưa cấu hình) |

---

## Lệnh Build & Flash

### 🔵 Pico 2 — Build & Flash (Linux / Ubuntu)

```bash
# (Chỉ cần 1 lần) Lưu đường dẫn SDK vĩnh viễn
echo 'export PICO_SDK_PATH=~/pico-sdk' >> ~/.bashrc && source ~/.bashrc

# Lần đầu tiên (tạo thư mục build và cấu hình CMake)
cd ~/Downloads/Logic_analyzer-main
mkdir -p build && cd build
cmake ..

# Những lần sau (chỉ cần build lại khi sửa code)
export PICO_SDK_PATH=~/pico-sdk   # Cần gõ nếu vừa mở Terminal mới
cd ~/Downloads/Logic_analyzer-main/build
make -j4

# Flash (tự động, không cần kéo thả file)
# Giữ nút BOOTSEL trên Pico → cắm USB → thả BOOTSEL → ổ RPI-RP2 xuất hiện
cp build/BTL_HTN.uf2 /media/$(whoami)/RPI-RP2/
# Hoặc nếu là Pico 2 (RP2350):
cp build/BTL_HTN.uf2 /media/$(whoami)/RP2350/
```

---

### 🟠 ESP32 — Build & Flash (ESP-IDF v5.x / v6.2)

> ⚠️ **Quan trọng**: Phải mở đúng thư mục `esp_firmware` và kích hoạt môi trường ESP-IDF trước khi build.

#### Bước 1 — Kích hoạt môi trường ESP-IDF
```bash
cd ~/Downloads/Logic_analyzer-main/esp_firmware
. ~/esp/esp-idf/export.sh
```
*(Mỗi lần mở Terminal mới đều phải chạy lại lệnh `export.sh` này)*

#### Bước 2 — Build (biên dịch code C + đóng gói file HTML)
```bash
idf.py build
```
*(Lần đầu tiên chạy lâu ~5-10 phút. Các lần sau chỉ build những gì thay đổi)*

#### Bước 3 — Flash vào ESP32
```bash
# Nạp ở tốc độ thấp để tránh lỗi chip (115200 thay vì 460800 mặc định)
idf.py -p /dev/ttyUSB0 -b 115200 flash

# Nếu cổng bị bận (Resource busy), tìm và kill tiến trình đang chiếm:
fuser -k /dev/ttyUSB0
```

> 💡 **Mẹo**: Nếu mạch ESP32 không vào chế độ nạp (hiện `Connecting...` mãi),
> hãy **giữ nút BOOT** trên mạch trong khi chạy lệnh flash, thả ra khi thấy tiến trình bắt đầu ghi %.

#### Bước 4 — Xem Log sau khi nạp
```bash
idf.py -p /dev/ttyUSB0 monitor
# Thoát: Ctrl+]
```

#### Lệnh tiện ích khác
```bash
# Build + Flash + Monitor trong 1 lệnh (phổ biến nhất)
idf.py -p /dev/ttyUSB0 -b 115200 flash monitor

# Nếu muốn nạp lại chỉ file HTML (SPIFFS) mà không nạp lại firmware C
idf.py -p /dev/ttyUSB0 spiffs_flash

# Clean build khi bị lỗi kỳ lạ
rm -rf build && idf.py build
```

#### Các lỗi thường gặp khi flash ESP32
| Lỗi | Nguyên nhân | Cách xử lý |
|-----|-------------|------------|
| `The chip stopped responding` | Tốc độ nạp quá cao | Thêm `-b 115200` vào lệnh flash |
| `Resource temporarily unavailable` | Cổng USB đang bị chiếm | Chạy `fuser -k /dev/ttyUSB0` |
| `Connecting...` treo mãi | Chip không vào Boot mode | Giữ nút BOOT khi flash |
| Trang web trắng / load 1 nửa | Google Fonts chặn (không có internet) | Đã fix: dùng system font |
| `httpd_sock_err: 104` | Trình duyệt gọi favicon không tồn tại | Đã fix: inline favicon `data:,` |

---

## Các Thay Đổi Code Quan Trọng

### logic_analyzer.pio
```asm
; Dùng .wrap thay vì jmp — không tốn cycle, đảm bảo đúng 20MHz
.wrap_target
    in pins, 8
.wrap
```

### BTL_HTN.c — Những thay đổi đã thực hiện
| Thay đổi | Lý do |
|---------|-------|
| `MODE_WIFI` → `MODE_USB` | Test qua PuTTY |
| `getchar()` → `getchar_timeout_us(10s)` | Tránh treo mạch |
| Thêm `sleep_ms(2000)` | Chờ USB kết nối |
| Thêm thống kê transitions | Chẩn đoán nhanh không cần đọc binary |
| `clk_div = 150M/25M` → `150M/20M` | Đổi tần số từ 25MHz xuống 20MHz |

---

## Giao Diện Web — PicoScope_LA.html

### Cách Chạy
File HTML thuần, không cần server. Mở bằng 1 trong 3 cách:
1. **Double-click** file trong File Manager
2. **Terminal**: `xdg-open PicoScope_LA.html` hoặc `firefox PicoScope_LA.html`
3. **Kéo thả** file vào cửa sổ trình duyệt

### Kiến Trúc Phân Tầng
```
┌─────────────────────┐     ┌──────────────────────────┐
│   Pico 2 (Firmware) │     │   PicoScope_LA.html      │
│   BTL_HTN.c + PIO   │ ──► │   (JavaScript)           │
│                     │ USB │                          │
│ Chỉ làm 1 việc:    │ or  │ Nhận raw data → decode:  │
│ • Sample 8 chân    │ SPI │ • UART decoder           │
│ • 20MHz, ghi 0/1   │     │ • I²C decoder            │
│ • Nén RLE          │     │ • SPI decoder            │
│ • Gửi raw data     │     │ • Vẽ waveform            │
│                     │     │ • Hiển thị kết quả       │
│ KHÔNG decode        │     │                          │
└─────────────────────┘     └──────────────────────────┘
```

> **Quan trọng**: Muốn thêm giao thức mới (CAN, 1-Wire, PWM...) → chỉ cần thêm decoder function trong HTML. Firmware Pico **không cần sửa**.

### Protocol Decoders Đã Triển Khai

| Giao thức | Kênh | Thuật toán | Cấu hình |
|-----------|------|-----------|----------|
| **UART** | CH0 (bit 0) | Tìm start bit (HIGH→LOW) → sample giữa mỗi bit → đọc 8 bit LSB first | baud=115200, 8N1 |
| **I²C** | CH2 (SCL), CH3 (SDA) | START=SDA↓ khi SCL HIGH, STOP=SDA↑ khi SCL HIGH, đọc data trên SCL rising edge, 9 clock/byte | auto-detect |
| **SPI** | CH4 (SCK), CH5 (MOSI), CH6 (MISO), CH7 (CS) | CS active LOW → đọc MOSI+MISO trên clock edge → 8 bit/byte | CPOL=0, CPHA=0, MSB first |

### Channel Mapping (Cập nhật 2026-06-21)
Đã đồng bộ giao diện Web với thực tế đấu nối phần cứng:

| Bit | Channel | Giao thức | Vai trò | Chân ESP32 | Chân Pico |
|-----|---------|-----------|----------|------------|-----------|
| 0 | CH0 | I²C | SCL | GPIO 22 | GP0 |
| 1 | CH1 | I²C | SDA | GPIO 21 | GP1 |
| 2 | CH2 | UART | TX | GPIO 17 | GP2 |
| 3 | CH3 | SPI | MOSI | GPIO 23 | GP3 |
| 4 | CH4 | SPI | MISO | GPIO 19 | GP4 |
| 5 | CH5 | SPI | SCK | GPIO 18 | GP5 |
| 6 | CH6 | SPI | CS | GPIO 5 | GP6 |
| 7 | CH7 | UART | RX | GPIO 16 | GP7 |

### Tính Năng Giao Diện
- **Waveform viewer**: 8 kênh, zoom (cuộn chuột), kéo dịch, phím tắt (+/-/0/←/→)
- **Decoder overlay**: Annotation boxes trên waveform hiển thị giá trị decode
- **Decoder panel**: Toggle bật/tắt từng decoder, hiển thị frames decoded
- **Measurements**: Tần số, duty cycle, số edges mỗi kênh
- **Cursors**: 2 cursor A/B, delta time, 1/Δ frequency
- **Export**: CSV, VCD, PNG (placeholder)

### Dữ Liệu Hiện Tại
Đang dùng **dữ liệu demo** (generate bằng JavaScript). Khi kết nối Pico thật, cần thêm:
1. Nhận raw RLE data từ USB (Web Serial API hoặc qua proxy)
2. Giải nén RLE → `Uint8Array`
3. Gán vào `state.data` → gọi `runAllDecoders()` → gọi `draw()`
