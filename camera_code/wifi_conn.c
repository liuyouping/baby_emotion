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
#include <unistd.h> 
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "soc_osal.h"
 
#include "wifi_conn.h"                      //
#include "camera.h"
 

#define WIFI_TASK_STACK_SIZE 0x2000        // Task stack size(8KB)
#define WIFI_SCAN_AP_LIMIT 64              // Maximum number of APs to scan
#define WIFI_CONN_STATUS_MAX_GET_TIMES 5   // Connection status check times
#define DHCP_BOUND_STATUS_MAX_GET_TIMES 20 // DHCP bound status check times
#define WIFI_STA_IP_MAX_GET_TIMES 5        // IP acquisition status check times 
 
#define CONFIG_WIFI_SSID        "iot"       //"CMCC-C2KH"         // Target WiFi SSID
#define CONFIG_WIFI_PWD         "12345678"  //3ksc3ubc"          // WiFi password  

#define Listen_Port             8866 
#define TCP_SERVER_ADRESS       "192.168.1.1"   //"172.20.10.7"      //"192.168.1.27"     // TCP服务器地址
#define TCP_PORT                8888                // TCP服务器端口号
  
//======================= wifi 操作 ===============
 
static int reconnCnt = 0;                           // 重连次数 
static osThreadId_t g_reconn_id = NULL;             // 重连线程ID    
int g_ConnectSuccess = 0;                           // 供外部使用: 1表示wifi成功   

static void wifi_scan_state_changed(int32_t state, int32_t size)
{
    UNUSED(state);
    UNUSED(size);
    osal_printk("=== Scan completed! \r\n");
}

static void wifi_connection_changed(int32_t state, const wifi_linked_info_stru *info, int32_t reason_code)
{
    UNUSED(reason_code);

    if (state == WIFI_STATE_AVALIABLE)
        osal_printk("=== WiFi: %s,  Signal Strength: %d\r\n", info->ssid, info->rssi);

    if (state == WIFI_STATE_AVALIABLE) {
        g_ConnectSuccess = 1;  
        osal_printk("### wifi is avaliable, state: %d. \r\n", state);    
    }
    else {
        g_ConnectSuccess = 0; 
        osal_printk("### wifi  is not avaliable, state: %d. \r\n",   state); 
    }      
}

static errcode_t example_get_match_network(const char *expected_ssid,
                                           const char *key,
                                           wifi_sta_config_stru *expected_bss)
{
    uint32_t num = WIFI_SCAN_AP_LIMIT; // Maximum number of APs to scan
    uint32_t bss_index = 0;

    // Allocate and initialize scan result buffer
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

    // Find target network in scan results
    for (bss_index = 0; bss_index < num; bss_index++) {
        if (strlen(expected_ssid) == strlen(result[bss_index].ssid)) {
            if (memcmp(expected_ssid, result[bss_index].ssid, strlen(expected_ssid)) == 0) {
                break;
            }
        }
    }
 
    // Copy configuration information of matched network 
    expected_bss->security_type = result[bss_index].security_type;
    if (memcpy_s(expected_bss->pre_shared_key, WIFI_MAX_KEY_LEN, key, strlen(key)) != EOK) {
        osal_kfree(result);
        return ERRCODE_MEMCPY;
    }
    expected_bss->ip_type = DHCP; // Use DHCP to get IP automatically
    osal_kfree(result);
    return ERRCODE_SUCC;
}

