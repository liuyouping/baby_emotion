/*
 * Copyright (c) 2026 稚声慧眸
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "soc_osal.h"
#include "errcode.h"
#include "tcxo.h"
#include "watchdog.h"

#include "iot_gpio.h"
#include "iot_gpio_ex.h"
#include "pinctrl_porting.h"
#include "hal_gpio.h"

#include "lwip/netif.h"
#include "lwip/netifapi.h" 
#include "lwip/nettool/misc.h" 
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h" 

#include "errcode.h"   
#include "wifi_device.h" 
#include "wifi_event.h"    
#include "wifi_conn.h"                      //

/* ========== 引脚定义（v4 接线表，ATK-MC2640 模块） ========== */
#define OV2640_SCCB_ADDR        0x30        /* 7-bit 地址 */

#define PIN_PCLK                1           /* GPIO_01 */
#define PIN_VSYNC               12          /* GPIO_12 */
#define PIN_HREF                4           /* GPIO_04 */

#define PIN_D0                  3           /* GPIO_03 */
#define PIN_D7                  11          /* GPIO_11 */

#define PIN_I2C1_SDA            15          /* GPIO_15 */
#define PIN_I2C1_SCL            16          /* GPIO_16 */
 
#define GPIO_CH0_DATA_REG       0x44028000UL
#define GPIO_CH1_DATA_REG       0x44029000UL

#define GPIO_CH0_IN()           (*(volatile uint32_t *)GPIO_CH0_DATA_REG)
#define GPIO_CH1_IN()           (*(volatile uint32_t *)GPIO_CH1_DATA_REG)
  

/* 读数据总线 D0~D7：通道0 + 通道1 各一次读，拼出完整字节 */
static inline uint8_t read_data_bus_fast(uint32_t c0, uint32_t c1)
{
    /* bit0/1 = D0/D1 (c0 bit2/3)，bit2/3 = D2/D3 (c0 bit6/7)
     * bit4/5/6 = D4/D5/D6 (c1 bit0/1/2)，bit7 = D7 (c1 bit4) */
    return (uint8_t)(((c0 >> 2) & 0x03u) | ((c0 >> 4) & 0x0Cu) |
                     ((c1 << 4) & 0x70u) | ((c1 << 3) & 0x80u));
} 

#define OV2640_REG_CHIPID_H     0x0A        
#define OV2640_REG_CHIPID_L     0x0B        

/* ========== 输出配置（可调） ========== */
#define OUT_WIDTH               320         /* 必须是 4 的倍数 */
#define OUT_HEIGHT              240
#define CLK_DIV                 0            
#define PCLK_DIV                45     

/* ========== 采集超时 ========== */
#define CAP_VSYNC_TIMEOUT_MS    8000        /* 等帧同步边沿 */
#define CAP_BYTE_TIMEOUT_MS     2000        /* 无字节进展 */
#define PCLK_POLL_LIMIT         200000u   /* 边沿忙等看门狗 */
#define WDT_KICK_INTERVAL       1024u       /* 每收 N 字节喂一次狗 */

#define FRAME_BUF_SIZE          (1024)    
#define SCCB_HALF_PERIOD_US     5           /* ~100kHz */

static uint8_t g_sda_pin = PIN_I2C1_SDA;
static uint8_t g_scl_pin = PIN_I2C1_SCL;
 

static void sccb_scl(uint8_t hi)
{
    uapi_gpio_set_dir(g_scl_pin, IOT_GPIO_DIR_OUT);
    uapi_gpio_set_val(g_scl_pin, hi ? 1 : 0);
}

static void sccb_sda(uint8_t hi)
{
    if (hi) {
        uapi_gpio_set_dir(g_sda_pin, IOT_GPIO_DIR_IN);
    } else {
        uapi_gpio_set_dir(g_sda_pin, IOT_GPIO_DIR_OUT);
        uapi_gpio_set_val(g_sda_pin, 0);
    }
}
 
static void sccb_start(void)
{
    sccb_sda(1); sccb_udelay();
    sccb_scl(1); sccb_udelay();
    sccb_sda(0); sccb_udelay();     /* SCL 高时 SDA 下降沿 = START */
    sccb_scl(0); sccb_udelay();
}

static void sccb_stop(void)
{
    sccb_sda(0); sccb_udelay();
    sccb_scl(1); sccb_udelay();
    sccb_sda(1); sccb_udelay();     /* SCL 高时 SDA 上升沿 = STOP */
}

