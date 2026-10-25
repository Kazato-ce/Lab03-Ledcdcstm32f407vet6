/**
 * app_cdc.c - Lab02 CE224.R12
 *
 * - Gui loi chao qua USB CDC (dinh ky)
 * - Nhan lenh "Turn on/off LED 1/2" tu may tinh -> bat/tat LED
 * - Moi lenh da thuc hien duoc luu NOI TIEP vao SPI Flash (thu vien nimaltd/spif)
 * - Lenh them (tuy chon): "Show log" (doc lai flash), "Clear log" (xoa log)
 *
 * Dat file nay vao Core/Src/ va them vao CMakeLists.txt (xem bao cao).
 *
 * Ten chan trong CubeMX (User Label): LED1, LED2, FLASH_CS
 *   -> main.h tu sinh LED1_Pin, LED1_GPIO_Port, LED2_..., FLASH_CS_...
 */
#include "app_cdc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "spi.h"          /* hspi1 (CubeMX: Generate peripheral init as pair of .c/.h) */
#include "spif.h"         /* thu vien nimaltd/spif */
#include "usb_device.h"   /* hUsbDeviceFS */
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

/* ===================== CAU HINH ===================== */
/* LED noi kieu active-low (board F407VET6 Black: PA6, PA7). Doi sang GPIO_PIN_SET neu active-high */
#define LED_ON_LEVEL        GPIO_PIN_RESET
#define LED_OFF_LEVEL       ((LED_ON_LEVEL == GPIO_PIN_RESET) ? GPIO_PIN_SET : GPIO_PIN_RESET)

#define GREETING_PERIOD_MS  3000u            /* chu ky gui loi chao */
#define RX_IDLE_TIMEOUT_MS  100u             /* khong co CR/LF: coi la het lenh sau 100 ms */
#define TX_TIMEOUT_MS       20u

#ifndef SPIF_PAGE_SIZE
#define SPIF_PAGE_SIZE      256u
#endif
#ifndef SPIF_SECTOR_SIZE
#define SPIF_SECTOR_SIZE    4096u
#endif
#define LOG_BASE_ADDR       0u               /* log bat dau o dia chi 0 cua flash */
#define LOG_SECTORS         4u               /* vung log = 4 sector = 16 KB */
#define LOG_SIZE            (LOG_SECTORS * SPIF_SECTOR_SIZE)

#define RX_RING_SIZE        256u             /* phai la luy thua cua 2 */
#define LINE_MAX_LEN        64u
#define TX_BUF_SIZE         160u

static const char GREETING[] =
    "Hello CE224.R12, this is USB CDC\r\n"
    "Do you want to turn on or off light\r\n";

/* ===================== BIEN TOAN CUC ===================== */
static SPIF_HandleTypeDef spif;
static bool     flash_ok;
static uint32_t log_end;      /* dia chi byte trong tiep theo de ghi */
static uint32_t log_count;    /* so dong da luu */

static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head;   /* ghi boi ngat USB */
static volatile uint32_t rx_tail;   /* doc boi main loop */

static char     line[LINE_MAX_LEN];
static uint32_t line_len;
static uint32_t last_rx_tick;
static uint32_t last_greet_tick;

static uint8_t  tx_buf[TX_BUF_SIZE];

/* ===================== LED ===================== */
static void led_write(uint8_t led, bool on)
{
    GPIO_PinState level = on ? LED_ON_LEVEL : LED_OFF_LEVEL;
    if (led == 1u) {
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, level);
    } else {
        HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, level);
    }
}

