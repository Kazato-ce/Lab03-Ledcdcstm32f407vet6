/**
 * @file    app_cdc.c
 * @brief   USB CDC communication and SPI W25Q16 Flash logging for STM32F407.
 *          Lab 03 - CE224.R12 Embedded System Design
 */

#include "app_cdc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "spi.h"          /* extern hspi2 */
#include "spif.h"         /* nimaltd/spif library */
#include "usb_device.h"   /* hUsbDeviceFS */
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

/* ===================== CẤU HÌNH PHẦN CỨNG ===================== */
/* LED active-low trên STM32F407VET6 Black Board: PA6 (D2), PA7 (D3) */
#define LED_ACTIVE_LOW      1

#if LED_ACTIVE_LOW
#define LED_ON_LEVEL        GPIO_PIN_RESET
#define LED_OFF_LEVEL       GPIO_PIN_SET
#else
#define LED_ON_LEVEL        GPIO_PIN_SET
#define LED_OFF_LEVEL       GPIO_PIN_RESET
#endif

/* Thời gian và buffer */
#define RX_IDLE_TIMEOUT_MS  100u     /* Nhận xong nếu không có thêm byte sau 100ms */
#define TX_TIMEOUT_MS       50u

#ifndef SPIF_PAGE_SIZE
#define SPIF_PAGE_SIZE      256u
#endif
#ifndef SPIF_SECTOR_SIZE
#define SPIF_SECTOR_SIZE    4096u
#endif

#define LOG_BASE_ADDR       0x000000u /* Địa chỉ bắt đầu lưu log trong Flash */
#define LOG_SECTORS         4u        /* Dành 4 sector = 16KB để lưu log */
#define LOG_SIZE            (LOG_SECTORS * SPIF_SECTOR_SIZE)

#define RX_RING_SIZE        512u      /* Kích thước bộ đệm vòng nhận (lũy thừa của 2) */
#define LINE_MAX_LEN        128u
#define TX_BUF_SIZE         256u

/* Câu chào yêu cầu */
static const char GREETING[] =
    "\r\nHello CE224.R12, this is USB CDC LED FLASH\r\n"
    "Do you want to turn on or off light\r\n";

/* ===================== BIẾN TOÀN CỤC ===================== */
static SPIF_HandleTypeDef spif;
static bool        flash_ok = false;
static const char *active_spi_name = "None";
static uint32_t    log_end = LOG_BASE_ADDR;  /* Địa chỉ byte trống tiếp theo để ghi nối tiếp */
static uint32_t    log_count = 0;             /* Số dòng lệnh đã lưu */

static GPIO_TypeDef *active_cs_port = FLASH_CS_GPIO_Port;
static uint16_t      active_cs_pin  = FLASH_CS_Pin;

static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head = 0;
static volatile uint32_t rx_tail = 0;

static char     line[LINE_MAX_LEN];
static uint32_t line_len = 0;
static uint32_t last_rx_tick = 0;
static bool     initial_greet_done = false;

static uint8_t  tx_buf[TX_BUF_SIZE];

/* ===================== ĐIỀU KHIỂN LED ===================== */
static void led_write(uint8_t led, bool on)
{
    GPIO_PinState state = on ? LED_ON_LEVEL : LED_OFF_LEVEL;
    if (led == 1u) {
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, state);
    } else if (led == 2u) {
        HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, state);
    }
}

/* ===================== TRUYỀN DỮ LIỆU QUA USB CDC ===================== */
static bool cdc_tx(const void *data, uint16_t len)
{
    uint32_t t0 = HAL_GetTick();
    USBD_CDC_HandleTypeDef *hcdc;

    if (len == 0u || len > TX_BUF_SIZE) {
        return false;
    }

    /* Đợi USB CDC sẵn sàng và lần truyền trước hoàn tất */
    for (;;) {
        if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) {
            return false;
        }
        hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
        if (hcdc == NULL) {
            return false;
        }
        if (hcdc->TxState == 0u) {
            break; /* Kênh truyền rỗi, có thể gửi */
        }
        if ((HAL_GetTick() - t0) > TX_TIMEOUT_MS) {
            return false;
        }
    }

    memcpy(tx_buf, data, len);
    return CDC_Transmit_FS(tx_buf, len) == USBD_OK;
}