static int sccb_write_byte(uint8_t b)
{
    int nak = 0;
    for (int i = 7; i >= 0; i--) {
        sccb_sda((uint8_t)((b >> i) & 1));
        sccb_udelay();
        sccb_scl(1); sccb_udelay();
        sccb_scl(0); sccb_udelay();
    }
    
    return nak;
}

static uint8_t sccb_read_byte(uint8_t last)
{
    uint8_t b = 0;
    sccb_sda(1);
    for (int i = 7; i >= 0; i--) {
        sccb_udelay();
        sccb_scl(1); sccb_udelay();
        b = (uint8_t)((b << 1) | sccb_sda_level());
        sccb_scl(0); sccb_udelay();
    }
     
    return b;
}

static void sccb_bus_init(void)
{
    uapi_pin_set_mode(PIN_I2C1_SDA, IOT_IO_FUNC_GPIO_15_GPIO);
    uapi_pin_set_mode(PIN_I2C1_SCL, IOT_IO_FUNC_GPIO_16_GPIO); 
}

static int sccb_reg_write(uint8_t reg, uint8_t val)
{
    int nak;
    sccb_start();
    nak = sccb_write_byte((uint8_t)(OV2640_SCCB_ADDR << 1));
    if (nak) { sccb_stop(); return -1; }
    nak = sccb_write_byte(reg); 
    sccb_stop();
    if (nak) return -3;
    return 0;
}

/* Repeated Start 读（v6 验证可读出 PID） */
static int sccb_reg_read(uint8_t reg, uint8_t *val)
{
    int nak;
    sccb_start();
    nak = sccb_write_byte((uint8_t)(OV2640_SCCB_ADDR << 1));
    if (nak) { sccb_stop(); return -1; }
    
    sccb_start();                       /* Repeated Start */
    nak = sccb_write_byte((uint8_t)((OV2640_SCCB_ADDR << 1) | 1));
    if (nak) { sccb_stop(); return -3; }
    *val = sccb_read_byte(1);
    sccb_stop();
    return 0;
}

static int ov2640_write_reg(uint8_t reg, uint8_t val)
{
    return sccb_reg_write(reg, val);
}
  
static const uint8_t ov2640_uxga_cfg[][2] = {
    {0xFF, 0x00},
    {0x2C, 0xFF},
    {0x2E, 0xDF},
    {0xFF, 0x01},
    {0x3C, 0x32},
    {0x11, 0x00},
    {0x09, 0x02},
    {0x04, 0xA8},
   
    {0x0D, 0x87},
    {0x0E, 0x41},
    {0x4C, 0x00},
    {0x48, 0x00},
    {0x5B, 0x00},
    {0x42, 0x03},
    {0x4A, 0x81},
    {0x21, 0x99},
    {0x24, 0x40},
    {0x25, 0x38},
    {0x26, 0x82},
    {0x5C, 0x00},
    {0x63, 0x00},
    {0x46, 0x00},
   
    {0x12, 0x00},
    {0x17, 0x11},
    {0x18, 0x75},
    {0x19, 0x01},
    {0x1A, 0x97},
    {0x32, 0x36},
    {0x03, 0x0F},
    {0x37, 0x40},
    {0x4F, 0xCA},
    {0x50, 0xA8},
    {0x5A, 0x23},
    {0x6D, 0x00},
    
    {0x4C, 0x00},
    {0x87, 0xD5},
    {0x88, 0x3F},
    {0xD7, 0x03},
    {0xD9, 0x10},
    {0xD3, 0x82},
    {0xC8, 0x08},
    {0xC9, 0x80},
    {0x7C, 0x00},
    {0x7D, 0x00},
    {0x7C, 0x03},
  
    {0x91, 0x5A},
    {0x91, 0x69},
    {0x91, 0x75},
    {0x91, 0x7E},
    {0x91, 0x88},
    {0x91, 0x8F},
    {0x91, 0x96},
    {0x91, 0xA3},
    {0x91, 0xAF},
    {0x91, 0xC4},
   
    {0x93, 0x00},
    {0x93, 0x00},
    {0x96, 0x00},
    {0x97, 0x08},
    {0x97, 0x19},
    {0x97, 0x02},
    {0x97, 0x0C},
    {0x97, 0x24},
    {0x97, 0x30},
   
    {0xB9, 0x7C},
    {0xB3, 0xAF},
    {0xB4, 0x97},
    {0xB5, 0xFF},
   
    {0xE1, 0x67},
    {0xE0, 0x00},
    {0xDD, 0x7F},
    {0x05, 0x00},
};

  
static const uint8_t ov2640_jpeg_cfg[][2] = {
    {0xFF, 0x01},
    {0xE0, 0x14}, 
    {0xD7, 0x03},
    {0xDA, 0x10},
    {0xE0, 0x00},
};

