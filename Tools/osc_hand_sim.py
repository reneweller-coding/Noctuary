"""Simulated hands for Noctuary's OSC input.

Sends /ambient/hand/L and /R (x y z pinch tilt, metres) and /ambient/head
(yaw pitch roll, degrees) at 60 Hz. The right hand pinches (the clutch) in
phases; the hands drift slowly apart and together, rise and fall, tilt.
Watch the standalone: Morph position, Depth, Brightness, Cosmos send ...
follow only while the right pinch is closed.

    python Tools/osc_hand_sim.py [--host 127.0.0.1] [--port 9000] [--seconds 120]
"""
import argparse
import math
import socket
import struct
import time


def osc_message(address, *args):
    def pad(b):
        return b + b"\0" * ((-len(b)) % 4)   # to a multiple of 4, nothing if already aligned
    data = pad(address.encode() + b"\0")
    tags = ","
    body = b""
    for a in args:
        if isinstance(a, float):
            tags += "f"
            body += struct.pack(">f", a)
        elif isinstance(a, int):
            tags += "i"
            body += struct.pack(">i", a)
        else:
            tags += "s"
            body += pad(str(a).encode() + b"\0")
    return data + pad(tags.encode() + b"\0") + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--seconds", type=float, default=120.0)
    a = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    t0 = time.time()
    n = 0
    while True:
        t = time.time() - t0
        if t > a.seconds:
            break
        # Slow, continuous choreography (periods in seconds).
        spread = 0.15 + 0.3 * (0.5 + 0.5 * math.sin(2 * math.pi * t / 40.0))   # hands apart: 0.15 .. 0.45 m each side
        ly = 1.3 + 0.35 * math.sin(2 * math.pi * t / 23.0)
        ry = 1.3 + 0.35 * math.sin(2 * math.pi * t / 31.0 + 1.0)
        lz = -0.45 + 0.2 * math.sin(2 * math.pi * t / 37.0)
        rz = -0.45 + 0.2 * math.sin(2 * math.pi * t / 29.0 + 2.0)
        ltilt = math.sin(2 * math.pi * t / 47.0)
        rtilt = math.sin(2 * math.pi * t / 53.0 + 0.7)
        # Clutch: right pinch closed for 20 s, open for 10 s.
        rpinch = 1.0 if (t % 30.0) < 20.0 else 0.0
        lpinch = 0.0
        yaw = 40.0 * math.sin(2 * math.pi * t / 61.0)
        sock.sendto(osc_message("/ambient/hand/L", -spread, ly, lz, lpinch, ltilt), (a.host, a.port))
        sock.sendto(osc_message("/ambient/hand/R", spread, ry, rz, rpinch, rtilt), (a.host, a.port))
        sock.sendto(osc_message("/ambient/head", yaw, 0.0, 0.0), (a.host, a.port))
        n += 3
        if n % 180 == 0:
            print(f"{t:6.1f} s  spread {2*spread:.2f} m  L {ly:.2f} m  R {ry:.2f} m  clutch {'closed' if rpinch else 'open'}")
        time.sleep(1.0 / 60.0)
    print(f"sent {n} messages")


if __name__ == "__main__":
    main()
