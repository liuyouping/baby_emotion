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
