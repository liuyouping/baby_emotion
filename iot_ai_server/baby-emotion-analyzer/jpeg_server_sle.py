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
# -*- coding: utf-8 -*-


import socket
import struct
import threading
import time
import os

HOST = '0.0.0.0'
PORT = 8886                 # 网关端 JPEG_TCP_PORT 保持一致（audio_relay.c）
CHUNK_SIZE = 64*1024

MAGIC = b'\xa5\x5a'         # 帧头魔数
HDR_SIZE = 10               # 帧头大小: 魔数2 + 长度4 + 校验4
MIN_FRAME_SIZE = 64         # 小于该长度的帧视为异常丢弃
MAX_FRAME_SIZE = 2 * 1024 * 1024   # 单帧上限 2MB（防长度字段损坏）
SAVE_DIR = "imgs/baby"      #"jpeg_files"
VERBOSE_RECV = False        # True: 打印每一次 recv 的字节数(排障用)

SOI = b'\xff\xd8'           # JPEG 帧头
EOI = b'\xff\xd9'           # JPEG 帧尾

running = True
stats_lock = threading.Lock()
stats = {"frames_ok": 0, "frames_trunc": 0, "frames_bad": 0, "bytes_rx": 0}


def log(msg):
    """带时间戳的日志, 强制 flush 防止输出缓冲看不到"""
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def list_local_ipv4():
    """列出本机所有 IPv4 地址, 便于确认网关 AT+SRVIP 配置的 IP 在本机"""
    ips = set()
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
        for info in infos:
            ips.add(info[4][0])
    except Exception:
        pass
    # 兜底: 通过 UDP connect 探测默认路由出口 IP(不会真正发包)
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        ips.add(probe.getsockname()[0])
        probe.close()
    except Exception:
        pass
    return sorted(ips)


def check_port_conflict(port):
    """检测端口是否已被其他进程监听"""
    try:
        test = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        test.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        test.bind((HOST, port))
        test.close()
        return None  # 绑定成功, 无冲突
    except OSError as e:
        log("!! 端口 %d 无法绑定: %s" % (port, e))
        if os.name == 'nt':
            log("   请执行 netstat -ano | findstr %d 查看占用进程, 或更换端口" % port)
        return str(e)


def save_frame(frame, truncated, addr):
    """保存一帧 JPEG 到文件"""
    ts = time.strftime("%Y%m%d_%H%M%S") + "_%03d" % (int(time.time() * 1000) % 1000)
    suffix = "_trunc.jpg" if truncated else ".jpg"
    filename = os.path.join(SAVE_DIR, ts + suffix)
    with open(filename, 'wb') as f:
        f.write(frame)
    tag = "TRUNCATED" if truncated else "OK"
    log("[FRAME] %s from %s size=%d -> %s" % (tag, addr, len(frame), filename))


def checksum32(data):
    """与设备端一致: 载荷字节和 (无符号 32 位)"""
    return sum(data) & 0xFFFFFFFF


