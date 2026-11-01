/**
 * BLE HID 快门模块（Consumer Control）
 *
 * 用 NimBLE 实现标准 HID over GATT（HOGP）设备，向手机发送
 * “音量 + / 音量 -”媒体键，触发手机相机快门。
 *
 * 服务：HID(0x1812) + Device Information(0x180A)
 * 报告：Report ID 1 -> bit0 音量+, bit1 音量-
 */

#include "crid_shutter.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* IDF 的 NimBLE 未在头文件里声明该初始化函数（上游实现带 SYSINIT 断言，此处为空宏） */
extern void ble_store_config_init(void);

static const char *TAG = "SHUTTER";

#define SHUTTER_NVS_NS      "shutter"
#define SHUTTER_DEF_NAME    "RID-Shutter"
#define SHUTTER_DEF_IVL     5
#define SHUTTER_PRESS_MS    80      /* 按键保持时间 */

/* ---------- Apple 兼容性参数（QA1931 / Bluetooth Accessory Design Guidelines） ----------
 * 广播间隔只能用这几个规定值：20ms（前 30s）/ 152.5ms / 211.25ms / 318.75ms ...
 * 单位 0.625ms：20ms -> 32，152.5ms -> 244 */
#define ADV_ITVL_FAST       32
#define ADV_ITVL_SLOW       244
#define ADV_FAST_MS         30000

/* 连接参数规则：
 *   Interval Min >= 15ms 且为 15ms 倍数；Interval Max >= Interval Min + 15ms
 *   Interval Max * (Latency + 1) <= 2s；该值 *3 < Supervision Timeout
 *   Latency <= 30；2s <= Supervision Timeout <= 6s
 * 取 15ms / 30ms / latency 0 / timeout 4s，全部满足（HID 设备 iOS 允许更低间隔）。
 * 单位：连接间隔 1.25ms，超时 10ms */
#define CONN_ITVL_MIN       12      /* 15 ms */
#define CONN_ITVL_MAX       24      /* 30 ms */
#define CONN_LATENCY        0
#define CONN_SUPERVISION    400     /* 4 s */

/* ================================================================
 * 状态
 * ================================================================ */
static bool     s_ready = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle = 0;      /* Report ID 2：消费者控制输入 */
static uint16_t s_kb_report_handle = 0;   /* Report ID 1：键盘输入 */
static uint16_t s_led_report_handle = 0;  /* Report ID 1：键盘 LED 输出 */

/* 诊断用：主机是否订阅了报告通知 / 链路是否已加密 / 实际发出多少条报告 */
static bool     s_sub_kb  = false;
static bool     s_sub_cc  = false;
static bool     s_encrypted = false;
static uint32_t s_tx_kb = 0;
static uint32_t s_tx_cc = 0;
static uint8_t  s_own_addr_type = 0;
static bool     s_adv_fast  = true;   /* 快广播（20ms）阶段；30s 后转慢广播 */

static bool     s_continuous = false;
static bool     s_paused     = false;   /* 运行态但暂停出片（任务书 §7.4） */
static uint32_t s_interval = SHUTTER_DEF_IVL;
static uint32_t s_gap = SHUTTER_GAP_MANUAL;   /* 屏幕间隔档位，0=手动 */
static TickType_t s_next_at = 0;              /* 定时连拍下一张的时刻 */
static uint8_t  s_key = SHUTTER_KEY_VOL_UP;
static uint8_t  s_alt = 0;               /* 每次拍照在 VOL+ / VOL- 之间交替 */
static uint32_t s_shots = 0;
static uint32_t s_passkey = 0;           /* 配对码（Web / 屏幕显示，手机端输入） */
static bool     s_pairing = false;       /* 正在等手机输入配对码 */
static TickType_t s_pair_at = 0;         /* 进入配对态的时刻（超时自动退出） */
#define SHUTTER_PAIR_TMO_MS  120000
static char     s_name[32] = SHUTTER_DEF_NAME;

static SemaphoreHandle_t s_tx_mutex = NULL;
static TaskHandle_t      s_task = NULL;

/* ================================================================
 * UUID
 * ================================================================ */
static const ble_uuid16_t UUID_SVC_HID          = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t UUID_SVC_DIS          = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t UUID_CHR_HID_INFO     = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t UUID_CHR_REPORT_MAP   = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t UUID_CHR_CONTROL_PT   = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t UUID_CHR_REPORT       = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t UUID_CHR_PROTOCOL     = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t UUID_CHR_MANUFACTURER = BLE_UUID16_INIT(0x2A29);
static const ble_uuid16_t UUID_CHR_PNP_ID       = BLE_UUID16_INIT(0x2A50);
static const ble_uuid16_t UUID_SVC_BAS          = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t UUID_CHR_BATT_LEVEL   = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t UUID_DSC_REPORT_REF   = BLE_UUID16_INIT(0x2908);

/* HID 报告长度。**都不带 Report ID 前缀**：报告 ID 由 Report Reference
   描述符携带，这是 HOGP 的规定，也是 iOS 唯一接受的写法。 */
#define SHUTTER_KB_REPORT_LEN   8     /* 键盘输入：[mod][res][k0..k5]   */
#define SHUTTER_LED_REPORT_LEN  1     /* 键盘输出：LED 位图             */
#define SHUTTER_CC_REPORT_LEN   2     /* 消费者控制输入：16bit usage 小端 */
#define HID_REPORT_ID_KEYBOARD  0x01
#define HID_REPORT_ID_CONSUMER  0x02
static uint8_t s_batt_level = 100;
static uint8_t s_led_state  = 0;      /* 主机回写的键盘 LED（Num/Caps…） */