static void cdc_puts(const char *s)
{
    size_t n = strlen(s);
    while (n > 0u) {
        uint16_t chunk = (n > (TX_BUF_SIZE - 1u)) ? (TX_BUF_SIZE - 1u) : (uint16_t)n;
        if (!cdc_tx(s, chunk)) {
            return;
        }
        s += chunk;
        n -= chunk;
    }
}

/* ===================== THAO TÁC FLASH W25Q16 ===================== */

/* Ghi khối dữ liệu vào Flash, tự động ngắt theo ranh giới trang (256 bytes) */
static bool flash_write(uint32_t addr, const uint8_t *data, uint32_t size)
{
    while (size > 0u) {
        uint32_t room = SPIF_PAGE_SIZE - (addr % SPIF_PAGE_SIZE);
        uint32_t chunk = (size < room) ? size : room;
        if (!SPIF_WriteAddress(&spif, addr, (uint8_t *)data, chunk)) {
            return false;
        }
        addr += chunk;
        data += chunk;
        size -= chunk;
    }
    return true;
}

/* Xóa toàn bộ vùng log (hoặc toàn chip nếu cần) */
static bool log_erase(void)
{
    if (!flash_ok) return false;

    for (uint32_t s = 0; s < LOG_SECTORS; s++) {
        uint32_t sector_num = (LOG_BASE_ADDR / SPIF_SECTOR_SIZE) + s;
        if (!SPIF_EraseSector(&spif, sector_num)) {
            return false;
        }
    }
    log_end   = LOG_BASE_ADDR;
    log_count = 0;
    return true;
}

/* Quét từ đầu vùng log đến byte 0xFF đầu tiên (Flash sau khi xóa là 0xFF)
 * -> Xác định vị trí ghi nối tiếp tiếp theo mà không bị mất sau khi Reset */
static void log_scan(void)
{
    uint8_t buf[64];

    log_end   = LOG_BASE_ADDR;
    log_count = 0;
    if (!flash_ok) return;

    /* Đọc kiểm tra 64 byte đầu tiên tại LOG_BASE_ADDR */
    if (!SPIF_ReadAddress(&spif, LOG_BASE_ADDR, buf, sizeof(buf))) {
        return;
    }
    /* Nếu byte đầu tiên chứa rác (không phải 0xFF và không phải ký tự ASCII in được)
     * -> Tự động format/xóa sạch 4 sector log để sẵn sàng ghi mới */
    if (buf[0] != 0xFFu && (buf[0] < 0x20 || buf[0] > 0x7E)) {
        log_erase();
        return;
    }

    for (uint32_t addr = LOG_BASE_ADDR; addr < LOG_BASE_ADDR + LOG_SIZE; addr += sizeof(buf)) {
        if (!SPIF_ReadAddress(&spif, addr, buf, sizeof(buf))) {
            return;
        }
        for (uint32_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] == 0xFFu) {
                log_end = addr + i;
                return;
            }
            if (buf[i] == '\n') {
                log_count++;
            }
        }
    }
    log_end = LOG_BASE_ADDR + LOG_SIZE; /* Vùng log đã đầy */
}

/* Ghi nối tiếp một dòng văn bản vào Flash; đọc lại để kiểm tra tính toàn vẹn */
static bool log_append(const char *text)
{
    char     rec[LINE_MAX_LEN + 4];
    uint8_t  verify[LINE_MAX_LEN + 4];
    uint32_t len;

    if (!flash_ok) {
        return false;
    }
    len = (uint32_t)snprintf(rec, sizeof(rec), "%s\r\n", text);

    /* Nếu vượt quá dung lượng vùng log -> xóa và ghi lại từ đầu */
    if (log_end + len > LOG_BASE_ADDR + LOG_SIZE) {
        if (!log_erase()) {
            return false;
        }
    }

    if (!flash_write(log_end, (const uint8_t *)rec, len)) {
        return false;
    }

    /* Đọc lại xác thực */
    if (!SPIF_ReadAddress(&spif, log_end, verify, len) || memcmp(verify, rec, len) != 0) {
        return false;
    }

    log_end += len;
    log_count++;
    return true;
}

