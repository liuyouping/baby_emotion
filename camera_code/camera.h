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
#ifndef __CAMERA_H__
#define __CAMERA_H__

#include "errcode.h"  
 

static void TcpClientInit(void);

static void SendToServer(int sock_client, uint8_t *data, uint8_t len);

static void TcpServerInit(void);

void camera_task(void);

#endif /* __CAMERA_H__ */