/* Report Map：两个应用集合，各自声明**非零** Report ID
 *
 * iOS 的硬要求：描述符里必须出现非零 Report ID（0x85, n）。没有 ID 时 iOS
 * 认为这不是标准 HID 设备，连上也不消费 Input Report —— 表象就是"蓝牙显示
 * 已连接，但一个字也打不出来 / 快门没反应"。
 *
 * 那 Report ID 要不要出现在通知负载首字节？不要。HOGP 规定报告 ID 由
 * Report Reference 描述符携带，ATT 通知里只放报告本体。iPhone SE / iOS 26.3
 * 上打字全通过的实现就是这么做的：键盘 ID=1 载荷 8 字节、消费者 ID=2 载荷
 * 2 字节，均不带前缀；Windows / macOS / Android 同样认。 */
static const uint8_t s_report_map[] = {
    /* ---- 键盘（Report ID 0x01） ---- */
    0x05, 0x01,        /* Usage Page (Generic Desktop)     */
    0x09, 0x06,        /* Usage (Keyboard)                 */
    0xA1, 0x01,        /* Collection (Application)         */
    0x85, 0x01,        /*   Report ID (1)                  */
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)   */
    0x19, 0xE0,        /*   Usage Minimum (Left Control)   */
    0x29, 0xE7,        /*   Usage Maximum (Right GUI)      */
    0x15, 0x00,        /*   Logical Minimum (0)            */
    0x25, 0x01,        /*   Logical Maximum (1)            */
    0x75, 0x01,        /*   Report Size (1)                */
    0x95, 0x08,        /*   Report Count (8)               */
    0x81, 0x02,        /*   Input modifiers                */
    0x75, 0x08,        /*   Report Size (8)                */
    0x95, 0x01,        /*   Report Count (1)               */
    0x81, 0x01,        /*   Input (Const) reserved byte    */

    /* LED 输出报告：主机回写 Num/Caps/Scroll Lock 等状态 */
    0x95, 0x05,        /*   Report Count (5)               */
    0x75, 0x01,        /*   Report Size (1)                */
    0x05, 0x08,        /*   Usage Page (LEDs)              */
    0x19, 0x01,        /*   Usage Minimum (Num Lock)       */
    0x29, 0x05,        /*   Usage Maximum (Kana)           */
    0x91, 0x02,        /*   Output (Data,Var,Abs)          */
    0x95, 0x01,        /*   Report Count (1)               */
    0x75, 0x03,        /*   Report Size (3)                */
    0x91, 0x01,        /*   Output (Const) padding         */

    /* 6KRO 键码数组 */
    0x95, 0x06,        /*   Report Count (6)               */
    0x75, 0x08,        /*   Report Size (8)                */
    0x15, 0x00,        /*   Logical Minimum (0)            */
    0x26, 0xE7, 0x00,  /*   Logical Maximum (0xE7)         */
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)   */
    0x19, 0x00,        /*   Usage Minimum (0)              */
    0x2A, 0xE7, 0x00,  /*   Usage Maximum (0xE7)           */
    0x81, 0x00,        /*   Input (Data,Array,Abs)         */
    0xC0,              /* End Collection                   */

    /* ---- Consumer Control（Report ID 0x02，16bit usage） ---- */
    0x05, 0x0C,        /* Usage Page (Consumer)            */
    0x09, 0x01,        /* Usage (Consumer Control)         */
    0xA1, 0x01,        /* Collection (Application)         */
    0x85, 0x02,        /*   Report ID (2)                  */
    0x15, 0x00,        /*   Logical Minimum (0)            */
    0x26, 0xFF, 0x03,  /*   Logical Maximum (0x3FF)        */
    0x19, 0x00,        /*   Usage Minimum (0)              */
    0x2A, 0xFF, 0x03,  /*   Usage Maximum (0x3FF)          */
    0x75, 0x10,        /*   Report Size (16)               */
    0x95, 0x01,        /*   Report Count (1)               */
    0x81, 0x00,        /*   Input (Data,Array,Abs)         */
    0xC0               /* End Collection                   */
};

static const uint8_t s_pnp_id[] = {
    0x01,              /* Vendor ID Source: Bluetooth SIG */
    0x3A, 0x30,        /* Vendor ID 0x303A (Espressif)     */
    0x01, 0x10,        /* Product ID 0x1001                */
    0x00, 0x01         /* Product Version 0x0100           */
};

static void shutter_advertise(void);
static int  shutter_gap_event(struct ble_gap_event *event, void *arg);

/* ================================================================
 * GATT 访问回调
 * ================================================================ */
enum {
    ARG_HID_INFO = 1,
    ARG_REPORT_MAP,
    ARG_PROTOCOL_MODE,
    ARG_CONTROL_POINT,
    ARG_REPORT_KB,      /* Report ID 1 输入（键盘） */
    ARG_REPORT_LED,     /* Report ID 1 输出（键盘 LED） */
    ARG_REPORT_CC,      /* Report ID 2 输入（消费者控制） */
    ARG_REPORT_REF_KB,
    ARG_REPORT_REF_LED,
    ARG_REPORT_REF_CC,
    ARG_MANUFACTURER,
    ARG_PNP_ID,
    ARG_BATT_LEVEL,
};

