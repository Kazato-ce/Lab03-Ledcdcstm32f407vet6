# Lab03 - Điều Khiển LED qua USB CDC và Lưu Trữ SPI Flash (STM32F407VET6)

Dự án cấu hình và lập trình vi điều khiển **STM32F407VET6** kết hợp giữa giao tiếp **USB CDC (Virtual COM Port)** và bộ nhớ ngoài **SPI Flash (W25Qxx)**. Hệ thống nhận lệnh điều khiển bật/tắt LED từ máy tính, phản hồi trạng thái và tự động ghi nối tiếp (sequential logging) lịch sử các lệnh vào Flash.

---

## 📌 Tính Năng Nổi Bật
- **Giao tiếp USB CDC**:
  - Giao tiếp 2 chiều qua cổng COM ảo (Virtual COM Port) sử dụng ngoại vi USB OTG FS (Device mode).
  - Tự động gửi lời chào định kỳ đến Terminal trên máy tính:
    ```text
    Hello CE224.R12, this is USB CDC
    Do you want to turn on or off light
    ```
- **Điều khiển LED**:
  - Nhận diện lệnh không phân biệt hoa thường và chuẩn hóa khoảng trắng:
    - `Turn on LED 1` / `Turn off LED 1`
    - `Turn on LED 2` / `Turn off LED 2`
- **Lưu trữ nhật ký trên SPI Flash**:
  - Sử dụng thư viện `spif` (W25Qxx) qua giao tiếp SPI1.
  - Mỗi lệnh thực thi hợp lệ được tự động ghi nối tiếp vào SPI Flash kèm kiểm tra toàn vẹn dữ liệu (verify sau khi ghi).
  - **Tự động khôi phục vị trí log**: Khi khởi động lại hoặc mất điện, vi điều khiển tự động quét tìm byte trống `0xFF` đầu tiên để tiếp tục ghi, không làm mất hay ghi đè log cũ.
- **Lệnh tiện ích quản lý log**:
  - `Show log`: Đọc và hiển thị toàn bộ lịch sử các lệnh đã lưu trữ trong Flash lên Terminal.
  - `Clear log`: Xóa các sector log trong Flash để bắt đầu chu kỳ ghi mới.
- **Video demo**: Video thực nghiệm chạy thực tế được lưu tại thư mục `Video/Videodemo.mp4`.

---

## ⚙️ Cấu Hình Phần Cứng (STM32F407VET6 Black Board)

| Ngoại vi / Thiết bị | Chân vi điều khiển | Chức năng / Ghi chú |
| :--- | :--- | :--- |
| **LED 1** | `PA6` | Output Push-Pull, Active Low |
| **LED 2** | `PA7` | Output Push-Pull, Active Low |
| **FLASH CS** | `PA4` | Chip Select cho SPI Flash (Active Low) |
| **SPI1 SCK** | `PA5` | SPI Clock |
| **SPI1 MISO** | `PA6` (hoặc `PB4`) | SPI Master In Slave Out |
| **SPI1 MOSI** | `PA7` (hoặc `PB5`) | SPI Master Out Slave In |
| **USB OTG FS DM** | `PA11` | USB D- |
| **USB OTG FS DP** | `PA12` | USB D+ |
| **HSE Clock** | 25 MHz External Crystal | Nhân tần PLL: Core 168MHz / USB 48MHz |

---

## 📂 Cấu Trúc Thư Mục Dự Án

```text
Ledcdcstm32f407vet6/
├── Core/
│   ├── Inc/
│   │   ├── app_cdc.h          # Header logic điều khiển CDC & SPI Flash
│   │   ├── main.h             # Cấu hình chân GPIO & định nghĩa hệ thống
│   │   ├── spi.h              # Header SPI
│   │   └── spif.h             # Header thư viện driver W25Qxx SPI Flash
│   └── Src/
│       ├── app_cdc.c          # Xử lý lệnh CDC, bộ đệm vòng (ring buffer) & log Flash
│       ├── main.c             # Khởi tạo Clock, GPIO, SPI, USB
│       └── spif.c             # Driver giao tiếp W25Qxx Flash
├── Drivers/                   # STM32F4xx HAL Driver & CMSIS
├── Middlewares/               # ST USB Device Library (CDC Class)
├── USB_DEVICE/                # Cấu hình lớp USB CDC (usbd_cdc_if,...)
├── Video/
│   └── Videodemo.mp4          # Video minh họa hoạt động của mạch
├── CMakeLists.txt             # File cấu hình build với CMake
└── README.md                  # Tài liệu hướng dẫn dự án
```

---

## 🚀 Hướng Dẫn Biên Dịch & Nạp Chương Trình

### 1. Biên dịch bằng CMake & GCC ARM
Yêu cầu đã cài đặt `arm-none-eabi-gcc`, `cmake` và `ninja`.

```bash
# Cấu hình và sinh build file
cmake -B build -G Ninja

# Biên dịch chương trình
cmake --build build
```

File thực thi đầu ra (`.elf`, `.hex`, `.bin`) sẽ nằm trong thư mục `build/`.

### 2. Nạp vi điều khiển
- Sử dụng **ST-Link**, nạp trực tiếp qua VS Code (ST-Link GDB Server), STM32CubeProgrammer hoặc OpenOCD.

---

## 🖥️ Hướng Dẫn Sử Dụng với Serial Terminal

1. Cắm cáp USB vào cổng micro-USB (USB OTG FS) trên kit STM32F407.
2. Mở trình quản lý thiết bị (**Device Manager**), kiểm tra cổng COM ảo (Virtual COM Port / STMicroelectronics Virtual COM Port).
3. Mở phần mềm Terminal (Hercules, Tera Term, PuTTY, hoặc VS Code Serial Monitor):
   - Chọn đúng cổng **COM**.
   - Baudrate: `115200` (hoặc bất kỳ do giao thức USB CDC không phụ thuộc baudrate vật lý).
   - Data bits: `8`, Parity: `None`, Stop bit: `1`.
4. Quan sát thông điệp lời chào gửi lên mỗi 3 giây:
   ```text
   Hello CE224.R12, this is USB CDC
   Do you want to turn on or off light
   ```
5. Gửi các lệnh kiểm tra:
   - `Turn on LED 1` -> LED 1 sáng, terminal phản hồi xác nhận và lưu vào Flash.
   - `Turn on LED 2` -> LED 2 sáng.
   - `Turn off LED 1` -> LED 1 tắt.
   - `Show log` -> Hiển thị toàn bộ lịch sử các lệnh đã lưu trong Flash.
   - `Clear log` -> Xóa dữ liệu log trong Flash.
