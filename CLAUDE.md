# BTL_HTN — Logic Analyzer Dùng Raspberry Pi Pico 2

## Mô Tả Dự Án
Xây dựng một **Logic Analyzer 25MHz, 8 kênh** dùng Raspberry Pi Pico 2.
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
Nguồn tín hiệu          Pico 2              Phần mềm
(Arduino/UART/SPI) ──► [Capture 0/1] ──► [Decode + Hiển thị]
                         ✅ Đã xong         ⏳ Làm sau
```

> **Lưu ý quan trọng**: Firmware Pico **không phân biệt** UART/SPI/I2C.
> Nó chỉ sample 8 chân ở 25MHz và lưu raw bits. Protocol decoder nằm ở tầng software.

---

## Cấu Trúc File

```
BTL_HTN/
├── BTL_HTN.c                          # Firmware Pico 2 (PIO+DMA+RLE)
├── logic_analyzer.pio                 # PIO program: đọc 8 pin song song
├── CMakeLists.txt                     # Build config
└── CLAUDE.md                          # File này
```

---

## Thông Số Kỹ Thuật

| Thông số | Giá trị |
|---------|---------|
| Board | Raspberry Pi Pico 2 (RP2350) |
| Tần số PIO | 25 MHz |
| Clock Pico 2 | 150 MHz → clk_div = 6.0 |
| Số kênh đo | 8 kênh song song (GP0–GP7) |
| CAPTURE_PIN_BASE | 0 (GP0) |
| Số mẫu | 100.000 mẫu ≈ 4ms |
| Buffer RAM | `uint32_t[25000]` = 100 KB |
| DMA | DMA_SIZE_32, dreq từ PIO RX FIFO |
| Binary type | `copy_to_ram` |

---

## Sơ Đồ Đấu Dây ✅ (Cập nhật 2026-05-07)

### IC SN74LVC8T245 — Nguồn & Điều Khiển

| Chân IC | Nối với | Ghi chú |
|---------|---------|---------|
| Pin 1 (VCCA) | 5V (VBUS Pico hoặc Arduino 5V) | A-side supply |
| Pin 2 (DIR) | 5V (HIGH) | Chiều A→B (Arduino→Pico) |
| Pin 11, 12, 13 (GND) | Rãnh âm breadboard | GND chung |
| Pin 22 (OE) | Rãnh âm breadboard (GND) | LOW = IC kích hoạt |
| Pin 23, 24 (VCCB) | 3.3V Pico | B-side supply |

> ⚠️ **GND chung bắt buộc**: Arduino GND + Pico GND + IC GND đều phải vào rãnh âm breadboard.

### IC SN74LVC8T245 — 8 Kênh Tín Hiệu

| Chân IC (A-side) | Arduino Pin | Chân IC (B-side) | Pico GP | Tần số (mới) |
|-----------------|-------------|-----------------|---------|-------------|
| Pin 3 (A1) | Pin 2 | Pin 21 (B1) | GP7 | 10.000 Hz |
| Pin 4 (A2) | Pin 3 | Pin 20 (B2) | GP6 |  5.000 Hz |
| Pin 5 (A3) | Pin 4 | Pin 19 (B3) | GP5 |  2.500 Hz |
| Pin 6 (A4) | Pin 5 | Pin 18 (B4) | GP4 |  1.250 Hz |
| Pin 7 (A5) | Pin 6 | Pin 17 (B5) | GP3 |    625 Hz |
| Pin 8 (A6) | Pin 7 | Pin 16 (B6) | GP2 |    312 Hz |
| Pin 9 (A7) | Pin 8 | Pin 15 (B7) | GP1 |    156 Hz |
| Pin 10 (A8) | Pin 9 | Pin 14 (B8) | GP0 |     78 Hz |

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

## Arduino — Máy Phát Xung 8 Kênh

File: `arduino_pulse_gen/arduino_pulse_gen.ino`

**Nguyên lý:** Dùng Timer2 interrupt ở 20kHz. Biến đếm 8-bit `cnt` tăng mỗi interrupt. Bit thứ n của `cnt` toggle ở tần số `20000 / 2^(n+1)`. Ghi thẳng ra Port D và Port B để tối ưu tốc độ ISR.

```cpp
ISR(TIMER2_COMPA_vect) {
    static uint8_t cnt = 0;
    cnt++;
    PORTD = (PORTD & 0x03) | ((cnt & 0x3F) << 2); // Pin 2-7
    PORTB = (PORTB & 0xFC) | ((cnt >> 6) & 0x03);  // Pin 8-9
}
// Timer2: CTC, prescaler=8, OCR2A=99 → 20kHz
```

**Tần số các kênh:**
| GP Pico | Arduino Pin | Tần số | Chu kỳ trong 4ms |
|---------|------------|--------|-----------------|
| GP7 | 2 | 10.000 Hz | 40 chu kỳ |
| GP6 | 3 |  5.000 Hz | 20 chu kỳ |
| GP5 | 4 |  2.500 Hz | 10 chu kỳ |
| GP4 | 5 |  1.250 Hz |  5 chu kỳ |
| GP3 | 6 |    625 Hz |  2.5 chu kỳ |
| GP2 | 7 |    312 Hz |  1.25 chu kỳ |
| GP1 | 8 |    156 Hz |  ~1 chu kỳ |
| GP0 | 9 |     78 Hz |  <1 chu kỳ |

---

## Trạng Thái Hiện Tại ✅ (Cập nhật 2026-05-07)

### Đã Hoàn Thành
- [x] B1: Đấu nối phần cứng 8 kênh qua IC SN74LVC8T245
- [x] B2: Firmware PIO + DMA hoạt động ở 25MHz
- [x] RLE nén dữ liệu và gửi qua USB
- [x] PuTTY nhận lệnh 'c' → chụp → in thống kê transitions
- [x] Arduino phát 8 kênh tần số khác nhau
- [x] Xác nhận tín hiệu đến được Pico (`transitions > 0`)

### Còn Lại
- [ ] Test phần SPI → ESP32 → Web Server
- [ ] Viết Python decoder để vẽ sóng từ RLE (tùy chọn)

### Cấu Hình Code Hiện Tại
```c
#define CAPTURE_PIN_BASE 0   // GP0-GP7
int current_mode = MODE_USB; // Test qua PuTTY
```

### Khi Đi Bảo Vệ (Đổi 1 Dòng)
```c
int current_mode = MODE_WIFI; // Gửi qua SPI sang ESP32
```

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

```bash
# Build Pico firmware
C:\Users\Minh Hung/.pico-sdk/ninja/v1.12.1/ninja.exe -C "C:\users\minh hung\BTL_HTN/build"

# Flash: Giữ BOOTSEL → cắm USB → copy file vào drive RPI-RP2
# File UF2: C:\users\minh hung\BTL_HTN\build\BTL_HTN.uf2
```

---

## Các Thay Đổi Code Quan Trọng

### logic_analyzer.pio
```asm
; Dùng .wrap thay vì jmp — không tốn cycle, đảm bảo đúng 25MHz
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