static int shutter_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    intptr_t id = (intptr_t)arg;
    (void)conn_handle;
    (void)attr_handle;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_READ_DSC:
        switch (id) {
        case ARG_HID_INFO: {
            uint8_t info[4] = { 0x11, 0x01, 0x00, 0x02 }; /* bcdHID 1.11, country 0, flags */
            return os_mbuf_append(ctxt->om, info, sizeof(info)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_REPORT_MAP:
            return os_mbuf_append(ctxt->om, s_report_map, sizeof(s_report_map)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        case ARG_PROTOCOL_MODE: {
            uint8_t mode = 0x01; /* Report Protocol */
            return os_mbuf_append(ctxt->om, &mode, 1) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_REPORT_KB: {
            uint8_t report[SHUTTER_KB_REPORT_LEN];
            memset(report, 0, sizeof(report));
            return os_mbuf_append(ctxt->om, report, sizeof(report)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_REPORT_LED:
            return os_mbuf_append(ctxt->om, &s_led_state, 1) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        case ARG_REPORT_CC: {
            uint8_t report[SHUTTER_CC_REPORT_LEN];
            memset(report, 0, sizeof(report));
            return os_mbuf_append(ctxt->om, report, sizeof(report)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_REPORT_REF_KB: {
            uint8_t ref[2] = { HID_REPORT_ID_KEYBOARD, 0x01 };   /* ID1 输入 */
            return os_mbuf_append(ctxt->om, ref, sizeof(ref)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_REPORT_REF_LED: {
            uint8_t ref[2] = { HID_REPORT_ID_KEYBOARD, 0x02 };   /* ID1 输出 */
            return os_mbuf_append(ctxt->om, ref, sizeof(ref)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_REPORT_REF_CC: {
            uint8_t ref[2] = { HID_REPORT_ID_CONSUMER, 0x01 };   /* ID2 输入 */
            return os_mbuf_append(ctxt->om, ref, sizeof(ref)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_MANUFACTURER: {
            const char *m = "ESP32-RID";
            return os_mbuf_append(ctxt->om, m, strlen(m)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case ARG_PNP_ID:
            return os_mbuf_append(ctxt->om, s_pnp_id, sizeof(s_pnp_id)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        case ARG_BATT_LEVEL:
            /* HOGP 要求 HID 设备带电池服务；设备是 USB 供电，固定报 100% */
            return os_mbuf_append(ctxt->om, &s_batt_level, 1) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        if (id == ARG_REPORT_LED) {
            /* 主机回写键盘 LED 状态：存下来即可（本机没有物理 LED） */
            s_led_state = 0;
            if (ctxt->om != NULL && os_mbuf_len(ctxt->om) > 0) {
                os_mbuf_copydata(ctxt->om, 0, 1, &s_led_state);
            }
        }
        /* Protocol Mode / HID Control Point：写入即接受 */
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* Report Reference 描述符：{ Report ID, Report Type }，Type 1=输入 2=输出 */
static struct ble_gatt_dsc_def s_kb_in_dscs[] = {
    {
        .uuid = &UUID_DSC_REPORT_REF.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_REPORT_REF_KB,
        .att_flags = BLE_ATT_F_READ,
    },
    { 0 }
};

static struct ble_gatt_dsc_def s_led_dscs[] = {
    {
        .uuid = &UUID_DSC_REPORT_REF.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_REPORT_REF_LED,
        .att_flags = BLE_ATT_F_READ,
    },
    { 0 }
};

static struct ble_gatt_dsc_def s_cc_dscs[] = {
    {
        .uuid = &UUID_DSC_REPORT_REF.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_REPORT_REF_CC,
        .att_flags = BLE_ATT_F_READ,
    },
    { 0 }
};

/* 电池服务（HOGP 强制要求，iOS 也会读） */
static struct ble_gatt_chr_def s_bas_chrs[] = {
    {
        .uuid = &UUID_CHR_BATT_LEVEL.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_BATT_LEVEL,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 }
};

static struct ble_gatt_chr_def s_hid_chrs[] = {
    {
        .uuid = &UUID_CHR_HID_INFO.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_HID_INFO,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &UUID_CHR_REPORT_MAP.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_REPORT_MAP,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &UUID_CHR_PROTOCOL.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_PROTOCOL_MODE,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &UUID_CHR_CONTROL_PT.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_CONTROL_POINT,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        /* 1) 键盘输入报告：read + notify */
        .uuid = &UUID_CHR_REPORT.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_REPORT_KB,
        .descriptors = s_kb_in_dscs,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        /* 2) 键盘 LED 输出报告：主机回写 Caps/Num Lock 等 */
        .uuid = &UUID_CHR_REPORT.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_REPORT_LED,
        .descriptors = s_led_dscs,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        /* 3) 消费者控制输入报告（手机快门 / 音量） */
        .uuid = &UUID_CHR_REPORT.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_REPORT_CC,
        .descriptors = s_cc_dscs,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 }
};

static struct ble_gatt_chr_def s_dis_chrs[] = {
    {
        .uuid = &UUID_CHR_MANUFACTURER.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_MANUFACTURER,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &UUID_CHR_PNP_ID.u,
        .access_cb = shutter_access,
        .arg = (void *)ARG_PNP_ID,
        .flags = BLE_GATT_CHR_F_READ,
    },
    { 0 }
};

static struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_HID.u,
        .characteristics = s_hid_chrs,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_DIS.u,
        .characteristics = s_dis_chrs,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_BAS.u,
        .characteristics = s_bas_chrs,
    },
    { 0 }
};

/* ================================================================
 * 广播
 * ================================================================ */
static void shutter_advertise(void)
{
    if (!s_ready) return;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    /* 0x03C1 = HID Keyboard。iOS 只按 appearance + 报告描述符判断设备类别，
       报成 "HID Generic"(0x03C0) 时 iOS 不把它当键盘，输入会被丢弃。 */
    fields.appearance = 0x03C1;
    fields.appearance_is_present = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    /* Apple 只允许这几个广播间隔（QA1931）：前 30s 用 20ms 保证快速被发现，
       之后降为 152.5ms。用非规定值（如常见的 30~60ms）iPhone 会发现慢/发现不了。 */
    const uint16_t itvl = s_adv_fast ? ADV_ITVL_FAST : ADV_ITVL_SLOW;

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = itvl;
    params.itvl_max = itvl;

    /* 快广播阶段限定 30s，超时由 BLE_GAP_EVENT_ADV_COMPLETE 切慢广播 */
    int32_t dur_ms = s_adv_fast ? ADV_FAST_MS : BLE_HS_FOREVER;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, dur_ms,
                           &params, shutter_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\" itvl=%.1fms",
                 s_name, itvl * 0.625);
    }
}

/* ================================================================
 * GAP 事件
 * ================================================================ */
static int shutter_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            /* 单用户 HID 设备：已经连着一台主机时直接拒绝新连接。
               否则 s_conn_handle 会被后来的连接顶掉，正在用键盘的那台
               （例如 iPhone）会突然"按键没反应"，而抢连接的往往是 PC 端
               残留配对触发的自动重连。 */
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ESP_LOGW(TAG, "busy: reject extra conn handle=%d",
                         event->connect.conn_handle);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            /* 打印对端 MAC：排查"谁在抢连接"——PC 端残留配对会不停自动重连，
               把 iPhone 挤掉（表象就是 iOS 键盘时好时坏）。 */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "BLE connected, handle=%d peer=%02X:%02X:%02X:%02X:%02X:%02X",
                         s_conn_handle,
                         desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                         desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                         desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            } else {
                ESP_LOGI(TAG, "BLE connected, handle=%d", s_conn_handle);
            }
            /* 主动发起配对：HOGP 主机要求加密链路 */
            ble_gap_security_initiate(s_conn_handle);

            /* 主动请求 Apple 合规的连接参数。默认的 7.5ms/0 超时组合不满足
               Apple 规则，iPhone 会拒绝或直接断开（表象就是"自己就断开了"）。 */
            struct ble_gap_upd_params up;
            memset(&up, 0, sizeof(up));
            up.itvl_min = CONN_ITVL_MIN;
            up.itvl_max = CONN_ITVL_MAX;
            up.latency  = CONN_LATENCY;
            up.supervision_timeout = CONN_SUPERVISION;
            up.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
            up.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
            int urc = ble_gap_update_params(s_conn_handle, &up);
            ESP_LOGI(TAG, "conn param req rc=%d (%d-%dms lat%d to%dms)",
                     urc, CONN_ITVL_MIN * 5 / 4, CONN_ITVL_MAX * 5 / 4,
                     CONN_LATENCY, CONN_SUPERVISION * 10);
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            shutter_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_adv_fast = true;            /* 断开后重新快广播，方便立刻重连 */
        s_continuous = false;
        s_pairing = false;
        s_encrypted = false;
        s_sub_kb = false;
        s_sub_cc = false;
        shutter_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* 快广播 30s 已过且无人连接：切到 Apple 认可的慢广播间隔 */
        if (s_adv_fast) {
            s_adv_fast = false;
            ESP_LOGI(TAG, "adv fast phase done -> slow interval");
        }
        shutter_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
            ESP_LOGI(TAG, "conn updated rc=%d -> itvl=%.2fms lat=%d to=%dms",
                     event->conn_update.status,
                     d.conn_itvl * 1.25, d.conn_latency,
                     d.supervision_timeout * 10);
        } else {
            ESP_LOGI(TAG, "conn updated rc=%d", event->conn_update.status);
        }
        return 0;
    }

    /* 主机（iPhone）请求的连接参数一律接受：返回 0 = 同意。
       若拒绝，iOS 很可能直接断开（就是\"自己就断开了\"的来源）。 */
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        if (event->conn_update_req.peer_params != NULL) {
            ESP_LOGI(TAG, "peer conn param req: %d-%dms lat%d to%dms -> accept",
                     event->conn_update_req.peer_params->itvl_min * 5 / 4,
                     event->conn_update_req.peer_params->itvl_max * 5 / 4,
                     event->conn_update_req.peer_params->latency,
                     event->conn_update_req.peer_params->supervision_timeout * 10);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change, status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            /* 配对完成：屏幕上的配对码可以撤了 */
            s_pairing = false;
            s_encrypted = true;
            ESP_LOGW(TAG, "pairing done, link encrypted");
        } else {
            s_encrypted = false;
            /* 加密失败多为双方密钥不一致：清掉本端旧 bond 并断开，允许重新配对 */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
                ESP_LOGW(TAG, "encryption failed, peer bond removed");
            }
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t h = event->subscribe.attr_handle;
        bool on = event->subscribe.cur_notify;
        /* iOS 没订阅键盘报告 = 一个字也打不出来，所以这里单独打出来 */
        if (h == s_kb_report_handle) {
            s_sub_kb = on;
            ESP_LOGW(TAG, "keyboard report notify=%d", on);
        } else if (h == s_report_handle) {
            s_sub_cc = on;
            ESP_LOGW(TAG, "consumer report notify=%d", on);
        } else {
            ESP_LOGI(TAG, "subscribe attr=%d notify=%d", h, on);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
            ESP_LOGW(TAG, "repeat pairing: old bond deleted, retry");
        } else {
            ESP_LOGW(TAG, "repeat pairing: conn_find rc=%d", rc);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io;
        memset(&io, 0, sizeof(io));
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            s_passkey = 100000u + (esp_random() % 900000u);
            io.action = BLE_SM_IOACT_DISP;
            io.passkey = s_passkey;
            ble_sm_inject_io(event->passkey.conn_handle, &io);
            s_pairing = true;
            s_pair_at = xTaskGetTickCount();
            ESP_LOGW(TAG, "pairing passkey: %06lu (屏幕 / Web 显示，手机端输入)",
                     (unsigned long)s_passkey);
        } else {
            io.action = BLE_SM_IOACT_NONE;
            ble_sm_inject_io(event->passkey.conn_handle, &io);
            ESP_LOGW(TAG, "passkey action %d not supported",
                     event->passkey.params.action);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated to %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ================================================================
 * 发送 HID 报告
 * ================================================================ */
/* 消费者控制报告：16bit usage，小端。0x0000 = 全部释放 */
static void shutter_notify(uint16_t usage)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_report_handle == 0) return;

    uint8_t rep[SHUTTER_CC_REPORT_LEN];
    rep[0] = (uint8_t)(usage & 0xFF);
    rep[1] = (uint8_t)(usage >> 8);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(rep, sizeof(rep));
    if (!om) return;
    s_tx_cc++;
    ble_gatts_notify_custom(s_conn_handle, s_report_handle, om);
}

bool crid_shutter_take_photo(void)
{
    if (!s_ready || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_report_handle == 0) {
        return false;
    }
    if (s_tx_mutex == NULL) return false;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(400)) != pdTRUE) return false;

    /* 音量键交替：VOL+ → VOL- → VOL+ …，避免一路只加音量 */
#define HID_USAGE_VOL_UP    0x00E9
#define HID_USAGE_VOL_DOWN  0x00EA
    uint16_t press = (s_key == SHUTTER_KEY_VOL_DOWN)
                     ? HID_USAGE_VOL_DOWN : HID_USAGE_VOL_UP;
    if (s_alt) press = (press == HID_USAGE_VOL_UP) ? HID_USAGE_VOL_DOWN : HID_USAGE_VOL_UP;
    s_alt ^= 1;

    shutter_notify(press);
    vTaskDelay(pdMS_TO_TICKS(SHUTTER_PRESS_MS));
    shutter_notify(0x0000);          /* 释放 */

    s_shots++;
    ESP_LOGI(TAG, "shutter fired (%s), total=%lu",
             press == HID_USAGE_VOL_DOWN ? "VOL-" : "VOL+", (unsigned long)s_shots);

    xSemaphoreGive(s_tx_mutex);
    return true;
}

/* ================================================================
 * 键盘报告（Report ID 1）— 远程键盘
 * ================================================================ */
#define KB_PRESS_MS   30      /* 按键按下保持时长 */
#define KB_GAP_MS     12      /* 相邻按键间隔 */

static void shutter_kb_notify(uint8_t mod, uint8_t k0, uint8_t k1, uint8_t k2,
                              uint8_t k3, uint8_t k4, uint8_t k5)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_kb_report_handle == 0) return;
    uint8_t rep[SHUTTER_KB_REPORT_LEN];
    memset(rep, 0, sizeof(rep));
    rep[0] = mod;                 /* 修饰键位 */
    rep[1] = 0x00;                /* 保留字节 */
    rep[2] = k0; rep[3] = k1; rep[4] = k2;
    rep[5] = k3; rep[6] = k4; rep[7] = k5;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(rep, sizeof(rep));
    if (!om) return;
    s_tx_kb++;
    ble_gatts_notify_custom(s_conn_handle, s_kb_report_handle, om);
}

/* 注册回调：三条 Report 特征 UUID 相同（都是 0x2A4D），靠注册顺序区分：
 *   1 = 键盘输入(ID1) / 2 = 键盘 LED 输出(ID1) / 3 = 消费者控制输入(ID2) */
static void shutter_gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (ctxt->op != BLE_GATT_REGISTER_OP_CHR) return;
    const ble_uuid_t *u = (ctxt->chr.chr_def != NULL) ? ctxt->chr.chr_def->uuid : NULL;
    if (u != NULL && u->type == BLE_UUID_TYPE_16 && BLE_UUID16(u)->value == 0x2A4D) {
        static int nth = 0;
        nth++;
        if (nth == 1) {
            s_kb_report_handle  = ctxt->chr.val_handle;
            ESP_LOGI(TAG, "report chr: keyboard in   handle=%d", s_kb_report_handle);
        } else if (nth == 2) {
            s_led_report_handle = ctxt->chr.val_handle;
            ESP_LOGI(TAG, "report chr: keyboard LED  handle=%d", s_led_report_handle);
        } else {
            s_report_handle     = ctxt->chr.val_handle;
            ESP_LOGI(TAG, "report chr: consumer in   handle=%d", s_report_handle);
        }
    }
}

/* ASCII → (修饰键, 键码) */
static bool ascii_to_hid(uint8_t c, uint8_t *mod, uint8_t *key)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') { *key = (uint8_t)(0x04 + (c - 'a')); return true; }
    if (c >= 'A' && c <= 'Z') { *key = (uint8_t)(0x04 + (c - 'A')); *mod = 0x02; return true; }
    if (c >= '1' && c <= '9') { *key = (uint8_t)(0x1E + (c - '1')); return true; }
    switch (c) {
        case '0':  *key = 0x27; return true;
        case '\n': case '\r': *key = 0x28; return true;
        case '\t': *key = 0x2B; return true;
        case '\b': *key = 0x2A; return true;
        case 0x1B: *key = 0x29; return true;
        case ' ':  *key = 0x2C; return true;
        case '-':  *key = 0x2D; return true;
        case '_':  *key = 0x2D; *mod = 0x02; return true;
        case '=':  *key = 0x2E; return true;
        case '+':  *key = 0x2E; *mod = 0x02; return true;
        case '[':  *key = 0x2F; return true;
        case '{':  *key = 0x2F; *mod = 0x02; return true;
        case ']':  *key = 0x30; return true;
        case '}':  *key = 0x30; *mod = 0x02; return true;
        case '\\': *key = 0x31; return true;
        case '|':  *key = 0x31; *mod = 0x02; return true;
        case ';':  *key = 0x33; return true;
        case ':':  *key = 0x33; *mod = 0x02; return true;
        case '\'': *key = 0x34; return true;
        case '"':  *key = 0x34; *mod = 0x02; return true;
        case '`':  *key = 0x35; return true;
        case '~':  *key = 0x35; *mod = 0x02; return true;
        case ',':  *key = 0x36; return true;
        case '<':  *key = 0x36; *mod = 0x02; return true;
        case '.':  *key = 0x37; return true;
        case '>':  *key = 0x37; *mod = 0x02; return true;
        case '/':  *key = 0x38; return true;
        case '?':  *key = 0x38; *mod = 0x02; return true;
        case '!':  *key = 0x1E; *mod = 0x02; return true;
        case '@':  *key = 0x1F; *mod = 0x02; return true;
        case '#':  *key = 0x20; *mod = 0x02; return true;
        case '$':  *key = 0x21; *mod = 0x02; return true;
        case '%':  *key = 0x22; *mod = 0x02; return true;
        case '^':  *key = 0x23; *mod = 0x02; return true;
        case '&':  *key = 0x24; *mod = 0x02; return true;
        case '*':  *key = 0x25; *mod = 0x02; return true;
        case '(':  *key = 0x26; *mod = 0x02; return true;
        case ')':  *key = 0x27; *mod = 0x02; return true;
        default: return false;
    }
}

/* mod: bit0 Ctrl / bit1 Shift / bit2 Alt / bit3 GUI */
bool crid_shutter_key_hold(uint8_t mod, uint8_t key, bool down)
{
    if (!s_ready || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_kb_report_handle == 0) {
        return false;
    }
    bool locked = (s_tx_mutex != NULL);
    if (locked && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(400)) != pdTRUE) return false;

    if (down) shutter_kb_notify(mod, key, 0, 0, 0, 0, 0);
    else      shutter_kb_notify(0, 0, 0, 0, 0, 0, 0);

    if (locked) xSemaphoreGive(s_tx_mutex);
    return true;
}

bool crid_shutter_key_tap(uint8_t mod, uint8_t key)
{
    if (!s_ready || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_kb_report_handle == 0) {
        return false;
    }
    bool locked = (s_tx_mutex != NULL);
    if (locked && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(400)) != pdTRUE) return false;

    shutter_kb_notify(mod, key, 0, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(KB_PRESS_MS));
    shutter_kb_notify(0, 0, 0, 0, 0, 0, 0);

    if (locked) xSemaphoreGive(s_tx_mutex);
    return true;
}

/* 逐字符输入 ASCII 文本；返回成功发送的字符数，-1 表示未连接/失败 */
int crid_shutter_type_text(const char *utf8)
{
    if (utf8 == NULL) return -1;
    if (!s_ready || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_kb_report_handle == 0) {
        ESP_LOGW(TAG, "type failed: ready=%d connected=%d kb_handle=%u",
                 s_ready, (s_conn_handle != BLE_HS_CONN_HANDLE_NONE),
                 (unsigned)s_kb_report_handle);
        return -1;
    }
    if (!s_sub_kb) {
        /* 主机没开通知：报告发出去对方也收不到，是最常见的"打不出字"原因 */
        ESP_LOGW(TAG, "type: keyboard notify NOT enabled by host!");
    }
    bool locked = (s_tx_mutex != NULL);
    if (locked && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(1200)) != pdTRUE) return -1;

    int sent = 0;
    const uint8_t *p = (const uint8_t *)utf8;
    while (*p) {
        if (*p < 0x80) {
            uint8_t mod, key;
            if (ascii_to_hid(*p, &mod, &key)) {
                shutter_kb_notify(mod, key, 0, 0, 0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(KB_PRESS_MS));
                shutter_kb_notify(0, 0, 0, 0, 0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(KB_GAP_MS));
                sent++;
            }
            p++;
        } else {
            /* 非 ASCII（中文等）无对应 HID 键码：跳过整个 UTF-8 序列 */
            p++;
            while ((*p & 0xC0) == 0x80) p++;
        }
    }

    if (locked) xSemaphoreGive(s_tx_mutex);
    if (sent > 0) ESP_LOGI(TAG, "typed %d chars", sent);
    return sent;
}

/* 诊断快照：连没连上、有没有加密、主机有没有订阅键盘报告、发了多少条。
 * iOS 打不出字时先看这个，能立刻区分是"没连上"还是"没订阅"还是"没发"。 */
void crid_shutter_ble_diag(char *out, size_t len)
{
    if (out == NULL || len == 0) return;
    snprintf(out, len,
             "ready=%d connected=%d encrypted=%d\r\n"
             "handles kb_in=%u kb_led=%u cc_in=%u\r\n"
             "notify kb=%d cc=%d\r\n"
             "tx kb=%lu cc=%lu shots=%lu",
             s_ready, (s_conn_handle != BLE_HS_CONN_HANDLE_NONE), s_encrypted,
             (unsigned)s_kb_report_handle, (unsigned)s_led_report_handle,
             (unsigned)s_report_handle,
             s_sub_kb, s_sub_cc,
             (unsigned long)s_tx_kb, (unsigned long)s_tx_cc,
             (unsigned long)s_shots);
}

/* ================================================================
 * 连拍任务
 * ================================================================ */
static void shutter_task(void *pv)
{
    (void)pv;
    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_ready) continue;
        if (!s_continuous) {
            last = xTaskGetTickCount();
            continue;
        }
        if (s_paused) {
            /* 暂停期间不出片，但保持任务存活；恢复时由 resume() 重排 s_next_at，
             * 不会因暂停期间的时间流逝而连续补拍。 */
            last = xTaskGetTickCount();
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        uint32_t ivl = s_interval > 0 ? s_interval : 1;
        if ((now - last) >= pdMS_TO_TICKS(ivl * 1000U)) {
            last = now;
            if (!crid_shutter_take_photo()) {
                /* 未连接则自动暂停，避免空转 */
                s_continuous = false;
                s_paused = false;
                ESP_LOGW(TAG, "continuous stopped: not connected");
            }
            /* 刷新倒计时终点（无论本次是否拍成功，节奏保持不变） */
            s_next_at = xTaskGetTickCount() + pdMS_TO_TICKS(ivl * 1000U);
        }
    }
}

