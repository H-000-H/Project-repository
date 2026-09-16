# 一点都不想写这个文件但是操蛋的tabby和一些上位机就是没有这种功能，只能自己写如果自己使用的上位机有这个功能就不需要用这个
import socket 
import time 
import argparse
import os
import shutil
import sys
from typing import Optional

try:
    import serial          # type: ignore  # pyserial: 仅 mode 1 (写串口) 需要, 缺失时给出可读提示
except ImportError:
    serial = None

_REFRESH_INTERVAL = 0.1    # 进度条刷新间隔(秒): 每块都刷会拖慢发送, 这里做节流
_last_draw = 0.0

def _draw_progress(sent: int, total: int, start: float, force: bool = False) -> None:
    '''
    brief : 在单行内刷新发送进度 (百分比 / 已发字节 / 速率 / 耗时)
    sent  : 已发送字节数
    total : 文件总字节数
    start : 发送开始时刻 (time.time())
    force : 忽略节流立刻刷新 (用于起始 0% 与收尾)
    '''
    global _last_draw
    now = time.time()
    if not force and (now - _last_draw) < _REFRESH_INTERVAL:
        return
    _last_draw = now

    ratio = sent / total if total else 1.0
    width = max(10, min(40, shutil.get_terminal_size((80, 24)).columns - 46))
    filled = int(width * ratio)
    bar = "#" * filled + "-" * (width - filled)
    elapsed = now - start
    speed = (sent / elapsed / 1024.0) if elapsed > 0 else 0.0

    sys.stdout.write(f"\r[{bar}] {ratio * 100:6.2f}%  {sent}/{total} B  {speed:8.1f} KB/s  {elapsed:6.1f}s")
    sys.stdout.flush()
    if sent >= total:
        sys.stdout.write("\n")      # 收尾换行, 免得后续输出黏在进度条上
        sys.stdout.flush()

def _send_trigger(host: str, port: int, fw_len: int) -> bool:
    '''
    brief : 连业务口发一行 "cmdota <len>", 让设备置位进入 OTA 接收状态
    host/port : 业务通信口 (USART1 / communicate), 不是 OTA 固件流口
    fw_len    : 镜像总字节数, 设备靠它定位镜像尾部 meta, 必须与随后发送的字节数一致
    '''
    try:
        with socket.create_connection((host, port), timeout=5.0) as s:
            s.sendall(f"cmdota {fw_len}\n".encode())
            time.sleep(0.2)   # 给对端把数据推给 UART 的时间; 发完立即 close 会被丢掉
        print(f"Trigger sent: 'cmdota {fw_len}' -> {host}:{port}")
        return True
    except Exception as e:
        print(f"Error sending trigger to {host}:{port}: {e}")
        return False

def _wait_ready(sock: socket.socket, timeout_s: float) -> bool:
    '''
    brief : 等设备发来就绪握手字节 'R'
    sock  : 已连接的 OTA 口 socket
    说明  : 正路 —— 设备擦完 flash 立刻在 OTA 信道回一个 'R', 上位机在同一条连接上收。
            一个端口搞定, 不碰设备日志, 也不会和"人看日志"抢串口。
            设备侧对应 app/ota/app_ota.cpp 的 OtaDownloadSource()。
    return: 收到就绪 True; 超时/对端关闭 False; timeout_s<=0 时直接放行 (兼容旧固件)
    '''
    if timeout_s <= 0:
        return True

    old_timeout = sock.gettimeout()
    sock.settimeout(timeout_s)
    try:
        skipped = 0
        while True:
            data = sock.recv(1)
            if not data:
                print("Error: OTA link closed before ready")
                return False
            if data == b"R":
                print("Device ready (flash erased), streaming image...")
                return True
            # 其余字节忽略 (线路初始字节 / 噪声 / 上一轮残留), 前几个打印出来便于排查
            skipped += 1
            if skipped <= 8:
                print(f"  [handshake] ignoring 0x{data[0]:02x}")
    except socket.timeout:
        print(f"Error: ready handshake timed out ({timeout_s}s); "
              f"use --ready-timeout 0 to skip for legacy firmware")
        return False
    except Exception as e:
        print(f"Error waiting for ready: {e}")
        return False
    finally:
        sock.settimeout(old_timeout)

