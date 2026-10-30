/*
 * Copyright (c) 2026 稚声慧眸
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"

#include "soc_osal.h"
#include "errcode.h"

/* ===== WS63 I2S(SIO) 驱动 ===== */
#include "i2s.h"            /* uapi_i2s_* / i2s_config_t / i2s_dma_attr_t */
#include "hal_sio.h"        /* MASTER/STD_MODE/THIRTY_TWO_BIT 等枚举 */
#include "dma.h"            /* uapi_dma_init / uapi_dma_open */
#include "sio_porting.h"    /* sio_porting_i2s_pinmux / SIO_BUS_0 */

/* ===== 网络 / WiFi ===== */
#include "lwip/sockets.h"
#include "lwip/netifapi.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "wifi_device.h"

#define TCP_SERVER_ADRESS   "192.168.100.199"    // 要连接的 TCP 服务器地址
#define TCP_PORT            8866                 // 要连接的 TCP 服务器端口号

#define WIFI_SSID           "iot"
#define WIFI_PASSWORD       "12345678"

/* ===== 音频参数 ===== */
#define SAMPLES_PER_PACKET      1024            /* 每包样本数（与 Hi3861 版一致） */
#define DMA_WORDS_PER_READ      (SAMPLES_PER_PACKET * 2)  /* L/R 交织，每字 32bit */

/* I2S 配置（对齐官方 i2s_dma_lli 示例） */
#define AUDIO_I2S_BUS           SIO_BUS_0
#define I2S_DIV_NUMBER          32
#define I2S_TX_INT_THRESHOLD    7
#define I2S_RX_INT_THRESHOLD    1
#define I2S_DMA_SRC_WIDTH       2               /* 4 字节 */
#define I2S_DMA_DEST_WIDTH      2
#define I2S_DMA_BURST_LENGTH    0

/* WiFi 连接重试参数 */
#define WIFI_CONN_STATUS_MAX_GET_TIMES      20
#define DHCP_BOUND_STATUS_MAX_GET_TIMES     20
#define WIFI_STA_IP_MAX_GET_TIMES           10

/* SDK 头文件未提供该宏（baby_gate 工程同样本地定义）：最多扫描 AP 数 */
#define WIFI_SCAN_AP_LIMIT                  64

/* 在 sock_fd 上收发 */
static int sock_fd = -1;

/* DMA 原始缓冲（左右声道交织的 32bit 字）与 PCM 输出缓冲 */
static uint32_t g_dma_words[DMA_WORDS_PER_READ];
static int16_t  g_pcm[SAMPLES_PER_PACKET];

/* ==================================================================
 * WiFi 连接（WS63 middleware wifi API，模式来自 ws63e baby_gate 工程）
 * ================================================================== */
static errcode_t example_get_match_network(const char *expected_ssid,
                                           const char *key,
                                           wifi_sta_config_stru *expected_bss)
{
    uint32_t num = WIFI_SCAN_AP_LIMIT;     /* 最多扫描 AP 数 */
    uint32_t bss_index = 0;
    uint32_t scan_len = sizeof(wifi_scan_info_stru) * WIFI_SCAN_AP_LIMIT;
    wifi_scan_info_stru *result = osal_kmalloc(scan_len, OSAL_GFP_ATOMIC);

    if (result == NULL) {
        return ERRCODE_MALLOC;
    }
    memset_s(result, scan_len, 0, scan_len);
    if (wifi_sta_get_scan_info(result, &num) != ERRCODE_SUCC) {
        osal_kfree(result);
        return ERRCODE_FAIL;
    }

    /* 在扫描结果中查找目标网络 */
    for (bss_index = 0; bss_index < num; bss_index++) {
        if (strlen(expected_ssid) == strlen(result[bss_index].ssid) &&
            memcmp(expected_ssid, result[bss_index].ssid, strlen(expected_ssid)) == 0) {
            break;
        }
    }
    if (bss_index >= num) {
        osal_kfree(result);
        return ERRCODE_FAIL;
    }

    /* 复制匹配网络的连接配置 */
    if (memcpy_s(expected_bss->ssid, WIFI_MAX_SSID_LEN, result[bss_index].ssid,
                 WIFI_MAX_SSID_LEN) != EOK) {
        osal_kfree(result);
        return ERRCODE_MEMCPY;
    }
    if (memcpy_s(expected_bss->bssid, WIFI_MAC_LEN, result[bss_index].bssid,
                 WIFI_MAC_LEN) != EOK) {
        osal_kfree(result);
        return ERRCODE_MEMCPY;
    }
    expected_bss->security_type = result[bss_index].security_type;
    if (memcpy_s(expected_bss->pre_shared_key, WIFI_MAX_KEY_LEN, key, strlen(key)) != EOK) {
        osal_kfree(result);
        return ERRCODE_MEMCPY;
    }
    expected_bss->ip_type = DHCP;
    osal_kfree(result);
    return ERRCODE_SUCC;
}