static errcode_t wifi_connect(void)
{
    char ifname[WIFI_IFNAME_MAX_SIZE + 1] = "wlan0"; // Network interface name
    wifi_sta_config_stru expected_bss = {0};         // Connection configuration
    char ssid_buf[WIFI_MAX_SSID_LEN] = {0};          // flash 配置缓冲
    char psk_buf[WIFI_MAX_KEY_LEN] = {0};            // flash 配置缓冲
    const char *expected_ssid = CONFIG_WIFI_SSID;    // Target SSID（默认）
    const char *key = CONFIG_WIFI_PWD;               // WiFi password（默认）
    struct netif *g_lwip_netif = NULL;               // Network interface 

    wifi_linked_info_stru wifi_status;               // WiFi connection status
    uint8_t index = 0;                               // Connection status check count 

    // Enable STA mode
    if (wifi_sta_enable() != ERRCODE_SUCC) {
        osal_printk("=!= STA enable failed!\r\n");
        return ERRCODE_FAIL;
    }

    do {
        osal_printk("\r\n=== Start scanning...\r\n");
        (void)osDelay(100); // Delay 100ms

        // Execute WiFi scan
        if (wifi_sta_scan() != ERRCODE_SUCC) {
            osal_printk("=!= Scan failed, retry!\r\n");
            continue;
        }

        (void)osDelay(300); // Delay 300ms

        // Find target network
        if (example_get_match_network(expected_ssid, key, &expected_bss) != ERRCODE_SUCC) {
            osal_printk("=-= Target AP not found, retry! \r\n");
            continue;
        }

        osal_printk("=== Attempting to connect...\r\n");
        // Start connection
        if (wifi_sta_connect(&expected_bss) != ERRCODE_SUCC) {
            continue;
        }

        // Check connection status
        for (index = 0; index < WIFI_CONN_STATUS_MAX_GET_TIMES; index++) {
            (void)osDelay(50); // Delay 50ms
            memset_s(&wifi_status, sizeof(wifi_status), 0, sizeof(wifi_status));
            if (wifi_sta_get_ap_info(&wifi_status) != ERRCODE_SUCC) {
                continue;
            }
            if (wifi_status.conn_state == WIFI_CONNECTED) {
                break;
            }
        }
        if (wifi_status.conn_state == WIFI_CONNECTED) {
            break; // Connection successful, exit loop
        }
    } while (1);

    // Get network interface
    g_lwip_netif = netifapi_netif_find(ifname);
    if (g_lwip_netif == NULL) {
        return ERRCODE_FAIL;
    }

    // Start DHCP client
    if (netifapi_dhcp_start(g_lwip_netif) != ERR_OK) {
        osal_printk("=!= DHCP start failed!\r\n");
        return ERRCODE_FAIL;
    }
 

    // Get and print IP address
    for (uint8_t i = 0; i < WIFI_STA_IP_MAX_GET_TIMES; i++) {
        osDelay(1); // Delay 1ms
        if (g_lwip_netif->ip_addr.u_addr.ip4.addr != 0) {
            osal_printk("=+= Local IP: %u.%u.%u.%u\r\n", (g_lwip_netif->ip_addr.u_addr.ip4.addr & 0x000000ff),
                     // >>8 Shift address right by 8 bits, move original second byte to lowest byte position
                   (g_lwip_netif->ip_addr.u_addr.ip4.addr >> 8) & 0xff, (g_lwip_netif->ip_addr.u_addr.ip4.addr >> 16) & 0xff,
                   (g_lwip_netif->ip_addr.u_addr.ip4.addr >> 24) & 0xff); // >>24 Shift address right by 24 bits, move original fourth byte to lowest byte position
            osal_printk("=+= Connection successful! \r\n\r\n"); 
 
            camera_task();

            return ERRCODE_SUCC;
        }
    }
    osal_printk("=!= Connection failed!\r\n");
    return ERRCODE_FAIL;
}

int wifi_tcp_sample_init(const char *argument)
{
    UNUSED(argument); 

    // Register WiFi event callbacks
    wifi_event_stru wifi_event_cb = {0}; 

    if (wifi_register_event_cb(&wifi_event_cb) != 0) {
        osal_printk("=!= Event callback registration failed\r\n");
        return -1;
    }
    osal_printk("=+= Event callback registration successful\r\n");
 
    // Wait for WiFi initialization to complete
    while (wifi_is_wifi_inited() == 0) {
        (void)osDelay(10); // Delay 10ms
    }

    // Connect to WiFi
    if (wifi_connect() != ERRCODE_SUCC) {
        return -1;
    } 
    
    return 0;
}

static void wifi_tcp_sample(void)
{
    // Configure task attributes
    osThreadAttr_t attr = {.name = "wifi_tcp_task", .stack_size = WIFI_TASK_STACK_SIZE, .priority = osPriorityNormal};

    // Create task
    if (osThreadNew((osThreadFunc_t)wifi_tcp_sample_init, NULL, &attr) == NULL) {
        osal_printk("=!= Task creation failed\r\n");
    }
    osal_printk("=== Task created successfully\r\n");
}
 
// Start application
APP_FEATURE_INIT(wifi_tcp_sample);