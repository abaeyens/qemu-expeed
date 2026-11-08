#!/usr/bin/env python3
# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the emulated camera into shooting mode, and optionally hand it over.
"""
import argparse
import atexit
import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import exmcu_replies  # noqa: E402

QEMU = os.environ.get("QEMU", "/opt/qemu-9.1.0/build/qemu-system-arm")

# the battery report (Ex MCU command 4)
BATT_SUBTYPE, BATT_PLEN = 3, 0x70
BATT_FIELDS = ["0x04=00", "0x0a=00", "0x12=68", "0x14=1e", "0x18=ad1b", "0x24=1e", "0x25=04"]

IDLE = 0x10107074  # the shared T-Kernel idle PC the hijack lands on
IDLE_REENTER = 0x10107060  # where a returned core re-enters the idle loop (ARM)
CODE = 0xD0000000  # payload, above the RTOS's memory so its allocators cannot reach it
DATA_OFF = 0x300  # the payload's data; the landing trap sits 0x40 below
LANDPC = CODE + DATA_OFF - 0x40
DEVICE_STATE_MACHINE = 0x10204BDA
DEVICE_STATE_OBJ = 0xC1818BE0

# gdb reads and writes memory through the selected thread's CPU, and with the slave Expeed in the machine
# (Z6II_SLAVE) some threads are slave cores, with the slave's own address space. A thread is a master core when it
# sees the master's idle loop (the slave's image differs there).
MASTER_GDB = """
import struct as _struct


def _is_master():
    try:
        w = gdb.selected_inferior().read_memory(0x10107070, 8).tobytes()
    except gdb.MemoryError:
        return False
    return _struct.unpack("<2I", w) == (0xE321F093, 0xE5958000)


def _select_master():
    for _inf in gdb.inferiors():
        for _th in sorted(_inf.threads(), key=lambda t: t.num):
            _th.switch()
            if _is_master():
                return True
    print("NO-MASTER-THREAD")
    return False


def _exec_on_master(cmd):
    # a memory access at a pause can fail on the core that stopped: try each master core
    err = None
    for _inf in gdb.inferiors():
        for _th in sorted(_inf.threads(), key=lambda t: t.num):
            _th.switch()
            if not _is_master():
                continue
            try:
                gdb.execute(cmd)
                return
            except gdb.MemoryError as e:
                err = e
    raise err
"""

GDB_HEAD = r"""
import gdb, os, time
gdb.execute("set confirm off"); gdb.execute("set pagination off")
gdb.execute("handle SIGINT stop print nopass")
gdb.execute("set python print-stack full")
gdb.execute("target remote %(target)s")
_ev = {"bps": ()}


def _on_stop(ev):
    _ev["bps"] = tuple(ev.breakpoints) if isinstance(ev, gdb.BreakpointEvent) else ()


gdb.events.stop.connect(_on_stop)


def _bp(addr):
    return gdb.Breakpoint("*%%#x" %% addr, gdb.BP_HARDWARE_BREAKPOINT)


def _setreg(n, v):
    # a raw P packet: "set $lr" makes gdb re-unwind the frame, which can fail with a MemoryError
    gdb.execute("maint packet P%%x=%%s" %% (n, (v & 0xFFFFFFFF).to_bytes(4, "little").hex()),
                to_string=True)
    gdb.execute("maint flush register-cache")


def go():
    # continue; the breakpoints that stopped it, or () at a EMU_PAUSE_AT pause
    _ev["bps"] = ()
    gdb.execute("continue")
    return _ev["bps"]


def run_to_pause():
    while go():
        pass


def payload(path, t):
    b = _bp(IDLE)
    while True:
        if go() != (b,):
            print("VTIME-ERROR a pause or a breakpoint came before a CPU reached the idle loop")
            raise SystemExit
        if _is_master():  # a slave core at the same address is not ours
            break
    b.delete()
    # on the master core that stopped: its memory, not the slave's
    gdb.execute("restore %%s binary %%#x" %% (path, CODE))
    _setreg(14, LANDPC | 1)
    _setreg(0x19, (int(gdb.parse_and_eval("$cpsr")) & 0xFFFFFFFF) | 0x20)
    _setreg(15, CODE)
    land = _bp(LANDPC)
    while True:
        hit = go()
        if not hit:
            print("VTIME-ERROR a pause came while the payload at t=%%gs ran" %% t)
            raise SystemExit
        if land in hit:
            break
    land.delete()
    # back to the scheduler through the Thumb return stub next to the landing trap
    _setreg(15, LANDPC + 4)