/* ===================== USB CDC TX ===================== */
static bool cdc_tx(const void *data, uint16_t len)
{
    uint32_t t0 = HAL_GetTick();
    USBD_CDC_HandleTypeDef *hcdc;

    if (len == 0u || len > TX_BUF_SIZE) {
        return false;
    }
    for (;;) {
        hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
        if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED || hcdc == NULL) {
            return false;                      /* chua cam / chua enumerate xong */
        }
        if (hcdc->TxState == 0u) {
            break;                             /* lan truyen truoc da xong */
        }
        if ((HAL_GetTick() - t0) > TX_TIMEOUT_MS) {
            return false;                      /* host khong doc (chua mo cong COM) */
        }
    }
    memcpy(tx_buf, data, len);                 /* CDC_Transmit_FS khong copy -> dung buffer tinh */
    return CDC_Transmit_FS(tx_buf, len) == USBD_OK;
}

static void cdc_puts(const char *s)
{
    size_t n = strlen(s);
    while (n > 0u) {                           /* chia nho neu chuoi dai hon TX_BUF_SIZE */
        uint16_t chunk = (n > TX_BUF_SIZE) ? TX_BUF_SIZE : (uint16_t)n;
        if (!cdc_tx(s, chunk)) {
            return;
        }
        s += chunk;
        n -= chunk;
    }
}

/* ===================== LOG TRONG SPI FLASH ===================== */

