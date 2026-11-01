/**
 * crid_oled.c — 0.42" OLED (SSD1306) I2C 驱动
 *
 * 面板特性（ABROBOT ESP32-C3 板载）：
 *   GDDRAM 仍按 128x64 组织，可见窗口为 72x40，起始偏移因批次而异。
 * 实现要点：
 *   - 缓冲覆盖 GDDRAM 全 8 页；绘制时 y 加当前 y 偏移，落在哪页就存哪页，
 *     刷屏时按同一偏移算出页窗口，两者永远一致。
 *   - 上电与每次改偏移都整片擦除 GDDRAM（128x64），否则窗口移走后
 *     露出的未初始化 RAM 会显示为花屏。
 *   - 偏移/对比度均可在运行时调整，便于不同批次面板现场校准。
 */

#include "crid_oled.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "OLED";

/* 5x7 点阵字模：列优先，每字符 5 字节，覆盖 ASCII 0x20~0x7E */
static const uint8_t font5x7[] = {
    0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00,
    0x14,0x7F,0x14,0x7F,0x14, 0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62,
    0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00, 0x00,0x1C,0x22,0x41,0x00,
    0x00,0x41,0x22,0x1C,0x00, 0x14,0x08,0x3E,0x08,0x14, 0x08,0x08,0x3E,0x08,0x08,
    0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x60,0x60,0x00,0x00,
    0x20,0x10,0x08,0x04,0x02, 0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00,
    0x42,0x61,0x51,0x49,0x46, 0x21,0x41,0x45,0x4B,0x31, 0x18,0x14,0x12,0x7F,0x10,
    0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x30, 0x01,0x71,0x09,0x05,0x03,
    0x36,0x49,0x49,0x49,0x36, 0x06,0x49,0x49,0x29,0x1E, 0x00,0x36,0x36,0x00,0x00,
    0x00,0x56,0x36,0x00,0x00, 0x08,0x14,0x22,0x41,0x00, 0x14,0x14,0x14,0x14,0x14,
    0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x51,0x09,0x06, 0x32,0x49,0x79,0x41,0x3E,
    0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
    0x7F,0x41,0x41,0x22,0x1C, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01,
    0x3E,0x41,0x49,0x49,0x7A, 0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00,
    0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41, 0x7F,0x40,0x40,0x40,0x40,
    0x7F,0x02,0x0C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
    0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46,
    0x46,0x49,0x49,0x49,0x31, 0x01,0x01,0x7F,0x01,0x01, 0x3F,0x40,0x40,0x40,0x3F,
    0x1F,0x20,0x40,0x20,0x1F, 0x7F,0x20,0x18,0x20,0x7F, 0x63,0x14,0x08,0x14,0x63,
    0x07,0x08,0x70,0x08,0x07, 0x61,0x51,0x49,0x45,0x43, 0x00,0x7F,0x41,0x41,0x00,
    0x02,0x04,0x08,0x10,0x20, 0x00,0x41,0x41,0x7F,0x00, 0x04,0x02,0x01,0x02,0x04,
    0x40,0x40,0x40,0x40,0x40, 0x00,0x01,0x02,0x04,0x00, 0x20,0x54,0x54,0x54,0x78,
    0x7F,0x48,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x20, 0x38,0x44,0x44,0x48,0x7F,
    0x38,0x54,0x54,0x54,0x18, 0x08,0x7E,0x09,0x01,0x02, 0x0C,0x52,0x52,0x52,0x3E,
    0x7F,0x08,0x04,0x04,0x78, 0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x44,0x3D,0x00,
    0x7F,0x10,0x28,0x44,0x00, 0x00,0x41,0x7F,0x40,0x00, 0x7C,0x04,0x18,0x04,0x78,
    0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38, 0x7C,0x14,0x14,0x14,0x08,
    0x08,0x14,0x14,0x18,0x7C, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x20,
    0x04,0x3F,0x44,0x40,0x20, 0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C,
    0x3C,0x40,0x30,0x40,0x3C, 0x44,0x28,0x10,0x28,0x44, 0x0C,0x50,0x50,0x50,0x3C,
    0x44,0x64,0x54,0x4C,0x44, 0x00,0x08,0x36,0x41,0x00, 0x00,0x00,0x7F,0x00,0x00,
    0x00,0x41,0x36,0x08,0x00, 0x08,0x08,0x2A,0x1C,0x08,
};

#define FONT_W      5
#define FONT_H      7
#define FONT_GAP    1
#define CHAR_ADV    (FONT_W + FONT_GAP)  /* 6 px */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool      s_ready   = false;
static uint8_t   s_buf[CRID_OLED_BUFSZ];
static int       s_xoff    = CRID_OLED_X_OFFSET;
static int       s_yoff    = CRID_OLED_Y_OFFSET;
static bool      s_test_lock = false;  /* 校准图案期间暂停屏幕刷新任务 */

