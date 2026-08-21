"""Inspect the setup wizard through MSAA, page by page.

Must run elevated. The installer asks for administrator rights, and Windows
blocks a lower-integrity process from reading an elevated window's
accessibility tree, so an unelevated inspector sees nothing at all -- which
looks exactly like an inaccessible wizard.

    python tools/check_installer_a11y.py [path-to-setup.exe]
"""

import ctypes
import ctypes.wintypes as w
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_msaa import (accessible_for, call_bstr, call_variant, role_text,
                        child_count, child_bstr, child_variant_value,
                        SLOT_GET_NAME, SLOT_GET_ROLE, SLOT_GET_VALUE)

user32 = ctypes.WinDLL("user32", use_last_error=True)
ole32 = ctypes.WinDLL("ole32")

GWL_STYLE = -16
WS_TABSTOP = 0x00010000
WS_VISIBLE = 0x10000000
BM_CLICK = 0x00F5


# Found by window class, not by process id: Inno's setup.exe is only a loader
# that extracts the real installer and runs it as a child process, so the pid
# that was launched never owns the wizard window.
def find_window(timeout=30.0):
    found = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, cls, 64)
        if cls.value == "TWizardForm":
            found.append(hwnd)
            return False
        return True

    deadline = time.time() + timeout
    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(cb, 0)
        if found:
            return found[0]
        time.sleep(0.3)
    return None


def window_pid(hwnd):
    pid = w.DWORD()
    user32.GetWindowThreadProcessId(w.HWND(hwnd), ctypes.byref(pid))
    return pid.value


def describe(hwnd):
    cls = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(w.HWND(hwnd), cls, 64)
    style = user32.GetWindowLongW(w.HWND(hwnd), GWL_STYLE)
    acc = accessible_for(hwnd)
    name = call_bstr(acc, SLOT_GET_NAME) if acc else ""
    role = role_text(call_variant(acc, SLOT_GET_ROLE)) if acc else "?"
    value = call_bstr(acc, SLOT_GET_VALUE) if acc else ""
    return {
        "class": cls.value,
        "tab": bool(style & WS_TABSTOP),
        "visible": bool(style & WS_VISIBLE),
        "name": name,
        "role": role,
        "value": value,
        "hwnd": hwnd,
    }


def page_controls(window):
    items = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(hwnd, _):
        info = describe(hwnd)
        if info["visible"] and (info["name"].strip() or info["tab"]):
            items.append(info)
        return True

    user32.EnumChildWindows(w.HWND(window), cb, 0)
    return items


def find_button(window, caption):
    for info in page_controls(window):
        if info["role"] == "push button" and caption.lower() in info["name"].lower():
            return info["hwnd"]
    return None


def main():
    setup = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "output", "EchoGPPC_SAPI5_Setup.exe")

    ole32.CoInitialize(None)
    proc = subprocess.Popen([setup])
    window = find_window()
    if window is None:
        proc.kill()
        raise SystemExit("the setup wizard never appeared (is this running elevated?)")
    wizard_pid = window_pid(window)

    title = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(w.HWND(window), title, 256)
    print("Wizard window: '%s'\n" % title.value)

    problems = []
    seen_pages = 0

    # Walk forward through the wizard, stopping before anything is installed.
    for step in range(7):
        time.sleep(0.6)
        controls = page_controls(window)
        heading = next((c["name"] for c in controls
                        if c["role"] == "text" and c["name"].strip()), "(no heading)")
        print("--- page %d: %s ---" % (step + 1, heading))
        seen_pages += 1
        for info in controls:
            # Non-tab-stop controls are shown too when they are named: only the
            # selected radio button in a group carries WS_TABSTOP, and the rest
            # are reached with the arrow keys -- leaving them out would hide
            # half of a licence page from this report.
            if not info["tab"] and info["role"] not in ("radio button", "check box"):
                continue
            mark = " " if info["tab"] else "."
            line = "   %s%-16s %-32s" % (mark, info["role"], info["name"][:32])
            if info["value"]:
                line += "  [%s]" % info["value"][:36]
            print(line)
            if not info["name"].strip():
                problems.append("page %d has an unnamed %s (%s)"
                                % (step + 1, info["role"], info["class"]))
            # Inno's tasks list is one window holding several items, so its
            # contents only show up by asking MSAA for its children. This is
            # where the desktop-shortcut checkbox actually lives.
            if info["role"] in ("outline", "list"):
                acc = accessible_for(info["hwnd"])
                if acc:
                    for i in range(1, child_count(acc) + 1):
                        item = child_bstr(acc, SLOT_GET_NAME, i)
                        item_role = role_text(child_variant_value(acc, SLOT_GET_ROLE, i))
                        if item.strip():
                            print("        item: %-40s %s" % (item[:40], item_role))
                        else:
                            problems.append("page %d has an unnamed item in its %s"
                                            % (step + 1, info["role"]))
        print()

        # The agreement has to be accepted before Next does anything. Matched on
        # "i accept" rather than "accept", because "I do not accept the
        # agreement" contains the latter and selecting it wedges the wizard.
        for info in controls:
            if info["role"] == "radio button" and info["name"].lower().startswith("i accept"):
                user32.SendMessageW(w.HWND(info["hwnd"]), BM_CLICK, 0, 0)
                time.sleep(0.3)

        nxt = find_button(window, "Next")
        if nxt is None:
            print("(no Next button -- stopping here rather than starting an install)")
            break
        user32.SendMessageW(w.HWND(nxt), BM_CLICK, 0, 0)

    subprocess.run(["taskkill", "/F", "/PID", str(wizard_pid)],
                   capture_output=True)
    proc.kill()
    time.sleep(0.5)

    print()
    if problems:
        for p in problems:
            print("FAIL:", p)
        return 1
    print("%d wizard pages inspected; every focusable control announces a name "
          "and a role." % seen_pages)
    return 0


if __name__ == "__main__":
    sys.exit(main())
