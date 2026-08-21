"""Inspect the configuration dialog the way NVDA sees it.

NVDA drives standard Win32 dialogs through MSAA (oleacc), so that -- not the
managed UI Automation client -- is the interface that decides whether a
control announces itself properly.  For each child window this reports the
window class, whether it is a tab stop, and the MSAA name, role and value.

    python tools/check_msaa.py [path-to-exe]
"""

import ctypes
import ctypes.wintypes as w
import os
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
oleacc = ctypes.WinDLL("oleacc")
ole32 = ctypes.WinDLL("ole32")
oleaut32 = ctypes.WinDLL("oleaut32")

OBJID_CLIENT = 0xFFFFFFFC
CHILDID_SELF = 0
GWL_STYLE = -16
WS_TABSTOP = 0x00010000
WS_DISABLED = 0x08000000
WS_GROUP = 0x00020000


class VARIANT(ctypes.Structure):
    _fields_ = [("vt", ctypes.c_ushort), ("r1", ctypes.c_ushort),
                ("r2", ctypes.c_ushort), ("r3", ctypes.c_ushort),
                ("lVal", ctypes.c_longlong), ("pad", ctypes.c_longlong)]


def self_variant():
    v = VARIANT()
    v.vt = 3            # VT_I4
    v.lVal = CHILDID_SELF
    return v


# IAccessible vtable slots after IUnknown (3) and IDispatch (4).
SLOT_GET_NAME = 10
SLOT_GET_VALUE = 11
SLOT_GET_ROLE = 13
SLOT_GET_STATE = 14


# The "m" form makes ctypes treat the prototype as a COM method: it supplies
# the interface pointer itself, so the declared argument types must cover only
# what comes after `this`.
def call_bstr(acc, slot):
    proto = ctypes.WINFUNCTYPE(ctypes.c_long, VARIANT,
                               ctypes.POINTER(ctypes.c_void_p))
    fn = proto(slot, "m")
    out = ctypes.c_void_p()
    if fn(acc, self_variant(), ctypes.byref(out)) != 0 or not out:
        return ""
    text = ctypes.wstring_at(out)
    oleaut32.SysFreeString(out)
    return text


def call_variant(acc, slot):
    proto = ctypes.WINFUNCTYPE(ctypes.c_long, VARIANT, ctypes.POINTER(VARIANT))
    fn = proto(slot, "m")
    out = VARIANT()
    if fn(acc, self_variant(), ctypes.byref(out)) != 0:
        return None
    return out.lVal


SLOT_GET_CHILD_COUNT = 8


def child_variant(child_id):
    v = VARIANT()
    v.vt = 3
    v.lVal = child_id
    return v


def child_count(acc):
    proto = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.POINTER(ctypes.c_long))
    fn = proto(SLOT_GET_CHILD_COUNT, "m")
    out = ctypes.c_long()
    if fn(acc, ctypes.byref(out)) != 0:
        return 0
    return out.value


def child_bstr(acc, slot, child_id):
    proto = ctypes.WINFUNCTYPE(ctypes.c_long, VARIANT,
                               ctypes.POINTER(ctypes.c_void_p))
    fn = proto(slot, "m")
    out = ctypes.c_void_p()
    if fn(acc, child_variant(child_id), ctypes.byref(out)) != 0 or not out:
        return ""
    text = ctypes.wstring_at(out)
    oleaut32.SysFreeString(out)
    return text


def child_variant_value(acc, slot, child_id):
    proto = ctypes.WINFUNCTYPE(ctypes.c_long, VARIANT, ctypes.POINTER(VARIANT))
    fn = proto(slot, "m")
    out = VARIANT()
    if fn(acc, child_variant(child_id), ctypes.byref(out)) != 0:
        return None
    return out.lVal


def role_text(role):
    if role is None:
        return "?"
    buf = ctypes.create_unicode_buffer(128)
    n = oleacc.GetRoleTextW(ctypes.c_ulong(role), buf, 128)
    return buf.value if n else "role %d" % role