static int WifiConnect(const char *ssid, const char *psk)
{
    char ifname[WIFI_IFNAME_MAX_SIZE + 1] = "wlan0";
    wifi_sta_config_stru expected_bss = {0};
    wifi_linked_info_stru wifi_status;
    struct netif *lwip_netif = NULL;
    uint8_t index = 0;

    /* 使能 STA 模式 */
    if (wifi_sta_enable() != ERRCODE_SUCC) {
        osal_printk("=!= STA enable failed!\r\n");
        return -1;
    }

    do {
        osal_printk("\r\n=== Start scanning...\r\n");
        (void)osDelay(100);

        if (wifi_sta_scan() != ERRCODE_SUCC) {
            osal_printk("=!= Scan failed, retry!\r\n");
            continue;
        }
        (void)osDelay(300);

        if (example_get_match_network(ssid, psk, &expected_bss) != ERRCODE_SUCC) {
            osal_printk("=-= Target AP not found, retry!\r\n");
            continue;
        }

        osal_printk("=== Attempting to connect...\r\n");
        if (wifi_sta_connect(&expected_bss) != ERRCODE_SUCC) {
            continue;
        }

        /* 轮询连接状态 */
        for (index = 0; index < WIFI_CONN_STATUS_MAX_GET_TIMES; index++) {
            (void)osDelay(50);
            memset_s(&wifi_status, sizeof(wifi_status), 0, sizeof(wifi_status));
            if (wifi_sta_get_ap_info(&wifi_status) != ERRCODE_SUCC) {
                continue;
            }
            if (wifi_status.conn_state == WIFI_CONNECTED) {
                break;
            }
        }
        if (wifi_status.conn_state == WIFI_CONNECTED) {
            break;
        }
    } while (1);

    /* 获取网卡并启动 DHCP */
    lwip_netif = netifapi_netif_find(ifname);
    if (lwip_netif == NULL) {
        osal_printk("=!= netif find failed!\r\n");
        return -1;
    }
    if (netifapi_dhcp_start(lwip_netif) != ERR_OK) {
        osal_printk("=!= DHCP start failed!\r\n");
        return -1;
    }
    for (uint8_t i = 0; i < DHCP_BOUND_STATUS_MAX_GET_TIMES; i++) {
        (void)osDelay(50);
        if (netifapi_dhcp_is_bound(lwip_netif) == ERR_OK) {
            osal_printk("=+= DHCP bound successfully!\r\n");
            break;
        }
    }

    /* 等待 IP 生效并打印 */
    for (uint8_t i = 0; i < WIFI_STA_IP_MAX_GET_TIMES; i++) {
        (void)osDelay(10);
        if (lwip_netif->ip_addr.u_addr.ip4.addr != 0) {
            osal_printk("=+= Local IP: %u.%u.%u.%u\r\n",
                        (lwip_netif->ip_addr.u_addr.ip4.addr) & 0xff,
                        (lwip_netif->ip_addr.u_addr.ip4.addr >> 8) & 0xff,
                        (lwip_netif->ip_addr.u_addr.ip4.addr >> 16) & 0xff,
                        (lwip_netif->ip_addr.u_addr.ip4.addr >> 24) & 0xff);
            osal_printk("=+= WiFi connection successful!\r\n\r\n");
            return 0;
        }
    }
    osal_printk("=!= Get IP failed!\r\n");
    return -1;
}

