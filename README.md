# Lab03 - Điều Khiển LED qua USB CDC và Lưu Trữ SPI Flash (STM32F407VET6)

Dự án cấu hình và lập trình vi điều khiển **STM32F407VET6** kết hợp giữa giao tiếp **USB CDC (Virtual COM Port)** và bộ nhớ ngoài **SPI Flash (W25Qxx)**. Hệ thống nhận lệnh điều khiển bật/tắt LED từ máy tính, phản hồi trạng thái, ghi nhận nhật ký nối tiếp (sequential logging) và cho phép lưu chuỗi dữ liệu tùy ý vào Flash.

---

## 📌 Tính Năng Nổi Bật

- **Giao tiếp USB CDC**:
  - Giao tiếp 2 chiều qua cổng COM ảo (Virtual COM Port) sử dụng ngoại vi USB OTG FS (Device mode).
  - Tự động nhận diện trạng thái kết nối và gửi thông báo khởi động cùng trạng thái SPI Flash khi vừa kết nối USB.
- **Điều khiển LED & Giao diện dòng lệnh (CLI)**:
  - Nhận diện lệnh không phân biệt hoa thường và chuẩn hóa khoảng trắng:
    - `Turn on LED 1` / `Turn off LED 1`
    - `Turn on LED 2` / `Turn off LED 2`
  - Hỗ trợ lệnh trợ giúp: `help` hoặc `?` hiển thị toàn bộ tập lệnh.
  - Hỗ trợ lệnh kiểm tra trạng thái: `status` hoặc `info` hiển thị chi tiết phần cứng.
- **Lưu trữ & Quản lý SPI Flash**:
  - Tự động quét và nhận diện cấu hình chân SPI / CS (hỗ trợ cả W25Q16 tích hợp sẵn trên board STM32F407VET6 Black Board chân CS: `PB0`, và module ngoài CS: `PB14`, `PA4`, `PB12`).
  - **Ghi chuỗi tùy ý**: Cú pháp `write <nội dung>` lưu dữ liệu bất kỳ xuống Flash.
  - **Sequential Logging**: Mỗi lệnh hợp lệ được tự động lưu nối tiếp vào Flash kèm xác thực toàn vẹn (verify sau khi ghi).
  - **Tự động khôi phục vị trí log**: Khi khởi động lại hoặc mất điện, vi điều khiển tự động quét tìm byte trống `0xFF` đầu tiên để tiếp tục ghi, không làm mất hay ghi đè log cũ.
  - `Show log` / `read`: Đọc và hiển thị toàn bộ lịch sử các bản ghi đã lưu trong Flash lên Terminal.
  - `Clear log` / `erase`: Xóa các sector log trong Flash để bắt đầu chu kỳ ghi mới.
- **Video demo**: Video thực nghiệm chạy thực tế được lưu tại thư mục `Video/Videodemo.mp4`.

---

## ⚙️ Cấu Hình Phần Cứng (STM32F407VET6 Black Board)

| Ngoại vi / Thiết bị | Chân vi điều khiển | Chức năng / Ghi chú |
| :--- | :--- | :--- |
| **LED 1** | `PA6` | Output Push-Pull, Active Low (kéo mức 0 là bật) |
| **LED 2** | `PA7` | Output Push-Pull, Active Low (kéo mức 0 là bật) |
| **Onboard Flash CS** | `PB0` | Chip Select cho SPI Flash W25Q16 có sẵn trên kit |
| **SPI1 Bus** | `PB3` (SCK), `PB4` (MISO), `PB5` (MOSI) | Giao tiếp SPI Flash chính |
| **Chân CS dự phòng** | `PB14`, `PA4`, `PB12` | Hỗ trợ tương thích module cắm ngoài |
| **USB OTG FS DM** | `PA11` | USB D- |
| **USB OTG FS DP** | `PA12` | USB D+ |
| **HSE Clock** | 25 MHz External Crystal | Nhân tần PLL cấp xung cho Core và USB (48 MHz) |

---

## 📂 Cấu Trúc Thư Mục Dự Án

```text
LEDCDCFLASH/
├── Core/
│   ├── Inc/
│   │   ├── app_cdc.h          # Header logic điều khiển CDC & SPI Flash
│   │   ├── main.h             # Cấu hình chân GPIO & định nghĩa hệ thống
│   │   ├── spi.h              # Header SPI
│   │   └── spif.h             # Header thư viện driver W25Qxx SPI Flash
│   └── Src/
│       ├── app_cdc.c          # Xử lý lệnh CDC, bộ đệm vòng (ring buffer) & log Flash
│       ├── main.c             # Khởi tạo Clock, GPIO, SPI, USB
│       ├── spif.c             # Driver giao tiếp W25Qxx Flash
│       └── stm32f4xx_hal_msp.c# Cấu hình phần cứng ngoại vi HAL (SPI, GPIO, USB)
├── Drivers/                   # STM32F4xx HAL Driver & CMSIS
├── Middlewares/               # ST USB Device Library (CDC Class)
├── USB_DEVICE/                # Cấu hình lớp USB CDC (usbd_cdc_if,...)
├── Video/
│   └── Videodemo.mp4          # Video minh họa hoạt động của mạch
├── CMakeLists.txt             # File cấu hình build với CMake (tự sinh file .bin & .hex)
├── LEDCDCFLASH.ioc            # File cấu hình STM32CubeMX
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

CMake đã được cấu hình tự động trích xuất file `.hex` và `.bin` trong thư mục `build/`.

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
4. Khi kết nối, mạch sẽ chào và báo trạng thái khởi tạo Flash:
   ```text
   Hello CE224.R12, this is USB CDC
   Do you want to turn on or off light

   [SPIF] Flash W25Qxx OK! (SPI1 (PB3/PB4/PB5, CS: PB0) [Onboard W25Q16])
   [SPIF] Log count: 0 records, next addr: 0x0000
   ```
5. Danh sách các lệnh điều khiển:
   - `Turn on LED 1` -> Bật LED 1 và lưu log vào Flash.
   - `Turn off LED 1` -> Tắt LED 1 và lưu log vào Flash.
   - `Turn on LED 2` -> Bật LED 2 và lưu log vào Flash.
   - `Turn off LED 2` -> Tắt LED 2 và lưu log vào Flash.
   - `write <nội dung>` -> Ghi chuỗi văn bản bất kỳ vào Flash.
   - `Show log` / `read` -> Hiển thị toàn bộ lịch sử các bản ghi trong Flash.
   - `Clear log` / `erase` -> Xóa sạch vùng nhớ log trong Flash.
   - `status` / `info` -> Kiểm tra trạng thái chi tiết của Flash và 2 LED.
   - `help` / `?` -> Hiển thị bảng trợ giúp lệnh.