"""


def gdb_script(target, events):
    """The whole timeline as one gdb session that stays attached from virtual time 0.

    events: (t, kind, arg), sorted; kinds "pause", "call" (arg: (payload path, label)), "poke" (arg: gdb
    commands) and "end" (arg: the file the driver creates once it is done with the held machine)."""
    lines = [GDB_HEAD % {"target": target}, MASTER_GDB]
    lines.append("CODE, IDLE, LANDPC = %#x, %#x, %#x" % (CODE, IDLE, LANDPC))
    for t, kind, arg in events:
        if kind == "pause":
            lines.append("run_to_pause()")
            lines.append("print('VTIME-AT %g')" % t)
        elif kind == "call":
            path, label = arg
            lines.append("payload(%r, %g)" % (path, t))
            lines.append("print('DRIVE-DONE %g %s')" % (t, label))
        elif kind == "poke":
            lines += ["_select_master()"] + ["_exec_on_master(%r)" % c for c in arg]
        elif kind == "end":
            lines += [
                "print('VTIME-END', flush=True)",
                "while not os.path.exists(%r):" % arg,
                "    time.sleep(0.2)",
            ]
    return "\n".join(lines) + "\n"


def timeline(a, main_payload, late, tmpd):
    """The run's events, in order, and the virtual times (s) the machine pauses at (EMU_PAUSE_AT)."""
    groups = {a.call_at: [("call", (main_payload, "main"))]}
    for k, (t_late, lcalls) in enumerate(late):
        lp = os.path.join(tmpd, "late%d.bin" % k)
        open(lp, "wb").write(build_payload(lcalls))
        groups.setdefault(t_late, []).append(("call", (lp, "late %d" % k)))
    start = [
        "restore %s binary %#x"
        % (os.path.abspath(sp.split(":", 1)[1]), int(sp.split(":", 1)[0], 0))
        for sp in a.load_before
    ]
    if start:
        groups.setdefault(a.poke_before_at, []).insert(
            0, ("poke", start + ["echo POKED-BEFORE\\n"])
        )
    for sp in a.snap:
        t, addr, size, path = sp.split(":", 3)
        addr, size = int(addr, 0), int(size, 0)
        dump = "dump binary memory %s %#x %#x" % (os.path.abspath(path), addr, addr + size)
        groups.setdefault(float(t), []).append(("poke", [dump]))
    t_end = a.call_at + a.settle
    groups.setdefault(t_end, []).append(("end", os.path.join(tmpd, "vtime_done")))
    events = []
    for t in sorted(groups):
        events.append((t, "pause", None))
        events += [(t, kind, arg) for kind, arg in groups[t]]
    return events, sorted(groups)


def qmp(f, cmd, **args):
    f.write((json.dumps({"execute": cmd, **({"arguments": args} if args else {})}) + "\n").encode())
    f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r


def assemble(asm):
    """Thumb-2 assembly -> machine code, with the GNU assembler the DOOM build uses."""
    with tempfile.TemporaryDirectory() as d:
        obj, out = os.path.join(d, "stub.o"), os.path.join(d, "stub.bin")
        src = ".syntax unified\n.thumb\n" + asm
        subprocess.run(
            ["arm-none-eabi-as", "-mcpu=cortex-a7", "-o", obj], input=src, text=True, check=True
        )
        subprocess.run(
            ["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", obj, out], check=True
        )
        return open(out, "rb").read()