/* ================================================================
 * NVS 配置
 * ================================================================ */
static void shutter_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(SHUTTER_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint32_t v32 = 0;
    if (nvs_get_u32(h, "interval", &v32) == ESP_OK && v32 >= 1 && v32 <= 3600) {
        s_interval = v32;
    }
    v32 = SHUTTER_GAP_MANUAL;
    if (nvs_get_u32(h, "gap", &v32) == ESP_OK && v32 <= 3600) {
        s_gap = v32;
    }
    v32 = 0;
    if (nvs_get_u32(h, "key", &v32) == ESP_OK && v32 <= 1) {
        s_key = (uint8_t)v32;
    }
    size_t len = sizeof(s_name);
    if (nvs_get_str(h, "name", s_name, &len) != ESP_OK) {
        strlcpy(s_name, SHUTTER_DEF_NAME, sizeof(s_name));
    }
    nvs_close(h);
}

void crid_shutter_save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(SHUTTER_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "interval", s_interval);
    nvs_set_u32(h, "gap", s_gap);
    nvs_set_u32(h, "key", s_key);
    nvs_set_str(h, "name", s_name);
    nvs_commit(h);
    nvs_close(h);
}

/* ================================================================
 * 对外接口
 * ================================================================ */
/* 清除本端所有配对信息（用户换机/忘记设备后重新配对时调用） */
int crid_shutter_clear_bonds(void)
{
    int rc = ble_store_clear();
    ESP_LOGW(TAG, "bond store cleared, rc=%d", rc);
    return rc;
}

