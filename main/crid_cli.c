/**
 * crid_cli.c — USB Serial/JTAG 命令行（调试用）
 *
 * 两个板子同时插在电脑上时，靠串口命令分头调测比来回切 WiFi 省事得多：
 * 每个 COM 口就是一块板，命令直接作用在那一块上。
 *
 *   stat                 一次性看模式 / 堆 / 屏幕 / 快门 / 配对状态
 *   mode rid|camera|relay 切模式
 *   shot                 拍一张（音量键）
 *   gap [n]              查看或设置拍照间隔档位
 *   type <text>          用 HID 键盘把文本打出去
 *   pair                 打印当前配对状态与配对码
 *   clearbonds           清掉本机保存的配对信息
 *   oled on|off|test|clear|dump|offset x y|contrast n
 *   scr page <n>|menu <n>
 *   reboot
 */

#include "crid_cli.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "crid_mode.h"
#include "crid_shutter.h"
#include "crid_screen.h"
#include "crid_oled.h"
#include "crid_relay.h"
#include "crid_ext.h"

static const char *TAG = "CLI";

/* ---------------- stat ---------------- */
static int cmd_stat(int argc, char **argv)
{
    (void)argc; (void)argv;

    shutter_status_t st;
    memset(&st, 0, sizeof(st));
    crid_shutter_get_status(&st);

    const char *ssid = "", *ip = "", *gw = "";
    bool napt = crid_relay_sta_info(&ssid, &ip, &gw);

    printf("mode      : %s%s\n", crid_mode_name(crid_mode_get()),
           crid_mode_is_switching() ? " (switching)" : "");
    printf("heap      : %u B (min %u)\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());
    printf("oled      : %s, page %d/%d, ui %d, xoff=%d yoff=%d\n",
           crid_oled_ready() ? "present" : "ABSENT",
           crid_screen_get_page(), crid_screen_get_page_count(),
           crid_screen_get_ui(),
           crid_oled_get_x_offset(), crid_oled_get_y_offset());
    printf("shutter   : ready=%d conn=%d run=%d gap=%lu next=%lu shots=%lu\n",
           st.ready, st.connected, st.continuous,
           (unsigned long)st.gap, (unsigned long)st.next_in,
           (unsigned long)st.shots);
    printf("pairing   : %s passkey=%06lu\n",
           crid_shutter_is_pairing() ? "WAITING" : "no",
           (unsigned long)(st.passkey % 1000000UL));
    printf("relay     : sta=%s ip=%s gw=%s napt=%d\n",
           ssid[0] ? ssid : "-", ip[0] ? ip : "-", gw[0] ? gw : "-", napt);
    return 0;
}

/* ---------------- mode ---------------- */
static int cmd_mode(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mode rid|camera|relay\n");
        return 0;
    }
    crid_mode_t t;
    if (strcmp(argv[1], "rid") == 0)          t = CRID_MODE_RID;
    else if (strcmp(argv[1], "camera") == 0)  t = CRID_MODE_CAMERA;
    else if (strcmp(argv[1], "relay") == 0)   t = CRID_MODE_RELAY;
    else {
        printf("unknown mode: %s\n", argv[1]);
        return 0;
    }
    esp_err_t e = crid_mode_request(t);
    printf("mode -> %s, rc=%s\n", crid_mode_name(t), esp_err_to_name(e));
    return 0;
}

/* ---------------- shot / gap / type ---------------- */
static int cmd_shot(int argc, char **argv)
{
    (void)argc; (void)argv;
    bool ok = crid_shutter_take_photo();
    printf("shot: %s\n", ok ? "sent" : "failed (not connected?)");
    return 0;
}

static int cmd_gap(int argc, char **argv)
{
    if (argc < 2) {
        printf("gap = %lu s (%s)\n",
               (unsigned long)crid_shutter_get_gap(),
               crid_shutter_get_gap() == 0 ? "manual" : "timed");
        for (int i = 0; i < SHUTTER_GAP_N; i++) {
            printf("  %s%lu", i ? " " : "", (unsigned long)crid_shutter_gaps[i]);
        }
        printf("\n");
        return 0;
    }
    if (strcmp(argv[1], "next") == 0) {
        printf("gap -> %lu s\n", (unsigned long)crid_shutter_gap_next());
        return 0;
    }
    uint32_t g = (uint32_t)strtoul(argv[1], NULL, 10);
    crid_shutter_set_gap(g);
    printf("gap -> %lu s\n", (unsigned long)crid_shutter_get_gap());
    return 0;
}