def extract_frames(buffer, addr):
    """
    从累积缓冲区中解析完整帧（就地消费已处理字节）。
    协议: A5 5A | LEN(4B BE) | CHK(4B BE) | 载荷 LEN 字节
    返回本次解析出的帧数
    """
    count = 0
    while True:
        # 1. 找魔数同步
        magic_pos = buffer.find(MAGIC)
        if magic_pos < 0:
            # 没有魔数: 丢弃噪声, 保留末尾 1 字节(可能是被截断的 0xA5)
            if buffer[-1:] == b'\xa5':
                del buffer[:-1]
            else:
                del buffer[:]
            break

        # 2. 帧头够不够 10 字节
        if len(buffer) < magic_pos + HDR_SIZE:
            if magic_pos > 0:
                del buffer[:magic_pos]     # 丢掉魔数前的噪声
            break                           # 等更多数据

        hdr = buffer[magic_pos:magic_pos + HDR_SIZE]
        length, chk = struct.unpack('>II', hdr[2:HDR_SIZE])

        # 3. 长度合法性检查（防长度字段损坏导致疯狂等待）
        if length < MIN_FRAME_SIZE or length > MAX_FRAME_SIZE:
            log("!! 非法长度 %d, 跳过该魔数重新同步 (from %s)" % (length, addr))
            del buffer[:magic_pos + 2]
            with stats_lock:
                stats["frames_bad"] += 1
            continue

        # 4. 载荷收够了没有
        total = magic_pos + HDR_SIZE + length
        if len(buffer) < total:
            if magic_pos > 0:
                del buffer[:magic_pos]
            break                           # 等更多数据

        # 5. 校验
        frame = bytes(buffer[magic_pos + HDR_SIZE:total])
        if checksum32(frame) != chk:
            log("!! 校验失败 (recv=0x%08X calc=0x%08X), 丢弃并重新同步" % (
                chk, checksum32(frame)))
            del buffer[:total]
            with stats_lock:
                stats["frames_bad"] += 1
            continue

        # 6. 校验通过, 保存（依据 EOI 判断是否截断帧）
        del buffer[:total]
        truncated = not (frame[:2] == SOI and frame[-2:] == EOI)
        save_frame(frame, truncated, addr)
        with stats_lock:
            if truncated:
                stats["frames_trunc"] += 1
            else:
                stats["frames_ok"] += 1
        count += 1
    return count


def handle_client(conn, addr):
    log(">>> 网关已连接: %s" % (addr,))
    buffer = bytearray()
    total = 0
    try:
        while True:
            data = conn.recv(CHUNK_SIZE)
            if not data:
                log("<<< 网关 %s 断开连接 (共接收 %d 字节)" % (addr, total))
                break
            total += len(data)
            with stats_lock:
                stats["bytes_rx"] += len(data)
            if VERBOSE_RECV:
                log("recv %d bytes (total=%d)" % (len(data), total))
            buffer.extend(data)
            extract_frames(buffer, addr)
    except ConnectionResetError:
        log("!! 网关 %s 强制断开 (共接收 %d 字节)" % (addr, total))
    except Exception as e:
        log("!! 与 %s 通信出错: %s (共接收 %d 字节)" % (addr, e, total))
    finally:
        conn.close()
        log("--- 连接 %s 已关闭" % (addr,))


def stats_task():
    """每 10 秒打印一次心跳+统计信息(证明进程活着)"""
    while running:
        time.sleep(10)
        with stats_lock:
            log("[STATS] 完整帧=%d 截断帧=%d 坏帧=%d 累计接收=%.1fKB" % (
                stats["frames_ok"], stats["frames_trunc"], stats["frames_bad"],
                stats["bytes_rx"] / 1024.0))


def start_server():
    # 启动诊断: 本机 IP + 端口冲突检测
    ips = list_local_ipv4()
    log("本机 IPv4 地址: %s" % (", ".join(ips) if ips else "未获取到"))
    log("提示: 网关端用 AT+SRVIP=<本机IP> 配置（与 audio_server.py 共用），"
        "端口固定 %d" % PORT)

    conflict = check_port_conflict(PORT)
    if conflict is not None:
        log("服务未启动。请先解决端口冲突后重试。")
        return

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(5)
    log("JPEG 服务器已启动, 监听 %s:%d, 等待网关连接..." % (HOST, PORT))
    log("帧协议: A5 5A + LEN(4B) + CHK(4B) + JPEG载荷")
    log("保存目录: %s" % os.path.abspath(SAVE_DIR))

    try:
        while True:
            conn, addr = s.accept()
            log("=== 收到新连接请求: %s ===" % (addr,))
            t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
            t.start()
    except KeyboardInterrupt:
        log("服务器停止...")
    finally:
        s.close()


if __name__ == "__main__":
    if not os.path.exists(SAVE_DIR):
        os.makedirs(SAVE_DIR)
    threading.Thread().start()
    start_server()
