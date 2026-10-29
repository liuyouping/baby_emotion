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
#ifndef __CAMERA_H__
#define __CAMERA_H__

#include "errcode.h"  
 

static void TcpClientInit(void);

static void SendToServer(int sock_client, uint8_t *data, uint8_t len);

static void TcpServerInit(void);

void camera_task(void);

#endif /* __CAMERA_H__ */