static void oled_clear_gddram(void);   /* 整片擦除 GDDRAM，init 早于其定义调用 */

/* 帧缓冲互斥：屏幕刷新任务与 HTTP 校准接口会并发改 s_buf，
   没有这把锁会出现"边框画上去了、中间却残留上一帧文字"的撕裂帧。 */
static SemaphoreHandle_t s_mutex = NULL;

/* ---------------- I2C 底层 ---------------- */

static esp_err_t oled_cmd(uint8_t c)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = {0x00, c};   /* 0x00 = 后续字节为命令 */
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static esp_err_t oled_cmd2(uint8_t c, uint8_t v)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t b[3] = {0x00, c, v};
    return i2c_master_transmit(s_dev, b, 3, 100);
}

static esp_err_t oled_data(const uint8_t *d, size_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    uint8_t tmp[33];
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > 32) chunk = 32;
        tmp[0] = 0x40;          /* 0x40 = 后续字节为显存数据 */
        memcpy(tmp + 1, d + off, chunk);
        esp_err_t e = i2c_master_transmit(s_dev, tmp, chunk + 1, 100);
        if (e != ESP_OK) return e;
        off += chunk;
    }
    return ESP_OK;
}

/* ---------------- 初始化 ---------------- */

static void oled_hw_init(void)
{
    oled_cmd(0xAE);             /* display off */
    oled_cmd2(0xD5, 0x80);      /* clock divide ratio */
    oled_cmd2(0xA8, 0x3F);      /* multiplex ratio = 64（保持完整 GDDRAM 扫描） */
    oled_cmd2(0xD3, 0x00);      /* display offset = 0 */
    oled_cmd(0x40);             /* start line = 0 */
    oled_cmd2(0x8D, 0x14);      /* charge pump on */
    oled_cmd2(0x20, 0x00);      /* horizontal addressing mode */
    oled_cmd(0xA1);             /* segment remap */
    oled_cmd(0xC8);             /* COM scan direction */
    oled_cmd2(0xDA, 0x12);      /* COM pins: alternative config */
    oled_cmd2(0x81, 0xCF);      /* contrast */
    oled_cmd2(0xD9, 0xF1);      /* pre-charge period */
    oled_cmd2(0xDB, 0x40);       /* VCOMH deselect */
    oled_cmd(0xA4);             /* resume to RAM content */
    oled_cmd(0xA6);             /* normal display */
    oled_cmd(0xAF);             /* display on */

    vTaskDelay(pdMS_TO_TICKS(20));
}

esp_err_t crid_oled_init(int sda_gpio, int scl_gpio, uint8_t i2c_addr)
{
    if (s_ready) return ESP_OK;

    i2c_master_bus_config_t buscfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)sda_gpio,
        .scl_io_num = (gpio_num_t)scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t e = i2c_new_master_bus(&buscfg, &s_bus);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus init failed: %s", esp_err_to_name(e));
        s_bus = NULL;
        return e;
    }

    i2c_device_config_t devcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 100000,
    };

    e = i2c_master_bus_add_device(s_bus, &devcfg, &s_dev);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "i2c device add failed: %s", esp_err_to_name(e));
        s_dev = NULL;
        return e;
    }

    /* 探测目标地址是否真的挂在总线上 */
    e = i2c_master_probe(s_bus, i2c_addr, 200);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no SSD1306 at 0x%02X (%s), display disabled",
                 i2c_addr, esp_err_to_name(e));
        /* 只在探测失败时才全总线扫描，便于现场确认实际地址 */
        ESP_LOGI(TAG, "I2C scan on SDA=%d SCL=%d:", sda_gpio, scl_gpio);
        for (int a = 0x03; a <= 0x77; a++) {
            if (i2c_master_probe(s_bus, (uint16_t)a, 20) == ESP_OK) {
                ESP_LOGI(TAG, "  device found at 0x%02X", a);
            }
        }
        s_dev = NULL;
        return e;
    }

    /* 确认能真正写入数据（探测成功不代表传输正常） */
    esp_err_t tx = oled_cmd(0xAE);
    if (tx != ESP_OK) {
        ESP_LOGE(TAG, "probe ok but write failed: %s", esp_err_to_name(tx));
        s_dev = NULL;
        return tx;
    }

    oled_hw_init();
    memset(s_buf, 0, sizeof(s_buf));
    oled_clear_gddram();   /* 清掉上电随机数据 */
    crid_oled_flush();

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    s_ready = true;
    ESP_LOGI(TAG, "SSD1306 ready: %dx%d @0x%02X (window offset %d,%d)",
             CRID_OLED_W, CRID_OLED_HEIGHT, i2c_addr, s_xoff, s_yoff);
    return ESP_OK;
}