/* ==================================================================
 * TCP 客户端（包格式与 Hi3861 版完全一致，服务端无需修改）
 * ================================================================== */
static void TCPClientInit(void)
{
    struct sockaddr_in send_addr = {0};

    /* 连接 WiFi */
    if (WifiConnect(WIFI_SSID, WIFI_PASSWORD) != 0) {
        osal_printk("=!= WiFi connect failed!\r\n");
        return;
    }
    (void)osDelay(300);

    /* 创建 socket */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        osal_printk("create socket failed!\r\n");
        return;
    }

    /* 初始化预连接的服务端地址 */
    send_addr.sin_family = AF_INET;
    send_addr.sin_port = htons(TCP_PORT);
    send_addr.sin_addr.s_addr = inet_addr(TCP_SERVER_ADRESS);

    if (connect(sock_fd, (struct sockaddr *)&send_addr, sizeof(send_addr)) < 0) {
        osal_printk("connect failed!\r\n");
        close(sock_fd);
        sock_fd = -1;
    } else {
        osal_printk("connect to server %s success!\r\n\r\n", TCP_SERVER_ADRESS);
    }
}

/* 简单校验和计算（按字节） */
static uint32_t calculate_checksum(const int16_t *data, size_t length)
{
    uint32_t checksum = 0;
    const uint8_t *byte_data = (const uint8_t *)data;

    for (size_t i = 0; i < length * 2; i++) {    /* 每个int16_t包含2个字节 */
        checksum += byte_data[i];
    }
    return checksum;
}

/* 发送所有数据，确保全部发送 */
static int send_all(int sock, const uint8_t *buffer, size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length) {
        int sent = send(sock, buffer + total_sent, length - total_sent, 0);
        if (sent < 0) {
            return -1;
        }
        total_sent += sent;
    }
    return (int)total_sent;
}

/* 打包发送：[4B 长度][PCM 数据][4B 校验和]，全部网络字节序 */
static int create_packet(const int16_t *buffer, size_t samples)
{
    uint32_t length = samples * sizeof(int16_t);           /* 1024*2 = 2048 */
    uint32_t checksum = calculate_checksum(buffer, samples);
    size_t packet_size = sizeof(length) + length + sizeof(checksum);
    uint8_t *packet = malloc(packet_size);
    uint32_t net_length = htonl(length);
    uint32_t net_checksum = htonl(checksum);

    if (packet == NULL) {
        osal_printk("Memory allocation failed.\r\n");
        return -1;
    }

    memcpy(packet, &net_length, sizeof(net_length));
    memcpy(packet + sizeof(net_length), buffer, length);
    memcpy(packet + sizeof(net_length) + length, &net_checksum, sizeof(net_checksum));

    if (send_all(sock_fd, packet, packet_size) != (int)packet_size) {
        osal_printk("=!= Send error, reconnect...\r\n");
        close(sock_fd);
        sock_fd = -1;
        free(packet);
        return -1;
    }
    free(packet);
    return 0;
}

/* ==================================================================
 * I2S 初始化（WS63 SIO 驱动，主模式 + DMA）
 * INMP441 为从设备，BCLK/WS 由 WS63E 产生，采样率固定 16kHz。
 * ================================================================== */