/* Ghi mot khoi du lieu, tu chia theo ranh gioi page 256 byte truoc khi goi SPIF_WriteAddress */
static bool flash_write(uint32_t addr, const uint8_t *data, uint32_t size)
{
    while (size > 0u) {
        uint32_t room  = SPIF_PAGE_SIZE - (addr % SPIF_PAGE_SIZE);
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

/* Xoa toan bo vung log (SPIF_EraseSector nhan SO THU TU sector, khong phai dia chi) */
static bool log_erase(void)
{
    for (uint32_t s = 0; s < LOG_SECTORS; s++) {
        if (!SPIF_EraseSector(&spif, (LOG_BASE_ADDR / SPIF_SECTOR_SIZE) + s)) {
            return false;
        }
    }
    log_end   = LOG_BASE_ADDR;
    log_count = 0;
    return true;
}

/* Quet tu dau vung log den byte 0xFF dau tien (flash da xoa = 0xFF) -> vi tri ghi tiep.
 * Nho vay khong can luu con tro rieng, mat dien van tim lai duoc. */
static void log_scan(void)
{
    uint8_t buf[64];

    log_end   = LOG_BASE_ADDR;
    log_count = 0;
    for (uint32_t addr = LOG_BASE_ADDR; addr < LOG_BASE_ADDR + LOG_SIZE; addr += sizeof(buf)) {
        if (!SPIF_ReadAddress(&spif, addr, buf, sizeof(buf))) {
            flash_ok = false;
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
    log_end = LOG_BASE_ADDR + LOG_SIZE;        /* day */
}

/* Them 1 dong vao cuoi log; tra ve true neu ghi + doc lai khop */
static bool log_append(const char *text)
{
    char     rec[LINE_MAX_LEN + 4];
    uint8_t  verify[LINE_MAX_LEN + 4];
    uint32_t len;

    if (!flash_ok) {
        return false;
    }
    len = (uint32_t)snprintf(rec, sizeof(rec), "%s\r\n", text);

    if (log_end + len > LOG_BASE_ADDR + LOG_SIZE) {    /* het cho -> xoa vung log, ghi lai tu dau */
        if (!log_erase()) {
            return false;
        }
    }
    if (!flash_write(log_end, (const uint8_t *)rec, len)) {
        return false;
    }
    if (!SPIF_ReadAddress(&spif, log_end, verify, len) || memcmp(verify, rec, len) != 0) {
        return false;
    }
    log_end += len;
    log_count++;
    return true;
}

static void log_show(void)
{
    uint8_t  buf[64 + 1];
    uint32_t addr = LOG_BASE_ADDR;
    char     head[48];

    if (!flash_ok) {
        cdc_puts("FLASH not available\r\n");
        return;
    }
    snprintf(head, sizeof(head), "--- FLASH LOG (%lu records) ---\r\n", (unsigned long)log_count);
    cdc_puts(head);
    while (addr < log_end) {
        uint32_t n = log_end - addr;
        if (n > 64u) {
            n = 64u;
        }
        if (!SPIF_ReadAddress(&spif, addr, buf, n)) {
            cdc_puts("FLASH read error\r\n");
            return;
        }
        cdc_tx(buf, (uint16_t)n);
        addr += n;
    }
    cdc_puts("--- END ---\r\n");
}

/* ===================== XU LY LENH ===================== */
typedef struct {
    const char *match;       /* chuoi so khop (chu thuong, 1 dau cach) */
    const char *canonical;   /* chuoi luu vao flash / phan hoi */
    uint8_t     led;         /* 1 hoac 2 */
    bool        on;
} led_cmd_t;

static const led_cmd_t k_cmds[] = {
    { "turn on led 1",  "Turn on LED 1",  1u, true  },
    { "turn on led 2",  "Turn on LED 2",  2u, true  },
    { "turn off led 1", "Turn off LED 1", 1u, false },
    { "turn off led 2", "Turn off LED 2", 2u, false },
};

/* chu thuong + bo khoang trang thua o dau/cuoi/giua */
static void normalize(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    bool   pending_space = false;

    for (; *in != '\0' && o + 1u < out_size; in++) {
        char c = *in;
        if (c == ' ' || c == '\t') {
            pending_space = (o > 0u);
            continue;
        }
        if (pending_space) {
            out[o++] = ' ';
            pending_space = false;
            if (o + 1u >= out_size) {
                break;
            }
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
    line_len = 0;
    normalize(line, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
        if (strcmp(norm, k_cmds[i].match) == 0) {
            led_write(k_cmds[i].led, k_cmds[i].on);             /* 1) thuc hien lenh */
            bool saved = log_append(k_cmds[i].canonical);       /* 2) luu lenh vao flash */
            if (saved) {
                snprintf(reply, sizeof(reply), "OK: %s | saved to flash (record #%lu)\r\n",
                         k_cmds[i].canonical, (unsigned long)log_count);
            } else {
                snprintf(reply, sizeof(reply), "OK: %s | FLASH SAVE FAILED\r\n", k_cmds[i].canonical);
            }
            cdc_puts(reply);
            return;
        }
    }
    if (strcmp(norm, "show log") == 0) {
        log_show();
        return;
    }
    if (strcmp(norm, "clear log") == 0) {
        cdc_puts((flash_ok && log_erase()) ? "OK: log cleared\r\n" : "ERROR: cannot clear log\r\n");
        return;
    }
    cdc_puts("ERROR: unknown command. Use: Turn on/off LED 1/2\r\n");
}

/* ===================== API ===================== */
void APP_CDC_OnReceive(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next = (rx_head + 1u) & (RX_RING_SIZE - 1u);
        if (next == rx_tail) {
            break;                             
        }
        rx_ring[rx_head] = buf[i];
        rx_head = next;
    }
}

void APP_CDC_Init(void)
{
    led_write(1u, false);
    led_write(2u, false);
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);

    flash_ok = SPIF_Init(&spif, &hspi1, FLASH_CS_GPIO_Port, FLASH_CS_Pin);
    if (flash_ok) {
        log_scan();
    }
    last_greet_tick = HAL_GetTick();
}

void APP_CDC_Process(void)
{
    /* 1) lay byte tu ring buffer, ghep thanh dong lenh */
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
            line_len = 0u;                      /* qua dai -> bo */
            cdc_puts("ERROR: line too long\r\n");
        }
    }

    /* 2) terminal khong gui CR/LF (vi du script gui nguyen chuoi): het thoi gian cho thi xu ly luon */
    if (line_len > 0u && (HAL_GetTick() - last_rx_tick) > RX_IDLE_TIMEOUT_MS) {
        process_line();
    }

    /* 3) gui loi chao dinh ky */
    if ((HAL_GetTick() - last_greet_tick) >= GREETING_PERIOD_MS) {
        last_greet_tick = HAL_GetTick();
        cdc_puts(GREETING);
    }
}