static int cmd_type(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: type <text>\n");
        return 0;
    }
    int n = crid_shutter_type_text(argv[1]);
    printf("typed %d chars\n", n);
    return 0;
}

static int cmd_ble(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[192];
    crid_shutter_ble_diag(buf, sizeof(buf));
    printf("%s\n", buf);
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    (void)argc; (void)argv;
    shutter_status_t st;
    memset(&st, 0, sizeof(st));
    crid_shutter_get_status(&st);
    if (crid_shutter_is_pairing()) {
        printf("PAIRING  code=%06lu  (在手机上输入)\n",
               (unsigned long)(st.passkey % 1000000UL));
    } else {
        printf("not pairing (last code %06lu, connected=%d)\n",
               (unsigned long)(st.passkey % 1000000UL), st.connected);
    }
    return 0;
}

static int cmd_clearbonds(int argc, char **argv)
{
    (void)argc; (void)argv;
    int rc = crid_shutter_clear_bonds();
    printf("bonds cleared, rc=%d\n", rc);
    return 0;
}

/* ---------------- oled ---------------- */
static int cmd_oled(int argc, char **argv)
{
    if (!crid_oled_ready()) {
        printf("no OLED on this board\n");
        return 0;
    }
    if (argc < 2) {
        printf("usage: oled on|off|test|clear|dump|offset x y|contrast n\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        crid_screen_set_on(true);
    } else if (strcmp(argv[1], "off") == 0) {
        crid_screen_set_on(false);
    } else if (strcmp(argv[1], "test") == 0) {
        crid_oled_test_pattern(3);
    } else if (strcmp(argv[1], "clear") == 0) {
        crid_oled_test_pattern(0);
    } else if (strcmp(argv[1], "dump") == 0) {
        char *art = malloc(3200);
        if (!art) { printf("oom\n"); return 0; }
        if (crid_oled_dump(art, 3200) == ESP_OK) printf("%s\n", art);
        free(art);
    } else if (strcmp(argv[1], "offset") == 0 && argc >= 4) {
        crid_oled_set_offset(atoi(argv[2]), atoi(argv[3]));
        printf("offset -> %d,%d\n", crid_oled_get_x_offset(), crid_oled_get_y_offset());
    } else if (strcmp(argv[1], "contrast") == 0 && argc >= 3) {
        crid_oled_contrast((uint8_t)strtoul(argv[2], NULL, 10));
    } else {
        printf("usage: oled on|off|test|clear|dump|offset x y|contrast n\n");
    }
    return 0;
}

/* ---------------- scr ---------------- */
static int cmd_scr(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: scr page <n> | scr menu <n>\n");
        return 0;
    }
    if (strcmp(argv[1], "page") == 0) {
        crid_screen_set_page(atoi(argv[2]));
    } else if (strcmp(argv[1], "menu") == 0) {
        crid_screen_set_ui(atoi(argv[2]));
    } else {
        printf("usage: scr page <n> | scr menu <n>\n");
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

/* ---------------- 注册 ---------------- */
void crid_cli_init(void)
{
    esp_console_repl_t *repl = NULL;

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "rid>";
    repl_cfg.max_cmdline_length = 160;
    repl_cfg.max_history_len = 8;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_console_register_help_command();

    const esp_console_cmd_t cmds[] = {
        { .command = "stat",  .help = "模式/堆/屏幕/快门/配对状态", .func = cmd_stat },
        { .command = "mode",  .help = "mode rid|camera|relay",      .func = cmd_mode },
        { .command = "shot",  .help = "拍一张（音量键）",           .func = cmd_shot },
        { .command = "gap",   .help = "gap [n|next]",               .func = cmd_gap },
        { .command = "type",  .help = "type <text> 键盘输入",       .func = cmd_type },
        { .command = "ble",   .help = "BLE/HID 诊断快照",           .func = cmd_ble },
        { .command = "pair",  .help = "显示配对码",                 .func = cmd_pair },
        { .command = "clearbonds", .help = "清除配对记录",          .func = cmd_clearbonds },
        { .command = "oled",  .help = "oled on|off|test|clear|dump|offset x y|contrast n",
                                                                    .func = cmd_oled },
        { .command = "scr",   .help = "scr page|menu <n>",          .func = cmd_scr },
        { .command = "reboot",.help = "重启",                       .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }

    esp_err_t e = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "console repl unavailable: %s", esp_err_to_name(e));
        return;
    }
    esp_console_start_repl(repl);
    ESP_LOGI(TAG, "USB console ready (try 'help')");
}
