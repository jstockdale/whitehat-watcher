"""USB-serial transport for the Watcher JSON line protocol.

SerialLink wraps pyserial with:
  - connect() that raises a clear, actionable SerialLinkError (no traceback
    wall) when the port is missing or busy,
  - send() with protocol validation,
  - start_reader() spawning a background thread that calls on_event(dict)
    for every valid protocol line and silently drops malformed ones,
  - reconnect() with retries, and close() / context-manager support.

Cross-platform: port names differ (Linux: /dev/ttyUSB0 or /dev/ttyACM0,
macOS: /dev/tty.usbserial-*), but nothing here is OS-specific.
"""

from __future__ import annotations

import threading
import time
from typing import Callable, Optional

import serial
from serial import SerialException
from serial.tools import list_ports

from protocol import COMMANDS, decode_line, encode, validate_command, ProtocolError


class SerialLinkError(Exception):
    """Raised for serial transport problems, with a human-readable message."""


def list_serial_ports():
    """Return [(device, description), ...] for available serial ports."""
    return [(p.device, p.description) for p in list_ports.comports()]


class SerialLink:
    """Newline-delimited JSON link to the Watcher over USB serial."""

    def __init__(self, port: str, baudrate: int = 115200, read_timeout: float = 1.0):
        self.port = port
        self.baudrate = baudrate
        self.read_timeout = read_timeout
        self._ser: Optional[serial.Serial] = None
        self._reader: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._write_lock = threading.Lock()

    # -- lifecycle --------------------------------------------------------

    @property
    def is_open(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def connect(self) -> "SerialLink":
        """Open the serial port. Raises SerialLinkError with guidance."""
        if self.is_open:
            return self
        try:
            # serial_for_url handles plain device paths exactly like Serial,
            # and additionally supports pyserial URLs (loop://, socket://, ...)
            # which is handy for testing without hardware.
            self._ser = serial.serial_for_url(
                self.port, baudrate=self.baudrate, timeout=self.read_timeout
            )
        except SerialException as exc:
            raise SerialLinkError(
                f"Could not open serial port {self.port!r} at {self.baudrate} baud: {exc}\n"
                "Hint: on Linux the Watcher usually shows up as /dev/ttyUSB0 or "
                "/dev/ttyACM0 (you may need to add your user to the 'dialout' group "
                "and re-log); on macOS look for /dev/tty.usbserial-*."
            ) from exc
        try:
            self._ser.reset_input_buffer()  # drop boot-loader chatter
        except SerialException:
            pass
        return self

    def reconnect(self, tries: int = 5, delay: float = 1.0) -> "SerialLink":
        """Close and re-open the port, retrying. Raises SerialLinkError if all fail."""
        last: Optional[Exception] = None
        for _ in range(tries):
            try:
                self.close()
                return self.connect()
            except SerialLinkError as exc:
                last = exc
                time.sleep(delay)
        raise SerialLinkError(f"Reconnect to {self.port!r} failed after {tries} tries: {last}")

    def close(self) -> None:
        self._stop.set()
        if self._reader is not None:
            self._reader.join(timeout=2.0)
            self._reader = None
        if self._ser is not None:
            try:
                self._ser.close()
            except SerialException:
                pass
            self._ser = None

    def __enter__(self) -> "SerialLink":
        return self.connect()

    def __exit__(self, *exc_info) -> None:
        self.close()

    # -- messaging ---------------------------------------------------------

    def send(self, obj: dict) -> None:
        """Validate and send one command dict. Raises SerialLinkError / ProtocolError."""
        if not self.is_open:
            raise SerialLinkError("Not connected: call connect() first.")
        if not isinstance(obj, dict) or "t" not in obj:
            raise ProtocolError("can only send a dict with a 't' field")
        if obj["t"] in COMMANDS:
            validate_command(obj)  # fail fast on bad commands
        data = encode(obj)
        with self._write_lock:
            try:
                self._ser.write(data)
                self._ser.flush()
            except SerialException as exc:
                raise SerialLinkError(f"Serial write to {self.port!r} failed: {exc}") from exc

    def start_reader(
        self,
        on_event: Callable[[dict], None],
        on_error: Optional[Callable[[str], None]] = None,
    ) -> None:
        """Start a daemon thread reading lines; calls on_event(dict) per valid
        protocol line. Malformed lines are ignored. Transport-level read errors
        go to on_error(str) if given, then the thread exits."""
        if not self.is_open:
            raise SerialLinkError("Not connected: call connect() first.")
        if self._reader is not None and self._reader.is_alive():
            raise SerialLinkError("Reader already running.")
        self._stop.clear()

        def _loop() -> None:
            while not self._stop.is_set():
                try:
                    raw = self._ser.readline()
                except (SerialException, OSError) as exc:
                    if not self._stop.is_set() and on_error is not None:
                        try:
                            on_error(f"Serial read error on {self.port!r}: {exc}")
                        except Exception:
                            pass
                    break
                if not raw:
                    continue  # read timeout; loop again
                event = decode_line(raw)
                if event is None:
                    continue  # malformed line: ignore per protocol spec
                try:
                    on_event(event)
                except Exception:
                    # Never let a callback kill the reader thread.
                    pass

        self._reader = threading.Thread(target=_loop, name="watcher-serial-reader", daemon=True)
        self._reader.start()
