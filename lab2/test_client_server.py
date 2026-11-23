#!/usr/bin/env python3
"""
Comprehensive end-to-end & protocol compliance test suite for Lab 2 (Coffee Shop).

Verifies that the Java client (lab2/client) and C++ server (lab2/server)
correspond to each other and to the protocol specification across happy paths,
asynchronous notifications, heartbeats, concurrency, TCP framing, and edge cases.

Usage:
    python3 lab2/test_client_server.py              # build and run all tests
    python3 lab2/test_client_server.py --no-build   # skip build step
    python3 lab2/test_client_server.py -k low_stock # run matching tests only
    python3 lab2/test_client_server.py -v           # verbose output
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
LAB2_DIR = Path(__file__).resolve().parent
SERVER_DIR = LAB2_DIR / "server"
SERVER_BUILD_DIR = SERVER_DIR / "build"
SERVER_BIN = SERVER_BUILD_DIR / "server"
CLIENT_DIR = LAB2_DIR / "client"
CLIENT_SRC_DIR = CLIENT_DIR / "src" / "main" / "java"
CLIENT_CLASSES_DIR = CLIENT_DIR / "target" / "classes"

INT32_MAX = 2_147_483_647
HEARTBEAT_TIMEOUT_S = 3.0
COFFEE_PREP_S = 4
TEA_PREP_S = 3
CROISSANT_PREP_S = 2
LOW_STOCK_THRESHOLD = 5
MAX_LINE_BYTES = 8192

USE_COLOR = sys.stdout.isatty()


def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if USE_COLOR else text


# ---------------------------------------------------------------------------
# Toolchain discovery & build
# ---------------------------------------------------------------------------


def _java_major_version(java_bin: Path) -> Optional[int]:
    try:
        proc = subprocess.run(
            [str(java_bin), "-version"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        out = proc.stdout + proc.stderr
        m = re.search(r'version "(\d+)', out)
        return int(m.group(1)) if m else None
    except Exception:
        return None


def find_jdk27() -> tuple[Path, Path]:
    """Locate java & javac >= 27 (matching client/pom.xml)."""
    candidates: list[Path] = []
    env_home = os.environ.get("JAVA_HOME")
    if env_home:
        candidates.append(Path(env_home))
    candidates.extend(sorted(Path.home().glob(".jdks/openjdk-27*")))
    candidates.extend(sorted(Path.home().glob(".jdks/*27*")))
    which_java = shutil.which("java")
    if which_java:
        candidates.append(Path(which_java).resolve().parent.parent)

    for home in candidates:
        java_bin = home / "bin" / "java"
        javac_bin = home / "bin" / "javac"
        if java_bin.is_file() and javac_bin.is_file():
            ver = _java_major_version(java_bin)
            if ver is not None and ver >= 27:
                return java_bin, javac_bin

    raise RuntimeError(
        "Could not find JDK >= 27 (required by lab2/client/pom.xml). "
        "Set JAVA_HOME or install OpenJDK 27 under ~/.jdks/."
    )


def find_jackson_jars() -> list[Path]:
    """Find Jackson 3.x jars in ~/.m2/repository."""
    m2 = Path.home() / ".m2" / "repository"
    patterns = [
        "tools/jackson/core/jackson-databind/3.2.4/jackson-databind-3.2.4.jar",
        "tools/jackson/core/jackson-core/3.2.4/jackson-core-3.2.4.jar",
        "com/fasterxml/jackson/core/jackson-annotations/2.22/jackson-annotations-2.22.jar",
    ]
    jars: list[Path] = []
    for rel in patterns:
        p = m2 / rel
        if p.is_file():
            jars.append(p)
        else:
            prefix = str(m2 / Path(rel).parent.parent)
            name_prefix = Path(rel).name.rsplit("-", 1)[0]
            matches = [
                Path(x)
                for x in glob.glob(f"{prefix}/*/{name_prefix}-*.jar")
                if not x.endswith(("-sources.jar", "-javadoc.jar"))
            ]
            if not matches:
                raise RuntimeError(f"Missing required Jackson jar: {p}")
            jars.append(sorted(matches)[-1])
    return jars


def build_all(java_bin: Path, javac_bin: Path, jars: list[Path], verbose: bool) -> None:
    print(_c("36", "==> Building C++ server..."), flush=True)
    subprocess.run(
        ["cmake", "-S", str(SERVER_DIR), "-B", str(SERVER_BUILD_DIR)],
        check=True,
        stdout=None if verbose else subprocess.DEVNULL,
        stderr=subprocess.STDOUT if verbose else subprocess.PIPE,
    )
    subprocess.run(
        ["cmake", "--build", str(SERVER_BUILD_DIR), "-j"],
        check=True,
        stdout=None if verbose else subprocess.DEVNULL,
        stderr=subprocess.STDOUT if verbose else subprocess.PIPE,
    )
    assert SERVER_BIN.is_file(), f"Server binary not found at {SERVER_BIN}"

    print(_c("36", "==> Building Java client..."), flush=True)
    CLIENT_CLASSES_DIR.mkdir(parents=True, exist_ok=True)
    java_files = sorted(CLIENT_SRC_DIR.glob("*.java"))
    assert java_files, f"No .java files in {CLIENT_SRC_DIR}"
    cp = os.pathsep.join(str(j) for j in jars)
    res = subprocess.run(
        [str(javac_bin), "-cp", cp, "-d", str(CLIENT_CLASSES_DIR), *map(str, java_files)],
        capture_output=True,
        text=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"javac failed:\n{res.stdout}\n{res.stderr}")
    print(_c("32", "==> Build succeeded.\n"), flush=True)


# ---------------------------------------------------------------------------
# Process & TCP helpers
# ---------------------------------------------------------------------------


def alloc_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class ServerProcess:
    """Manages a real C++ `server` subprocess with line-buffered background log capture."""

    def __init__(self, port: Optional[int] = None, verbose: bool = False):
        self.port = port if port is not None else alloc_free_port()
        self.verbose = verbose
        self.proc: Optional[subprocess.Popen[str]] = None
        self.logs: list[str] = []
        self._log_lock = threading.Lock()
        self._ready_event = threading.Event()
        self.initial_stock: dict[str, int] = {}
        self._reader: Optional[threading.Thread] = None

    def start(self, timeout: float = 5.0) -> "ServerProcess":
        cmd = [str(SERVER_BIN), str(self.port)]
        stdbuf = shutil.which("stdbuf")
        if stdbuf:
            cmd = [stdbuf, "-oL", *cmd]

        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if self._ready_event.wait(timeout=0.05):
                    return self
                if self.proc.poll() is not None:
                    raise RuntimeError(
                        f"Server exited prematurely (code {self.proc.returncode}):\n{self.dump_logs()}"
                    )
            raise TimeoutError(f"Server did not start listening on {self.port}:\n{self.dump_logs()}")
        except BaseException:
            self.stop()
            raise

    def _read_loop(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.rstrip("\r\n")
            with self._log_lock:
                self.logs.append(line)
            if self.verbose:
                print(f"    [srv:{self.port}] {line}", flush=True)
            m = re.search(
                r"Listening on port (\d+) \| Initial stock: coffee=(\d+), tea=(\d+), croissant=(\d+)",
                line,
            )
            if m:
                self.initial_stock = {
                    "coffee": int(m.group(2)),
                    "tea": int(m.group(3)),
                    "croissant": int(m.group(4)),
                }
                self._ready_event.set()

    def log_count(self) -> int:
        with self._log_lock:
            return len(self.logs)

    def dump_logs(self) -> str:
        with self._log_lock:
            return "\n".join(self.logs)

    def wait_for_log(self, pattern: str, timeout: float = 5.0, start_index: int = 0) -> str:
        deadline = time.monotonic() + timeout
        regex = re.compile(pattern)
        while time.monotonic() < deadline:
            with self._log_lock:
                for line in self.logs[start_index:]:
                    if regex.search(line):
                        return line
            time.sleep(0.05)
        raise TimeoutError(
            f"Server log never matched /{pattern}/ (from idx {start_index}) within {timeout}s:\n"
            f"{self.dump_logs()}"
        )

    def stop(self, sig: int = signal.SIGTERM, timeout: float = 5.0) -> int:
        if self.proc is None:
            return 0
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2.0)
        if self._reader is not None:
            self._reader.join(timeout=1.0)
        return int(self.proc.returncode or 0)

    def __enter__(self) -> "ServerProcess":
        return self.start()

    def __exit__(self, *_: Any) -> None:
        self.stop()


class JavaClientProcess:
    """Drives the real Java `Main` client/provider over stdin/stdout."""

    def __init__(
        self,
        java_bin: Path,
        classpath: str,
        host: str,
        port: int,
        role: str,
        verbose: bool = False,
        name: str = "",
    ):
        self.java_bin = java_bin
        self.classpath = classpath
        self.host = host
        self.port = port
        self.role = role
        self.verbose = verbose
        self.name = name or role
        self.proc: Optional[subprocess.Popen[str]] = None
        self.lines: list[str] = []
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._msg_queue: queue.Queue[dict[str, Any]] = queue.Queue()
        self._reader: Optional[threading.Thread] = None

    def start(self) -> "JavaClientProcess":
        self.proc = subprocess.Popen(
            [str(self.java_bin), "-cp", self.classpath, "Main", self.host, str(self.port), self.role],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        return self

    def _read_loop(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        for raw_line in self.proc.stdout:
            line = raw_line.rstrip("\r\n")
            if self.verbose:
                print(f"    [cli:{self.name}] {line}", flush=True)
            with self._cond:
                self.lines.append(line)
                self._cond.notify_all()
            if "[SERVER]: " in line:
                payload = line.split("[SERVER]: ", 1)[1]
                try:
                    parsed = json.loads(payload)
                    self._msg_queue.put(parsed)
                except json.JSONDecodeError:
                    self._msg_queue.put({"_raw": payload})

    def send_lines(self, *lines: str) -> None:
        assert self.proc is not None and self.proc.stdin is not None
        for line in lines:
            self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def cmd_menu(self, timeout: float = 5.0) -> dict[str, Any]:
        self.send_lines("menu")
        return self.expect_server_msg("MENU_RESPONSE", timeout=timeout)

    def cmd_order(self, coffee: int, tea: int, croissant: int) -> None:
        self.send_lines("order", str(coffee), str(tea), str(croissant))

    def cmd_deliver(self, coffee: int, tea: int, croissant: int) -> None:
        self.send_lines("deliver", str(coffee), str(tea), str(croissant))

    def cmd_raw(self, payload: str) -> None:
        self.send_lines("raw", payload)

    def expect_server_msg(
        self,
        expected_type: Optional[str] = None,
        timeout: float = 5.0,
        predicate: Optional[Callable[[dict[str, Any]], bool]] = None,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        seen: list[dict[str, Any]] = []
        while True:
            rem = deadline - time.monotonic()
            if rem <= 0:
                raise TimeoutError(
                    f"[{self.name}] Timed out waiting for server msg type={expected_type!r}. "
                    f"Seen={seen}\nOutput:\n{self.dump_output()}"
                )
            try:
                msg = self._msg_queue.get(timeout=min(rem, 0.2))
            except queue.Empty:
                continue
            if (expected_type is None or msg.get("type") == expected_type) and (
                predicate is None or predicate(msg)
            ):
                return msg
            seen.append(msg)

    def assert_no_server_msg(self, duration: float = 0.5) -> None:
        try:
            msg = self._msg_queue.get(timeout=duration)
            raise AssertionError(f"[{self.name}] Expected no server message, but received: {msg}")
        except queue.Empty:
            return

    def wait_for_output(self, substring: str, timeout: float = 5.0, start_index: int = 0) -> int:
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                for i in range(start_index, len(self.lines)):
                    if substring in self.lines[i]:
                        return i
                rem = deadline - time.monotonic()
                if rem <= 0:
                    raise TimeoutError(
                        f"[{self.name}] Timed out waiting for stdout containing {substring!r}.\n"
                        f"Output:\n" + "\n".join(self.lines)
                    )
                self._cond.wait(timeout=min(rem, 0.2))

    def output_line_count(self) -> int:
        with self._lock:
            return len(self.lines)

    def dump_output(self) -> str:
        with self._lock:
            return "\n".join(self.lines)

    def assert_no_parse_errors(self) -> None:
        out = self.dump_output()
        assert "Invalid server message:" not in out, f"Java ServerListener parse error!\n{out}"
        assert "Missing 'type' field" not in out, f"Java ServerListener missing type!\n{out}"
        assert "Ignored invalid message type:" not in out, f"Java ServerListener ignored type!\n{out}"

    def exit_cleanly(self, timeout: float = 5.0) -> int:
        if self.proc is None:
            return 0
        if self.proc.poll() is None:
            try:
                self.send_lines("exit")
            except (BrokenPipeError, OSError):
                pass
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=2.0)
        if self._reader is not None:
            self._reader.join(timeout=1.0)
        return int(self.proc.returncode or 0)

    def kill(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=2.0)
        if self._reader is not None:
            self._reader.join(timeout=1.0)

    def __enter__(self) -> "JavaClientProcess":
        return self.start()

    def __exit__(self, *_: Any) -> None:
        self.exit_cleanly()


class RawTCPClient:
    """Direct TCP client for wire-level protocol & framing edge cases."""

    def __init__(self, port: int, auto_heartbeat: bool = True, heartbeat_interval: float = 1.0):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buf = b""
        self._send_lock = threading.Lock()
        self._stop_hb = threading.Event()
        self._hb_thread: Optional[threading.Thread] = None
        if auto_heartbeat:
            self.send_json({"type": "HEARTBEAT"})
            self._hb_thread = threading.Thread(
                target=self._hb_loop, args=(heartbeat_interval,), daemon=True
            )
            self._hb_thread.start()

    def _hb_loop(self, interval: float) -> None:
        while not self._stop_hb.wait(interval):
            try:
                self.send_json({"type": "HEARTBEAT"})
            except OSError:
                break

    def stop_heartbeat(self) -> None:
        self._stop_hb.set()

    def send_raw(self, data: bytes) -> None:
        with self._send_lock:
            self.sock.sendall(data)

    def send_line(self, line: str) -> None:
        self.send_raw((line + "\n").encode("utf-8"))

    def send_json(self, obj: Any) -> None:
        self.send_line(json.dumps(obj, separators=(",", ":")))

    def recv_line(self, timeout: float = 5.0) -> Optional[str]:
        self.sock.settimeout(timeout)
        deadline = time.monotonic() + timeout
        while True:
            nl = self._buf.find(b"\n")
            if nl != -1:
                line = self._buf[:nl].decode("utf-8")
                self._buf = self._buf[nl + 1 :]
                return line.rstrip("\r")
            rem = deadline - time.monotonic()
            if rem <= 0:
                raise TimeoutError("Timed out waiting for line from server")
            self.sock.settimeout(rem)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout as exc:
                raise TimeoutError("Timed out waiting for line from server") from exc
            if not chunk:
                return None
            self._buf += chunk

    def recv_json(self, timeout: float = 5.0) -> dict[str, Any]:
        line = self.recv_line(timeout=timeout)
        assert line is not None, "Server closed connection unexpectedly"
        return json.loads(line)

    def expect_closed(self, timeout: float = 5.0) -> float:
        t0 = time.monotonic()
        self.sock.settimeout(timeout)
        deadline = t0 + timeout
        while time.monotonic() < deadline:
            self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            try:
                data = self.sock.recv(4096)
            except (ConnectionResetError, BrokenPipeError):
                return time.monotonic() - t0
            except socket.timeout:
                break
            if data == b"":
                return time.monotonic() - t0
        raise AssertionError(f"Expected server to close socket within {timeout}s, but it stayed open")

    def close(self) -> None:
        self._stop_hb.set()
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self) -> "RawTCPClient":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()


# ---------------------------------------------------------------------------
# Test context & registry
# ---------------------------------------------------------------------------


@dataclass
class TestCtx:
    java_bin: Path
    classpath: str
    verbose: bool = False

    def server(self, port: Optional[int] = None) -> ServerProcess:
        return ServerProcess(port=port, verbose=self.verbose)

    def java_client(self, port: int, role: str = "client", name: str = "") -> JavaClientProcess:
        return JavaClientProcess(
            self.java_bin, self.classpath, "127.0.0.1", port, role, verbose=self.verbose, name=name
        )


TESTS: list[tuple[str, Callable[[TestCtx], None]]] = []


def test(fn: Callable[[TestCtx], None]) -> Callable[[TestCtx], None]:
    TESTS.append((fn.__name__, fn))
    return fn


def assert_error(msg: dict[str, Any], expected_code: str, ctx_label: str = "") -> None:
    assert msg.get("type") == "ERROR", f"{ctx_label}: expected ERROR, got {msg}"
    assert msg.get("code") == expected_code, (
        f"{ctx_label}: expected error code {expected_code!r}, got {msg}"
    )
    assert isinstance(msg.get("message"), str) and len(msg["message"]) > 0, (
        f"{ctx_label}: expected non-empty error message, got {msg}"
    )


def get_menu_raw(port: int) -> dict[str, int]:
    with RawTCPClient(port) as c:
        c.send_json({"type": "MENU_REQUEST"})
        resp = c.recv_json()
        assert resp["type"] == "MENU_RESPONSE", resp
        return dict(resp["items"])


# ===========================================================================
# SECTION 1: Wire-level verification of Java Client (against mock TCP server)
# ===========================================================================


@test
def test_java_client_wire_format_and_server_listener_framing(ctx: TestCtx) -> None:
    """Verify exact JSON emitted by Java Main and ServerListener handling of all message types & framing."""
    port = alloc_free_port()
    received_lines: list[tuple[float, str]] = []
    conn_holder: dict[str, socket.socket] = {}
    accepted_ev = threading.Event()
    stop_ev = threading.Event()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(2)
    srv.settimeout(5.0)

    def accept_and_read() -> None:
        conn, _ = srv.accept()
        conn.settimeout(0.2)
        conn_holder["conn"] = conn
        accepted_ev.set()
        buf = b""
        while not stop_ev.is_set():
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                received_lines.append((time.monotonic(), line.decode("utf-8").rstrip("\r")))

    t = threading.Thread(target=accept_and_read, daemon=True)
    t.start()

    try:
        with ctx.java_client(port, role="provider", name="wire-provider") as cli:
            assert accepted_ev.wait(timeout=5.0), "Java client never connected to mock server"
            conn = conn_holder["conn"]

            # Exercise every command that emits JSON (including whitespace & uppercase CLI handling)
            cli.send_lines("  MENU  ")
            cli.cmd_order(1, 2, 3)
            cli.cmd_deliver(4, 0, 5)
            cli.cmd_raw('{"custom":"raw_test"}')

            # Wait enough time for at least 2 heartbeats (~1.5s interval)
            deadline = time.monotonic() + 3.5
            while time.monotonic() < deadline:
                hb_count = sum(
                    1 for _, s in received_lines if s == '{"type":"HEARTBEAT"}'
                )
                if hb_count >= 2 and len(received_lines) >= 6:
                    break
                time.sleep(0.05)

            parsed_non_hb = [
                json.loads(line)
                for _, line in received_lines
                if line != '{"type":"HEARTBEAT"}'
            ]
            hb_times = [ts for ts, s in received_lines if s == '{"type":"HEARTBEAT"}']

            assert len(hb_times) >= 2, f"Expected >=2 HEARTBEATs, got {received_lines}"
            interval = hb_times[1] - hb_times[0]
            assert 1.0 <= interval <= 2.3, f"Unexpected HEARTBEAT interval: {interval:.2f}s"

            # First non-heartbeat message from provider MUST be PROVIDER_IDENTIFY
            assert parsed_non_hb[0] == {"type": "PROVIDER_IDENTIFY"}, parsed_non_hb
            assert {"type": "MENU_REQUEST"} in parsed_non_hb, parsed_non_hb
            assert {
                "type": "ORDER_REQUEST",
                "items": {"coffee": 1, "tea": 2, "croissant": 3},
            } in parsed_non_hb, parsed_non_hb
            assert {
                "type": "DELIVERY",
                "items": {"coffee": 4, "tea": 0, "croissant": 5},
            } in parsed_non_hb, parsed_non_hb
            assert {"custom": "raw_test"} in parsed_non_hb, parsed_non_hb

            # Now test ServerListener with:
            # 1) Partial TCP write (split mid-JSON)
            partial = b'{"type":"MENU_RESPONSE","items":{"coffee":11,"tea":22,"croissant":33}}\n'
            conn.sendall(partial[:20])
            time.sleep(0.05)
            conn.sendall(partial[20:45])
            time.sleep(0.05)
            conn.sendall(partial[45:])
            cli.wait_for_output("Coffee: 11")
            cli.wait_for_output("Tea: 22")
            cli.wait_for_output("Croissants: 33")

            # 2) Coalesced multi-message TCP packet covering every remaining Server->Client type
            coalesced = (
                '{"type":"ORDER_ACCEPTED","orderId":77,"preparationTime":13}\n'
                '{"type":"ORDER_REFUSED","reason":"Out of café beans"}\n'
                '{"type":"ORDER_READY","orderId":77}\n'
                '{"type":"LOW_STOCK","product":"tea","remaining":2}\n'
                '{"type":"ERROR","code":"INVALID_QUANTITY","message":"Bad qty"}\n'
            ).encode("utf-8")
            conn.sendall(coalesced)

            cli.wait_for_output("Order accepted! Your order number: 77")
            cli.wait_for_output("ETA: 13 seconds")
            cli.wait_for_output("Order refused. Reason: Out of café beans")
            cli.wait_for_output("Your order is ready! Order number: 77")
            cli.wait_for_output("Notice: Low tea stock!")
            cli.wait_for_output("2 left")
            cli.wait_for_output("[ERROR - INVALID_QUANTITY]: Bad qty")
            cli.assert_no_parse_errors()

            # 3) Malformed/unexpected server messages must NOT crash ServerListener
            conn.sendall(
                b"not valid json\n"
                b'{"no_type":true}\n'
                b'{"type":"UNKNOWN_FUTURE_TYPE"}\n'
                b'{"type":"MENU_REQUEST"}\n'
                b'{"type":"ORDER_READY","orderId":999}\n'
            )
            cli.wait_for_output("Invalid server message:")
            cli.wait_for_output("Missing 'type' field")
            cli.wait_for_output("Ignored invalid message type: MENU_REQUEST")
            cli.wait_for_output("Your order is ready! Order number: 999")
    finally:
        stop_ev.set()
        if "conn" in conn_holder:
            conn_holder["conn"].close()
        srv.close()
        t.join(timeout=2.0)


@test
def test_java_client_cli_usage_missing_args(ctx: TestCtx) -> None:
    """Java Main exits cleanly with usage message when invoked with < 3 args."""
    res = subprocess.run(
        [str(ctx.java_bin), "-cp", ctx.classpath, "Main"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert res.returncode == 0, res.stderr
    assert "Usage:" in res.stdout, res.stdout


# ===========================================================================
# SECTION 2: Real Java Client <-> Real C++ Server End-to-End Tests
# ===========================================================================


@test
def test_e2e_client_menu_matches_server_initial_stock(ctx: TestCtx) -> None:
    """Java client `menu` returns exact server initial stock in [10, 50] and renders cleanly."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        for prod, qty in srv.initial_stock.items():
            assert 10 <= qty <= 50, f"Initial {prod} stock out of [10,50]: {qty}"

        resp = cli.cmd_menu()
        assert resp["items"] == srv.initial_stock, f"{resp['items']} != {srv.initial_stock}"
        cli.wait_for_output("Menu of the day")
        cli.wait_for_output(f"Coffee: {srv.initial_stock['coffee']}")
        cli.wait_for_output(f"Tea: {srv.initial_stock['tea']}")
        cli.wait_for_output(f"Croissants: {srv.initial_stock['croissant']}")
        cli.assert_no_parse_errors()


