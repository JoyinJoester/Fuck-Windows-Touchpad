"""Offline analysis of captured touchpad HID frames.

Layout hypothesis (derived from raw hex for PixArt 093A:0255):
  report[0]      = report id (0x01)
  payload        = report[1:]
  contact i      = payload[i*5 : i*5+5]
      status = c[0]:  id = status>>4, tip = status&1, conf = (status>>1)&1
      x = c[1] | c[2]<<8      (0..1500)
      y = c[3] | c[4]<<8      (0..927)
  payload[25]    = contact count
  payload[27:29] = scan time (LE)
"""
import re
import sys
from collections import Counter

LOG = sys.argv[1] if len(sys.argv) > 1 else "probe_run.log"

frames = []
for line in open(LOG, encoding="utf-8", errors="ignore"):
    m = re.search(r"HEX.*?:\s*((?:[0-9A-F]{2}\s+)*[0-9A-F]{2})\s*$", line)
    if not m:
        continue
    b = bytes(int(x, 16) for x in m.group(1).split())
    if len(b) >= 30:
        frames.append(b)

print(f"parsed frames: {len(frames)}")
if not frames:
    sys.exit(0)


def decode(b):
    p = b[1:]
    cc = p[25]
    contacts = []
    for i in range(5):
        c = p[i * 5:(i + 1) * 5]
        s = c[0]
        if s & 1:
            x = c[1] | (c[2] << 8)
            y = c[3] | (c[4] << 8)
            contacts.append({"idx": i, "id": s >> 4, "x": x, "y": y,
                             "conf": (s >> 1) & 1})
    return cc, contacts


counts = Counter()
bad = 0
for b in frames:
    cc, cs = decode(b)
    counts[len(cs)] += 1
    if cc != len(cs):
        bad += 1

print(f"\n触点数量分布: {dict(sorted(counts.items()))}")
print(f"contact_count 字段与实际解析数不一致的帧: {bad} / {len(frames)}")

# 打印前 6 帧的详细解析
print("\n--- 前 6 帧 ---")
for b in frames[:6]:
    cc, cs = decode(b)
    desc = "  ".join(f"id{c['id']}({c['x']},{c['y']})" for c in cs)
    print(f"  cc={cc} n={len(cs)}  {desc}")

# 找触点最多的帧，检查三指能否正确解析
best = max(frames, key=lambda b: len(decode(b)[1]))
cc, cs = decode(best)
print(f"\n--- 触点最多的帧 (cc={cc}) ---")
for c in cs:
    print(f"  id={c['id']}  x={c['x']}  y={c['y']}  conf={c['conf']}")

# 坐标范围检查
xs = [c["x"] for b in frames for c in decode(b)[1]]
ys = [c["y"] for b in frames for c in decode(b)[1]]
if xs:
    print(f"\nX 范围: {min(xs)} .. {max(xs)}   (declared 0..1500)")
    print(f"Y 范围: {min(ys)} .. {max(ys)}   (declared 0..927)")

# 质心轨迹（仅取触点数的众数，观察运动）
mode_n = counts.most_common(1)[0][0]
print(f"\n--- 质心轨迹 (只取 {mode_n} 触点的帧, 每 10 帧采样) ---")
traj = []
for b in frames:
    cc, cs = decode(b)
    if len(cs) == mode_n:
        cx = sum(c["x"] for c in cs) / len(cs)
        cy = sum(c["y"] for c in cs) / len(cs)
        traj.append((cx, cy))

for i in range(0, len(traj), max(1, len(traj) // 20)):
    cx, cy = traj[i]
    print(f"  f{i:4d}  cx={cx:7.1f}  cy={cy:7.1f}")

if len(traj) > 1:
    dx = traj[-1][0] - traj[0][0]
    dy = traj[-1][1] - traj[0][1]
    print(f"\n总位移: dx={dx:+.1f}  dy={dy:+.1f}")
