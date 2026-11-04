#!/usr/bin/env python3
"""Optional real VTE resize regression (GTK 3, VTE >= 0.80, PyGObject).

Build tests/oca-fixture, then run this script with an X11/Wayland display
(or xvfb-run). No real provider credentials or external services are used; the
HTTP fixture binds only to loopback. --cols/--rows set the exhaustive grid.
"""

import argparse
import json
import os
from pathlib import Path
import signal
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Vte", "2.91")
from gi.repository import Gdk, GLib, Gtk, Pango, Vte  # noqa: E402


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert self.headers["Authorization"] == "Bearer fixture-api-key"
        messages = [m for m in request["messages"] if m["role"] != "system"]
        last = messages[-1]
        prompt = last.get("content", "")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def delta(data, finish=None):
            choice = {"index": 0, "delta": data}
            if finish:
                choice["finish_reason"] = finish
            self.wfile.write(("data: " + json.dumps({"choices": [choice]}) + "\n\n").encode())
            self.wfile.flush()

        try:
            delta({"role": "assistant"})
            if last["role"] == "tool":
                delta({"content": "Answer recorded."}, "stop")
            elif prompt == "resize-question":
                args = {"questions": [{"question": "Which color?", "options": ["red", "blue"]}]}
                delta({"tool_calls": [{"index": 0, "id": "resize-question-1", "type": "function",
                       "function": {"name": "ask_user", "arguments": json.dumps(args)}}]}, "tool_calls")
            elif prompt == "resize-stream":
                for i in range(80):
                    delta({"content": f"stream-{i:03d} Unicode 界 e\u0301\n"})
                    time.sleep(0.01)
                delta({}, "stop")
            else:
                delta({"content": "Received: " + prompt}, "stop")
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def pump(seconds=0.005):
    end = time.monotonic() + seconds
    while True:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        if time.monotonic() >= end:
            return
        time.sleep(0.001)


class Session:
    def __init__(self, executable, url, directory):
        self.window = Gtk.OffscreenWindow()
        self.terminal = Vte.Terminal()
        self.terminal.set_font(Pango.FontDescription("Monospace 10"))
        self.terminal.set_scrollback_lines(10000)
        self.window.add(self.terminal)
        self.window.show_all()
        pump()
        self.resize(24, 80)
        self.terminal.feed(b"SHELL-SENTINEL\r\n")
        self.wait(lambda: "SHELL-SENTINEL" in self.text(), "initial shell screen")
        self.primary_before = self.text()
        home, work = directory / "home", directory / "work"
        (home / ".oca").mkdir(parents=True)
        work.mkdir()
        (home / ".oca" / "config.json").write_text(json.dumps({
            "last_used": {"model": "kimi-k3", "reasoning": "low"}}))
        env = dict(os.environ, HOME=str(home), TERM="xterm-256color", LC_ALL="C.UTF-8",
                   KIMI_API_KEY="fixture-api-key", OCA_TEST_PROVIDER_URL=url)
        self.exited = False
        self.terminal.connect("child-exited", self.child_exited)
        _ok, self.pid = self.terminal.spawn_sync(
            Vte.PtyFlags.DEFAULT, str(work), [str(executable)],
            [key + "=" + value for key, value in env.items()],
            GLib.SpawnFlags.DEFAULT, None, None, None)
        self.wait(lambda: "Ask OCA anything" in self.text(), "startup")

    def child_exited(self, _terminal, status):
        self.exited = True
        self.exit_status = status

    def text(self):
        return self.terminal.get_text_format(Vte.Format.TEXT) or ""

    def rows(self):
        # OCA redraws with hard line breaks and autowrap disabled. VTE's
        # absolute cursor row can retain an internal offset even in alt mode.
        rows = self.text().splitlines()
        return (rows + [""] * self.terminal.get_row_count())[:self.terminal.get_row_count()]

    def resize(self, rows, cols):
        terminal = self.terminal
        cols = max(2, cols)  # VTE enforces a two-column minimum.
        width, height = cols * terminal.get_char_width() + 2, rows * terminal.get_char_height() + 2
        terminal.set_size_request(width, height)
        self.window.resize(width, height)
        # Let GTK perform the allocation once. Calling set_size as well would
        # introduce an extra resize before the window allocation reaches VTE.
        self.wait(lambda: (terminal.get_row_count(), terminal.get_column_count()) == (rows, cols),
                  f"terminal allocation {cols}x{rows}")

    def send(self, data):
        self.terminal.feed_child(data.encode())
        pump()

    def scroll(self, direction, smooth=False):
        event = Gdk.Event.new(Gdk.EventType.SCROLL)
        event.window = self.terminal.get_window()
        event.x, event.y = 10.0, 10.0
        event.direction = Gdk.ScrollDirection.SMOOTH if smooth else direction
        if smooth:
            event.delta_x = 0.0
            event.delta_y = -1.0 if direction == Gdk.ScrollDirection.UP else 1.0
        event.set_device(Gdk.Display.get_default().get_default_seat().get_pointer())
        self.terminal.event(event)
        pump(.04)

    def paste(self, text):
        self.send("\x1b[200~" + text + "\x1b[201~")

    def wait(self, predicate, label, seconds=3):
        end = time.monotonic() + seconds
        while not predicate():
            if time.monotonic() >= end:
                raise AssertionError(f"{label}: {self.terminal.get_cursor_position()}\n{self.text()}")
            pump()

    def close(self):
        if not self.exited:
            os.kill(self.pid, signal.SIGTERM)
            self.wait(lambda: self.exited, "child exit")
        self.window.destroy()
        pump()