void crid_oled_lock(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void crid_oled_unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

bool crid_oled_ready(void)
{
    return s_ready;
}

void crid_oled_set_offset(int xoff, int yoff)
{
    crid_oled_lock();
    bool moved = false;
    if (xoff >= 0 && xoff + CRID_OLED_W <= 128 && xoff != s_xoff) {
        s_xoff = xoff;
        moved = true;
    }
    if (yoff >= 0 && yoff + CRID_OLED_HEIGHT <= 64 && yoff != s_yoff) {
        s_yoff = yoff;
        moved = true;
    }
    if (moved) oled_clear_gddram();   /* 窗口移走了，旧区域要擦干净 */
    crid_oled_flush();                /* 立即按新窗口重刷，现场校准当场可见 */
    crid_oled_unlock();
}

int crid_oled_get_x_offset(void) { return s_xoff; }
int crid_oled_get_y_offset(void) { return s_yoff; }

/* ---------------- 绘制 ---------------- */

void crid_oled_clear(void)
{
    memset(s_buf, 0, sizeof(s_buf));
}

void crid_oled_pixel(int x, int y, int on)
{
    if (x < 0 || x >= CRID_OLED_W || y < 0 || y >= CRID_OLED_HEIGHT) return;

    int hy   = y + s_yoff;          /* 映射到 GDDRAM 行 */
    int page = hy >> 3;             /* 0..7 */
    int bit  = hy & 7;

    if (page < 0 || page >= CRID_OLED_PAGES) return;

    size_t idx = (size_t)page * CRID_OLED_W + x;
    if (on) s_buf[idx] |=  (uint8_t)(1u << bit);
    else    s_buf[idx] &= (uint8_t)~(1u << bit);
}

void crid_oled_text(int x, int y, const char *s, int inverse)
{
    if (!s) return;

    int cx = x;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c <= 0x7E) {
            const uint8_t *g = &font5x7[(c - 0x20) * FONT_W];
            for (int col = 0; col < FONT_W; col++) {
                uint8_t bits = g[col];
                for (int row = 0; row < FONT_H; row++) {
                    int glyph = (bits >> row) & 1;
                    /* 正常：字形亮；反显：字形镂空、背景点亮 */
                    crid_oled_pixel(cx + col, y + row, inverse ? (glyph ? 0 : 1) : glyph);
                }
            }
        }
        /* 越界或不支持的字符按空白推进，避免无限循环 */
        cx += CHAR_ADV;
        s++;
        if (cx >= CRID_OLED_W) break;
    }
}

void crid_oled_hline(int x, int y, int w, int on)
{
    for (int i = 0; i < w; i++) crid_oled_pixel(x + i, y, on);
}

void crid_oled_rect(int x, int y, int w, int h, int on)
{
    crid_oled_hline(x, y, w, on);
    crid_oled_hline(x, y + h - 1, w, on);
    for (int i = 0; i < h; i++) {
        crid_oled_pixel(x, y + i, on);
        crid_oled_pixel(x + w - 1, y + i, on);
    }
}

void crid_oled_fill(int x, int y, int w, int h, int on)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            crid_oled_pixel(x + i, y + j, on);
        }
    }
}

int crid_oled_get_pixel(int x, int y)
{
    if (x < 0 || x >= CRID_OLED_W || y < 0 || y >= CRID_OLED_HEIGHT) return 0;

    int hy   = y + s_yoff;
    int page = hy >> 3;
    int bit  = hy & 7;
    if (page < 0 || page >= CRID_OLED_PAGES) return 0;

    size_t idx = (size_t)page * CRID_OLED_W + x;
    return (s_buf[idx] >> bit) & 1;
}