/* 配对态超时保护：手机放弃配对时屏幕不能一直挂着旧配对码 */
static void pairing_check_timeout(void)
{
    if (!s_pairing) return;
    if ((xTaskGetTickCount() - s_pair_at) > pdMS_TO_TICKS(SHUTTER_PAIR_TMO_MS)) {
        s_pairing = false;
        ESP_LOGI(TAG, "pairing window expired");
    }
}

/** 是否正在等手机输入配对码（屏幕据此把配对码放到最显眼的位置） */
bool crid_shutter_is_pairing(void)
{
    pairing_check_timeout();
    return s_pairing;
}

void crid_shutter_get_status(shutter_status_t *out)
{
    if (!out) return;
    pairing_check_timeout();
    out->connected = (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
    out->ready = s_ready;
    out->continuous = s_continuous;
    out->paused = s_paused;
    out->interval = s_interval;
    out->key = s_key;
    out->shots = s_shots;
    out->passkey = s_passkey;
    out->pairing = s_pairing;
    out->gap = s_gap;
    if (s_continuous && !s_paused) {
        TickType_t now = xTaskGetTickCount();
        uint32_t ms = (s_next_at > now)
                        ? (uint32_t)(s_next_at - now) * portTICK_PERIOD_MS : 0;
        out->next_in = ms / 1000U;
    } else {
        out->next_in = 0;
    }
    strlcpy(out->name, s_name, sizeof(out->name));
}

bool crid_shutter_set_continuous(bool on)
{
    if (on && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        s_continuous = false;
        s_paused = false;
        return false;
    }
    s_continuous = on;
    if (on) {
        s_paused = false;
        uint32_t ivl = s_interval > 0 ? s_interval : 1;
        s_next_at = xTaskGetTickCount() + pdMS_TO_TICKS(ivl * 1000U);
    }
    return true;
}

/** 暂停定时连拍（仅在运行态有意义；手动档/停止态视为无操作） */
void crid_shutter_pause(void)
{
    if (s_continuous && !s_paused) {
        s_paused = true;
        ESP_LOGI(TAG, "continuous paused");
    }
}

/** 恢复定时连拍：重排下一张时刻，避免补拍 */
void crid_shutter_resume(void)
{
    if (s_continuous && s_paused) {
        s_paused = false;
        uint32_t ivl = s_interval > 0 ? s_interval : 1;
        s_next_at = xTaskGetTickCount() + pdMS_TO_TICKS(ivl * 1000U);
        ESP_LOGI(TAG, "continuous resumed");
    }
}

/** 停止定时连拍（运行态与暂停态一并清除），手动档不受影响 */
void crid_shutter_stop(void)
{
    s_continuous = false;
    s_paused = false;
    ESP_LOGI(TAG, "continuous stopped");
}

/** 当前是否处于暂停态 */
bool crid_shutter_is_paused(void)
{
    return s_paused;
}

void crid_shutter_set_interval(uint32_t seconds)
{
    if (seconds < 1) seconds = 1;
    if (seconds > 3600) seconds = 3600;
    s_interval = seconds;
    /* 网页改间隔时同步屏幕档位，避免两边不一致 */
    s_gap = seconds;
}

/* 屏幕可选档位：手动 / 3 / 5 / 10 / 15 / 20 / 30 / 60 秒 */
const uint32_t crid_shutter_gaps[SHUTTER_GAP_N] = {0, 3, 5, 10, 15, 20, 30, 60};

void crid_shutter_set_gap(uint32_t gap)
{
    if (gap == SHUTTER_GAP_MANUAL) {
        /* 手动档：停掉定时连拍，按键才拍 */
        s_continuous = false;
        s_paused = false;
        s_gap = SHUTTER_GAP_MANUAL;
        return;
    }
    if (gap > 3600) gap = 3600;
    s_gap = gap;
    s_interval = gap;
    /* 正在连拍时改档：以新间隔重排下一张 */
    if (s_continuous) s_next_at = xTaskGetTickCount() + pdMS_TO_TICKS(gap * 1000U);
}

uint32_t crid_shutter_get_gap(void) { return s_gap; }

uint32_t crid_shutter_gap_next(void)
{
    int idx = 0;
    for (int i = 0; i < SHUTTER_GAP_N; i++) {
        if (crid_shutter_gaps[i] == s_gap) { idx = i; break; }
    }
    uint32_t next = crid_shutter_gaps[(idx + 1) % SHUTTER_GAP_N];
    crid_shutter_set_gap(next);
    return next;
}

bool crid_shutter_set_key(uint8_t key)
{
    if (key > 1) return false;
    s_key = key;
    return true;
}

esp_err_t crid_shutter_set_name(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(name);
    if (len == 0 || len >= sizeof(s_name)) return ESP_ERR_INVALID_ARG;

    if (strcmp(name, s_name) == 0) return ESP_OK;
    strlcpy(s_name, name, sizeof(s_name));

    if (s_ready) {
        ble_svc_gap_device_name_set(s_name);
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        shutter_advertise();
    }
    return ESP_OK;
}

/* ================================================================
 * NimBLE 启动
 * ================================================================ */
static void shutter_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
        return;
    }

    /* 兜底：注册回调没抓到时（例如重复 sync）按 UUID 找第一条 Report 特征。
       第一条就是键盘输入，别拿它当消费者控制句柄用。 */
    if (s_kb_report_handle == 0) {
        rc = ble_gatts_find_chr(&UUID_SVC_HID.u, &UUID_CHR_REPORT.u, NULL,
                                &s_kb_report_handle);
        if (rc != 0) {
            ESP_LOGE(TAG, "find report chr failed rc=%d", rc);
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "BLE HID ready, kb in=%d kb led=%d consumer in=%d",
             s_kb_report_handle, s_led_report_handle, s_report_handle);
    shutter_advertise();
}

