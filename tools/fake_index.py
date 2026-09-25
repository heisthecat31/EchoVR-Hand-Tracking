"""Fake Valve Index finger tracking for EchoXR Hands -- test without an Index.

    python fake_index.py                  cycle through real hand gestures (default)
    python fake_index.py --mode random    random anatomically plausible poses
    python fake_index.py --mode single    bend ONE finger at a time, named on screen
    python fake_index.py --mode ripple    fingers wave thumb -> pinky in turn
    python fake_index.py --hand left      only the left hand (default: both)
    python fake_index.py --mirror         both hands do the same pose

It stands in for EchoXRHands.exe: it sends the exact HtvFrame packets the
bridge sends, to the plugin on 127.0.0.1:8768, at the bridge's ~120 Hz. Close the
real bridge first -- both would feed the same port.

HOW IT MIMICS AN INDEX CONTROLLER
---------------------------------
The bridge asks SteamVR for the skeletal summary "from device": the raw finger
sensing, one curl per finger (0 = straight, 1 = fully curled) plus four splays.
This script shapes its numbers the way that data behaves:

* **Curls never sit on exactly 0 or 1.** Capacitive sensing reads a straight
  finger at a few percent and a hard fist just short of 1.
* **"Holding the controller" is not "open".** With the strap on and the hand
  resting, the grip fingers wrap the handle (~0.6-0.75) and the index rests near
  the trigger (~0.3). An open hand means actively lifting the fingers off.
* **The thumb reads by where it sits.** Off the controller ~0.05, resting on the
  trackpad or face buttons ~0.55-0.7, pressing in ~0.85.
* **Fingers are coupled.** A real ring finger drags the pinky and middle with it,
  so a pose can't curl the ring alone without some pull on its neighbours.
* **Splay collapses as fingers curl.** The spread between two fingers is large
  on an open hand and near zero in a fist.
* **Finite speed and sensor noise.** Each finger follows its target with a
  critically-damped spring (a finger takes ~0.15-0.3 s to close), and the
  reading carries small jitter.

Every run starts with ~2.5 s of FULLY OPEN hands, then sends `Calibrate = 1`.
That is the plugin's open-hand reference capture, the same as a player holding
their hands flat once.
"""
from __future__ import annotations

import argparse
import math
import random
import socket
import struct
import sys
import time

PORT = 8768
MAGIC = 0x31565448            # "HTV1"
RATE_HZ = 120
FMT = "<IIBBBB10f8f"          # HtvFrame, packed: magic, seq, valid[2], pad[2], curl[2][5], splay[2][4]
assert struct.calcsize(FMT) == 84

THUMB, INDEX, MIDDLE, RING, PINKY = range(5)
NAMES = ["thumb", "index", "middle", "ring", "pinky"]

# ---------------------------------------------------------------------------
# Poses, as an Index reports them: [thumb, index, middle, ring, pinky]
# ---------------------------------------------------------------------------
OPEN     = [0.04, 0.03, 0.03, 0.04, 0.05]    # fingers actively lifted off
RELAXED  = [0.60, 0.30, 0.66, 0.70, 0.72]    # resting hand, strap on
GESTURES = {
    "open hand":       OPEN,
    "holding (rest)":  RELAXED,
    "fist":            [0.86, 0.96, 0.97, 0.97, 0.96],
    "point":           [0.70, 0.04, 0.95, 0.96, 0.95],
    "thumbs up":       [0.05, 0.95, 0.96, 0.97, 0.96],
    "peace":           [0.78, 0.04, 0.05, 0.94, 0.95],
    "rock horns":      [0.80, 0.05, 0.94, 0.95, 0.06],
    "gun":             [0.05, 0.05, 0.05, 0.94, 0.95],
    "pinch":           [0.82, 0.62, 0.20, 0.18, 0.16],
    "ok sign":         [0.84, 0.70, 0.08, 0.06, 0.07],
    "three":           [0.82, 0.04, 0.05, 0.05, 0.94],
    "trigger pull":    [0.62, 0.92, 0.68, 0.71, 0.73],
    "grab":            [0.66, 0.84, 0.94, 0.95, 0.94],
    "call me":         [0.05, 0.94, 0.95, 0.94, 0.06],
}

