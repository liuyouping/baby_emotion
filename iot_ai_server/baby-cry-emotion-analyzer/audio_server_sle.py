# Copyright (c) 2026 稚声慧眸
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
import socket
import wave
import threading
import time
import struct 
import os
import sys

HOST = '0.0.0.0'
PORT = 8866                 # 与 sle_gate 网关 audio_relay.c 的 TCP_PORT 一致（可用命令行参数覆盖）
CHUNK_SIZE = 4096
CHANNELS = 1
SAMPLE_WIDTH = 2
SAMPLE_RATE = 16000

# 双缓冲机制
active_buffer = bytearray()
backup_buffer = bytearray()
buffer_lock = threading.Lock()
running = True
wav_files = "cries"


def save_task():
    global active_buffer, backup_buffer 

    while running:
        time.sleep(5)
        with buffer_lock:
            active_buffer, backup_buffer = backup_buffer, active_buffer
        if backup_buffer: 
            filename = os.path.join(wav_files, time.strftime("fim_%Y%m%d_%H%M%S.wav"))

            print(f"Saving {filename} (size={len(backup_buffer)} bytes)")
            with wave.open(filename, 'wb') as wf:
                wf.setnchannels(CHANNELS)
                wf.setsampwidth(SAMPLE_WIDTH)
                wf.setframerate(SAMPLE_RATE)
                wf.writeframes(backup_buffer)
            backup_buffer.clear()

def calculate_checksum(data):
    return sum(data) & 0xFFFFFFFF

def handle_client(conn, addr):
    print(f"Client connected: {addr}")
    buffer = bytearray()
    HEADER_SIZE = 4
    CHECKSUM_SIZE = 4
    resync_count = 0

    try:
        while True:
            data = conn.recv(CHUNK_SIZE)
            if not data:
                print(f"Client {addr} disconnected.")
                break
            buffer.extend(data)
            while True:
                if len(buffer) < HEADER_SIZE:
                    break

                length = struct.unpack('!I', buffer[:HEADER_SIZE])[0]
                total_packet_size = HEADER_SIZE + length + CHECKSUM_SIZE

                # 异常长度（超上限视为失步）：滑 1 字节找帧头，不丢弃整个缓冲
                if length == 0 or length > 2048:
                    resync_count += 1
                    if resync_count % 100 == 1:
                        print(f"[{addr}] bad frame len={length}, resyncing... (total {resync_count})")
                    del buffer[:1]
                    continue

                if len(buffer) < total_packet_size:
                    break

                frame = buffer[HEADER_SIZE:HEADER_SIZE+length]
                checksum_received = struct.unpack('!I', buffer[HEADER_SIZE+length:total_packet_size])[0]
                checksum_calculated = calculate_checksum(frame)

                if checksum_calculated == checksum_received:
                    with buffer_lock:
                        active_buffer.extend(frame)
                    del buffer[:total_packet_size]
                else:
                    # 校验失败：只滑 1 字节重找帧头（32 位求和校验，
                    # 随机数据误命中概率约 2^-32，滑动重同步是安全的）
                    resync_count += 1
                    if resync_count % 100 == 1:
                        print(f"[{addr}] checksum error, resyncing... (total {resync_count})")
                    del buffer[:1]

    except ConnectionResetError:
        print(f"Client {addr} forcibly closed.")
    except Exception as e:
        print(f"Error with {addr}: {str(e)}")
    finally:
        conn.close()
        print(f"Connection to {addr} closed. (resync_count={resync_count})")

def get_local_ips():
    """枚举本机所有 IPv4，方便确认客户端连接的目标 IP"""
    ips = set()
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    # 兜底：建 UDP socket 不发包也能拿到默认路由出口 IP
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(ips)

def print_at_hint(port):
    """打印可通过 AT 串口(UART0/MGPIO0/1, 115200 8N1)发送的配置指令。
    注意：AT 指令发往 sle_gate 网关板（WiFi/TCP 都在网关上，audio_gate 采集板无 WiFi）。"""
    ips = get_local_ips()
    print("=" * 60)
    print("链路: audio_gate(INMP441, SLE) -> sle_gate 网关 -> 本服务")
    print("网关板 AT 配置指令（串口 UART0/MGPIO0/1，115200 8N1，CR/LF 结尾）：")
    for ip in ips:
        if not ip.startswith('127.'):
            print(f"    AT+SRVIP={ip},{port}          <- 指向本机（推荐）")
    print(f"    AT+WIFI=<ssid>,<密码>       <- 修改 WiFi（保存后自动重启）")
    print("    AT+WIFI?                    <- 查询当前 WiFi 配置")
    print("    AT+WIFICFG=0                <- 清除保存的 WiFi（恢复默认）")
    print("    AT+SRVIP?                   <- 查询服务端地址来源")
    print("    AT+SRVIP=0.0.0.0            <- 清除保存的服务端地址（恢复默认）")
    print("=" * 60)

def start_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(10)
    print(f"Server started on {HOST}:{PORT}")
    print("Local IPs (sle_gate 网关 audio_relay.c 中 TCP_SERVER_ADRESS 应为其中之一):")
    for ip in get_local_ips():
        print(f"    {ip}")
    print_at_hint(PORT)
    try:
        while True:
            conn, addr = s.accept()
            print(f"New connection from {addr}")
            client_thread = threading.Thread(target=handle_client, args=(conn, addr))
            client_thread.start()
    except KeyboardInterrupt:
        print("Server shutting down...")
    finally:
        s.close()

if __name__ == "__main__":
    if not os.path.exists(wav_files):
        os.makedirs(wav_files)

    # 支持命令行指定端口：python audio_server.py [port]（与设备端 AT+SRVIP=<ip>,<port> 配合）
    if len(sys.argv) > 1:
        try:
            PORT = int(sys.argv[1])
        except ValueError:
            print(f"用法: python {sys.argv[0]} [port]  （端口须为整数，默认 {PORT}）")
            sys.exit(1)

    saver_thread = threading.Thread(target=save_task, daemon=True)
    saver_thread.start()
    start_server()
    running = False
    saver_thread.join()