/* Đọc toàn bộ dữ liệu log từ Flash và truyền qua USB CDC */
static void log_show(void)
{
    uint8_t  buf[64 + 1];
    uint32_t addr = LOG_BASE_ADDR;
    char     head[80];

    if (!flash_ok) {
        cdc_puts("[FLASH] Error: SPI Flash is not initialized or not detected!\r\n");
        return;
    }

    snprintf(head, sizeof(head), "\r\n--- FLASH LOG (%lu records, %lu bytes) ---\r\n",
             (unsigned long)log_count, (unsigned long)(log_end - LOG_BASE_ADDR));
    cdc_puts(head);

    while (addr < log_end) {
        uint32_t n = log_end - addr;
        if (n > 64u) {
            n = 64u;
        }
        if (!SPIF_ReadAddress(&spif, addr, buf, n)) {
            cdc_puts("[FLASH] Error reading from Flash\r\n");
            return;
        }
        cdc_tx(buf, (uint16_t)n);
        addr += n;
    }
    cdc_puts("--- END OF FLASH LOG ---\r\n\r\n");
}

/* ===================== XỬ LÝ LỆNH NHẬN ĐƯỢC ===================== */
typedef struct {
    const char *match;       /* Chuỗi so khớp (chữ thường, chuẩn hóa) */
    const char *canonical;   /* Chuỗi chuẩn hiển thị và lưu vào Flash */
    uint8_t     led;         /* 1 hoặc 2 */
    bool        on;
} led_cmd_t;

static const led_cmd_t k_led_cmds[] = {
    { "turn on led 1",  "Turn on LED 1",  1u, true  },
    { "turn on led 2",  "Turn on LED 2",  2u, true  },
    { "turn off led 1", "Turn off LED 1", 1u, false },
    { "turn off led 2", "Turn off LED 2", 2u, false },
};