# How strongly each finger drags its neighbour (anatomical coupling).
COUPLING = {(MIDDLE, RING): 0.35, (RING, PINKY): 0.45, (RING, MIDDLE): 0.25, (PINKY, RING): 0.30}
MAX_SPLAY = [0.45, 0.30, 0.25, 0.35]         # thumb-index, index-middle, middle-ring, ring-pinky


def couple(target):
    """Pull coupled fingers toward each other, as tendons do."""
    out = list(target)
    for (a, b), k in COUPLING.items():
        out[b] = out[b] + (target[a] - target[b]) * k * 0.5
    return [min(0.98, max(0.02, v)) for v in out]


def jitter_pose(pose, amount=0.05):
    """A human never makes exactly the same pose twice."""
    return [min(0.98, max(0.02, v + random.uniform(-amount, amount))) for v in pose]


def random_pose():
    base = random.choice([OPEN, RELAXED, GESTURES["fist"]])
    return couple([min(0.98, max(0.02, b + random.gauss(0, 0.28))) for b in base])


# ---------------------------------------------------------------------------
# One hand: springs per finger + sensor noise
# ---------------------------------------------------------------------------
class Hand:
    def __init__(self, name):
        self.name = name
        self.x = list(OPEN)
        self.v = [0.0] * 5
        self.target = list(OPEN)
        # per-finger speed: thumb and index are quicker than the grip fingers
        self.omega = [random.uniform(20, 26), random.uniform(22, 28),
                      random.uniform(17, 22), random.uniform(16, 21), random.uniform(16, 21)]
        self.label = "open hand"

    def set(self, pose, label):
        self.target = list(pose)
        self.label = label

    def step(self, dt):
        for i in range(5):
            w = self.omega[i]
            # critically damped spring
            a = w * w * (self.target[i] - self.x[i]) - 2.0 * w * self.v[i]
            self.v[i] += a * dt
            self.x[i] += self.v[i] * dt

    def reading(self):
        curl = [min(0.99, max(0.01, v + random.gauss(0, 0.008))) for v in self.x]
        pairs = [(THUMB, INDEX), (INDEX, MIDDLE), (MIDDLE, RING), (RING, PINKY)]
        splay = []
        for k, (a, b) in enumerate(pairs):
            openness = 1.0 - max(curl[a], curl[b])
            splay.append(max(0.0, MAX_SPLAY[k] * openness + random.gauss(0, 0.01)))
        return curl, splay


# ---------------------------------------------------------------------------
# Pose scripts
# ---------------------------------------------------------------------------
def gesture_script():
    names = [n for n in GESTURES if n != "open hand"]
    while True:
        random.shuffle(names)
        for n in names:
            yield jitter_pose(couple(GESTURES[n]), 0.03), n, random.uniform(1.2, 2.4)
            if random.random() < 0.35:
                yield jitter_pose(RELAXED, 0.04), "holding (rest)", random.uniform(0.6, 1.2)


def random_script():
    while True:
        yield random_pose(), "random", random.uniform(0.4, 1.6)


def single_script():
    while True:
        for f in range(5):
            pose = list(OPEN)
            pose[f] = 0.95
            yield couple(pose), "ONLY %s curled" % NAMES[f].upper(), 2.0
            yield list(OPEN), "open hand", 1.0


def ripple_script():
    while True:
        for f in range(5):
            pose = list(OPEN)
            pose[f] = 0.92
            yield pose, "ripple: %s" % NAMES[f], 0.28
        yield list(OPEN), "open hand", 0.5


SCRIPTS = {"gestures": gesture_script, "random": random_script,
           "single": single_script, "ripple": ripple_script}