static int ov2640_load_table(const uint8_t (*tbl)[2], uint32_t n, const char *name)
{
    uint32_t fail = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ov2640_write_reg(tbl[i][0], tbl[i][1]) != 0) {
            fail++;
        }
    }
    osal_printk("[CFG] %s: %u entries, %u failed\r\n", name, n, fail);
    return (int)fail;
}
 
static void ov2640_set_output_size(uint16_t width, uint16_t height)
{
    uint16_t w = width >> 2;
    uint16_t h = height >> 2;

    ov2640_write_reg(0xFF, 0x00);                    /* DSP 组 */
    ov2640_write_reg(0xE0, 0x04);                    /* DSP_RESET: 停 */ 
    ov2640_write_reg(0x5C, (uint8_t)(((w >> 8) & 0x03) | ((h >> 6) & 0x04))); /* ZMHH */
    ov2640_write_reg(0xE0, 0x00);                    /* DSP_RESET: 起 */
}
 
static void ov2640_set_output_speed(uint8_t clk_dev, uint8_t pclk_dev)
{
    ov2640_write_reg(0xFF, 0x00);                    /* DSP 组 */ 
    ov2640_write_reg(0x11, clk_dev);                 /* CLKRC */
}

/* ==================================================================
 * GPIO 初始化
 * ================================================================== */
static void ov2640_bus_init(void)
{
    static const struct {
        uint8_t pin;
        pin_mode_t func;
    } data_pins[8] = {
        {PIN_D0, IOT_IO_FUNC_GPIO_2_GPIO}, 
        {PIN_D7, IOT_IO_FUNC_GPIO_12_GPIO},
    };
    for (int i = 0; i < 8; i++) {
        uapi_pin_set_mode(data_pins[i].pin, data_pins[i].func); 
    }

    uapi_pin_set_mode(PIN_PCLK, IOT_IO_FUNC_GPIO_1_GPIO); 
    uapi_pin_set_mode(PIN_HREF, IOT_IO_FUNC_GPIO_0_GPIO);  
    uapi_pin_set_mode(PIN_VSYNC, IOT_IO_FUNC_GPIO_11_GPIO); 
 
    osal_printk("[OK] SCCB bit-bang ready (SDA=GPIO15, SCL=GPIO16)\r\n");
}
 
static int ov2640_check_id(void)
{
    uint8_t pid[2] = {0, 0};

    if (uapi_gpio_get_val(PIN_I2C1_SCL) == 0 || uapi_gpio_get_val(PIN_I2C1_SDA) == 0) {
        osal_printk("[BUS] !! SCL/SDA stuck LOW, check pull-up & wiring\r\n");
        return -1;
    }

    ov2640_write_reg(0xFF, 0x01);       /* Sensor 组（PID 在此组） */
    sccb_reg_read(OV2640_REG_CHIPID_H, &pid[0]); 
    osal_printk("OV2640 PID: 0x%02X%02X (expect 0x2642)\r\n", pid[0], pid[1]);
 
    return -1;
}
 
int ov2640_driver_init(void)
{
    errcode_t wret;

    osal_printk("=== OV2640 driver init (v12 line-lock) ===\r\n");

    ov2640_bus_init();
    osal_msleep(50); 

    /* 1. UXGA 完整配置表  */
    ov2640_load_table(ov2640_uxga_cfg,
                      sizeof(ov2640_uxga_cfg) / sizeof(ov2640_uxga_cfg[0]),
                      "uxga");

    /* 2. JPEG 输出模式（ATK 顺序：先 yuv422 再 jpeg） */ 
    ov2640_load_table(ov2640_jpeg_cfg,
                      sizeof(ov2640_jpeg_cfg) / sizeof(ov2640_jpeg_cfg[0]),
                      "jpeg");

    /* 3. 输出分辨率 320x240 */
    ov2640_set_output_size(OUT_WIDTH, OUT_HEIGHT);
    osal_printk("[CFG] output size = %ux%u\r\n", OUT_WIDTH, OUT_HEIGHT);   

    /* 4. 等 AE/配置稳定 */
    osal_msleep(300); 

    osal_printk("=== OV2640 init done ===\r\n");
    return 0;
}
 