def _wait_ready_by_log(host: str, port: int, timeout_s: float) -> bool:
    '''
    brief : 连日志口, 等设备打出就绪标记 OTA_READY (兼容旧固件的备用路径)
    说明  : 只有设备固件尚未实现 OTA 信道自握手时才需要走这条 —— 拿 另外一个端口 日志当控制面。
            代价: 上位机要按字节匹配日志文本, 且日志口单客户端, 会与"人看日志"互斥。
            设备每轮 download_stream 重试都会重新打这个标记, 晚连一会儿也能等到。
    return: 见到标记 True; 超时/连接失败 False; port<=0 或 timeout<=0 时直接放行
    '''
    if port <= 0 or timeout_s <= 0:
        return True

    buf = b""
    try:
        with socket.create_connection((host, port), timeout=5.0) as s:
            s.settimeout(0.5)
            deadline = time.time() + timeout_s
            while time.time() < deadline:
                try:
                    d = s.recv(512)
                except socket.timeout:
                    continue
                if not d:
                    break
                buf = (buf + d)[-4096:]     # 只留尾部, 免得日志量大时无限增长
                if b"OTA_READY" in buf:
                    print("Device ready (flash erased), streaming image...")
                    return True
    except Exception as e:
        print(f"Error waiting for ready on log port {port}: {e}")
        return False

    print(f"Error: OTA_READY not seen on log port {port} within {timeout_s}s "
          f"(got {len(buf)} bytes, tail={buf[-200:]!r})")
    return False

def down_load(image_file:str,size : int,Host:str,Port:str,mode:int,
              serial_port:Optional[str]=None,baudrate:int=115200,
              cmd_port:int=0,cmd_wait:float=0.3,ready_timeout:float=15.0,
              ready_port:int=0) -> bool:
    '''
    brief : 读取本地镜像文件, 按 mode 选择经 TCP 转发到串口或直接写本机串口
    image_file : str : 镜像文件路径
    size : int : 文件每次下载的块大小
    Host : str : 下载镜像的主机地址 (mode 0)
    Port : str : 连接主机的端口 (mode 0), 即 OTA 固件流口
    mode : int : 下载模式，0表示读取本地文件经网络转发到串口，1表示读文件直接写本机串口
    serial_port : str : mode 1 使用的本机串口名, 如 "COM3" / "/dev/ttyUSB0"
    baudrate : int : mode 1 使用的串口波特率
    cmd_port : int : mode 0 可选, 先向该端口发 'cmdota <len>' 触发设备再灌镜像 (0 = 不触发)
    cmd_wait : float : 触发命令发出后等待秒数 (仅在 ready_timeout<=0 时起作用)
    ready_timeout : float : 等设备就绪的秒数 (默认 15, 覆盖 1~3 秒擦除; 0 = 不等)
    ready_port : int : 兼容旧固件: 就绪标记所在的日志口 (如 9090); 0 (默认) = 走正路,
                       直接在 OTA 口连接上等握手字节 'R'
    return : bool : 成功返回 True, 失败返回 False
    '''
    if not image_file or not os.path.isfile(image_file):
        print(f"Error: image file not found: {image_file}")
        return False
    if size <= 0:
        print(f"Error: invalid block size: {size}")
        return False

    total = os.path.getsize(image_file)    # 触发命令要用到长度, 提前取

    sock = None
    port = None
    try:
        if mode == 0:
            # 读本地镜像 -> TCP 端点 (对端通常是串口服务器/转发器)
            if not Host:
                print("Error: Host is empty")
                return False
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10.0)
            sock.connect((Host, int(Port)))
            send = sock.sendall
            label = f"{Host}:{Port}"

            if cmd_port > 0:
                # 数据通道已就绪再触发: 反过来会让 OtaStep 先读到一次超时
                if not _send_trigger(Host, cmd_port, total):
                    return False
                if ready_port > 0:
                    # 就绪标记在日志口 (OTA 口只有 RX, 发不回来)
                    if not _wait_ready_by_log(Host, ready_port, ready_timeout):
                        return False
                elif ready_timeout > 0:
                    # 备选: 直接在同一连接上等 'R' (需要 UART3 的 TX 有通路)
                    if not _wait_ready(sock, ready_timeout):
                        return False
                else:
                    time.sleep(cmd_wait)   # 旧固件: 退化为定时等待
        elif mode == 1:
            # 读本地镜像 -> 本机串口
            if serial is None:
                print("Error: pyserial not installed (pip install pyserial)")
                return False
            if not serial_port:
                print("Error: serial_port is required for mode 1")
                return False
            port = serial.Serial(serial_port, baudrate, timeout=1, write_timeout=5)
            send = port.write
            label = f"{serial_port}@{baudrate}"
        else:
            print(f"Error: unknown mode: {mode} (expect 0 or 1)")
            return False

        start = time.time()
        sent = 0
        _draw_progress(sent, total, start, force=True)
        with open(image_file, 'rb') as f:
            while True:
                data = f.read(size)
                if not data:
                    break
                send(data)              # sendall 保证发完 / Serial.write 受 write_timeout 约束
                sent += len(data)
                _draw_progress(sent, total, start)
        _draw_progress(sent, total, start, force=True)   # 收尾: 保证打到 100% 并换行

        if port is not None:
            port.flush()                    # 等 OS 缓冲吐完再收工
        if sock is not None:
            sock.shutdown(socket.SHUT_WR)   # 半关闭: 告知对端本端发送结束

        print(f"Sent {sent} bytes to {label}")
        return True
    except Exception as e:
        print(f"\nError downloading file: {e}")
        return False
    finally:
        if port is not None:
            port.close()
        if sock is not None:
            sock.close()