/* Chuẩn hóa chuỗi: chuyển thành chữ thường, cắt bỏ khoảng trắng thừa đầu/cuối/giữa */
static void normalize(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    bool   pending_space = false;

    for (; *in != '\0' && o + 1u < out_size; in++) {
        char c = *in;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = (o > 0u);
            continue;
        }
        if (pending_space) {
            out[o++] = ' ';
            pending_space = false;
            if (o + 1u >= out_size) break;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

static void process_line(void)
{
    char norm[LINE_MAX_LEN];
    char reply[TX_BUF_SIZE];

    line[line_len] = '\0';
    normalize(line, norm, sizeof(norm));
    line_len = 0;

    if (norm[0] == '\0') {
        /* Nhấn Enter -> gửi lại câu chào */
        cdc_puts(GREETING);
        return;
    }

    /* 1. Lệnh Lab 2: "Hello STM" */
    if (strcmp(norm, "hello stm") == 0) {
        cdc_puts("Do you want to turn on or off light\r\n");
        return;
    }

    /* 2. So khớp 4 lệnh điều khiển LED */
    for (size_t i = 0; i < sizeof(k_led_cmds) / sizeof(k_led_cmds[0]); i++) {
        if (strcmp(norm, k_led_cmds[i].match) == 0) {
            /* 1) Bật / Tắt LED */
            led_write(k_led_cmds[i].led, k_led_cmds[i].on);

            /* 2) Ghi nối tiếp vào Flash */
            bool saved = log_append(k_led_cmds[i].canonical);

            /* 3) Phản hồi Terminal */
            if (saved) {
                snprintf(reply, sizeof(reply), "OK: %s | Saved to Flash (record #%lu, addr 0x%04lX)\r\n",
                         k_led_cmds[i].canonical, (unsigned long)log_count, (unsigned long)log_end);
            } else {
                snprintf(reply, sizeof(reply), "OK: %s | [WARNING: Flash save failed]\r\n",
                         k_led_cmds[i].canonical);
            }
            cdc_puts(reply);
            return;
        }
    }

    /* 3. Lệnh đọc dữ liệu từ Flash (Chức năng 2 đề bài) */
    if (strcmp(norm, "show log") == 0 || strcmp(norm, "read flash") == 0 ||
        strcmp(norm, "read") == 0 || strcmp(norm, "show") == 0) {
        log_show();
        return;
    }

    /* 4. Lệnh xóa dữ liệu Flash (Chức năng 3 đề bài) */
    if (strcmp(norm, "clear log") == 0 || strcmp(norm, "erase flash") == 0 ||
        strcmp(norm, "clear") == 0 || strcmp(norm, "erase") == 0) {
        if (flash_ok && log_erase()) {
            cdc_puts("OK: Flash log cleared successfully (0 records)\r\n");
        } else {
            cdc_puts("ERROR: Failed to erase Flash log\r\n");
        }
        return;
    }

    /* 5. Ghi dữ liệu bất kỳ do người dùng nhập xuống Flash (Chức năng 1 đề bài) */
    /* Cú pháp: "write <dữ liệu>" hoặc tự động lưu chuỗi bất kỳ */
    if (strncmp(norm, "write ", 6) == 0) {
        const char *custom_data = line + 6; /* Lấy phần chuỗi nguyên bản */
        while (*custom_data == ' ') custom_data++;
        if (*custom_data != '\0') {
            bool saved = log_append(custom_data);
            if (saved) {
                snprintf(reply, sizeof(reply), "OK: Custom data saved to Flash (record #%lu)\r\n",
                         (unsigned long)log_count);
            } else {
                snprintf(reply, sizeof(reply), "ERROR: Failed to save to Flash\r\n");
            }
            cdc_puts(reply);
            return;
        }
    }

    /* 6. Lệnh kiểm tra trạng thái / Status */
    if (strcmp(norm, "status") == 0 || strcmp(norm, "info") == 0) {
        snprintf(reply, sizeof(reply),
                 "\r\n=== SYSTEM STATUS ===\r\n"
                 "- Flash Detected : %s\r\n"
                 "- Interface      : %s\r\n"
                 "- Flash Capacity : %lu KB\r\n"
                 "- Log Records    : %lu\r\n"
                 "- Next Flash Addr: 0x%04lX\r\n"
                 "- LED 1 (PA6)    : %s\r\n"
                 "- LED 2 (PA7)    : %s\r\n"
                 "=====================\r\n\r\n",
                 flash_ok ? "YES (OK)" : "NO (Failed)",
                 active_spi_name,
                 flash_ok ? ((unsigned long)spif.SectorCnt * SPIF_SECTOR_SIZE / 1024u) : 0ul,
                 (unsigned long)log_count,
                 (unsigned long)log_end,
                 (HAL_GPIO_ReadPin(LED1_GPIO_Port, LED1_Pin) == LED_ON_LEVEL) ? "ON" : "OFF",
                 (HAL_GPIO_ReadPin(LED2_GPIO_Port, LED2_Pin) == LED_ON_LEVEL) ? "ON" : "OFF");
        cdc_puts(reply);
        return;
    }

    /* 7. Lệnh trợ giúp / Help */
    if (strcmp(norm, "help") == 0 || strcmp(norm, "?") == 0) {
        cdc_puts("\r\n=== CE224.R12 LAB 03 COMMAND LIST ===\r\n"
                 "- Turn on LED 1     : Bat LED 1 & Luu Flash\r\n"
                 "- Turn on LED 2     : Bat LED 2 & Luu Flash\r\n"
                 "- Turn off LED 1    : Tat LED 1 & Luu Flash\r\n"
                 "- Turn off LED 2    : Tat LED 2 & Luu Flash\r\n"
                 "- Show log / read   : Doc lai toan bo du lieu tu Flash\r\n"
                 "- Clear log / erase : Xoa toan bo log Flash\r\n"
                 "- Write <text>      : Ghi chuoi bat ky vao Flash\r\n"
                 "- Status / info     : Kiem tra trang thai Flash & LED\r\n"
                 "- Hello STM         : Tra loi cau hoi trang thai\r\n"
                 "=====================================\r\n");
        return;
    }

    /* Lệnh không xác định -> gợi ý */
    cdc_puts("ERROR: Unknown command. Type 'help' or use: Turn on/off LED 1/2, Show log, Clear log, Write <text>\r\n");
}

/* ===================== CÁC HÀM GIAO TIẾP VỚI HỆ THỐNG ===================== */

/* Gọi từ callback ngắt USB (CDC_Receive_FS) */
void APP_CDC_OnReceive(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next = (rx_head + 1u) & (RX_RING_SIZE - 1u);
        if (next == rx_tail) {
            break; /* Buffer đầy -> bỏ qua byte dư */
        }
        rx_ring[rx_head] = buf[i];
        rx_head = next;
    }
}

/* Khởi tạo ứng dụng */
void APP_CDC_Init(void)
{
    /* Tắt cả 2 LED lúc khởi động */
    led_write(1u, false);
    led_write(2u, false);

    /* Danh sách các cấu hình SPI và chân CS:
     * 1) SPI1 (PB3=SCK, PB4=MISO, PB5=MOSI) với CS=PB0 (Chuẩn trên STM32F407VET6 Black Board onboard W25Q16)
     * 2) SPI1 với CS=PB14
     * 3) SPI1 với CS=PA4
     * 4) SPI2 (PB10=SCK, PC2=MISO, PC3=MOSI) với CS=PB12 (External Header)
     * 5) SPI2 với CS=PB0
     */
    typedef struct {
        SPI_HandleTypeDef *hspi;
        GPIO_TypeDef      *cs_port;
        uint16_t           cs_pin;
        const char        *name;
    } spi_target_t;

    const spi_target_t targets[] = {
        { &hspi1, GPIOB, GPIO_PIN_0,  "SPI1 (PB3/PB4/PB5, CS: PB0) [Onboard W25Q16]" },
        { &hspi1, GPIOB, GPIO_PIN_14, "SPI1 (PB3/PB4/PB5, CS: PB14)" },
        { &hspi1, GPIOA, GPIO_PIN_4,  "SPI1 (PB3/PB4/PB5, CS: PA4)" },
        { &hspi2, GPIOB, GPIO_PIN_12, "SPI2 (PB10/PC2/PC3, CS: PB12) [External Header]" },
        { &hspi2, GPIOB, GPIO_PIN_0,  "SPI2 (PB10/PC2/PC3, CS: PB0)" },
    };

    flash_ok = false;
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        HAL_GPIO_WritePin(targets[t].cs_port, targets[t].cs_pin, GPIO_PIN_SET);
        memset(&spif, 0, sizeof(spif)); /* Đặt lại Inited = 0 để SPIF_Init cho phép thử */
        if (SPIF_Init(&spif, targets[t].hspi, targets[t].cs_port, targets[t].cs_pin)) {
            active_cs_port  = targets[t].cs_port;
            active_cs_pin   = targets[t].cs_pin;
            active_spi_name = targets[t].name;
            flash_ok        = true;
            break;
        }
    }

    if (flash_ok) {
        log_scan(); /* Tìm vị trí ghi tiếp theo trong Flash */
    }

    initial_greet_done = false;
}