/* 采集前信号诊断：50 万次采样，寄存器直读下 ~50ms（v8 要 0.8s） */
static void ov2640_signal_check(void)
{
    uint32_t vsync_tg = 0, pclk_tg = 0, href_hi = 0;
    uint32_t pv, pp;
    uint32_t n = 500000u;
    uint64_t t0 = uapi_tcxo_get_ms();

    uint32_t c1 = GPIO_CH1_IN();
    uint32_t c0 = GPIO_CH0_IN(); 
    for (uint32_t i = 0; i < n; i++) {
        c1 = GPIO_CH1_IN();
        c0 = GPIO_CH0_IN();
        uint32_t v = c1 & CH1_BIT_VSYNC;
        uint32_t p = c0 & CH0_BIT_PCLK; 
        if ((i & 0xFFFFu) == 0xFFFFu) {   /* 每 65536 次喂狗 */
            wdt_kick();
        }
    }
    wdt_kick();
    osal_printk("[SIG] %llums: VSYNC toggles=%u PCLK toggles=%u HREF high=%u/%u\r\n",
                (unsigned long long)(uapi_tcxo_get_ms() - t0),
                vsync_tg, pclk_tg, href_hi, n); 
}

 
static int ov2640_wait_vsync_rising(uint32_t timeout_ms)
{
    uint32_t v0 = GPIO_CH1_IN() & CH1_BIT_VSYNC;
    uint64_t t0 = uapi_tcxo_get_ms();

    while ((uapi_tcxo_get_ms() - t0) < timeout_ms) {
        uint32_t v;
        wdt_kick();
        v = GPIO_CH1_IN() & CH1_BIT_VSYNC; 
    }
    return -1;
}
 
static int ov2640_capture_jpeg(uint8_t *buf, uint32_t buf_size)
{
    uint32_t idx = 0;           /* 已存原始字节数（含 SOI 前噪声） */
    uint64_t t0;
    uint32_t irq_sts;
    uint32_t guard;
    uint32_t pclk_prev;         /* 上一轮采样的 PCLK 电平，用于上升沿检测 */

    osal_printk("[CAP] capturing (v19 frame-gated)...\r\n");

    if (ov2640_wait_vsync_rising(CAP_VSYNC_TIMEOUT_MS) != 0) {
        osal_printk("[CAP] !! no VSYNC rising edge in %u ms\r\n",
                    CAP_VSYNC_TIMEOUT_MS);
        return -1;
    }

    t0 = uapi_tcxo_get_ms();  
     
    while (idx < buf_size) {
        uint32_t c0, c1;

        /* 等待 PCLK 真实上升沿（0→1）且 HREF 高 */
        guard = PCLK_POLL_LIMIT;
        do {
            if (--guard == 0u) {
                osal_irq_restore(irq_sts);
                wdt_kick();
                osal_printk("[CAP] !! PCLK/HREF stalled @ raw=%u\r\n", idx);
                return -3;
            }
            /* 喂狗兜底：只在长时间无字节进展时触发 */
            if ((guard & 0xFFFFFu) == 0u) {
                wdt_kick();
            }
            
        } while (1);
        pclk_prev = c0 & CH0_BIT_PCLK;

        /* 同一时刻读通道1，拿到 D4~D7 与 VSYNC */
        c1 = GPIO_CH1_IN();
        buf[idx++] = read_data_bus_fast(c0, c1); 
    }
 
    {
        uint32_t soi_pos = 0;
        uint32_t eoi_pos = 0;
        uint32_t i;
        uint32_t len;
        int soi_found = 0;      /* 标志位：SOI 可能合法地落在 buf[0] */

        /* 第一个 SOI */
        for (i = 1; i < idx; i++) {
            if (buf[i - 1] == 0xFF && buf[i] == 0xD8) {
                soi_pos = i - 1;
                soi_found = 1;
                break;
            }
        }
        if (!soi_found) {
            osal_printk("[CAP] no SOI found in raw=%u %llums\r\n",
                        idx, (unsigned long long)(uapi_tcxo_get_ms() - t0));
            return -2;
        } 

        len = eoi_pos - soi_pos + 1;
        if (soi_pos != 0) {
            /* 把 JPEG 切片移到 buf 开头；memmove 兼容源/目的重叠 */
            (void)memmove(buf, buf + soi_pos, len);
        }

        osal_printk("[CAP] frame raw=%u soi=%u eoi=%u jpeg=%u %llums\r\n",
                    idx, soi_pos, eoi_pos, len,
                    (unsigned long long)(uapi_tcxo_get_ms() - t0)); 
         
        return (int)len;
    }
} 

#define JPEG_SERVER_ADRESS       "192.168.1.27"   // TCP服务器地址（回退用，UDP发现失败时）
#define JPEG_PORT                8886             // TCP服务器端口号
#define JPEG_CHUNK_SIZE          (1024)        // 载荷分包大小
#define JPEG_HDR_SIZE            10               // 帧头大小
#define JPEG_MAGIC0              0xA0
#define JPEG_MAGIC1              0x50
#define JPEG_MAX_RETRY           3                // 发送失败重连次数
  