def build_payload(calls, data=struct.pack("<III", 1, 1, 2)):
    """A Thumb payload: each call with up to four register arguments, then a busy delay so the workers it
    woke actually run before the next one. `data` is the class-1 power event's payload {[0]=1,[4]=1,[8]=2}.
    """
    land_off = DATA_OFF - 0x40
    asm = "push {r4, lr}\n"
    for k, (fn, args) in enumerate(calls):
        if fn == "delay":
            asm += (
                f"movw r0, #{args & 0xFFFF}\nmovt r0, #{(args >> 16) & 0xFFFF}\n"
                f"d{k}: subs r0, #1\nbne d{k}\n"
            )
            continue
        asm += "".join(
            f"movw r{i}, #{a & 0xFFFF}\nmovt r{i}, #{(a >> 16) & 0xFFFF}\n"
            for i, a in enumerate(args)
        )
        asm += f"movw r4, #{(fn | 1) & 0xFFFF}\nmovt r4, #{((fn | 1) >> 16) & 0xFFFF}\nblx r4\n"
    asm += "pop {r4, pc}\n"
    code = assemble(asm)
    if len(code) > land_off:
        raise SystemExit(f"drive_poweron: payload is {len(code)} bytes, over {land_off:#x}")
    blob = bytearray(0x400)
    blob[: len(code)] = bytes(code)
    blob[land_off : land_off + 2] = b"\xfe\xe7"  # b . -- the landing trap
    # The way back to the scheduler, entered by ONE $pc write while the core is still in Thumb:
    # ldr r0, [pc, #0]; bx r0; .word IDLE_REENTER -- `bx` to the even ARM address switches state itself.
    blob[land_off + 4 : land_off + 8] = b"\x00\x48\x00\x47"
    blob[land_off + 8 : land_off + 12] = struct.pack("<I", IDLE_REENTER)
    blob[DATA_OFF : DATA_OFF + len(data)] = data
    return bytes(blob)


def make_frame(path, cmd, subtype, plen, fields):
    """An Ex MCU frame (class 2) for the machine's push schedule (Z6II_EXMCU_PUSH)."""
    with open(path, "wb") as f:
        f.write(exmcu_replies.block(cmd, 2, plen, subtype, fields))
    return path


