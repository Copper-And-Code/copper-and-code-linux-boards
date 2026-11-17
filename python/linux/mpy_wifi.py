# SPDX-License-Identifier: MIT
# Wi-Fi for apps on Linux boards: scan, connect, status. The network is in the "wifi" section of
# mpy_settings; ensure() connects to it, so every app that needs the internet calls it at start.
#
# It configures the system the standard way: /etc/wpa_supplicant/wpa_supplicant-<if>.conf for
# wpa_supplicant@<if>.service, and /etc/systemd/network/25-mpy-wifi.network for DHCP by
# systemd-networkd. Slow work (scan, connect) runs in the background: start it, then call poll()
# from an LVGL timer, so the screen stays alive.

import binascii
import os
import time

import linuxproc
import mpy_settings

MARKER = "# written by mpy_wifi"
NETWORK_FILE = "/etc/systemd/network/25-mpy-wifi.network"


def interface():
    name = os.getenv("MPY_WIFI_IF")
    if name:
        return name
    for name in sorted(os.listdir("/sys/class/net")):
        try:
            os.stat("/sys/class/net/%s/wireless" % name)
            return name
        except OSError:
            pass
    return None


def _run(argv):
    # A short command, for its output (str) and status.
    chunks = []
    status = linuxproc.run(argv, chunks.append, input_fd=-1)
    return status, b"".join(chunks).replace(b"\r", b"").decode("utf-8", "ignore")


class Job:
    # A slow command in the background: poll() returns None while it runs, then its status.
    def __init__(self, argv):
        self.output = b""
        self.status = None
        self._process = linuxproc.Process(argv, cols=200)

    def poll(self):
        if self._process is not None:
            self.output += self._process.read()
            status = self._process.poll()
            if status is not None:
                self.output += self._process.read()
                self._process.close()
                self._process = None
                self.status = status
        return self.status


def start_scan():
    name = interface()
    if name is None:
        raise OSError("no Wi-Fi interface")
    return Job(["/bin/sh", "-c", "ip link set %s up && iw dev %s scan" % (name, name)])


def scan_results(job):
    # [(ssid, signal_dbm, secured)] strongest first, one entry per name, hidden networks left out
    networks = {}
    ssid, signal, secured = None, -100.0, False

    def keep():
        if ssid and (ssid not in networks or networks[ssid][0] < signal):
            networks[ssid] = (signal, secured)

    for line in job.output.replace(b"\r", b"").decode("utf-8", "ignore").split("\n"):
        line = line.strip()
        if line.startswith("BSS ") and "(on " in line:
            keep()
            ssid, signal, secured = None, -100.0, False
        elif line.startswith("signal:"):
            try:
                signal = float(line.split()[1])
            except (IndexError, ValueError):
                pass
        elif line.startswith("SSID:"):
            ssid = line[5:].strip()
        elif line.startswith("RSN:") or line.startswith("WPA:") or "Privacy" in line:
            secured = True
    keep()
    found = [(name, value[0], value[1]) for name, value in networks.items()]
    found.sort(key=lambda item: -item[1])
    return found


def _wpa_config(ssid, password):
    lines = [MARKER, "ctrl_interface=/run/wpa_supplicant", "update_config=0", "network={"]
    lines.append("    ssid=" + binascii.hexlify(ssid.encode()).decode())  # hex: any name, no quoting
    if password:
        status, output = _run(["wpa_passphrase", ssid, password])
        psk = [line.strip() for line in output.split("\n") if line.strip().startswith("psk=")]
        if status != 0 or not psk:
            raise ValueError("the password must have 8 to 63 characters")
        lines.append("    " + psk[0])  # the hashed key, not the password
    else:
        lines.append("    key_mgmt=NONE")
    lines.append("}")
    return "\n".join(lines) + "\n"


def _write(path, text, private=False):
    folder = path.rsplit("/", 1)[0]
    try:
        os.stat(folder)
    except OSError:
        os.mkdir(folder)  # /etc/wpa_supplicant is missing on some images
    temporary = path + ".tmp"
    with open(temporary, "w") as target:
        target.write(text)
    if private:  # the hashed key: readable by root only
        _run(["chmod", "600", temporary])
    os.rename(temporary, path)


def start_connect(ssid, password):
    # Write the system configuration and (re)start Wi-Fi; returns the Job that does it.
    name = interface()
    if name is None:
        raise OSError("no Wi-Fi interface")
    _write("/etc/wpa_supplicant/wpa_supplicant-%s.conf" % name, _wpa_config(ssid, password), True)
    _write(NETWORK_FILE, "%s\n[Match]\nName=%s\n\n[Network]\nDHCP=ipv4\n" % (MARKER, name))
    return Job(["/bin/sh", "-c",
                "systemctl enable wpa_supplicant@{0} >/dev/null 2>&1; "
                "systemctl restart wpa_supplicant@{0} && networkctl reload && "
                "networkctl reconfigure {0}".format(name)])


def status():
    # {"state": "COMPLETED", "ssid": ..., "ip": ...}; state "DISCONNECTED" when there is none
    name = interface()
    if name is None:
        return {"state": "NO_INTERFACE"}
    code, output = _run(["wpa_cli", "-i", name, "status"])
    if code != 0:
        return {"state": "INACTIVE"}
    found = {}
    for line in output.split("\n"):
        key, _, value = line.strip().partition("=")
        if key == "wpa_state":
            found["state"] = value
        elif key == "ssid":
            found["ssid"] = value
        elif key == "ip_address":
            found["ip"] = value
    found.setdefault("state", "UNKNOWN")
    return found


def online():
    # True when the board has a default route, through Wi-Fi or a cable: cheap enough for a timer.
    try:
        with open("/proc/net/route") as routes:
            next(routes)
            return any(line.split()[1] == "00000000" for line in routes if line.strip())
    except (OSError, StopIteration, IndexError):
        return False


def connected():
    info = status()
    return info.get("state") == "COMPLETED" and "ip" in info


def ensure():
    # Connect to the network of settings.json unless already on it. Returns a Job, or None when
    # there is nothing to do (connected, or no network configured).
    wifi = mpy_settings.get("wifi")
    if not wifi or not wifi.get("ssid"):
        return None
    info = status()
    if info.get("state") == "COMPLETED" and info.get("ssid") == wifi["ssid"]:
        return None
    return start_connect(wifi["ssid"], wifi.get("password", ""))


def wait_online(seconds=20):
    # For scripts without a GUI: True once the interface has an address.
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        if connected():
            return True
        time.sleep_ms(500)
    return False
