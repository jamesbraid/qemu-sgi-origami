#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""A raw-Ethernet peer on the far end of QEMU's socket back end.

The user-mode back end runs a TCP/IP stack in the emulator's main loop and caps
every frame at the standard size, which is why no oversized frame has ever
reached the wire in any measurement here. This speaks the socket back end's
framing instead -- a 32-bit big-endian length followed by the frame -- so frames
cross at whatever size the guest emits, and the peer costs a host process rather
than a second emulated machine.

It answers ARP so the guest will send to it, answers ICMP echo so the link can
be checked, and counts UDP payload bytes so a blast from the guest can be timed.
Nothing else: it is an instrument, not a network stack.
"""
import socket
import struct
import sys
import time

PEER_MAC = bytes.fromhex('525400aabbcc')
PEER_IP = bytes([10, 0, 3, 2])


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def main():
    port = int(sys.argv[1])
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port))
    srv.listen(1)
    print('LISTENING %d' % port, flush=True)
    conn, _ = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print('PEER CONNECTED', flush=True)

    buf = b''
    udp_bytes = 0
    udp_frames = 0
    largest = 0
    first = None
    last = None
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        conn.settimeout(2.0)
        try:
            chunk = conn.recv(262144)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 4:
            n = struct.unpack('>I', buf[:4])[0]
            if n > 65536 or len(buf) < 4 + n:
                break
            frame = buf[4:4 + n]
            buf = buf[4 + n:]
            largest = max(largest, len(frame))
            if len(frame) < 14:
                continue
            ethertype = frame[12:14]
            reply = None

            if ethertype == b'\x08\x06' and len(frame) >= 42:
                # ARP request for our address: answer it.
                if frame[20:22] == b'\x00\x01' and frame[38:42] == PEER_IP:
                    sha, spa = frame[22:28], frame[28:32]
                    reply = (sha + PEER_MAC + b'\x08\x06'
                             + b'\x00\x01\x08\x00\x06\x04\x00\x02'
                             + PEER_MAC + PEER_IP + sha + spa)
            elif ethertype == b'\x08\x00' and len(frame) >= 34:
                ihl = (frame[14] & 0x0f) * 4
                proto = frame[23]
                off = 14 + ihl
                if proto == 1 and len(frame) >= off + 8 and frame[off] == 8:
                    # ICMP echo request -> reply, so the link is checkable.
                    body = bytearray(frame[off:])
                    body[0] = 0
                    body[2:4] = b'\0\0'
                    body[2:4] = struct.pack('>H', checksum(bytes(body)))
                    ip = bytearray(frame[14:off])
                    ip[12:16], ip[16:20] = frame[30:34], frame[26:30]
                    ip[10:12] = b'\0\0'
                    ip[10:12] = struct.pack('>H', checksum(bytes(ip)))
                    reply = (frame[6:12] + PEER_MAC + b'\x08\x00'
                             + bytes(ip) + bytes(body))
                elif proto == 17:
                    total = struct.unpack('>H', frame[16:18])[0]
                    payload = max(0, total - ihl - 8)
                    # Only bulk datagrams are the measurement. The guest's own
                    # background chatter is tens of bytes and would otherwise
                    # stretch the window and bury the rate.
                    if payload >= 512:
                        udp_bytes += payload
                        udp_frames += 1
                        now = time.monotonic()
                        if first is None:
                            first = now
                        last = now

            if reply is not None:
                conn.sendall(struct.pack('>I', len(reply)) + reply)

    elapsed = (last - first) if (first and last and last > first) else 0.0
    rate = (udp_bytes * 8 / elapsed / 1e6) if elapsed > 0 else 0.0
    print('RESULT udp_frames=%d udp_bytes=%d largest_frame=%d '
          'elapsed=%.3f mbits=%.1f'
          % (udp_frames, udp_bytes, largest, elapsed, rate), flush=True)


if __name__ == '__main__':
    main()
