#!/usr/bin/env python3
"""Diagnostic: two-process live test that spawns, tests, kills servers, reports state."""
import json, os, signal, socket, struct, subprocess, sys, time

V2_MAGIC = 0x4D434854
V2_VERSION = 2
V2_HEADER_LEN = 20
V2_CONTENT_TYPE_JSON = 1
BIN = sys.argv[1]
DB = sys.argv[2]
PW = sys.argv[3]
WORK = sys.argv[4]

def log(*a):
    print(*a, flush=True)

def frame(body):
    head = struct.pack(">IBBHIHBB", V2_MAGIC, V2_VERSION, 0, V2_HEADER_LEN,
                       len(body), 0, V2_CONTENT_TYPE_JSON, 0)
    head += struct.pack(">I", 0)
    return head + body

class C:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
    def send(self, o):
        self.s.sendall(frame(json.dumps(o, separators=(",", ":")).encode()))
    def recv(self, t=5.0):
        self.s.settimeout(t)
        try:
            while len(self.buf) < V2_HEADER_LEN:
                c = self.s.recv(4096)
                if not c: return None
                self.buf += c
            bl = struct.unpack(">I", self.buf[8:12])[0]
            while len(self.buf) < V2_HEADER_LEN + bl:
                c = self.s.recv(4096)
                if not c: return None
                self.buf += c
            body = self.buf[V2_HEADER_LEN:V2_HEADER_LEN+bl]
            self.buf = self.buf[V2_HEADER_LEN+bl:]
            return json.loads(body.decode())
        except (socket.timeout, ValueError, ConnectionResetError):
            return None

def wait_ready(log, port, p, t=30):
    dl = time.time()+t
    lr = False
    while time.time()<dl:
        if not lr:
            try:
                if "Server started" in open(log, errors="replace").read(): lr=True
            except OSError: pass
        try:
            s=socket.create_connection(("127.0.0.1",port),timeout=1); s.close(); return True
        except OSError: pass
        if p.poll() is not None: return False
        time.sleep(0.2)
    return False

def write_cfg(path, gid, v1, v2):
    cfg={"server":{"v1":{"ip":"127.0.0.1","port":v1,"threads":2},"v2":{"port":v2}},
         "db":{"host":"127.0.0.1","port":3306,"user":"root","password":PW,"dbname":DB,"pool_size":4},
         "executor":{"workers":2,"queue_capacity":64},
         "reliable":{"ack_timeout_ms":3000,"backoff_base_ms":1000,"backoff_cap_ms":2000,
                     "backoff_multiplier":2,"jitter_fraction":0.0,"jitter_seed":20260820,
                     "message_retention_ms":300000,"acked_retention_ms":3600000,
                     "expired_retention_ms":3600000,"cleanup_batch":100,
                     "cleanup_cycle_ms":3600000,"retry_batch_limit":500},
         "outbox":{"claim_batch":8,"scan_interval_ms":200,"claim_lease_ms":30000},
         "gateway":{"id":gid,"presence":{"host":"127.0.0.1","port":6379,"db":0,"ttl_ms":30000,
                    "connect_timeout_ms":1000,"command_timeout_ms":1000},
                    "kafka":{"host":"127.0.0.1","port":9092},
                    "consumer":{"topic":"muduo-outbox","fetch_batch_limit":100,"poll_deadline_ms":5000}}}
    json.dump(cfg, open(path,"w"))

write_cfg(os.path.join(WORK,"gw1.json"),1,16201,16211)
write_cfg(os.path.join(WORK,"gw2.json"),2,16202,16212)
l1=open(os.path.join(WORK,"gw1.log"),"w"); l2=open(os.path.join(WORK,"gw2.log"),"w")
def spawn(cfg,log,v1):
    cmd=["setarch","x86_64","-R",BIN,"127.0.0.1",str(v1),"--config",cfg]
    return subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ))
p1=spawn(os.path.join(WORK,"gw1.json"),l1,16201)
p2=spawn(os.path.join(WORK,"gw2.json"),l2,16202)
try:
    log("gw1_ready",wait_ready(os.path.join(WORK,"gw1.log"),16211,p1))
    log("gw2_ready",wait_ready(os.path.join(WORK,"gw2.log"),16212,p2))
    time.sleep(2)

    ca=C(16211); cb=C(16212)
    def reg(c,name):
        c.send({"msgid":4,"name":name,"password":"pwd"})
        r=c.recv(); return r.get("id",0) if r and r.get("errno")==0 else 0
    def login(c,uid):
        c.send({"msgid":1,"id":uid,"password":"pwd"})
        r=c.recv(); return r is not None and r.get("errno")==0
    aid=reg(ca,"d_alice"); bid=reg(cb,"d_bob")
    log("alice_id",aid,"bob_id",bid)
    log("login_alice_gw1",login(ca,aid))
    log("login_bob_gw2",login(cb,bid))
    time.sleep(2)

    import subprocess as sp
    log("PRESENCE:", sp.check_output(["redis-cli","-n","0","--scan","--pattern","presence:v1:*"]).decode().splitlines())

    ca.send({"msgid":6,"id":aid,"toid":bid,"client_message_id":"diag-cmn-1","content":"hi"})
    t0=time.time()
    r=ca.recv(6)
    log("send_ack",r,"after %.2fs"%(time.time()-t0))
    dl=time.time()+30
    got=None
    while time.time()<dl:
        r=cb.recv(max(0.1,dl-time.time()))
        if r is None: break
        if r.get("message_id",0)>0 and "content" in r:
            got=r; break
    log("bob_got",got,"after %.2fs"%(time.time()-t0))
    time.sleep(3)
    log("PRESENCE2:", sp.check_output(["redis-cli","-n","0","--scan","--pattern","presence:v1:*"]).decode().splitlines())

    def sql(q):
        try:
            return sp.check_output(["mysql","-uroot","-p123456","-N","-B",DB,"-e",q],stderr=sp.DEVNULL).decode()
        except Exception as e:
            return "ERR %r" % e
    log("DB_ChatMessage:", sql("SELECT message_id,content FROM ChatMessage"))
    log("DB_Delivery:", sql("SELECT * FROM MessageDelivery"))
    log("DB_DeadLetter:", sql("SELECT reason,message_id FROM KafkaDeadLetter"))
finally:
    for p in (p1,p2):
        if p.poll() is None:
            os.kill(p.pid, signal.SIGKILL)
            try: p.wait(5)
            except subprocess.TimeoutExpired: pass
    l1.close(); l2.close()
    try: ca.close()
    except NameError: pass
    try: cb.close()
    except NameError: pass
