# -*- coding: ascii -*-
# Smoke test: real PDU login against Dockerized server (VM 192.168.232.166:5000).
# Flow mirrors Client source: CAPTCHA_REQUEST(44) -> LOGIN_REQUEST(3) with
# caData=name[32]+md5hex(pwd)[32], caMsg=captcha+NUL+clientId.
import socket
import struct
import hashlib

HOST = "192.168.232.166"
PORT = 5000
HDR = 76  # 3*uint + caData[64]

def mk_pdu(msg_len, ptype, cadata=b"", msg=b""):
    total = HDR + msg_len
    return (struct.pack("<III", total, msg_len, ptype)
            + cadata.ljust(64, b"\0")[:64] + msg)

def read_pdu(sock):
    buf = b""
    while len(buf) < HDR:
        chunk = sock.recv(HDR - len(buf))
        if not chunk:
            raise RuntimeError("connection closed during header")
        buf += chunk
    total, msg_len, ptype = struct.unpack("<III", buf[:12])
    cadata = buf[12:HDR]
    msg = b""
    while len(msg) < msg_len:
        chunk = sock.recv(msg_len - len(msg))
        if not chunk:
            break
        msg += chunk
    return ptype, cadata, msg

sock = socket.create_connection((HOST, PORT), timeout=10)
print("[1] TCP connected to %s:%d" % (HOST, PORT))

sock.sendall(mk_pdu(0, 44))  # CAPTCHA_REQUEST
ptype, cadata, msg = read_pdu(sock)
assert ptype == 45, "expected CAPTCHA_RESPOND, got %d" % ptype
status, remaining = struct.unpack("<ii", cadata[:8])
code = msg.split(b"\0")[0].decode("ascii")
print("[2] CAPTCHA_RESPOND status=%d remaining=%ds code=%s" % (status, remaining, code))
assert status == 0 and len(code) == 6

pwd_md5 = hashlib.md5(b"123456").hexdigest().encode("ascii")  # 32 hex chars
cadata = b"tester".ljust(32, b"\0") + pwd_md5
msg = code.encode("ascii") + b"\0" + b"smoketest"
sock.sendall(mk_pdu(len(msg), 3, cadata, msg))  # LOGIN_REQUEST
ptype, cadata, msg = read_pdu(sock)
assert ptype == 4, "expected LOGIN_RESPOND, got %d" % ptype
ret = struct.unpack("<i", cadata[:4])[0]
print("[3] LOGIN_RESPOND ret=%d %s" % (ret, "(SUCCESS)" if ret == 1 else "(FAIL)"))

sock.close()
print("SMOKE TEST %s" % ("PASSED" if ret == 1 else "FAILED"))
