"""Build and flash the Nicla Voice firmware.

The COM port selects the board. OpenOCD programs it through the CMSIS-DAP
interface on that same USB device, using the port's USB serial number.

Examples:
    python fwTool.py -b
    python fwTool.py -f -p COM7
    python fwTool.py -b -f -p COM7
"""

import argparse
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

try:
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required. It is installed with PlatformIO.")

FIRMWARE_DIR = Path(__file__).resolve().parent
# Nicla Voice application port. The onboard CMSIS-DAP debugger uses this
# same VID, PID, and serial number.
NICLA_VOICE_VID = 0x2341
NICLA_VOICE_PID = 0x0065


def env_name(ini_path):
    for line in ini_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("[env:") and line.endswith("]"):
            return line[len("[env:"):-1].strip()
    sys.exit(f"No [env:...] section in {ini_path}")


def hex_path(environment):
    return FIRMWARE_DIR / ".pio" / "build" / environment / "firmware.hex"


def confirm(prompt):
    answer = input(f"{prompt} [Y/n] ").strip().lower()
    return answer in ("", "y", "yes")


def find_port(name):
    wanted = name.strip().lower()
    ports = list(list_ports.comports())
    for port in ports:
        if port.device.lower() == wanted:
            return port
    available = ", ".join(port.device for port in ports) or "(none)"
    sys.exit(f"Port {name} was not found. Available ports: {available}")


def cmsis_dap_serial(port):
    if port.vid != NICLA_VOICE_VID or port.pid != NICLA_VOICE_PID:
        vid = f"{port.vid:04X}" if port.vid is not None else "none"
        pid = f"{port.pid:04X}" if port.pid is not None else "none"
        sys.exit(
            f"{port.device} is USB {vid}:{pid}, not a Nicla Voice "
            f"({NICLA_VOICE_VID:04X}:{NICLA_VOICE_PID:04X})."
        )
    serial = (port.serial_number or "").strip()
    if not serial:
        sys.exit(
            f"{port.device} has no USB serial number, so OpenOCD cannot "
            "select its CMSIS-DAP interface."
        )
    return serial


def latest_dir(path):
    if not path.is_dir():
        sys.exit(f"Arduino tool directory not found: {path}")
    children = [child for child in path.iterdir() if child.is_dir()]
    if not children:
        sys.exit(f"No version installed under {path}")
    return sorted(children, key=lambda child: child.name)[-1]


def arduino_openocd():
    local_app = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local")))
    arduino15 = local_app / "Arduino15" / "packages" / "arduino"
    openocd_root = latest_dir(arduino15 / "tools" / "openocd")
    core_root = latest_dir(arduino15 / "hardware" / "mbed_nicla")
    openocd = openocd_root / "bin" / "openocd.exe"
    scripts = openocd_root / "share" / "openocd" / "scripts"
    swd_cfg = core_root / "debugger" / "select_swd.cfg"
    missing = [path for path in (openocd, scripts, swd_cfg) if not path.exists()]
    if missing:
        sys.exit(
            "Arduino Mbed Nicla OpenOCD is incomplete:\n"
            + "\n".join(str(path) for path in missing)
        )
    return openocd, scripts, swd_cfg


def build(environment):
    print(f"Building PlatformIO environment {environment}")
    subprocess.run(
        [sys.executable, "-m", "platformio", "run", "-e", environment],
        cwd=FIRMWARE_DIR,
        check=True,
    )
    firmware = hex_path(environment)
    if not firmware.is_file():
        sys.exit(f"Build finished without producing {firmware}")
    return firmware


def flash(firmware, serial):
    openocd, scripts, swd_cfg = arduino_openocd()
    print(f"Flashing {firmware}")
    print(f"CMSIS-DAP serial {serial}")
    subprocess.run(
        [
            str(openocd),
            "-s",
            str(scripts),
            "-f",
            "interface/cmsis-dap.cfg",
            "-f",
            str(swd_cfg),
            "-f",
            "target/nrf52.cfg",
            "-c",
            (
                f"cmsis_dap_serial {serial}; "
                "telnet_port disabled; init; reset init; halt; "
                "adapter speed 10000; "
                f"program {{{firmware.as_posix()}}}; reset run; shutdown"
            ),
        ],
        check=True,
    )


def firmware_for_flash(environment, rebuild):
    firmware = hex_path(environment)
    if rebuild:
        return build(environment)
    if firmware.is_file():
        built = datetime.fromtimestamp(firmware.stat().st_mtime)
        print("Build was not requested. Using the previously built firmware:")
        print(f"  {firmware}")
        print(f"  last written {built:%Y-%m-%d %H:%M:%S}")
        if not confirm("Flash this firmware?"):
            sys.exit("Not flashing.")
        return firmware
    print(f"No previously built firmware at {firmware}.")
    if not confirm("Build it now?"):
        sys.exit("Not flashing.")
    return build(environment)


def main():
    parser = argparse.ArgumentParser(
        description="Build and flash the Nicla Voice firmware."
    )
    parser.add_argument("-b", "--build", action="store_true", help="build the firmware")
    parser.add_argument("-f", "--flash", action="store_true", help="flash the firmware")
    parser.add_argument(
        "-p",
        "--port",
        help="serial port of the board to flash, for example COM7",
    )
    args = parser.parse_args()

    if not args.build and not args.flash:
        parser.error("specify --build, --flash, or both")
    if args.flash and not args.port:
        parser.error("--port is required when flashing")

    environment = env_name(FIRMWARE_DIR / "platformio.ini")
    if not args.flash:
        build(environment)
        return

    port = find_port(args.port)
    serial = cmsis_dap_serial(port)
    print(
        f"{port.device}: USB {port.vid:04X}:{port.pid:04X}, "
        f"OpenOCD CMSIS-DAP serial {serial}"
    )
    firmware = firmware_for_flash(environment, args.build)
    flash(firmware, serial)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)
