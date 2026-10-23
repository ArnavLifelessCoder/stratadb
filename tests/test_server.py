"""End-to-end tests for the explorer server (explorer/lsm_server.cpp).

Starts the real binary on a free port with a fresh database and drives its HTTP
API. Set LSM_SERVER to the built executable (the explorer preset builds
build/explorer/lsm_server); the tests are skipped when it is not set.
"""

import json
import os
import socket
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import pytest

SERVER = os.environ.get("LSM_SERVER")
pytestmark = pytest.mark.skipif(not SERVER, reason="LSM_SERVER is not set")


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture
def server(tmp_path: Path):
    port = free_port()
    proc = subprocess.Popen(
        [SERVER, f"--port={port}", f"--db={tmp_path / 'db'}", "--memtable_bytes=65536"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    base = f"http://127.0.0.1:{port}"
    deadline = time.monotonic() + 30
    while True:
        try:
            urllib.request.urlopen(base + "/api/stats", timeout=1).read()
            break
        except OSError:
            if proc.poll() is not None or time.monotonic() > deadline:
                proc.kill()
                output = proc.stdout.read().decode()
                raise RuntimeError("server did not start: " + output) from None
            time.sleep(0.1)
    yield base
    proc.terminate()
    proc.wait(timeout=30)


def call(base, method, path, **params):
    data = urllib.parse.urlencode(params).encode()
    if method == "GET":
        request = urllib.request.Request(base + path + "?" + data.decode())
    else:
        request = urllib.request.Request(base + path, data=data, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def test_put_get_delete_and_binary_escaping(server):
    assert call(server, "POST", "/api/put", key="a", value="one") == (200, {"ok": True})
    assert call(server, "GET", "/api/get", key="a") == (200, {"found": True, "value": "one"})
    assert call(server, "POST", "/api/put", key="q", value='say "hi"\n\x01', sync="true")[0] == 200
    assert call(server, "GET", "/api/get", key="q")[1]["value"] == 'say "hi"\n\x01'
    assert call(server, "POST", "/api/delete", key="a") == (200, {"ok": True})
    assert call(server, "GET", "/api/get", key="a") == (200, {"found": False})
    status, body = call(server, "GET", "/api/get")
    assert status == 400 and "missing key" in body["error"]


def test_snapshot_keeps_old_values_until_released(server):
    call(server, "POST", "/api/put", key="user", value="alice")
    snapshot = call(server, "POST", "/api/snapshot")[1]["id"]
    call(server, "POST", "/api/put", key="user", value="bob")
    call(server, "POST", "/api/put", key="later", value="x")
    assert call(server, "GET", "/api/get", key="user")[1]["value"] == "bob"
    assert call(server, "GET", "/api/get", key="user", snapshot=snapshot)[1]["value"] == "alice"
    assert call(server, "GET", "/api/get", key="later", snapshot=snapshot)[1] == {"found": False}
    keys = [e["key"] for e in call(server, "GET", "/api/scan", snapshot=snapshot)[1]["entries"]]
    assert keys == ["user"]
    assert call(server, "GET", "/api/stats")[1]["snapshots"] == [snapshot]
    assert call(server, "POST", "/api/snapshot/release", id=snapshot) == (200, {"ok": True})
    assert call(server, "GET", "/api/stats")[1]["snapshots"] == []
    assert call(server, "GET", "/api/get", key="user", snapshot=snapshot)[0] == 400
    assert call(server, "POST", "/api/snapshot/release", id=snapshot)[0] == 404


def test_scan_is_ordered_and_paged(server):
    for key in ["d", "a", "c", "b", "e"]:
        call(server, "POST", "/api/put", key=key, value=key.upper())
    status, body = call(server, "GET", "/api/scan", start="b", limit=3)
    assert status == 200
    assert [(e["key"], e["value"]) for e in body["entries"]] == [("b", "B"), ("c", "C"), ("d", "D")]
    assert body["more"] is True
    assert call(server, "GET", "/api/scan", start="e", limit=3)[1]["more"] is False
    assert call(server, "GET", "/api/scan", limit=5000)[0] == 400


def test_bulk_load_flushes_and_reports_progress(server):
    status, _ = call(server, "POST", "/api/load", count=20000, value_size=100, order="random")
    assert status == 200
    assert call(server, "POST", "/api/load", count=10)[0] in (200, 409)
    deadline = time.monotonic() + 120
    while True:
        stats = call(server, "GET", "/api/stats")[1]
        if not stats["load"]["running"]:
            break
        assert time.monotonic() < deadline, "load did not finish"
        time.sleep(0.2)
    assert stats["load"]["error"] == ""
    assert stats["load"]["done"] == stats["load"]["total"]
    assert stats["flushes"] > 0
    assert len(stats["levels"]) >= 1
    for field in ("write_calls", "wal_records", "compactions", "stall_micros", "memtable_limit"):
        assert field in stats
    assert call(server, "POST", "/api/load", count=0)[0] == 400


def test_serves_the_page(server):
    with urllib.request.urlopen(server + "/", timeout=10) as response:
        assert b"stratadb explorer" in response.read()