def parse_call(spec):
    """FN[,r0[,r1[,r2[,r3]]]] -> (fn, [r0, r1, r2, r3])."""
    v = [int(x, 0) for x in spec.split(",")]
    if len(v) > 5:
        raise SystemExit(f"--late-call {spec}: at most four arguments")
    return v[0], (v[1:] + [0, 0, 0, 0])[:4]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--vtime",
        type=int,
        metavar="SHIFT",
        default=3,
        help="-icount shift: the guest's cores share 2^-SHIFT instructions per virtual ns (default 3)",
    )
    ap.add_argument("--push-at", type=float, default=2.0, help="virtual s of the battery report")
    ap.add_argument("--call-at", type=float, default=10.0, help="virtual s of the power-on")
    ap.add_argument("--settle", type=float, default=25.0, help="virtual s to run after --call-at")
    ap.add_argument(
        "--late-call",
        action="append",
        default=[],
        metavar="T:FN[,r0[,r1[,r2[,r3]]]]",
        help="call FN at virtual second T in its own payload; repeatable (calls with the same T share one "
        "payload and run in order)",
    )
    ap.add_argument(
        "--load-before",
        action="append",
        default=[],
        metavar="ADDR:FILE",
        help="write FILE's bytes into guest RAM at --poke-before-at; repeatable",
    )
    ap.add_argument(
        "--poke-before-at",
        type=float,
        default=1.0,
        help="virtual s of --load-before (default 1, after the boot has initialised its data)",
    )
    ap.add_argument(
        "--snap",
        action="append",
        default=[],
        metavar="T:ADDR:SIZE:PATH",
        help="write SIZE bytes of guest memory at ADDR to PATH at virtual second T; repeatable",
    )
    ap.add_argument(
        "--exmcu-frame",
        action="append",
        default=[],
        metavar="CMD:SUB:PLEN:T[:OFF=HEX[+OFF=HEX...]]",
        help="push one more Ex MCU frame (class 2) at T (START+PERIOD: from START on, every PERIOD "
        "seconds); repeatable",
    )
    ap.add_argument(
        "--status-field",
        action="append",
        default=[],
        metavar="OFF=HEX",
        help="also set this payload field in every status frame pushed (command 2 or 8, subtype 1) that "
        "does not set that offset itself; repeatable",
    )
    ap.add_argument(
        "--interactive",
        metavar="gtk|vnc=:N",
        help="open the display (a GTK window, or VNC display :N) and, at the end of --settle, hand the "
        "running machine over instead of quitting: the window's keys are the camera's buttons (F1 lists "
        "them). Ends when QEMU does",
    )
    ap.add_argument("--pid-file", help="write QEMU's PID here while it runs (removed at the end)")
    ap.add_argument(
        "--shot-dir",
        default=".",
        help="with --interactive: where Enter in the terminal saves a screenshot of the panel, "
        "shot_NNN.png, numbered on from the ones already there (default: the current directory)",
    )
    a = ap.parse_args()

    tmpd = tempfile.mkdtemp(prefix="drivepwr_")
    atexit.register(shutil.rmtree, tmpd, True)

    batt = make_frame(os.path.join(tmpd, "batt.bin"), 4, BATT_SUBTYPE, BATT_PLEN, BATT_FIELDS)

    main_payload = os.path.join(tmpd, "payload.bin")
    open(main_payload, "wb").write(
        build_payload(
            [(DEVICE_STATE_MACHINE, [DEVICE_STATE_OBJ, 3, CODE + DATA_OFF, 0]), ("delay", 0x200000)]
        )
    )
    late = []  # (time, [(fn, args)]), one payload per time
    for spec in a.late_call:
        t, _, rest = spec.partition(":")
        same = [c for tt, c in late if tt == float(t)]
        if same:
            same[0].append(parse_call(rest))
        else:
            late.append((float(t), [parse_call(rest)]))
    late.sort(key=lambda x: x[0])

    pushes = []
    # a few ordinary status frames after the power-on, as the GUI's display-target request waits on a status
    # change: each frame a genuine change (the master signals the status observer only when something moved),
    # the mode-dial byte, payload +0x0f, toggling
    for i, dial in enumerate(("01", "00", "01", "00", "01", "00")):
        f = make_frame(
            os.path.join(tmpd, "status%d.bin" % i),
            8,
            1,
            0x7C,
            ["0x0f=" + dial] + a.status_field,
        )
        pushes.append("%s@%g" % (f, max(a.call_at, a.push_at) + 18 + i * 12))
    for k, spec in enumerate(a.exmcu_frame):
        cmd, sub, plen, at, *rest = spec.split(":")
        cmd, sub, plen = int(cmd, 0), int(sub, 0), int(plen, 0)
        fields = rest[0].split("+") if rest and rest[0] else []
        if cmd in (2, 8) and sub == 1:
            # a status frame: --status-field holds in it too, unless the frame sets that offset itself
            own = {int(x.split("=")[0], 0) for x in fields}
            fields += [x for x in a.status_field if int(x.split("=")[0], 0) not in own]
        f = make_frame(os.path.join(tmpd, "extra%d.bin" % k), cmd, sub, plen, fields)
        pushes.append("%s@%s" % (f, at))
    pushes.append(f"{batt}@{a.push_at:g}")

    events, pauses = timeline(a, main_payload, late, tmpd)
    sock = os.path.join(tmpd, "qmp")
    env = dict(
        os.environ,
        Z6II_FLASH=os.environ.get("Z6II_FLASH", "out/flash.img"),
        Z6II_EXMCU_REPLY_DIR=os.environ.get("Z6II_EXMCU_REPLY_DIR", "out/exmcu"),
        Z6II_EXMCU_PUSH=";".join(pushes),
        EMU_PAUSE_AT=",".join(str(int(round(t * 1000))) for t in pauses),
    )
    # the machine carries on without these, and then never reaches live view: refuse instead
    missing = {
        "Z6II_FLASH": "python3 scripts/make_flash_image.py",
        "Z6II_EXMCU_REPLY_DIR": "python3 scripts/emu/exmcu_replies.py",
    }
    for key, how in missing.items():
        if not os.path.exists(env[key]):
            sys.exit(f"{key}={env[key]} does not exist; build it with: {how}")

    # gdb on a private unix socket: a TCP port can collide with another QEMU on the host
    gdb_dir = tempfile.mkdtemp(
        prefix="nkgdb", dir="/tmp"
    )  # /tmp: a socket path is at most 107 bytes
    atexit.register(shutil.rmtree, gdb_dir, True)
    gdb_target = os.path.join(gdb_dir, "gdb")
    cmd = [QEMU, "-M", "z6ii", "-display", "none", "-serial", "null"]
    cmd += ["-qmp", f"unix:{sock},server,nowait", "-gdb", f"unix:{gdb_target},server=on,wait=off"]
    cmd += ["-S", "-icount", f"shift={a.vtime},sleep=off"]
    if a.interactive == "gtk":
        cmd += ["-display", "gtk"]  # overrides the -display none above
    elif a.interactive and a.interactive.startswith("vnc="):
        cmd += ["-vnc", a.interactive[4:]]
    elif a.interactive:
        sys.exit(f"--interactive {a.interactive}: want gtk or vnc=:N")

    p = subprocess.Popen(
        cmd,
        env=env,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        # QEMU's stderr is ours: why it failed to start and, interactive, one line per key and the F1 help
    )
    if a.pid_file:
        with open(a.pid_file, "w") as f:
            f.write(f"{p.pid}\n")

        def _drop_pid_file(path=a.pid_file, pid=p.pid):
            try:
                if open(path).read().strip() == str(pid):
                    os.remove(path)
            except OSError:
                pass

        atexit.register(_drop_pid_file)
    try:
        t_start = time.time()
        while not os.path.exists(sock):
            if p.poll() is not None:
                sys.exit(f"QEMU exited with code {p.returncode} before it started")
            if time.time() - t_start > 60:
                sys.exit("QEMU did not open its QMP socket within 60 s")
            time.sleep(0.05)
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock)
        s.settimeout(900)
        f = s.makefile("rwb")
        f.readline()
        qmp(f, "qmp_capabilities")
        drive(p, gdb_target, events, tmpd, a.call_at + a.settle)
        if a.interactive:
            qmp(f, "cont")  # the run is held at the end of the settle
            red, off = ("", "") if os.environ.get("NO_COLOR") else ("\033[1;91m", "\033[0m")
            print(
                f"  {red}boot completed ! "
                f"Press F1 in the emulator window to list the keyboard keys. "
                f"Close the window to end.{off}"
            )
            print(f"  Enter in this terminal saves a screenshot to {a.shot_dir}/shot_NNN.png")
            sys.stdout.flush()
            threading.Thread(target=shots_on_enter, args=(f, a.shot_dir), daemon=True).start()
            p.wait()
        else:
            qmp(f, "quit")
        try:
            p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass
    finally:
        p.kill()
        p.wait()
    return 0