def composer_matches(session, rows, cols):
    lines = session.rows()
    border = rows >= 4 and cols >= 8
    row = 1 if border else 0
    content_cols = cols - (4 if border else 0)
    col = min(2, content_cols - 1) + (2 if border else 0)
    cursor = session.terminal.get_cursor_position()
    if cursor.column != col:
        return False
    cursor_line = session.terminal.get_text_range_format(
        Vte.Format.TEXT, cursor.row, 0, cursor.row, cols)[0]
    if len(cursor_line) <= col or cursor_line[col] != "X":
        return False
    if len(lines[row]) <= col or lines[row][col] != "X":
        return False
    if border and (lines[0] != "╭" + "─" * (cols - 2) + "╮" or
                   lines[-2] != "╰" + "─" * (cols - 2) + "╯"):
        return False
    return rows < 2 or cols < 10 or "kimi-k3" in lines[-1]


def exercise(session, max_rows, max_cols):
    for rows, cols in [(48, 120), (12, 50), (2, 8), (24, 80)]:
        session.resize(rows, cols)
        if rows >= 12:
            session.wait(lambda: "OCA" in session.rows()[1], "welcome banner at top")
    # A tall draft fills the viewport at every grid point. The cursor remains
    # at the first logical byte, making geometry assertions independent of
    # history height and of whether the editor's remaining lines are clipped.
    session.paste("X\n" + "Y\n" * (max_rows + 5) + "draft-last 界 e\u0301")
    session.send("\x1b[H")
    count = 0
    for rows in range(1, max_rows + 1):
        columns = range(2, max_cols + 1) if rows % 2 else range(max_cols, 1, -1)
        for cols in columns:
            session.resize(rows, cols)
            session.wait(lambda: composer_matches(session, rows, cols), f"composer {cols}x{rows}")
            count += 1
    for rows, cols in [(2, 320), (100, 3), (24, 80), (60, 240), (1, 1), (24, 80)]:
        session.resize(rows, cols)
    session.send("\x1b[F")
    session.wait(lambda: "draft-last 界 e\u0301" in session.text(), "draft restoration")
    session.send("\x03/")
    session.wait(lambda: "/help" in session.text(), "command menu")
    for rows, cols in [(1, 1), (2, 7), (4, 8), (6, 20), (10, 40), (24, 80)]:
        session.resize(rows, cols)
    session.wait(lambda: "/help" in session.text(), "menu restoration")
    session.send("\x1b")
    pump(.06)
    session.send("resize-stream\r")
    session.wait(lambda: "stream-000" in session.text(), "stream start")
    for i in range(100):
        session.resize(1 + (i * 7) % 48, 1 + (i * 13) % 160)
    session.resize(24, 80)
    session.wait(lambda: "stream-079" in session.text(), "stream completion")
    session.send("scroll-draft")
    for smooth in (False, True):
        for _ in range(8):
            session.scroll(Gdk.ScrollDirection.UP, smooth)
        session.wait(lambda: "stream-079" not in session.text(), "wheel scroll up")
        assert "scroll-draft" in session.text(), "wheel must preserve the draft"
        for _ in range(12):
            session.scroll(Gdk.ScrollDirection.DOWN, smooth)
        session.wait(lambda: "stream-079" in session.text(), "wheel scroll down")
        assert "scroll-draft" in session.text(), "wheel must preserve the draft"
    session.send("\x1b[A")
    session.wait(lambda: "scroll-draft" not in session.text(), "arrow recalls prompt")
    session.send("\x03")
    session.terminal.select_all()
    assert "stream-079" in session.terminal.get_text_selected(Vte.Format.TEXT)
    session.terminal.unselect_all()
    session.send("\x1b[5~")
    pump(.08)
    assert "stream-079" not in session.text(), "history must scroll away from tail"
    session.resize(12, 32)
    session.resize(24, 80)
    session.send("\x1b[6~" * 12)
    session.wait(lambda: "stream-079" in session.text(), "history tail")
    session.send("resize-question\r")
    session.wait(lambda: "Which color?" in session.text(), "question")
    for rows, cols in [(1, 1), (2, 3), (5, 7), (6, 8), (7, 12), (12, 40), (24, 80)]:
        session.resize(rows, cols)
    session.wait(lambda: "Which color?" in session.text(), "question restoration")
    session.send("\t")
    session.paste("detail-first\n" + "界 e\u0301 " * 20 + "\ndetail-last")
    for rows, cols in [(1, 1), (2, 8), (4, 20), (8, 32), (24, 80)]:
        session.resize(rows, cols)
    session.wait(lambda: "detail-last" in session.text(), "question details")
    session.send("\r")
    session.wait(lambda: "Answer recorded." in session.text(), "question answer")
    session.send("/btw\r")
    session.wait(lambda: "Temporary chat" in session.text(), "temporary view")
    session.paste("temporary draft 界")
    for rows, cols in [(1, 1), (2, 8), (6, 20), (24, 80)]:
        session.resize(rows, cols)
    session.wait(lambda: "temporary draft 界" in session.text(), "temporary draft")
    session.send("\x03\x03")
    session.wait(lambda: "Answer recorded." in session.text(), "return to main")
    session.send("/quit\r")
    session.wait(lambda: "Save" in session.text(), "save question")
    session.send("n\r")
    session.wait(lambda: session.exited, "normal exit")
    assert session.exit_status == 0
    assert session.text() == session.primary_before, (
        f"shell screen restoration: before={session.primary_before!r}, after={session.text()!r}")
    print(f"VTE: {count} grid sizes, abrupt/rapid resizes, menus, wheel/touchpad, streaming, questions, history, temporary chat, shell restoration passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", default="tests/oca-fixture", type=Path)
    parser.add_argument("--cols", type=int, default=80)
    parser.add_argument("--rows", type=int, default=30)
    args = parser.parse_args()
    assert 2 <= args.cols <= 320 and 1 <= args.rows <= 100
    server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="oca-vte-test-") as directory:
            session = Session(args.executable.resolve(), f"http://127.0.0.1:{server.server_port}/", Path(directory))
            try:
                exercise(session, args.rows, args.cols)
            finally:
                session.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