def main():
    parser = argparse.ArgumentParser(description="Download a file from a host.")
    # 前 5 个保持位置参数 (与 down_load 的签名同序), 允许省略以取默认值
    parser.add_argument("image_file", nargs="?", type=str, default=None,
                        help="Path to the image file to be downloaded.")
    parser.add_argument("size", nargs="?", type=int, default=512,
                        help="Size of each download chunk.")
    parser.add_argument("Host", nargs="?", type=str, default="127.0.0.1",
                        help="Host address to download the image from.")
    parser.add_argument("Port", nargs="?", type=int, default=9092,
                        help="Port to connect to on the host.")
    parser.add_argument("mode", nargs="?", type=int, default=1,
                        help="Download mode, 0 for network forwarding to serial, 1 for direct network download.")
    parser.add_argument("--serial", dest="serial_port", type=str, default="COM3",
                        help="mode 1 使用的本机串口 (默认 COM3, 例如 /dev/ttyUSB0)")
    parser.add_argument("--baud", dest="baudrate", type=int, default=115200,
                        help="mode 1 使用的串口波特率 (默认 115200)")
    parser.add_argument("--cmd-port", dest="cmd_port", type=int, default=0,
                        help="mode 0 可选: 先向该端口发 'cmdota <len>' 触发设备再灌镜像 (例如业务口 9092)")
    parser.add_argument("--cmd-wait", dest="cmd_wait", type=float, default=0.3,
                        help="触发命令发出后等待秒数 (仅在 --ready-timeout 0 时起作用)")
    parser.add_argument("--ready-timeout", dest="ready_timeout", type=float, default=15.0,
                        help="等待设备就绪的秒数 (默认 15; 0 = 不等, 兼容旧固件)")
    parser.add_argument("--ready-port", dest="ready_port", type=int, default=0,
                        help="兼容旧固件: 从该日志口认 OTA_READY 标记 (如 9090); "
                             "不填则走正路 —— 直接在 OTA 口等握手字节 'R'")
    try:
        args = parser.parse_args()
        ok = down_load(args.image_file, args.size, args.Host, args.Port, args.mode,
                       args.serial_port, args.baudrate, args.cmd_port, args.cmd_wait,
                       args.ready_timeout, args.ready_port)
    except Exception as e:
        print(f"Error parsing arguments or downloading file: {e}")
        sys.exit(1)

    # 失败必须给非零退出码, 否则调用方 (脚本/CI) 无从判断
    sys.exit(0 if ok else 1)
    # down_load(args.image_file, args.size, args.Host, args.Port, args.mode)

if __name__ == "__main__":
    main()