def shots_on_enter(f, shot_dir):
    """Save the panel as shot_NNN.png in shot_dir on every line read from the terminal, until EOF.
    The machine keeps running: a screendump is the frame shown when the request arrives."""
    os.makedirs(shot_dir, exist_ok=True)
    taken = [int(n[5:-4]) for n in os.listdir(shot_dir) if re.fullmatch(r"shot_\d+\.png", n)]
    n = max(taken, default=0)
    for _ in sys.stdin:
        n += 1
        path = os.path.abspath(os.path.join(shot_dir, "shot_%03d.png" % n))
        r = qmp(f, "screendump", filename=path, format="png")
        print("  screenshot -> %s%s" % (path, (" FAILED: %s" % r["error"]) if "error" in r else ""))
        sys.stdout.flush()


def exit_status(proc):
    """How a child process ended, in words (a crash by signal leaves no message of its own)."""
    rc = proc.poll()
    if rc is None:
        return "still running"
    if rc < 0:
        return "killed by " + signal.Signals(-rc).name
    return f"exited with code {rc}"


def drive(qemu, gdb_target, events, tmpd, t_end, timeout=7200):
    """Run the timeline and return once the machine is held at its final pause."""
    script = os.path.join(tmpd, "vtime.py")
    open(script, "w").write(gdb_script(gdb_target, events))
    log = os.path.join(tmpd, "vtime_gdb.txt")
    gp = subprocess.Popen(
        ["gdb-multiarch", "-batch", "-x", script], stdout=open(log, "w"), stderr=subprocess.STDOUT
    )
    atexit.register(gp.kill)
    t0 = time.time()
    while True:
        txt = open(log).read()
        if "VTIME-END" in txt or "VTIME-ERROR" in txt:
            break
        if gp.poll() is not None or qemu.poll() is not None:
            break
        if time.time() - t0 > timeout:
            print(f"  no final pause within {timeout:.0f} s of wall time")
            break
        time.sleep(1)
    for line in txt.splitlines():
        if line.startswith(("VTIME", "NO-MASTER-THREAD")):
            print("  " + line)
        elif line.startswith("POKED-BEFORE"):
            print("  poked before the boot's data was used")
        elif line.startswith("DRIVE-DONE"):
            _, t, label = line.split(" ", 2)
            what = "power-on" if label == "main" else label.replace("late", "late call")
            print(f"  t={float(t):5.1f}s  {what} ran and returned")
    print(f"  {time.time() - t0:.0f} s of wall time for {t_end:g} virtual s")
    if "VTIME-END" not in txt:
        atexit.unregister(shutil.rmtree)
        status = f"QEMU {exit_status(qemu)}, gdb {exit_status(gp)}"
        gp.kill()
        sys.exit(f"the run did not reach its final pause ({status}); gdb log {log}")


if __name__ == "__main__":
    sys.exit(main())