# ---------------------------------------------------------------------------
# Plugin I/O
# ---------------------------------------------------------------------------
def send_text(sock, text, wait=True):
    sock.sendto(text.encode(), ("127.0.0.1", PORT))
    if not wait:
        return None
    try:
        return sock.recv(256).decode(errors="replace")
    except (socket.timeout, ConnectionResetError):
        # Windows reports "port unreachable" on a UDP socket as WinError 10054:
        # nothing is listening on 127.0.0.1:8768, i.e. the plugin is not loaded.
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=sorted(SCRIPTS), default="gestures")
    ap.add_argument("--hand", choices=["both", "left", "right"], default="both")
    ap.add_argument("--mirror", action="store_true", help="both hands do the same pose")
    ap.add_argument("--no-calibrate", action="store_true", help="skip the open-hand calibration at start")
    ap.add_argument("--dropouts", action="store_true",
                    help="occasionally lose a hand for ~0.5 s, like a controller losing tracking")
    ap.add_argument("--seed", type=int, default=None)
    a = ap.parse_args(argv)
    if a.seed is not None:
        random.seed(a.seed)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.5)
    reply = send_text(sock, "Ping")
    if reply != "PONG":
        print("!! no reply from the plugin on 127.0.0.1:%d -- is Echo VR running with "
              "EchoXRHands.dll loaded? Sending anyway." % PORT)
    else:
        print("plugin is loaded (PONG)")

    hands = [Hand("left"), Hand("right")]
    use = [a.hand in ("both", "left"), a.hand in ("both", "right")]
    scripts = [SCRIPTS[a.mode](), SCRIPTS[a.mode]()]
    next_change = [0.0, 0.0]
    dropout_until = [0.0, 0.0]

    dt = 1.0 / RATE_HZ
    t0 = time.perf_counter()
    next_tick = t0
    seq = 0
    calibrated = a.no_calibrate
    last_print = 0.0

    print("holding both hands OPEN for calibration..." if not calibrated else "starting")
    try:
        while True:
            now = time.perf_counter() - t0

            if not calibrated and now >= 2.5:
                r = send_text(sock, "Calibrate = 1")
                print("calibrate ->", r or "(no reply)")
                calibrated = True
                next_change = [now, now + (0.0 if a.mirror else random.uniform(0.2, 0.8))]

            if calibrated:
                for h in range(2):
                    if now >= next_change[h]:
                        if a.mirror and h == 1:
                            hands[1].set(hands[0].target, hands[0].label)
                            next_change[1] = next_change[0]
                        else:
                            pose, label, hold = next(scripts[h])
                            hands[h].set(pose, label)
                            next_change[h] = now + hold
                    if a.dropouts and now > dropout_until[h] and random.random() < 0.0015:
                        dropout_until[h] = now + random.uniform(0.3, 0.7)

            valid, curls, splays = [], [], []
            for h in range(2):
                hands[h].step(dt)
                c, s = hands[h].reading()
                live = use[h] and now >= dropout_until[h]
                valid.append(1 if live else 0)
                curls += c
                splays += s
            seq = (seq + 1) & 0xFFFFFFFF
            pkt = struct.pack(FMT, MAGIC, seq, valid[0], valid[1], 0, 0, *curls, *splays)
            try:
                sock.sendto(pkt, ("127.0.0.1", PORT))
            except ConnectionResetError:
                pass   # plugin not listening (game closed / restarting) -- keep going

            if now - last_print > 0.1:
                last_print = now
                line = []
                for h in range(2):
                    if not use[h]:
                        continue
                    c = curls[h * 5:h * 5 + 5]
                    tag = "LOST" if not valid[h] else hands[h].label
                    line.append("%s %-22s T%.2f I%.2f M%.2f R%.2f P%.2f" % (hands[h].name[0].upper(), tag[:22], *c))
                sys.stdout.write("\r" + "   ".join(line) + "  ")
                sys.stdout.flush()

            next_tick += dt
            delay = next_tick - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_tick = time.perf_counter()
    except KeyboardInterrupt:
        # Stop sending; the plugin drops stale data after StaleMs and the game's
        # own finger animation takes back over.
        print("\nstopped")


if __name__ == "__main__":
    main()