esp_err_t crid_oled_dump(char *out, size_t n)
{
    crid_oled_lock();
    size_t need = (size_t)CRID_OLED_HEIGHT * (CRID_OLED_W + 1) + 1;
    if (out == NULL || n < need) {
        crid_oled_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t p = 0;
    for (int y = 0; y < CRID_OLED_HEIGHT; y++) {
        for (int x = 0; x < CRID_OLED_W; x++) {
            out[p++] = crid_oled_get_pixel(x, y) ? '#' : '.';
        }
        out[p++] = '\n';
    }
    out[p] = 0;
    crid_oled_unlock();
    return ESP_OK;
}

/* 整片擦除 GDDRAM（128x64）。
   只清可见窗口是不够的：偏移一改，窗口就移到别的页，
   原先没碰过的 RAM 里是上电随机数据，直接表现为花屏。 */
static void oled_clear_gddram(void)
{
    oled_cmd(0x21);
    oled_cmd(0);
    oled_cmd(127);
    oled_cmd(0x22);
    oled_cmd(0);
    oled_cmd(7);

    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    for (int i = 0; i < 1024 / (int)sizeof(zero); i++) {
        oled_data(zero, sizeof(zero));
    }
}

void crid_oled_flush(void)
{
    if (!s_ready && !s_dev) return;

    /* 页窗口必须由当前 y 偏移算出，否则偏移一改底部就刷不到 */
    int p0 = s_yoff >> 3;
    int p1 = (s_yoff + CRID_OLED_HEIGHT - 1) >> 3;
    if (p0 < 0) p0 = 0;
    if (p1 > CRID_OLED_PAGES - 1) p1 = CRID_OLED_PAGES - 1;
    if (p1 < p0) p1 = p0;

    /* 只刷新可见窗口，减少 I2C 数据量 */
    oled_cmd(0x21);
    oled_cmd((uint8_t)s_xoff);
    oled_cmd((uint8_t)(s_xoff + CRID_OLED_W - 1));
    oled_cmd(0x22);
    oled_cmd((uint8_t)p0);
    oled_cmd((uint8_t)p1);

    size_t n = (size_t)(p1 - p0 + 1) * CRID_OLED_W;
    oled_data(&s_buf[(size_t)p0 * CRID_OLED_W], n);
}

void crid_oled_contrast(uint8_t v)
{
    oled_cmd2(0x81, v);
}

void crid_oled_display_on(bool on)
{
    oled_cmd(on ? 0xAF : 0xAE);
}

void crid_oled_test_pattern(int mode)
{
    crid_oled_lock();

    if (mode == 0) {
        /* 退出校准：解除锁定，把屏幕交还给刷新任务 */
        s_test_lock = false;
        crid_oled_clear();
        crid_oled_flush();
        crid_oled_unlock();
        return;
    }

    /* 校准期间锁住刷新任务，否则 400 ms 后图案就被正常界面冲掉了 */
    s_test_lock = true;
    crid_oled_clear();

    if (mode == 1) {
        /* 直接填满缓冲，可见区外的内容不应出现在面板上 */
        memset(s_buf, 0xFF, sizeof(s_buf));
    } else if (mode == 2) {
        /* 只填满逻辑 72x40 区域，正常时应正好铺满整块面板 */
        for (int y = 0; y < CRID_OLED_HEIGHT; y++) {
            for (int x = 0; x < CRID_OLED_W; x++) {
                crid_oled_pixel(x, y, 1);
            }
        }
    } else if (mode == 3) {
        /* 校准图：72x40 边框 + 四角 3x3 实心块 + 中心十字
           偏移正确   -> 四角块完整且紧贴面板四角，四边边框都在
           xoff 偏大  -> 内容右移，左边框/左角块被推出面板
           xoff 偏小  -> 内容左移，右侧留出黑边 */
        const int w = CRID_OLED_W;
        const int h = CRID_OLED_HEIGHT;
        for (int x = 0; x < w; x++) {
            crid_oled_pixel(x, 0, 1);
            crid_oled_pixel(x, h - 1, 1);
        }
        for (int y = 0; y < h; y++) {
            crid_oled_pixel(0, y, 1);
            crid_oled_pixel(w - 1, y, 1);
        }
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                crid_oled_pixel(i, j, 1);
                crid_oled_pixel(w - 1 - i, j, 1);
                crid_oled_pixel(i, h - 1 - j, 1);
                crid_oled_pixel(w - 1 - i, h - 1 - j, 1);
            }
        }
        int cx = w / 2, cy = h / 2;
        for (int i = -3; i <= 3; i++) {
            crid_oled_pixel(cx + i, cy, 1);
            crid_oled_pixel(cx, cy + i, 1);
        }
    } else if (mode == 4) {
        /* 边框 + 当前偏移数值，方便一边看屏幕一边报数 */
        const int w = CRID_OLED_W;
        const int h = CRID_OLED_HEIGHT;
        for (int x = 0; x < w; x++) {
            crid_oled_pixel(x, 0, 1);
            crid_oled_pixel(x, h - 1, 1);
        }
        for (int y = 0; y < h; y++) {
            crid_oled_pixel(0, y, 1);
            crid_oled_pixel(w - 1, y, 1);
        }
        char b[16];
        snprintf(b, sizeof(b), "X%02d Y%02d", s_xoff, s_yoff);
        crid_oled_text(3, 3, "OFFSET", 0);
        crid_oled_text(3, 13, b, 0);
        crid_oled_text(3, 23, "30,12", 0);
    }

    crid_oled_flush();
    crid_oled_unlock();
}

bool crid_oled_test_locked(void)
{
    return s_test_lock;
}
