#!/usr/bin/env python3
"""
Synchronized Motor + Event Camera Recording
============================================
Controls a Cinetics 3-axis machine (Dragonframe serial protocol)
and triggers recording on a Prophesee event camera inside Docker,
using UNIX signals (SIGUSR1/SIGUSR2) sent to the patched prophesee_viewer.

Prerequisites
-------------
1. Copy the patched viewer into the container and build it:
     docker cp prophesee_viewer_patched.cpp test2:/usr/share/prophesee_driver/samples/prophesee_viewer/prophesee_viewer.cpp
     docker exec -it test2 bash -c "cd /usr/share/prophesee_driver/samples/prophesee_viewer && mkdir -p build && cd build && cmake .. && make -j\$(nproc)"

2. Start the Docker container (X11 forwarding for the viewer window):
     sudo docker run -it --rm --name test2 --net=host \
       --env DISPLAY=$DISPLAY \
       --volume "$HOME/.Xauthority:/root/.Xauthority:rw" \
       --volume "$HOME/prophesee_recording:/prophesee_recording" \
       --privileged --volume /dev/bus/usb:/dev/bus/usb \
       custom-event-camera-image /bin/bash

3. Run this script:
     python3 record_motor_sync.py [--launch-viewer]

     --launch-viewer  lets the script start prophesee_viewer inside Docker.
     Without it, start the viewer manually first:
       docker exec -it test2 prophesee_viewer -o /prophesee_recording/<name>/events.raw
"""

import argparse
import json
import os
import subprocess
import time
from datetime import datetime
from pathlib import Path

import serial as pyserial          # pip install pyserial

# ---------------------------------------------------------------------------
DEFAULT_PORT      = "/dev/ttyACM0"
DEFAULT_BAUDRATE  = 57600
DEFAULT_MOTOR     = 2    #3
DEFAULT_SPEED     = 1000    #700
DEFAULT_TARGET    = 300_000
DEFAULT_OUTDIR    = os.path.expanduser("~/prophesee_recording")
DEFAULT_CONTAINER = "test2"

PID_FILE_IN_CONTAINER    = "/tmp/prophesee_viewer.pid"
STATUS_FILE_IN_CONTAINER = "/tmp/prophesee_status.txt"

SERIAL_TIMEOUT   = 2
POLL_INTERVAL    = 0.5
MOTOR_START_WAIT = 1.5
MAX_MOTION_TIME  = 300
VIEWER_READY_WAIT = 15

# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

def open_serial(port, baudrate):
    ser = pyserial.Serial(port, baudrate, timeout=SERIAL_TIMEOUT)
    time.sleep(1)
    return ser

def send_cmd(ser, cmd):
    ser.write(f"{cmd}\r\n".encode())
    time.sleep(0.15)
    return ser.read(256).decode(errors="replace").strip()

def handshake(ser):
    resp = send_cmd(ser, "hi")
    if not resp.startswith("hi"):
        raise RuntimeError(f"Unexpected handshake: {resp!r}")
    return resp

def get_position(ser, motor):
    resp = send_cmd(ser, f"mp {motor}")
    parts = resp.split()
    if len(parts) >= 3:
        return int(parts[2])
    raise ValueError(f"Cannot parse position: {resp!r}")

def is_motor_busy(ser):
    resp = send_cmd(ser, "ms")
    parts = resp.split()
    if len(parts) >= 2:
        return parts[1] != "000"
    return False

def set_speed(ser, motor, speed):
    return send_cmd(ser, f"pr {motor} {speed}")

def move_motor(ser, motor, target):
    return send_cmd(ser, f"mm {motor} {target}")

def stop_all(ser):
    return send_cmd(ser, "sa")

# ---------------------------------------------------------------------------
# Docker / viewer helpers
# ---------------------------------------------------------------------------

def container_is_running(container):
    r = subprocess.run(
        ["docker", "inspect", "--format", "{{.State.Running}}", container],
        capture_output=True, text=True)
    return r.stdout.strip() == "true"

def docker_read_file(container, path):
    r = subprocess.run(["docker", "exec", container, "cat", path],
                       capture_output=True, text=True)
    return r.stdout.strip()

def get_viewer_pid(container):
    try:
        val = docker_read_file(container, PID_FILE_IN_CONTAINER)
        return int(val) if val else None
    except Exception:
        return None

def get_viewer_status(container):
    try:
        return docker_read_file(container, STATUS_FILE_IN_CONTAINER)
    except Exception:
        return ""

def send_signal(container, sig):
    pid = get_viewer_pid(container)
    if pid is None:
        raise RuntimeError(f"Viewer PID not found in {PID_FILE_IN_CONTAINER}")
    subprocess.run(["docker", "exec", container, "kill", f"-{sig}", str(pid)], check=True)

