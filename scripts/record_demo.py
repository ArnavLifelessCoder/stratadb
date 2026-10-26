"""Record docs/explorer-demo.gif: the explorer driving a fresh database.

Starts lsm_server on a fresh database, opens the page in headless Edge or
Chrome at 1280x860, and drives it through the DevTools protocol: a put, a
snapshot, an overwrite, reads at the latest state and at the snapshot, a
1M-key random bulk load captured about once a second, and a final scan. The
frames are joined into a GIF with Pillow (installed with matplotlib).

    python scripts/record_demo.py --server build/explorer/lsm_server \
        --browser "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
"""

import argparse
import base64
import io
import json
import os
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 1280, 860


class DevTools:
    """Just enough of a WebSocket client to speak the DevTools protocol."""

    def __init__(self, url: str):
        rest = url.removeprefix("ws://")
        host_port, path = rest.split("/", 1)
        host, port = host_port.split(":")
        self.sock = socket.create_connection((host, int(port)), timeout=60)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            (
                f"GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\n"
                f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).encode()
        )
        response = b""
        while b"\r\n\r\n" not in response:
            response += self.sock.recv(4096)
        if b" 101 " not in response.split(b"\r\n", 1)[0]:
            raise RuntimeError("websocket handshake failed: " + response.decode(errors="replace"))
        self.buffer = response.split(b"\r\n\r\n", 1)[1]
        self.next_id = 0

    def _read(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("devtools connection closed")
            self.buffer += chunk
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def _receive(self) -> dict:
        message = b""
        while True:
            first, second = self._read(2)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            message += self._read(length)
            if first & 0x80:
                return json.loads(message)

    def call(self, method: str, **params) -> dict:
        self.next_id += 1
        payload = json.dumps({"id": self.next_id, "method": method, "params": params}).encode()
        header = bytes([0x81])
        if len(payload) < 126:
            header += bytes([0x80 | len(payload)])
        elif len(payload) < 1 << 16:
            header += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", len(payload))
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)
        while True:
            message = self._receive()
            if message.get("id") == self.next_id:
                if "error" in message:
                    raise RuntimeError(f"{method}: {message['error']}")
                return message.get("result", {})

    def evaluate(self, expression: str):
        result = self.call(
            "Runtime.evaluate", expression=expression, awaitPromise=True, returnByValue=True
        )
        return result.get("result", {}).get("value")

    def capture(self) -> Image.Image:
        data = self.call("Page.captureScreenshot", format="png")["data"]
        return Image.open(io.BytesIO(base64.b64decode(data))).convert("RGB")


def wait_http(url: str, timeout: float = 30) -> bytes:
    deadline = time.monotonic() + timeout
    while True:
        try:
            with urllib.request.urlopen(url, timeout=2) as response:
                return response.read()
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.2)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--browser", required=True, help="Edge or Chrome executable")
    parser.add_argument("--output", type=Path, default=ROOT / "docs" / "explorer-demo.gif")
    parser.add_argument("--load", type=int, default=1_000_000)
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix="stratadb-demo-"))
    app_port, debug_port = free_port(), free_port()
    server = subprocess.Popen(
        [str(args.server), f"--port={app_port}", f"--db={work / 'db'}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    browser = subprocess.Popen(
        [
            args.browser,
            "--headless=new",
            f"--remote-debugging-port={debug_port}",
            f"--user-data-dir={work / 'profile'}",
            f"--window-size={WIDTH},{HEIGHT}",
            "--hide-scrollbars",
            "about:blank",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    frames: list[tuple[Image.Image, int]] = []
    try:
        app = f"http://127.0.0.1:{app_port}"
        wait_http(app + "/api/stats")
        targets = json.loads(wait_http(f"http://127.0.0.1:{debug_port}/json/list"))
        page = next(t for t in targets if t.get("type") == "page")
        tools = DevTools(page["webSocketDebuggerUrl"])
        tools.call(
            "Emulation.setDeviceMetricsOverride",
            width=WIDTH,
            height=HEIGHT,
            deviceScaleFactor=1,
            mobile=False,
        )
        tools.call(
            "Emulation.setEmulatedMedia",
            features=[{"name": "prefers-color-scheme", "value": "dark"}],
        )
        tools.call("Page.enable")
        tools.call("Page.navigate", url=app + "/")
        time.sleep(1.5)

        def step(script: str, hold_ms: int, settle: float = 0.8) -> None:
            if script:
                tools.evaluate(script)
            time.sleep(settle)
            frames.append((tools.capture(), hold_ms))

        set_value = "document.getElementById('{0}').value = {1};"
        step("", 1200)
        step(
            set_value.format("key", "'user:42'")
            + set_value.format("value", "'alice'")
            + "document.getElementById('put').click();",
            1400,
        )
        step("document.getElementById('snap').click();", 1800)
        step(
            set_value.format("value", "'bob'")
            + "document.getElementById('put').click();"
            + "new Promise(r => setTimeout(r, 400))"
            + ".then(() => document.getElementById('get').click());",
            1800,
        )
        step(
            set_value.format("readSnap", "'1'") + "document.getElementById('get').click();",
            2600,
        )
        step(
            set_value.format("loadCount", f"'{args.load}'")
            + set_value.format("loadOrder", "'random'")
            + "document.getElementById('load').click();",
            500,
            settle=0.6,
        )
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            step("", 450, settle=0.9)
            stats = json.loads(wait_http(app + "/api/stats"))
            if not stats["load"]["running"]:
                break
        step(
            set_value.format("scanStart", "'key0500000'")
            + set_value.format("scanLimit", "'20'")
            + "document.getElementById('scan').click();",
            3500,
            settle=1.2,
        )
    finally:
        browser.terminate()
        server.terminate()
        browser.wait(timeout=30)
        server.wait(timeout=60)

    # Shrink and share one palette so the GIF stays small and colors stay put.
    scale = 960 / WIDTH
    size = (960, round(HEIGHT * scale))
    images = [image.resize(size, Image.LANCZOS) for image, _ in frames]
    palette = images[-1].quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    paletted = [image.quantize(palette=palette, dither=Image.Dither.NONE) for image in images]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    paletted[0].save(
        args.output,
        save_all=True,
        append_images=paletted[1:],
        duration=[hold for _, hold in frames],
        loop=0,
        optimize=True,
    )
    print(f"{args.output}: {len(frames)} frames, {args.output.stat().st_size / 1024:.0f} KiB")


if __name__ == "__main__":
    main()