static void shutter_on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble host reset, reason=%d", reason);
    s_ready = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void shutter_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t crid_shutter_init(void)
{
    if (s_ready) return ESP_OK;          /* 幂等 */

    shutter_load_config();

    /* 复位运行时状态（可能来自上一次 deinit） */
    s_ready = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_report_handle = 0;
    s_kb_report_handle = 0;
    s_led_report_handle = 0;
    s_own_addr_type = 0;
    s_continuous = false;

    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (!s_tx_mutex) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = shutter_on_reset;
    ble_hs_cfg.sync_cb = shutter_on_sync;

    /* 配对参数：绑定 + Authenticated Pairing（MITM）
     * iOS 只接受经过认证加密链路上的 HID 报告，Just Works 时能连上但按键
     * 完全无反应（既不拍照也无音量变化）。设备无键盘，故用 DisplayOnly：
     * 随机 6 位配对码显示在 Web 页面，手机端输入确认。 */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* 通过 gatts_register_cb 捕获两条 Report 特征的真实 value handle */
    ble_hs_cfg.gatts_register_cb = shutter_gatt_register_cb;

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs failed rc=%d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();

    nimble_port_freertos_init(shutter_host_task);

    if (s_task == NULL) {
        if (xTaskCreate(shutter_task, "shutter", 3072, NULL, 3, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "create shutter task failed");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "BLE shutter init done: name=%s interval=%lu key=%d heap=%u",
             s_name, (unsigned long)s_interval, s_key,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

esp_err_t crid_shutter_deinit(void)
{
    ESP_LOGW(TAG, "shutter deinit start, heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    if (!s_ready) {
        /* host 未同步时 nimble_port_stop() 会永久阻塞，只能交给重启兜底 */
        ESP_LOGE(TAG, "BLE stack not ready, deinit unsupported -> need reboot");
        return ESP_FAIL;
    }

    s_continuous = false;

    /* 1. 停止广播、断开已连接的主机 */
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    /* 2. 停掉连拍任务 */
    if (s_task != NULL) {
        TaskHandle_t t = s_task;
        s_task = NULL;
        vTaskDelete(t);
    }

    /* 3. 停止并释放 NimBLE host + BLE 控制器 */
    s_ready = false;
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed rc=%d", rc);
        return ESP_FAIL;
    }

    /* 等 host 任务从 nimble_port_run() 退出并自注销 */
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* 4. 回收发送锁 */
    if (s_tx_mutex != NULL) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
    }

    s_report_handle = 0;
    s_shots = 0;

    ESP_LOGW(TAG, "shutter deinit done, heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

bool crid_shutter_is_ready(void)
{
    return s_ready;
}