def launch_viewer(container, container_output_path):
    cmd = ["docker", "exec", "-d", container,
           "env", "DISPLAY=" + os.environ.get("DISPLAY", ":0"),
           "QT_X11_NO_MITSHM=1", "NO_AT_BRIDGE=1",
           "/usr/share/prophesee_driver/samples/prophesee_viewer/build/prophesee_viewer",
           "-o", container_output_path]
    subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def wait_for_viewer_ready(container, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if get_viewer_status(container) == "READY":
            return
        time.sleep(0.4)
    raise TimeoutError(
        f"prophesee_viewer did not become READY within {timeout}s.\n"
        f"Make sure the PATCHED viewer is built and the container is running.")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run(args):
    dt_str      = datetime.now().strftime("%d-%m-%Y_%H-%M-%S")
    session     = f"recording_{dt_str}"
    session_dir = Path(args.outdir) / session
    session_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(session_dir, 0o777)

    # /prophesee_recording inside Docker == ~/prophesee_recording on host
    container_out = f"/prophesee_recording/{session}/events_{dt_str}"
    metadata_file = session_dir / f"metadata_{dt_str}.json"

    print(f"\n{'='*60}")
    print(f"  Motor + Event Camera — Synchronized Recording")
    print(f"{'='*60}")
    print(f"  Session   : {session_dir}")
    print(f"  Motor {args.motor}   speed={args.speed}  target={args.target}")
    print(f"  Serial    : {args.port}")
    print(f"  Container : {args.container}")
    print(f"{'='*60}\n")

    # 1. Docker ---------------------------------------------------------------
    print("[1/6] Checking Docker container …")
    if not container_is_running(args.container):
        raise RuntimeError(f"Container '{args.container}' is not running. Start it first.")
    print("    ✓ Container running")

    # 2. Viewer ---------------------------------------------------------------
    if args.launch_viewer:
        print(f"[2/6] Launching prophesee_viewer → {container_out} …")
        launch_viewer(args.container, container_out)
        print(f"    Waiting up to {VIEWER_READY_WAIT}s for READY …")
        wait_for_viewer_ready(args.container, VIEWER_READY_WAIT)
        print("    ✓ Viewer READY")
    else:
        print("[2/6] Verifying prophesee_viewer is running …")
        pid = get_viewer_pid(args.container)
        if pid is None:
            raise RuntimeError(
                "Viewer not found. Start it manually or use --launch-viewer.\n"
                f"  docker exec -it {args.container} prophesee_viewer -o {container_out}")
        print(f"    ✓ Viewer PID={pid}  status={get_viewer_status(args.container)}")

    # 3. Serial ---------------------------------------------------------------
    print(f"[3/6] Connecting to serial {args.port} …")
    ser = open_serial(args.port, DEFAULT_BAUDRATE)
    hello    = handshake(ser)
    pos_start = get_position(ser, args.motor)
    print(f"    ✓ {hello}  |  Motor {args.motor} at {pos_start} pulses")

    # 4. Speed ----------------------------------------------------------------
    print(f"[4/6] Setting motor {args.motor} speed = {args.speed} …")
    resp = set_speed(ser, args.motor, args.speed)
    print(f"    ✓ {resp}")

    # 5. Fire! ----------------------------------------------------------------
    print(f"[5/6] Starting camera recording + motor …")
    t_start  = time.time()
    start_dt = datetime.now()

    send_signal(args.container, "SIGUSR1")   # → start recording
    time.sleep(0.2)
    status = get_viewer_status(args.container)
    if status != "RECORDING":
        raise RuntimeError(f"Viewer did not enter RECORDING state (got: {status!r})")
    print(f"    ✓ Camera: {status}")

    # resp = move_motor(ser, args.motor, args.target)
    # print(f"    ✓ Motor moving → {args.target}  ({resp})")
    # time.sleep(MOTOR_START_WAIT)

    # # 6. Wait -----------------------------------------------------------------
    # print(f"[6/6] Monitoring motor …")
    # elapsed = 0.0
    # while elapsed < MAX_MOTION_TIME:
    #     if not is_motor_busy(ser):
    #         break
    #     elapsed = time.time() - t_start
    #     pos_now = get_position(ser, args.motor)
    #     total   = max(1, abs(args.target - pos_start))
    #     pct     = min(100.0, abs(pos_now - pos_start) / total * 100)
    #     print(f"    … {elapsed:6.1f}s  pos={pos_now:>10}  {pct:5.1f}%", end="\r", flush=True)
    #     time.sleep(POLL_INTERVAL)
    # else:
    #     print("\n    ⚠ Safety timeout — stopping motor")
    #     stop_all(ser)

    # t_end    = time.time()
    # end_dt   = datetime.now()
    # duration = t_end - t_start
    # pos_end  = get_position(ser, args.motor)
    # print(f"\n    ✓ Stopped at {pos_end} pulses  ({duration:.1f}s)")

    # --- GO to target --------------------------------------------------------
    resp = move_motor(ser, args.motor, args.target)
    print(f"    ✓ Motor moving → {args.target}  ({resp})")
    time.sleep(MOTOR_START_WAIT)

    # 6. Wait for forward move ------------------------------------------------
    print(f"[6/6] Monitoring motor (forward) …")
    elapsed = 0.0
    while elapsed < MAX_MOTION_TIME:
        if not is_motor_busy(ser):
            break
        elapsed = time.time() - t_start
        pos_now = get_position(ser, args.motor)
        total   = max(1, abs(args.target - pos_start))
        pct     = min(100.0, abs(pos_now - pos_start) / total * 100)
        print(f"    … {elapsed:6.1f}s  pos={pos_now:>10}  {pct:5.1f}%", end="\r", flush=True)
        time.sleep(POLL_INTERVAL)
    else:
        print("\n    ⚠ Safety timeout — stopping motor")
        stop_all(ser)

    pos_mid = get_position(ser, args.motor)
    print(f"\n    ✓ Reached {pos_mid} pulses — returning to start ({pos_start}) …")

    # --- RETURN to initial position ------------------------------------------
    resp = move_motor(ser, args.motor, pos_start)
    print(f"    ✓ Motor returning → {pos_start}  ({resp})")
    time.sleep(MOTOR_START_WAIT)

    elapsed = 0.0
    t_return = time.time()
    while elapsed < MAX_MOTION_TIME:
        if not is_motor_busy(ser):
            break
        elapsed = time.time() - t_return
        pos_now = get_position(ser, args.motor)
        total   = max(1, abs(pos_mid - pos_start))
        pct     = min(100.0, abs(pos_now - pos_mid) / total * 100)
        print(f"    … {elapsed:6.1f}s  pos={pos_now:>10}  {pct:5.1f}%", end="\r", flush=True)
        time.sleep(POLL_INTERVAL)
    else:
        print("\n    ⚠ Safety timeout — stopping motor")
        stop_all(ser)

    t_end    = time.time()
    end_dt   = datetime.now()
    duration = t_end - t_start
    pos_end  = get_position(ser, args.motor)
    print(f"\n    ✓ Returned to {pos_end} pulses  (total duration: {duration:.1f}s)")

    send_signal(args.container, "SIGUSR2")   # → stop recording
    time.sleep(0.3)
    print(f"    ✓ Camera: {get_viewer_status(args.container)}")

    # Metadata ----------------------------------------------------------------
    metadata = {
        "session"          : session,
        "recording_file"   : f"{container_out}.raw",
        "motor"            : args.motor,
        "speed_pulse_rate" : args.speed,
        "target_pulses"    : args.target,
        "position_start"   : pos_start,
        "position_end"     : pos_end,
        "pulses_travelled" : pos_end - pos_start,
        "start_time"       : start_dt.isoformat(),
        "end_time"         : end_dt.isoformat(),
        "duration_seconds" : round(duration, 3),
        "serial_port"      : args.port,
        "docker_container" : args.container,
        "notes"            : ""
    }
    with open(metadata_file, "w") as f:
        json.dump(metadata, f, indent=2)

    ser.close()

    print(f"\n{'='*60}")
    print(f"  Done!   Duration: {duration:.1f}s   Pulses: {pos_end - pos_start}")
    print(f"  Raw  : {container_out}.raw")
    print(f"  Meta : {metadata_file}")
    print(f"{'='*60}\n")


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--port",          default=DEFAULT_PORT)
    p.add_argument("--motor",         default=DEFAULT_MOTOR,  type=int)
    p.add_argument("--speed",         default=DEFAULT_SPEED,  type=int)
    p.add_argument("--target",        default=DEFAULT_TARGET, type=int)
    p.add_argument("--outdir",        default=DEFAULT_OUTDIR)
    p.add_argument("--container",     default=DEFAULT_CONTAINER)
    p.add_argument("--launch-viewer", action="store_true",
                   help="Auto-launch prophesee_viewer inside the container")
    return p.parse_args()


if __name__ == "__main__":
    args = parse_args()
    try:
        run(args)
    except KeyboardInterrupt:
        print("\n⚠ Interrupted.")
    except Exception as e:
        print(f"\n✗ ERROR: {e}")
        raise