@test
def test_e2e_client_heartbeat_keeps_connection_alive(ctx: TestCtx) -> None:
    """Java client automatic 1.5s HEARTBEAT keeps session alive beyond server's 3s timeout."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        first = cli.cmd_menu()
        time.sleep(4.2)
        second = cli.cmd_menu()
        assert first["items"] == second["items"]
        assert "Server disconnected." not in cli.dump_output()
        cli.assert_no_parse_errors()


@test
def test_e2e_client_order_accepted_ready_and_stock_decremented(ctx: TestCtx) -> None:
    """Java client places an order, gets ORDER_ACCEPTED with exact ETA, receives ORDER_READY, and stock updates."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        init = cli.cmd_menu()["items"]

        t0 = time.monotonic()
        cli.cmd_order(0, 0, 1)  # 1 croissant -> 2s prep
        accepted = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        assert accepted["orderId"] == 1, accepted
        assert accepted["preparationTime"] == CROISSANT_PREP_S, accepted
        cli.wait_for_output("Order accepted! Your order number: 1")
        cli.wait_for_output(f"ETA: {CROISSANT_PREP_S} seconds")

        # Stock must be decremented immediately upon acceptance
        mid = cli.cmd_menu()["items"]
        assert mid["croissant"] == init["croissant"] - 1
        assert mid["coffee"] == init["coffee"]
        assert mid["tea"] == init["tea"]

        ready = cli.expect_server_msg("ORDER_READY", timeout=CROISSANT_PREP_S + 3.0)
        elapsed = time.monotonic() - t0
        assert ready["orderId"] == 1, ready
        assert elapsed >= CROISSANT_PREP_S - 0.3, f"ORDER_READY arrived too early ({elapsed:.2f}s)"
        cli.wait_for_output("Your order is ready! Order number: 1")
        cli.assert_no_parse_errors()