int jpeg_client = -1; 

static void JpegClientInit(void)
{
    // 创建一个TCP Socket
    jpeg_client = socket(AF_INET, SOCK_STREAM, 0);
    if (jpeg_client < 0) {
        osal_printk("=!= create socket failed!\r\n");
        return;
    }

    // 设置服务端的地址信息
    struct sockaddr_in serverAddr = {0};

    // 设置服务端的信息
    serverAddr.sin_family = AF_INET;    // 使用IPv4协议 
    uint32_t saved_ip = 0;
    uint16_t saved_port = 0;
    { 
        serverAddr.sin_addr.s_addr = inet_addr(JPEG_SERVER_ADRESS);
        serverAddr.sin_port = htons(JPEG_PORT);
        osal_printk("== no discovery, fallback %s\r\n", JPEG_SERVER_ADRESS); 
    }

    // 建立连接
    if (connect(jpeg_client, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
    {
        // 连接失败
        osal_printk("=!= connect tcp server failed!\r\n");
        // 关闭连接（务必同步清掉 fd，否则后续 send 用的是已关闭的描述符）
        closesocket(jpeg_client);
        jpeg_client = -1;
        return;
    }

    // 连接成功
    osal_printk("== connect to tcp server success!\r\n");
}
 
// 发送所有数据，确保全部发送（返回 <0 = 失败）
static int JpegSendData(uint8_t *buffer, size_t length) {
    size_t total_sent = 0;
    if (jpeg_client < 0) {
        return -1;
    }
    while (total_sent < length) {
        int sent = send(jpeg_client, buffer + total_sent, length - total_sent, 0);
        if (sent <= 0) {
            return -1;  // 发送错误
        }
        total_sent += sent;
    }
    return (int)total_sent;
}

// 发送一帧：帧头 + 分包载荷（每包 JPEG_CHUNK_SIZE，包间 sleep）
static int JpegSendFrame(uint8_t *frame, size_t len) {
    uint8_t hdr[JPEG_HDR_SIZE];
    uint32_t chk = 0;
    size_t i, off;

    // 计算载荷字节和
    for (i = 0; i < len; i++) {
        chk += frame[i];
    }

    // 组帧头: 魔数 + 大端长度 + 大端校验和
    hdr[0] = JPEG_MAGIC0;
    hdr[1] = JPEG_MAGIC1;
    hdr[2] = (uint8_t)((len >> 24) & 0xFF);
    hdr[3] = (uint8_t)((len >> 16) & 0xFF);
    hdr[4] = (uint8_t)((len >> 8) & 0xFF);
    hdr[5] = (uint8_t)(len & 0xFF);
    hdr[6] = (uint8_t)((chk >> 24) & 0xFF);
    hdr[7] = (uint8_t)((chk >> 16) & 0xFF); 

    // 发送帧头（失败自动重连一次再试）
    if (JpegSendData(hdr, JPEG_HDR_SIZE) < 0) {
        if (JpegReconnect() != 0 || JpegSendData(hdr, JPEG_HDR_SIZE) < 0) {
            return -1;
        }
    }

    // 分包发送载荷
    off = 0;
    while (off < len) {
        size_t n = (len - off > JPEG_CHUNK_SIZE) ? JPEG_CHUNK_SIZE : (len - off);
        if (JpegSendData(&frame[off], n) < 0) { 
            return -1;
        }
        off += n;
        osal_msleep(5);       // 让 lwIP 消化发送缓冲
    }

    osal_printk("== frame sent: %u bytes\r\n", (unsigned int)len);
    return (int)len;
}



/* ==================================================================
 * 业务任务
 * ================================================================== */
void camera_task(void)
{
    if (ov2640_driver_init() != 0) {
        osal_printk("camera init failed\r\n");
        return;
    }
    osal_printk("camera init succ!\r\n"); 

    uint8_t *frame = osal_vmalloc(FRAME_BUF_SIZE);
    if (frame == NULL) {
        osal_printk("vmalloc %u failed\r\n", FRAME_BUF_SIZE);
        return;
    } 

    int frame_no = 0;
    while (1) {
         
        int len = ov2640_capture_jpeg(frame, FRAME_BUF_SIZE);
        frame_no++;
        if (len > 0) { 
            // 发送数据到云端（帧头 + 分包）
            if (jpeg_client >= 0) {
                JpegSendFrame(frame, (size_t)len);
            }

        } else {
            osal_printk("frame#%d capture failed, ret=%d\r\n", frame_no, len);
        }
        osal_msleep(30);
    }
}