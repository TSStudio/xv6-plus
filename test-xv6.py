#!/usr/bin/env python3

#
# python script that tests xv6 without having to boot it and type to its shell
#

import os, re, signal, subprocess, sys, time
from subprocess import run

class QEMU(object):

    def __init__(self, reset=False):
        if reset:
            self.build_xv6()
            self.reset_fs()
        q = ["make", "qemu"]
        self.proc = subprocess.Popen(q, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
        self.output = ""
        self.outbytes = bytearray()       
        time.sleep(1)

    def reset_fs(self):
        try:
            run(["rm", "fs.img"], check=True)
            run(["make", "fs.img"], check=True)
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")

    def build_xv6(self):
        try:
            run(["make", "kernel/kernel"], check=True)
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")
        
    def cmd(self, c):
        if isinstance(c, str):
            c = c.encode('utf-8')
        self.proc.stdin.write(c)
        self.proc.stdin.flush()
        
    def crash(self):
        ps = run(['ps', '-opid', '--no-headers', '--ppid', str(self.proc.pid)], stdout=subprocess.PIPE, encoding='utf8')
        kids = [int(line) for line in ps.stdout.splitlines()]
        if len(kids) == 0:
            print("no qemu")
            os.exit(1)
        print("kill", kids[0])
        os.kill(kids[0], signal.SIGKILL)

    def stop(self):
        self.proc.terminate()

    def read(self):
        buf = os.read(self.proc.stdout.fileno(), 4096)
        self.outbytes.extend(buf)
        self.output = self.outbytes.decode("utf-8", "replace")

    def lines(self):
        return self.output.splitlines()

    def match(self, *regexps):
        lines = self.lines()
        good = set()
        for i, line in enumerate(lines):
            if any(re.match(r, line) for r in regexps):
                good.add(i)
                regexps = [r for r in regexps if not re.match(r, line)]
        if len(good) == 0:
            print("match failed", regexps, good)
            sys.exit(1)

def crash():
    q = QEMU(True)
    q.cmd("logstress f0 f1 f2 f3 f4 f5\n")
    time.sleep(2)
    q.crash()
    q.stop()

def recover_log():
    q = QEMU()
    time.sleep(2)
    q.read()
    q.match('^recovering')
    q.cmd("ls\n")
    time.sleep(2)
    q.read()
    q.match('f5')
    q.stop()

def forphan():
    q = QEMU(True)
    q.cmd("forphan\n")
    time.sleep(5)
    q.read()
    q.match('wait')
    q.crash()
    q.stop()

def dorphan():
    q = QEMU(True)
    q.cmd("dorphan\n")
    time.sleep(5)
    q.read()
    q.match('wait')
    q.crash()
    q.stop()

def recover_orphan():
    q = QEMU()
    time.sleep(2)
    q.read()
    q.match('^ireclaim')
    q.cmd("ls\n")
    time.sleep(2)
    q.read()
    print(q.lines())
    q.stop()

def test_log():
    print("Test recovery of log")
    crash()
    recover_log()
    print("OK")

def test_porphan():
    print("Test recovery of an orphaned file")
    forphan()
    recover_orphan()
    print("OK")

def test_dorphan():
    print("Test recovery of an orphaned file")
    dorphan()
    recover_orphan()
    print("OK")

test_log()
test_porphan()
test_dorphan()