@test
def test_e2e_client_mixed_order_eta_formula(ctx: TestCtx) -> None:
    """Verify prep time formula (coffee*4 + tea*3 + croissant*2) and completion across mixed products."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        # 0 coffee + 1 tea (3s) + 1 croissant (2s) = 5s (also crosses 3s heartbeat timeout!)
        cli.cmd_order(0, 1, 1)
        acc = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        assert acc["preparationTime"] == TEA_PREP_S + CROISSANT_PREP_S, acc
        ready = cli.expect_server_msg("ORDER_READY", timeout=8.0)
        assert ready["orderId"] == acc["orderId"]

        # Also check all-3-products ETA (1*4 + 1*3 + 1*2 = 9s), then exit to avoid waiting 9s
        cli.cmd_order(1, 1, 1)
        acc2 = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        assert acc2["orderId"] == acc["orderId"] + 1
        assert acc2["preparationTime"] == COFFEE_PREP_S + TEA_PREP_S + CROISSANT_PREP_S, acc2
        cli.assert_no_parse_errors()


@test
def test_e2e_client_order_refused_all_products(ctx: TestCtx) -> None:
    """Ordering more than available stock for coffee, tea, or croissant yields ORDER_REFUSED without touching stock."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        init = cli.cmd_menu()["items"]

        cases = [
            ("coffee", (init["coffee"] + 1, 0, 0)),
            ("tea", (0, init["tea"] + 1, 0)),
            ("croissant", (0, 0, init["croissant"] + 1)),
        ]
        for prod, (c, t, cr) in cases:
            idx = cli.output_line_count()
            cli.cmd_order(c, t, cr)
            refused = cli.expect_server_msg("ORDER_REFUSED", timeout=3.0)
            assert prod in refused["reason"].lower(), refused
            cli.wait_for_output("Order refused. Reason:", start_index=idx)

        after = cli.cmd_menu()["items"]
        assert after == init, f"Stock mutated after refused orders: {after} != {init}"
        cli.assert_no_parse_errors()