/* Vòng lặp xử lý chính (gọi trong while(1) của main) */
void APP_CDC_Process(void)
{
    /* 1. Gửi lời chào khởi động khi USB CDC đã được kết nối (chỉ gửi 1 lần khi kết nối) */
    if (!initial_greet_done && hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) {
        char init_info[160];
        if (flash_ok) {
            snprintf(init_info, sizeof(init_info),
                     "[SPIF] Flash W25Qxx OK! (%s)\r\n"
                     "[SPIF] Log count: %lu records, next addr: 0x%04lX\r\n\r\n",
                     active_spi_name, (unsigned long)log_count, (unsigned long)log_end);
        } else {
            snprintf(init_info, sizeof(init_info),
                     "[SPIF] WARNING: Flash W25Qxx not detected!\r\n\r\n");
        }
        cdc_puts(GREETING);
        cdc_puts(init_info);
        initial_greet_done = true;
    } else if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) {
        initial_greet_done = false; /* Cho phép gửi lại lời chào nếu rút cắm lại cáp USB */
    }

    /* 2. Lấy từng byte từ ring buffer và ghép thành dòng lệnh */
    while (rx_tail != rx_head) {
        uint8_t c = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1u) & (RX_RING_SIZE - 1u);
        last_rx_tick = HAL_GetTick();

        if (c == '\r' || c == '\n') {
            if (line_len > 0u) {
                process_line();
            }
        } else if (line_len < LINE_MAX_LEN - 1u) {
            line[line_len++] = (char)c;
        } else {
            line_len = 0u;
            cdc_puts("ERROR: Line too long\r\n");
        }
    }

    /* 3. Trường hợp terminal không gửi ký tự xuống dòng (CR/LF) */
    if (line_len > 0u && (HAL_GetTick() - last_rx_tick) > RX_IDLE_TIMEOUT_MS) {
        process_line();
    }
}