def accessible_for(hwnd):
    # {618736E0-3C3D-11CF-810C-00AA00389B71} in GUID byte order.
    IID_IAccessible = (ctypes.c_byte * 16)(
        *bytes.fromhex("E03687613D3CCF11810C00AA00389B71"))
    acc = ctypes.c_void_p()
    hr = oleacc.AccessibleObjectFromWindow(
        w.HWND(hwnd), ctypes.c_ulong(OBJID_CLIENT),
        ctypes.byref(IID_IAccessible), ctypes.byref(acc))
    return acc if hr == 0 and acc else None


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "output", "EchoGPPCConfig.exe")

    proc = subprocess.Popen([exe])
    time.sleep(1.5)
    if proc.poll() is not None:
        raise SystemExit("the utility exited immediately (code %d)" % proc.returncode)

    ole32.CoInitialize(None)

    # Find the dialog by title.
    target = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def find_top(hwnd, _):
        pid = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == proc.pid and user32.IsWindowVisible(hwnd):
            target.append(hwnd)
            return False
        return True

    user32.EnumWindows(find_top, 0)
    if not target:
        proc.kill()
        raise SystemExit("no visible window found")
    dialog = target[0]

    title = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(w.HWND(dialog), title, 256)
    print("Dialog: '%s'\n" % title.value)
    print("%-6s %-16s %-4s %-22s %-34s %s"
          % ("id", "class", "tab", "MSAA role", "MSAA name", "MSAA value"))
    print("-" * 118)

    rows = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def each_child(hwnd, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(w.HWND(hwnd), cls, 64)
        ctrl_id = user32.GetDlgCtrlID(w.HWND(hwnd))
        style = user32.GetWindowLongW(w.HWND(hwnd), GWL_STYLE)
        tab = "yes" if style & WS_TABSTOP else "-"
        if style & WS_DISABLED:
            tab = "DISABLED"

        acc = accessible_for(hwnd)
        name = role = value = ""
        if acc:
            name = call_bstr(acc, SLOT_GET_NAME)
            value = call_bstr(acc, SLOT_GET_VALUE)
            role = role_text(call_variant(acc, SLOT_GET_ROLE))
        rows.append((ctrl_id, cls.value, tab, role, name, value))
        return True

    user32.EnumChildWindows(w.HWND(dialog), each_child, 0)

    for ctrl_id, cls, tab, role, name, value in rows:
        print("%-6d %-16s %-4s %-22s %-34s %s"
              % (ctrl_id, cls, tab, role, name[:34], value[:30]))

    # Tab order, asked of Windows rather than simulated with keystrokes.
    # GetNextDlgTabItem is the very function the dialog manager uses when Tab
    # is pressed, so walking it gives the real order without stealing focus.
    print("\nTab order (via GetNextDlgTabItem):\n")
    order = []
    first = user32.GetNextDlgTabItem(w.HWND(dialog), None, False)
    current = first
    while current:
        ctrl_id = user32.GetDlgCtrlID(w.HWND(current))
        acc = accessible_for(current)
        name = call_bstr(acc, SLOT_GET_NAME) if acc else ""
        role = role_text(call_variant(acc, SLOT_GET_ROLE)) if acc else "?"
        value = call_bstr(acc, SLOT_GET_VALUE) if acc else ""
        announced = "%s  %s" % (name, role)
        if value:
            announced += "  %s" % value
        print("%2d. %s" % (len(order) + 1, announced))
        order.append((ctrl_id, name, role))
        current = user32.GetNextDlgTabItem(w.HWND(dialog), w.HWND(current), False)
        if current == first:
            break

    proc.kill()

    problems = []
    if len(order) != sum(1 for r in rows if r[2] == "yes"):
        problems.append("the tab order visits %d controls but %d are marked as "
                        "tab stops" % (len(order), sum(1 for r in rows if r[2] == "yes")))
    for ctrl_id, name, role in order:
        if not name.strip():
            problems.append("tab stop %d announces no name" % ctrl_id)
    for ctrl_id, cls, tab, role, name, value in rows:
        if tab == "DISABLED":
            problems.append("control %d is disabled, so Tab skips it" % ctrl_id)
        if tab == "yes" and not name.strip():
            problems.append("control %d is a tab stop with no accessible name" % ctrl_id)

    print()
    if problems:
        for p in problems:
            print("FAIL:", p)
        return 1
    print("%d tab stops, every one named, none disabled, "
          "and the tab order visits them all." % len(order))
    return 0


if __name__ == "__main__":
    sys.exit(main())