@test
def test_e2e_client_local_input_validation_and_unknown_command(ctx: TestCtx) -> None:
    """Java client rejects negative/non-integer/all-zero inputs and unknown commands locally without breaking session."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        # Unknown command
        cli.send_lines("foobar")
        cli.wait_for_output("Unknown command.")

        # Order with negative, non-integer, then all zeros
        cli.send_lines("order", "-5", "abc", "0", "0", "0")
        cli.wait_for_output("Amount cannot be negative.")
        cli.wait_for_output("Please enter a valid integer.")
        cli.wait_for_output("Order must contain at least one item!")

        # Server should not have received any malformed message; menu still works
        cli.assert_no_server_msg(0.3)
        resp = cli.cmd_menu()
        assert resp["items"] == srv.initial_stock
        cli.assert_no_parse_errors()


@test
def test_e2e_client_unauthorized_delivery(ctx: TestCtx) -> None:
    """A `client`-role Java process sending `deliver` receives ERROR UNAUTHORIZED_OPERATION."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        init = cli.cmd_menu()["items"]
        cli.cmd_deliver(5, 0, 0)
        err = cli.expect_server_msg("ERROR", timeout=3.0)
        assert_error(err, "UNAUTHORIZED_OPERATION")
        cli.wait_for_output("[ERROR - UNAUTHORIZED_OPERATION]:")

        after = cli.cmd_menu()["items"]
        assert after == init
        cli.assert_no_parse_errors()


@test
def test_e2e_client_raw_command_protocol_errors(ctx: TestCtx) -> None:
    """Java client `raw` command sends verbatim invalid payloads and displays server ERROR responses."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        cases = [
            ("{not-json", "INVALID_MESSAGE"),
            ("{}", "MISSING_FIELD"),
            ('{"type":"ORDER_REQUEST","items":{"latte":1}}', "UNKNOWN_PRODUCT"),
            ('{"type":"ORDER_REQUEST","items":{"coffee":-2}}', "INVALID_QUANTITY"),
            ('{"type":"ORDER_REQUEST","items":{"coffee":0}}', "INVALID_QUANTITY"),
            ('{"type":"ORDER_READY","orderId":1}', "INVALID_MESSAGE"),
        ]
        for payload, code in cases:
            idx = cli.output_line_count()
            cli.cmd_raw(payload)
            err = cli.expect_server_msg("ERROR", timeout=3.0)
            assert_error(err, code, payload)
            cli.wait_for_output(f"[ERROR - {code}]:", start_index=idx)

        # Connection stays healthy after errors
        assert cli.cmd_menu()["items"] == srv.initial_stock
        cli.assert_no_parse_errors()


@test
def test_e2e_client_disconnect_cancels_unfinished_order_and_restores_stock(ctx: TestCtx) -> None:
    """Disconnecting a Java client mid-preparation cancels its order and restores reserved stock."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client", name="observer") as obs:
        init = obs.cmd_menu()["items"]

        cli = ctx.java_client(srv.port, "client", name="canceller").start()
        log_idx = srv.log_count()
        try:
            cli.cmd_order(2, 1, 1)  # 2*4 + 1*3 + 1*2 = 13s prep
            acc = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
            assert acc["preparationTime"] == 13

            mid = obs.cmd_menu()["items"]
            assert mid == {
                "coffee": init["coffee"] - 2,
                "tea": init["tea"] - 1,
                "croissant": init["croissant"] - 1,
            }, f"Stock not reserved: {mid} vs {init}"
        finally:
            cli.exit_cleanly()

        srv.wait_for_log(r"ORDER CANCELLED #1", timeout=3.0, start_index=log_idx)
        restored = obs.cmd_menu()["items"]
        assert restored == init, f"Stock not restored after disconnect: {restored} != {init}"


@test
def test_e2e_abrupt_sigkill_cancels_only_unfinished_orders(ctx: TestCtx) -> None:
    """SIGKILLing a Java client with 1 completed + 2 active orders restores ONLY the 2 unfinished orders."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client", name="observer") as obs:
        init = obs.cmd_menu()["items"]

        cli = ctx.java_client(srv.port, "client", name="victim").start()
        try:
            # Order 1: completes (1 croissant, 2s)
            cli.cmd_order(0, 0, 1)
            acc1 = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
            ready1 = cli.expect_server_msg("ORDER_READY", timeout=5.0)
            assert ready1["orderId"] == acc1["orderId"]

            # Orders 2 & 3: long orders left unfinished when client is SIGKILLed
            cli.cmd_order(2, 0, 0)  # 8s
            acc2 = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
            cli.cmd_order(0, 3, 0)  # 9s
            acc3 = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
            assert acc3["orderId"] > acc2["orderId"] > acc1["orderId"]

            log_idx = srv.log_count()
        finally:
            cli.kill()

        srv.wait_for_log(rf"ORDER CANCELLED #{acc2['orderId']}", timeout=3.0, start_index=log_idx)
        srv.wait_for_log(rf"ORDER CANCELLED #{acc3['orderId']}", timeout=3.0, start_index=log_idx)

        after = obs.cmd_menu()["items"]
        assert after == {
            "coffee": init["coffee"],  # order 2 restored
            "tea": init["tea"],  # order 3 restored
            "croissant": init["croissant"] - 1,  # order 1 stayed consumed
        }, f"Wrong partial restoration after SIGKILL: {after} vs init={init}"


@test
def test_e2e_completed_order_not_restored_on_disconnect(ctx: TestCtx) -> None:
    """After ORDER_READY is sent, disconnecting the client must NOT restore stock."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client", name="observer") as obs:
        init = obs.cmd_menu()["items"]

        log_idx = srv.log_count()
        with ctx.java_client(srv.port, "client", name="buyer") as buyer:
            buyer.cmd_order(0, 0, 1)
            acc = buyer.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
            ready = buyer.expect_server_msg("ORDER_READY", timeout=5.0)
            assert ready["orderId"] == acc["orderId"]
            log_idx = srv.log_count()

        srv.wait_for_log(r"Disconnected:", timeout=3.0, start_index=log_idx)
        after = obs.cmd_menu()["items"]
        assert after["croissant"] == init["croissant"] - 1, (
            f"Completed order was wrongly restored: {after} vs {init}"
        )