static int i2s_init_demo(void)
{
    errcode_t ret;

    ret = uapi_i2s_deinit(AUDIO_I2S_BUS);
    if (ret != ERRCODE_SUCC) {
        osal_printk("Failed to deinit i2s! ret=0x%x\r\n", ret);
    }

    ret = uapi_i2s_init(AUDIO_I2S_BUS, NULL);      /* DMA 模式无需回调 */
    if (ret != ERRCODE_SUCC) {
        osal_printk("Failed to init i2s! ret=0x%x\r\n", ret);
        return -1;
    }

    /* MGPIO9~12 → I2S 复用（模式 4） */
    sio_porting_i2s_pinmux();

    /* 主模式 / 标准时序 / 32bit / 双通道（L+R） */
    i2s_config_t config = {
        .drive_mode = MASTER,
        .transfer_mode = STD_MODE,
        .data_width = THIRTY_TWO_BIT,
        .channels_num = TWO_CH,
        .timing = NONE_TIMING_MODE,
        .clk_edge = RISING_EDGE,
        .div_number = I2S_DIV_NUMBER,
        .number_of_channels = 2,
    };

    /* RX 走 DMA（TX 不用，保持关闭） */
    i2s_dma_attr_t attr = {
        .tx_dma_enable = 0,
        .tx_int_threshold = I2S_TX_INT_THRESHOLD,
        .rx_dma_enable = 1,
        .rx_int_threshold = I2S_RX_INT_THRESHOLD,
    };

    uapi_i2s_set_config(AUDIO_I2S_BUS, &config);
    uapi_i2s_dma_config(AUDIO_I2S_BUS, &attr);

    /* DMA 初始化（传输完成后通道自动释放，可循环调用） */
    uapi_dma_init();
    uapi_dma_open();

    osal_printk("I2S init success! (master, 32bit slot, 16kHz, pins MGPIO10/11/12)\r\n");
    return 0;
}

/* ==================================================================
 * 主任务：WiFi → TCP → I2S 循环采集发送
 * ================================================================== */
static void i2s_main_demo(void *arg)
{
    (void)arg;
    i2s_dma_config_t dma_cfg = {
        .src_width = I2S_DMA_SRC_WIDTH,
        .dest_width = I2S_DMA_DEST_WIDTH,
        .burst_length = I2S_DMA_BURST_LENGTH,
        .priority = 0,
    };

    TCPClientInit();
    if (sock_fd == -1) {
        osal_printk("=!= TCP not ready, audio task exit.\r\n");
        return;
    }

    if (i2s_init_demo() != 0) {
        return;
    }
    (void)osDelay(200);      /* 等 I2S 时钟稳定 */

    while (1) {
        /* 阻塞式 DMA 读取：读满 2048 个 32bit 字（L/R 交织，约 128ms @16kHz） */
        int32_t ret = uapi_i2s_merge_read_by_dma(AUDIO_I2S_BUS, g_dma_words,
                                                 DMA_WORDS_PER_READ, &dma_cfg,
                                                 (uintptr_t)NULL, true);
        if (ret <= 0) {
            osal_printk("==> i2s merge read failed: %d\r\n", ret);
            (void)osDelay(10);
            continue;
        }

        /* merge 数据左右声道按字交织：偶数下标为左声道（INMP441 L/R 接地）。
         * INMP441 24bit 数据在 32bit slot 高 24 位，取高 16 位 → 16bit PCM。
         * （若录到的全是静音，说明左右声道顺序相反，把 i*2 改成 i*2+1） */
        for (int i = 0; i < SAMPLES_PER_PACKET; i++) {
            g_pcm[i] = (int16_t)(g_dma_words[i * 2] >> 16);
        }

        if (create_packet(g_pcm, SAMPLES_PER_PACKET) != 0) {
            /* 发送失败：create_packet 已关闭 sock_fd，退出采集 */
            osal_printk("=!= audio task exit.\r\n");
            return;
        }
    }
}

/* demo线程创建 */
static void INMP441TestDemo(void)
{
    osThreadAttr_t attr = {0};

    attr.name = "INMP441Task";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = 10240;
    attr.priority = osPriorityNormal;

    if (osThreadNew((osThreadFunc_t)i2s_main_demo, NULL, &attr) == NULL) {
        osal_printk("[INMP441Task] Failed to create INMP441Task!\r\n");
    }
}

APP_FEATURE_INIT(INMP441TestDemo);
