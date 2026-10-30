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
#ifndef __WIFI_CONNECT_H__
#define __WIFI_CONNECT_H__

#include "errcode.h"
#include <stdint.h>

static errcode_t wifi_connect(void);
 
/* v23: 读取 AT+SRVIP 手动保存的 TCP 服务端地址（wifi_conn.c 实现）。
 * 返回 0 = 已保存（*ip_be 网络字节序，*port 0=用默认端口），-1 = 无手动配置。 */
int cfg_load_srv_ip(uint32_t *ip_be, uint16_t *port);

/* 保存服务端地址到 NV（ip 网络字节序，port 0=用默认端口）；成功返回 0 */
static int cfg_save_srv_ip(uint32_t ip_be, uint16_t port);

#endif /* __WIFI_CONNECT_H__ */