@test
def test_e2e_provider_identify_and_delivery(ctx: TestCtx) -> None:
    """Java provider identifies automatically, delivers stock, and updates are visible to clients."""
    with (
        ctx.server() as srv,
        ctx.java_client(srv.port, "provider") as prov,
        ctx.java_client(srv.port, "client") as cli,
    ):
        srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0)
        init = cli.cmd_menu()["items"]

        log_idx = srv.log_count()
        prov.cmd_deliver(7, 4, 9)
        srv.wait_for_log(r"DELIVERY received", timeout=3.0, start_index=log_idx)
        # Delivery is silent on success (no ERROR)
        prov.assert_no_server_msg(0.3)

        expected = {
            "coffee": init["coffee"] + 7,
            "tea": init["tea"] + 4,
            "croissant": init["croissant"] + 9,
        }
        assert prov.cmd_menu()["items"] == expected
        assert cli.cmd_menu()["items"] == expected
        prov.assert_no_parse_errors()
        cli.assert_no_parse_errors()


@test
def test_e2e_low_stock_threshold_boundary_and_alerts(ctx: TestCtx) -> None:
    """LOW_STOCK is NOT sent at remaining==5, IS sent at remaining==4 and remaining==0."""
    with (
        ctx.server() as srv,
        ctx.java_client(srv.port, "provider") as prov,
        ctx.java_client(srv.port, "client") as cli,
    ):
        srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0)
        init = cli.cmd_menu()["items"]

        # Drop croissant to exactly 5 -> must NOT trigger LOW_STOCK
        to_five = init["croissant"] - 5
        cli.cmd_order(0, 0, to_five)
        cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        prov.assert_no_server_msg(0.5)

        # Drop croissant from 5 -> 4 -> MUST trigger LOW_STOCK(croissant, 4)
        cli.cmd_order(0, 0, 1)
        cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        alert1 = prov.expect_server_msg("LOW_STOCK", timeout=3.0)
        assert alert1 == {"type": "LOW_STOCK", "product": "croissant", "remaining": 4}, alert1
        prov.wait_for_output("Notice: Low croissant stock!")
        prov.wait_for_output("4 left")

        # Drop croissant from 4 -> 0 AND tea to 3 in one mixed order -> BOTH alerts delivered
        to_tea_three = init["tea"] - 3
        cli.cmd_order(0, to_tea_three, 4)
        cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)

        a1 = prov.expect_server_msg("LOW_STOCK", timeout=3.0)
        a2 = prov.expect_server_msg("LOW_STOCK", timeout=3.0)
        got = {(a["product"], a["remaining"]) for a in (a1, a2)}
        assert got == {("tea", 3), ("croissant", 0)}, f"Unexpected alerts: {a1}, {a2}"
        prov.assert_no_parse_errors()


@test
def test_e2e_full_restock_cycle_drain_refuse_low_stock_deliver_reorder(ctx: TestCtx) -> None:
    """Full loop: Java client drains croissant to 0 -> refused -> Java provider gets LOW_STOCK & delivers -> reorder succeeds."""
    with (
        ctx.server() as srv,
        ctx.java_client(srv.port, "provider") as prov,
        ctx.java_client(srv.port, "client") as cli,
    ):
        srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0)
        init = cli.cmd_menu()["items"]

        # Drain all croissants to 0
        cli.cmd_order(0, 0, init["croissant"])
        cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        alert = prov.expect_server_msg("LOW_STOCK", timeout=3.0)
        assert alert == {"type": "LOW_STOCK", "product": "croissant", "remaining": 0}, alert

        # Next croissant order must be refused
        cli.cmd_order(0, 0, 1)
        refused = cli.expect_server_msg("ORDER_REFUSED", timeout=3.0)
        assert "croissant" in refused["reason"].lower(), refused

        # Provider restocks 10 croissants
        log_idx = srv.log_count()
        prov.cmd_deliver(0, 0, 10)
        srv.wait_for_log(r"DELIVERY received", timeout=3.0, start_index=log_idx)

        # Client orders 1 croissant again -> accepted and becomes ready!
        cli.cmd_order(0, 0, 1)
        acc = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        ready = cli.expect_server_msg(
            "ORDER_READY",
            timeout=5.0,
            predicate=lambda m: m.get("orderId") == acc["orderId"],
        )
        assert ready["orderId"] == acc["orderId"]
        assert cli.cmd_menu()["items"]["croissant"] == 9
        prov.assert_no_parse_errors()
        cli.assert_no_parse_errors()


@test
def test_e2e_low_stock_sent_immediately_on_provider_reconnect(ctx: TestCtx) -> None:
    """A newly connecting Java provider immediately receives LOW_STOCK for any product already < 5."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        init = cli.cmd_menu()["items"]
        # Drain coffee to 2 while NO provider is connected
        cli.cmd_order(init["coffee"] - 2, 0, 0)
        cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)

        # Now connect a Java provider -> should receive LOW_STOCK immediately on connect
        with ctx.java_client(srv.port, "provider", name="late-provider") as prov:
            alert = prov.expect_server_msg("LOW_STOCK", timeout=3.0)
            assert alert == {"type": "LOW_STOCK", "product": "coffee", "remaining": 2}, alert
            prov.wait_for_output("Notice: Low coffee stock!")
            prov.wait_for_output("2 left")
            prov.assert_no_parse_errors()


@test
def test_e2e_provider_heartbeat_and_self_order_low_stock(ctx: TestCtx) -> None:
    """Java provider stays alive past 3s timeout via HEARTBEAT and can order & receive LOW_STOCK on itself."""
    with ctx.server() as srv, ctx.java_client(srv.port, "provider") as prov:
        srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0)
        time.sleep(3.8)
        init = prov.cmd_menu()["items"]

        # Provider places an order dropping tea to 1 -> gets both ORDER_ACCEPTED and LOW_STOCK
        prov.cmd_order(0, init["tea"] - 1, 0)
        m1 = prov.expect_server_msg(timeout=3.0)
        m2 = prov.expect_server_msg(timeout=3.0)
        by_type = {m["type"]: m for m in (m1, m2)}
        assert "ORDER_ACCEPTED" in by_type and "LOW_STOCK" in by_type, (m1, m2)
        assert by_type["LOW_STOCK"] == {"type": "LOW_STOCK", "product": "tea", "remaining": 1}
        prov.assert_no_parse_errors()


@test
def test_e2e_provider_disconnect_does_not_stop_server(ctx: TestCtx) -> None:
    """Disconnecting the provider leaves the server running and clients able to order (even hitting low stock)."""
    with ctx.server() as srv, ctx.java_client(srv.port, "client") as cli:
        log_idx = srv.log_count()
        with ctx.java_client(srv.port, "provider") as prov:
            srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0, start_index=log_idx)
            prov.cmd_deliver(1, 0, 0)
            srv.wait_for_log(r"DELIVERY received", timeout=3.0, start_index=log_idx)
            log_idx = srv.log_count()

        srv.wait_for_log(r"Disconnected:", timeout=3.0, start_index=log_idx)

        # Client can still query menu and complete an order
        menu = cli.cmd_menu()["items"]
        assert menu["coffee"] == srv.initial_stock["coffee"] + 1

        cli.cmd_order(0, 0, 1)
        acc = cli.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
        ready = cli.expect_server_msg("ORDER_READY", timeout=5.0)
        assert ready["orderId"] == acc["orderId"]
        cli.assert_no_parse_errors()


@test
def test_e2e_provider_delivery_overflow_rejected(ctx: TestCtx) -> None:
    """Java provider delivering INT32_MAX triggers server overflow protection (ERROR INVALID_DELIVERY)."""
    with ctx.server() as srv, ctx.java_client(srv.port, "provider") as prov:
        srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0)
        init = prov.cmd_menu()["items"]

        prov.cmd_deliver(INT32_MAX, 0, 0)
        err = prov.expect_server_msg("ERROR", timeout=3.0)
        assert_error(err, "INVALID_DELIVERY")
        prov.wait_for_output("[ERROR - INVALID_DELIVERY]:")

        assert prov.cmd_menu()["items"] == init
        prov.assert_no_parse_errors()


@test
def test_e2e_server_shutdown_disconnects_java_clients_cleanly(ctx: TestCtx) -> None:
    """When server stops on SIGTERM, connected Java client and provider print disconnect and exit 0."""
    srv = ctx.server().start()
    cli = ctx.java_client(srv.port, "client").start()
    prov = ctx.java_client(srv.port, "provider").start()
    try:
        cli.cmd_menu()
        srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0)

        srv_rc = srv.stop(signal.SIGTERM, timeout=5.0)
        assert srv_rc == 0, f"Expected server exit 0 on SIGTERM, got {srv_rc}"
        assert "Server stopped" in srv.dump_logs()

        cli.wait_for_output("Server disconnected.", timeout=5.0)
        prov.wait_for_output("Server disconnected.", timeout=5.0)

        assert cli.proc is not None and cli.proc.wait(timeout=5.0) == 0
        assert prov.proc is not None and prov.proc.wait(timeout=5.0) == 0
    finally:
        cli.kill()
        prov.kill()
        srv.stop()


@test
def test_e2e_multiple_java_clients_and_provider_concurrent(ctx: TestCtx) -> None:
    """3 real Java clients + 1 Java provider operating concurrently preserve exact stock accounting."""
    with (
        ctx.server() as srv,
        ctx.java_client(srv.port, "provider") as prov,
        ctx.java_client(srv.port, "client", name="cli-1") as c1,
        ctx.java_client(srv.port, "client", name="cli-2") as c2,
        ctx.java_client(srv.port, "client", name="cli-3") as c3,
    ):
        srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0)
        init = dict(srv.initial_stock)

        log_idx = srv.log_count()
        prov.cmd_deliver(2, 2, 2)
        srv.wait_for_log(r"DELIVERY received", timeout=3.0, start_index=log_idx)

        # All 3 clients simultaneously place 1-croissant orders (2s prep each)
        for c in (c1, c2, c3):
            c.cmd_order(0, 0, 1)

        order_ids = []
        for c in (c1, c2, c3):
            acc = c.expect_server_msg("ORDER_ACCEPTED", timeout=3.0)
            order_ids.append(acc["orderId"])
        assert len(set(order_ids)) == 3, f"Duplicate order IDs: {order_ids}"

        for c, oid in zip((c1, c2, c3), order_ids):
            ready = c.expect_server_msg("ORDER_READY", timeout=6.0)
            assert ready["orderId"] == oid

        final_menu = c1.cmd_menu()["items"]
        assert final_menu == {
            "coffee": init["coffee"] + 2,
            "tea": init["tea"] + 2,
            "croissant": init["croissant"] + 2 - 3,
        }, f"Accounting mismatch: {final_menu} vs init={init}"
        for c in (prov, c1, c2, c3):
            c.assert_no_parse_errors()


# ===========================================================================
# SECTION 3: Server CLI, Protocol Validation, Framing & Concurrency Edge Cases
# ===========================================================================


@test
def test_server_cli_invalid_ports_and_sigint(ctx: TestCtx) -> None:
    """Server rejects invalid port arguments and exits 0 cleanly on SIGINT."""
    for bad_args in (["0"], ["65536"], ["-1"], ["abc"], ["8000", "extra"]):
        res = subprocess.run(
            [str(SERVER_BIN), *bad_args],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert res.returncode != 0, f"Expected failure for args {bad_args}"
        assert "Usage:" in res.stderr, f"Missing usage for args {bad_args}: {res.stderr}"

    srv = ctx.server().start()
    try:
        rc = srv.stop(signal.SIGINT, timeout=5.0)
        assert rc == 0, f"Expected exit 0 on SIGINT, got {rc}"
        assert "Server stopped" in srv.dump_logs()
    finally:
        srv.stop()


@test
def test_edge_protocol_envelope_and_server_only_types(ctx: TestCtx) -> None:
    """Verify every malformed JSON, envelope, extra-field, and server-only message type error code."""
    with ctx.server() as srv, RawTCPClient(srv.port) as c:
        cases = [
            ("", "INVALID_MESSAGE"),
            ("   ", "INVALID_MESSAGE"),
            ("{broken", "INVALID_MESSAGE"),
            ("[]", "INVALID_MESSAGE"),
            ("null", "INVALID_MESSAGE"),
            ("42", "INVALID_MESSAGE"),
            ('"hello"', "INVALID_MESSAGE"),
            ("true", "INVALID_MESSAGE"),
            ("{}", "MISSING_FIELD"),
            ('{"type":123}', "INVALID_MESSAGE"),
            ('{"type":null}', "INVALID_MESSAGE"),
            ('{"type":"DOES_NOT_EXIST"}', "INVALID_MESSAGE"),
            ('{"type":"menu_request"}', "INVALID_MESSAGE"),  # case-sensitive enum
            ('{"type":"MENU_REQUEST","extra":1}', "INVALID_MESSAGE"),
            ('{"type":"PROVIDER_IDENTIFY","extra":true}', "INVALID_MESSAGE"),
            ('{"type":"HEARTBEAT","extra":"nope"}', "INVALID_MESSAGE"),
            ('{"type":"ORDER_REQUEST","items":{"coffee":1},"extra":1}', "INVALID_MESSAGE"),
            # Server-only message types sent by client
            (
                '{"type":"MENU_RESPONSE","items":{"coffee":1,"tea":1,"croissant":1}}',
                "INVALID_MESSAGE",
            ),
            ('{"type":"ORDER_ACCEPTED","orderId":1,"preparationTime":4}', "INVALID_MESSAGE"),
            ('{"type":"ORDER_REFUSED","reason":"no"}', "INVALID_MESSAGE"),
            ('{"type":"ORDER_READY","orderId":1}', "INVALID_MESSAGE"),
            ('{"type":"LOW_STOCK","product":"coffee","remaining":2}', "INVALID_MESSAGE"),
            ('{"type":"ERROR","code":"INVALID_MESSAGE","message":"hi"}', "INVALID_MESSAGE"),
        ]
        for raw, expected_code in cases:
            c.send_line(raw)
            resp = c.recv_json()
            assert_error(resp, expected_code, ctx_label=raw)

        # Verify session is still healthy
        c.send_json({"type": "MENU_REQUEST"})
        assert c.recv_json()["type"] == "MENU_RESPONSE"


@test
def test_edge_items_quantities_utf8_and_partial_objects(ctx: TestCtx) -> None:
    """Verify item validation rules, UTF-8/case sensitivity, unauthorized delivery precedence, and partial objects."""
    with ctx.server() as srv:
        # Non-provider sending malformed DELIVERY must still get UNAUTHORIZED_OPERATION first
        with RawTCPClient(srv.port) as client:
            client.send_line('{"type":"DELIVERY","items":{"coffee":0}}')
            assert_error(client.recv_json(), "UNAUTHORIZED_OPERATION", "client bad delivery")

            order_errors = [
                ('{"type":"ORDER_REQUEST"}', "MISSING_FIELD"),
                ('{"type":"ORDER_REQUEST","items":null}', "INVALID_MESSAGE"),
                ('{"type":"ORDER_REQUEST","items":[]}', "INVALID_MESSAGE"),
                ('{"type":"ORDER_REQUEST","items":"coffee"}', "INVALID_MESSAGE"),
                ('{"type":"ORDER_REQUEST","items":{"espresso":1}}', "UNKNOWN_PRODUCT"),
                ('{"type":"ORDER_REQUEST","items":{"Coffee":1}}', "UNKNOWN_PRODUCT"),
                ('{"type":"ORDER_REQUEST","items":{"café":1}}', "UNKNOWN_PRODUCT"),
                ('{"type":"ORDER_REQUEST","items":{"coffee":-1}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{"tea":1.5}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{"croissant":"2"}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{"coffee":true}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{"coffee":null}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{"coffee":2147483648}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{"coffee":99999999999999999999}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{}}', "INVALID_QUANTITY"),
                ('{"type":"ORDER_REQUEST","items":{"coffee":0,"tea":0,"croissant":0}}', "INVALID_QUANTITY"),
            ]
            for raw, code in order_errors:
                client.send_line(raw)
                assert_error(client.recv_json(), code, ctx_label=raw)

            # Partial items object (missing tea & croissant -> 0) must be accepted
            client.send_json({"type": "ORDER_REQUEST", "items": {"croissant": 1}})
            acc = client.recv_json()
            assert acc["type"] == "ORDER_ACCEPTED" and acc["preparationTime"] == CROISSANT_PREP_S, acc

        # Provider delivery validation & partial delivery
        with RawTCPClient(srv.port) as prov:
            log_idx = srv.log_count()
            prov.send_json({"type": "PROVIDER_IDENTIFY"})
            srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0, start_index=log_idx)

            delivery_errors = [
                ('{"type":"DELIVERY"}', "MISSING_FIELD"),
                ('{"type":"DELIVERY","items":{}}', "INVALID_DELIVERY"),
                ('{"type":"DELIVERY","items":{"coffee":0}}', "INVALID_DELIVERY"),
                ('{"type":"DELIVERY","items":{"coffee":-3}}', "INVALID_QUANTITY"),
                ('{"type":"DELIVERY","items":{"muffin":5}}', "UNKNOWN_PRODUCT"),
                ('{"type":"DELIVERY","items":{"coffee":1},"extra":2}', "INVALID_MESSAGE"),
            ]
            for raw, code in delivery_errors:
                prov.send_line(raw)
                assert_error(prov.recv_json(), code, ctx_label=raw)

            before = get_menu_raw(srv.port)
            log_idx = srv.log_count()
            # Partial items object in DELIVERY (only tea specified)
            prov.send_json({"type": "DELIVERY", "items": {"tea": 6}})
            srv.wait_for_log(r"DELIVERY received", timeout=3.0, start_index=log_idx)
            after = get_menu_raw(srv.port)
            assert after == {
                "coffee": before["coffee"],
                "tea": before["tea"] + 6,
                "croissant": before["croissant"],
            }


@test
def test_edge_tcp_framing_partial_coalesced_and_crlf(ctx: TestCtx) -> None:
    """Server correctly handles byte-by-byte partial writes, coalesced messages, and CRLF endings."""
    with ctx.server() as srv, RawTCPClient(srv.port) as c:
        # 1) Partial chunked send
        msg = b'{"type":"MENU_REQUEST"}\n'
        for i in range(0, len(msg), 3):
            c.send_raw(msg[i : i + 3])
            time.sleep(0.01)
        resp = c.recv_json()
        assert resp["type"] == "MENU_RESPONSE", resp
        init = resp["items"]

        # 2) CRLF line ending
        c.send_raw(b'{"type":"MENU_REQUEST"}\r\n')
        assert c.recv_json()["items"] == init

        # 3) Multiple messages coalesced in a single TCP write
        batch = (
            b'{"type":"MENU_REQUEST"}\n'
            b'{"type":"ORDER_REQUEST","items":{"croissant":1}}\n'
            b'{"type":"MENU_REQUEST"}\n'
        )
        c.send_raw(batch)
        r1 = c.recv_json()
        r2 = c.recv_json()
        r3 = c.recv_json()
        assert r1["type"] == "MENU_RESPONSE" and r1["items"] == init
        assert r2["type"] == "ORDER_ACCEPTED" and r2["preparationTime"] == CROISSANT_PREP_S
        assert r3["type"] == "MENU_RESPONSE" and r3["items"]["croissant"] == init["croissant"] - 1


@test
def test_edge_oversized_line_exact_boundary_and_recovery(ctx: TestCtx) -> None:
    """Exactly MAX_LINE_BYTES (8192) succeeds; 8193+ bytes yields INVALID_MESSAGE and cleanly recovers."""
    with ctx.server() as srv, RawTCPClient(srv.port) as c:
        # Case 0: Exact boundary (8192 bytes before '\n') must SUCCEED
        base = b'{"type":"MENU_REQUEST"}'
        exact = (b" " * (MAX_LINE_BYTES - len(base))) + base
        assert len(exact) == MAX_LINE_BYTES
        c.send_raw(exact + b"\n")
        assert c.recv_json()["type"] == "MENU_RESPONSE"

        # Case 1: 8193 bytes before '\n' must FAIL with INVALID_MESSAGE
        over_by_one = (b" " * (MAX_LINE_BYTES + 1 - len(base))) + base
        assert len(over_by_one) == MAX_LINE_BYTES + 1
        c.send_raw(over_by_one + b"\n")
        assert_error(c.recv_json(), "INVALID_MESSAGE", "8193 bytes")

        # Case 2: Oversized line sent in chunks without newline yet, then terminated
        big = b'{"type":"MENU_REQUEST","pad":"' + (b"x" * (MAX_LINE_BYTES + 100)) + b'"}'
        c.send_raw(big[:5000])
        time.sleep(0.05)
        c.send_raw(big[5000:])
        err1 = c.recv_json()
        assert_error(err1, "INVALID_MESSAGE", "oversized chunked")
        assert "maximum allowed length" in err1["message"].lower()

        # Terminate oversized line and immediately follow with valid MENU_REQUEST
        c.send_raw(b'still_oversized\n{"type":"MENU_REQUEST"}\n')
        resp1 = c.recv_json()
        assert resp1["type"] == "MENU_RESPONSE", f"Failed to recover after oversized line: {resp1}"

        # Case 3: Oversized line arriving with newline in one write + valid message right after
        one_shot = (b"A" * (MAX_LINE_BYTES + 50)) + b'\n{"type":"MENU_REQUEST"}\n'
        c.send_raw(one_shot)
        err2 = c.recv_json()
        assert_error(err2, "INVALID_MESSAGE", "oversized one-shot")
        resp2 = c.recv_json()
        assert resp2["type"] == "MENU_RESPONSE", resp2


@test
def test_edge_heartbeat_timeout_modes_and_order_cancellation(ctx: TestCtx) -> None:
    """Server disconnects after ~3s without HEARTBEAT (idle, non-HB traffic, and active order with stock restore)."""
    with ctx.server() as srv:
        init = get_menu_raw(srv.port)
        log_idx = srv.log_count()
        results: dict[str, Any] = {}
        errors: list[BaseException] = []

        def idle_worker() -> None:
            try:
                with RawTCPClient(srv.port, auto_heartbeat=False) as c:
                    results["idle_dt"] = c.expect_closed(timeout=5.5)
            except BaseException as e:
                errors.append(e)

        def non_hb_traffic_worker() -> None:
            try:
                with RawTCPClient(srv.port, auto_heartbeat=False) as c:
                    for _ in range(2):
                        time.sleep(0.7)
                        c.send_json({"type": "MENU_REQUEST"})
                        assert c.recv_json()["type"] == "MENU_RESPONSE"
                    results["non_hb_dt"] = c.expect_closed(timeout=4.0)
            except BaseException as e:
                errors.append(e)

        def active_order_worker() -> None:
            try:
                with RawTCPClient(srv.port, auto_heartbeat=False) as c:
                    c.send_json({"type": "ORDER_REQUEST", "items": {"coffee": 2}})  # 8s prep
                    acc = c.recv_json()
                    assert acc["type"] == "ORDER_ACCEPTED", acc
                    results["order_dt"] = c.expect_closed(timeout=5.5)
            except BaseException as e:
                errors.append(e)

        threads = [
            threading.Thread(target=idle_worker),
            threading.Thread(target=non_hb_traffic_worker),
            threading.Thread(target=active_order_worker),
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=8.0)

        assert not errors, f"Heartbeat timeout subtest errors: {errors}"
        assert 2.5 <= results["idle_dt"] <= 4.5, f"Unexpected idle timeout: {results['idle_dt']:.2f}s"
        assert results["non_hb_dt"] <= 3.0, (
            f"Non-HEARTBEAT traffic should not reset 3s timer; remaining dt={results['non_hb_dt']:.2f}s"
        )
        assert 2.5 <= results["order_dt"] <= 4.5, f"Unexpected order timeout: {results['order_dt']:.2f}s"

        srv.wait_for_log(r"Heartbeat timeout", timeout=2.0, start_index=log_idx)
        srv.wait_for_log(r"ORDER CANCELLED", timeout=2.0, start_index=log_idx)
        assert get_menu_raw(srv.port) == init, "Stock not restored after heartbeat-timeout cancellation"


@test
def test_edge_multiple_orders_same_connection_out_of_order_ready(ctx: TestCtx) -> None:
    """On one TCP connection, a shorter second order finishes (ORDER_READY) before a longer first order."""
    with ctx.server() as srv, RawTCPClient(srv.port) as c:
        c.send_json({"type": "ORDER_REQUEST", "items": {"coffee": 1}})  # 4s
        acc1 = c.recv_json()
        assert acc1["type"] == "ORDER_ACCEPTED" and acc1["preparationTime"] == COFFEE_PREP_S

        c.send_json({"type": "ORDER_REQUEST", "items": {"croissant": 1}})  # 2s
        acc2 = c.recv_json()
        assert acc2["type"] == "ORDER_ACCEPTED" and acc2["preparationTime"] == CROISSANT_PREP_S
        assert acc2["orderId"] > acc1["orderId"]

        ready1 = c.recv_json(timeout=5.0)
        ready2 = c.recv_json(timeout=5.0)
        assert ready1 == {"type": "ORDER_READY", "orderId": acc2["orderId"]}, (
            f"Expected shorter order #{acc2['orderId']} ready first, got {ready1}"
        )
        assert ready2 == {"type": "ORDER_READY", "orderId": acc1["orderId"]}, (
            f"Expected longer order #{acc1['orderId']} ready second, got {ready2}"
        )


@test
def test_edge_multiple_providers_switchover_and_reidentify(ctx: TestCtx) -> None:
    """Re-identifying is safe; a second provider takes over LOW_STOCK alerts and survives first provider disconnect."""
    with ctx.server() as srv:
        init = get_menu_raw(srv.port)
        p1 = RawTCPClient(srv.port)
        p2 = RawTCPClient(srv.port)
        cli = RawTCPClient(srv.port)
        try:
            log_idx = srv.log_count()
            p1.send_json({"type": "PROVIDER_IDENTIFY"})
            p1.send_json({"type": "PROVIDER_IDENTIFY"})
            srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0, start_index=log_idx)

            log_idx = srv.log_count()
            p2.send_json({"type": "PROVIDER_IDENTIFY"})
            srv.wait_for_log(r"Identified as PROVIDER", timeout=3.0, start_index=log_idx)

            log_idx = srv.log_count()
            p1.close()
            srv.wait_for_log(r"Disconnected:", timeout=3.0, start_index=log_idx)

            cli.send_json({"type": "ORDER_REQUEST", "items": {"tea": init["tea"] - 3}})
            assert cli.recv_json()["type"] == "ORDER_ACCEPTED"
            alert = p2.recv_json(timeout=3.0)
            assert alert == {"type": "LOW_STOCK", "product": "tea", "remaining": 3}, alert
        finally:
            p1.close()
            p2.close()
            cli.close()


@test
def test_edge_concurrent_stock_contention_stress(ctx: TestCtx) -> None:
    """Many concurrent clients racing for the exact remaining stock never oversell or duplicate order IDs."""
    with ctx.server() as srv:
        init_croissant = srv.initial_stock["croissant"]
        drain = init_croissant - 8
        with RawTCPClient(srv.port) as drainer:
            drainer.send_json({"type": "ORDER_REQUEST", "items": {"croissant": drain}})
            assert drainer.recv_json()["type"] == "ORDER_ACCEPTED"

            num_threads = 16
            barrier = threading.Barrier(num_threads)
            accepted_ids: list[int] = []
            refused_count = 0
            lock = threading.Lock()
            worker_errors: list[BaseException] = []
            open_clients: list[RawTCPClient] = []

            def racer() -> None:
                c = RawTCPClient(srv.port)
                with lock:
                    open_clients.append(c)
                try:
                    barrier.wait(timeout=5.0)
                    c.send_json({"type": "ORDER_REQUEST", "items": {"croissant": 1}})
                    resp = c.recv_json(timeout=5.0)
                    with lock:
                        if resp["type"] == "ORDER_ACCEPTED":
                            accepted_ids.append(resp["orderId"])
                        elif resp["type"] == "ORDER_REFUSED":
                            nonlocal refused_count
                            refused_count += 1
                        else:
                            raise AssertionError(f"Unexpected response: {resp}")
                except BaseException as e:
                    with lock:
                        worker_errors.append(e)

            threads = [threading.Thread(target=racer) for _ in range(num_threads)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=10.0)

            try:
                assert not worker_errors, f"Racer errors: {worker_errors}"
                assert len(accepted_ids) == 8, f"Expected 8 accepted, got {len(accepted_ids)}"
                assert refused_count == num_threads - 8, (
                    f"Expected {num_threads - 8} refused, got {refused_count}"
                )
                assert len(set(accepted_ids)) == 8, f"Duplicate orderIds: {accepted_ids}"
                remaining = get_menu_raw(srv.port)["croissant"]
                assert remaining == 0, f"Expected 0 croissants remaining, got {remaining}"
            finally:
                for c in open_clients:
                    c.close()


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="Lab 2 Client/Server Integration & Edge-Case Test Suite")
    parser.add_argument("--no-build", action="store_true", help="Skip building C++ server and Java client")
    parser.add_argument("-v", "--verbose", action="store_true", help="Stream server/client stdout live")
    parser.add_argument("-k", "--filter", default="", help="Only run tests whose name contains this substring")
    args = parser.parse_args()

    java_bin, javac_bin = find_jdk27()
    jars = find_jackson_jars()
    classpath = os.pathsep.join([str(CLIENT_CLASSES_DIR), *(str(j) for j in jars)])

    if not args.no_build:
        build_all(java_bin, javac_bin, jars, verbose=args.verbose)
    else:
        assert SERVER_BIN.is_file(), f"Missing {SERVER_BIN}; run without --no-build"
        assert (CLIENT_CLASSES_DIR / "Main.class").is_file(), "Missing compiled client; run without --no-build"

    ctx = TestCtx(java_bin=java_bin, classpath=classpath, verbose=args.verbose)

    selected = [(name, fn) for name, fn in TESTS if args.filter.lower() in name.lower()]
    if not selected:
        print(_c("31", f"No tests matched filter {args.filter!r}"))
        return 1

    print(_c("1", f"Running {len(selected)} test(s)...\n"), flush=True)
    passed = 0
    failed: list[tuple[str, str]] = []
    suite_start = time.monotonic()

    for idx, (name, fn) in enumerate(selected, 1):
        t0 = time.monotonic()
        print(f"[{idx:02d}/{len(selected):02d}] {name} ... ", end="", flush=True)
        try:
            fn(ctx)
            dt = time.monotonic() - t0
            print(f"{_c('32', 'PASS')} ({dt:.2f}s)", flush=True)
            passed += 1
        except BaseException:
            dt = time.monotonic() - t0
            tb = traceback.format_exc()
            print(f"{_c('31', 'FAIL')} ({dt:.2f}s)", flush=True)
            print(_c("31", tb), flush=True)
            failed.append((name, tb))

    total_dt = time.monotonic() - suite_start
    print("\n" + "=" * 72)
    summary = f"Passed: {passed}/{len(selected)} | Failed: {len(failed)} | Time: {total_dt:.2f}s"
    print(_c("32" if not failed else "31", summary))
    if failed:
        print(_c("31", "Failed tests:"))
        for name, _ in failed:
            print(f"  - {name}")
    print("=" * 72)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
