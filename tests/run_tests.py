#!/usr/bin/env python3
"""
Інтеграційні тести та заміри p2pd.

Запускає справжні процеси p2pd в ізольованих каталогах (ваші реальні ключі,
історія й конфігурація не зачіпаються), керує ними через IPC-сокет (той самий
протокол, що й у GUI) і перевіряє сценарії Т-1..Т-10 із технічного завдання.

Використання:   python3 tests/run_tests.py             (усі сценарії, близько 1,5 хв)
                python3 tests/run_tests.py --valgrind  (лише перевірка витоків пам'яті)
Перед запуском закрийте всі свої p2pd та GUI: вузли знаходять один одного
через UDP-розсилку, і тестові вузли могли б підключитися до ваших.
Результат: таблиця в терміналі та файл test_report.md (test_report_valgrind.md для --valgrind)
"""
import atexit, os, shutil, signal, socket, statistics, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P2PD = os.path.join(ROOT, "build", "p2pd")
TMP = tempfile.mkdtemp(prefix="p2pm-test-")
REPORT = "test_report_valgrind.md" if "--valgrind" in sys.argv else "test_report.md"
ALL = []          # усі запущені вузли (для прибирання)
RESULTS = []      # (id, назва, вимога, результат, ok)


# ---------- IPC-клієнт (протокол GUI <-> демон) ----------
def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace("\r", "")

def unesc(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            i += 1
            out.append("\n" if s[i] == "n" else "\t" if s[i] == "t" else s[i])
        else:
            out.append(s[i])
        i += 1
    return "".join(out)

class Msg(list):
    t = 0.0

class Ipc:
    def __init__(self, path, wait=10):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        end = time.time() + wait
        while True:
            try:
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.05)
        self.buf = b""
        self.q = []

    def send(self, *fields):
        self.s.settimeout(30)
        self.s.sendall(("\t".join(esc(f) for f in fields) + "\n").encode())

    def _read(self, timeout):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(1 << 20)
        except socket.timeout:
            return
        if not d:
            raise ConnectionError("демон закрив IPC-сокет")
        now = time.perf_counter()
        self.buf += d
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            m = Msg(unesc(x) for x in line.decode().split("\t"))
            m.t = now
            self.q.append(m)

    def wait(self, pred, timeout):
        end = time.time() + timeout
        while True:
            for i, m in enumerate(self.q):
                if pred(m):
                    return self.q.pop(i)
            left = end - time.time()
            if left <= 0:
                return None
            self._read(min(left, 0.5))

    def close(self):
        try: self.s.close()
        except OSError: pass


# ---------- Вузол ----------
class Node:
    def __init__(self, port, args=(), name=None, prefix=()):
        self.port, self.args, self.prefix = port, list(args), list(prefix)
        self.name = name or f"N{port}"
        self.root = os.path.join(TMP, f"n{port}")
        self.home = os.path.join(self.root, "home")
        self.run = os.path.join(self.root, "run")
        os.makedirs(self.home, exist_ok=True)
        os.makedirs(self.run, exist_ok=True)
        self.proc = self.ipc = self.id = None

    @property
    def sock(self): return f"{self.run}/p2pm-{self.port}.sock"
    @property
    def logfile(self): return os.path.join(self.root, "log.txt")
    @property
    def datadir(self): return f"{self.home}/.local/share/p2pm"

    def start(self):
        env = os.environ.copy()
        env.update(HOME=self.home, XDG_RUNTIME_DIR=self.run)
        for k in ("XDG_DATA_HOME", "XDG_CONFIG_HOME"): env.pop(k, None)
        self.log = open(self.logfile, "ab")
        # stdin=/dev/null: так демон працює і під systemd
        self.proc = subprocess.Popen(self.prefix + [P2PD, str(self.port)] + self.args + ["-n", self.name],
                                     stdin=subprocess.DEVNULL, stdout=self.log,
                                     stderr=subprocess.STDOUT, env=env)
        if self not in ALL: ALL.append(self)
        slow = 60 if self.prefix else 10        # під valgrind запуск повільний
        self.ipc = Ipc(self.sock, slow)
        info = self.ipc.wait(lambda m: m[0] == "INFO", slow)
        if not info: raise RuntimeError(f"{self.name}: немає INFO від демона")
        self.id = info[1]

    def stop(self, sig=signal.SIGTERM, timeout=5):
        if self.ipc: self.ipc.close(); self.ipc = None
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(sig)
            try: self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.wait()
        return self.proc.returncode if self.proc else None

    def online(self):
        self.ipc.send("LIST")
        res = {}
        while True:
            m = self.ipc.wait(lambda m: m[0] in ("PEER", "LISTEND"), 5)
            if m is None or m[0] == "LISTEND": return res
            if m[3] == "1": res[m[1]] = m[2]

    def wait_online(self, other_id, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if other_id in self.online(): return True
            time.sleep(0.3)
        return False

    def say(self, to_id, text): self.ipc.send("SEND", to_id, text)

    def history(self, peer_id, n):
        self.ipc.send("HISTORY", peer_id, str(n))
        out = []
        while True:
            m = self.ipc.wait(lambda m: m[0] in ("HMSG", "HISTEND"), 5)
            if m is None or m[0] == "HISTEND": return out
            out.append(m)

    def cpu_ticks(self):
        with open(f"/proc/{self.proc.pid}/stat") as f:
            p = f.read().rsplit(")", 1)[1].split()
        return int(p[11]) + int(p[12])      # utime + stime

    def rss_mb(self):
        with open(f"/proc/{self.proc.pid}/status") as f:
            for l in f:
                if l.startswith("VmRSS"): return int(l.split()[1]) / 1024
        return 0.0

    def fd_count(self): return len(os.listdir(f"/proc/{self.proc.pid}/fd"))


def cleanup():
    for n in ALL:
        try:
            if n.proc and n.proc.poll() is None: n.proc.kill()
        except Exception: pass
    shutil.rmtree(TMP, ignore_errors=True)
atexit.register(cleanup)


def record(tid, name, req, result, ok):
    RESULTS.append((tid, name, req, result, ok))
    print(f"  [{'OK  ' if ok else 'FAIL'}] {tid}: {result}", flush=True)

def group(title): print(f"\n== {title}", flush=True)

def msg_in(frm): return lambda m: m[0] == "MSG" and m[1] == frm and m[2] == "0"


# ---------- Сценарії ----------
def scenario_pair():
    group("Пара вузлів: Т-1, Т-2, затримка, Т-6, права доступу")
    A, B = Node(9101), None
    A.start()
    B = Node(9102, ["127.0.0.1", "9101"])      # ручне підключення за IP і портом
    B.start()
    try:
        ok = A.wait_online(B.id, 15) and B.wait_online(A.id, 15)
        A.say(B.id, "привіт від A")
        m1 = B.ipc.wait(msg_in(A.id), 5)
        B.say(A.id, "привіт від B")
        m2 = A.ipc.wait(msg_in(B.id), 5)
        good = ok and m1 and m1[4] == "привіт від A" and m2 and m2[4] == "привіт від B"
        record("Т-1", "Обмін повідомленнями", "повідомлення доставляються в обидва боки",
               "доставлено в обидва боки" if good else "не доставлено", bool(good))

        # Т-2: 300 повідомлень поспіль + дуже велике повідомлення
        N = 300
        texts = [f"msg-{i}" for i in range(N)]
        t0 = time.perf_counter()
        A.ipc.send  # (поле)
        blob = "".join("\t".join(("SEND", esc(B.id), esc(t))) + "\n" for t in texts).encode()
        A.ipc.s.settimeout(30); A.ipc.s.sendall(blob)
        got = []
        while len(got) < N:
            m = B.ipc.wait(msg_in(A.id), 10)
            if m is None: break
            got.append(m[4])
        dt = time.perf_counter() - t0
        order_ok = got == texts
        record("Т-2", "Швидкий потік", "повідомлення не склеюються і не губляться",
               f"{len(got)}/{N} за {dt:.2f} с ({N/dt:.0f} повід./с), порядок {'збережено' if order_ok else 'ПОРУШЕНО'}",
               order_ok)
        big = "Я" * 100_000                     # ~200 КБ у UTF-8: багато TCP-сегментів
        A.say(B.id, big)
        mb = B.ipc.wait(msg_in(A.id), 15)
        record("Т-2б", "Велике повідомлення", "повідомлення 200 КБ доставляється цілим",
               "доставлено цілим" if mb and mb[4] == big else "пошкоджено або втрачено",
               bool(mb and mb[4] == big))

        # Затримка: ping-pong 200 разів, односторонній час ~ RTT/2
        rtts = []
        for i in range(200):
            t = time.perf_counter()
            A.say(B.id, f"p{i}")
            B.ipc.wait(lambda m: m[0] == "MSG" and m[4] == f"p{i}", 5)
            B.say(A.id, f"q{i}")
            A.ipc.wait(lambda m: m[0] == "MSG" and m[4] == f"q{i}", 5)
            rtts.append((time.perf_counter() - t) * 1000)
        one = sorted(r / 2 for r in rtts)
        p95 = one[int(len(one) * 0.95) - 1]
        record("Затримка", "Час доставки", "не більше 100 мс",
               f"мін {one[0]:.2f}, медіана {statistics.median(one):.2f}, p95 {p95:.2f}, макс {one[-1]:.2f} мс "
               f"(200 вимірів, цикл із шифруванням)", p95 <= 100)

        # Т-6: історія та сталість id після перезапуску
        A.say(B.id, "останнє-перед-перезапуском")
        B.ipc.wait(msg_in(A.id), 5)
        old_id = A.id
        rc = A.stop()
        A.start()
        hist = A.history(B.id, 5)
        last_ok = bool(hist) and hist[-1][4] == "останнє-перед-перезапуском" and hist[-1][2] == "1"
        record("Т-6", "Перезапуск", "історія збережена, id вузла не змінився",
               f"id {'той самий' if A.id == old_id else 'ЗМІНИВСЯ'}, історія {'збережена' if last_ok else 'втрачена'} "
               f"(виходу при перезапуску: код {rc})", A.id == old_id and last_ok)

        # Права доступу до ключа, бази й сокета
        db = f"{A.datadir}/node-{A.port}.db"
        modes = {k: oct(os.stat(p).st_mode & 0o777) for k, p in
                 (("база", db), ("каталог", A.datadir), ("IPC-сокет", A.sock))}
        sec_ok = modes["база"] == "0o600" and modes["каталог"] == "0o700" and modes["IPC-сокет"] == "0o600"
        record("Безпека", "Права доступу", "секретний ключ і сокет доступні лише власнику",
               ", ".join(f"{k} {v[2:]}" for k, v in modes.items()), sec_ok)
    finally:
        for n in (A, B):
            if n: n.stop()


def scenario_discovery():
    group("Виявлення вузлів: Т-3, Т-4, Т-5, Т-10")
    C, D = Node(9103), Node(9104)
    t0 = time.perf_counter()
    C.start(); D.start()
    try:
        up = C.ipc.wait(lambda m: m[0] == "PEER_UP", 30)
        dt = (up.t - t0) if up else None
        record("Т-3", "Автовиявлення", "вузол з'являється в списку за ≤ 10 с, без введення адрес",
               f"за {dt:.1f} с" if dt else "не виявлено", bool(dt and dt <= 10))
        D.wait_online(C.id, 15)

        # Т-10: GUI (IPC-клієнт) закривається й підключається знову
        C.ipc.close()
        C.ipc = Ipc(C.sock)
        info = C.ipc.wait(lambda m: m[0] == "INFO", 5)
        pup = C.ipc.wait(lambda m: m[0] == "PEER_UP" and m[1] == D.id, 5)
        record("Т-10", "Перепідключення GUI", "клієнт підключається до працюючого демона, стан відновлюється",
               "INFO і список вузлів отримано" if info and pup else "стан не відновлено", bool(info and pup))

        # Т-5: некоректні дані на TCP-порт
        cases = [("довжина 4 ГБ", bytes([2]) + b"\xff\xff\xff\xff"),
                 ("Hello із сміттям", bytes([1]) + (11).to_bytes(4, "big") + b"zz|zz|hello"),
                 ("Text без рукостискання", bytes([2]) + (5).to_bytes(4, "big") + b"hello")]
        closed_all = True
        for title, payload in cases:
            s = socket.create_connection(("127.0.0.1", C.port), 3)
            s.sendall(payload)
            s.settimeout(3)
            closed = False
            try:
                while True:
                    d = s.recv(4096)
                    if not d: closed = True; break
            except (socket.timeout, ConnectionResetError) as e:
                closed = isinstance(e, ConnectionResetError)
            s.close()
            closed_all &= closed
        alive = C.proc.poll() is None and C.online() is not None
        record("Т-5", "Некоректні пакети", "з'єднання закривається, демон продовжує працювати",
               f"3 з 3 з'єднань закрито, демон {'працює' if alive else 'ВПАВ'}" if closed_all else "з'єднання не закрито",
               closed_all and alive)

        # Т-4: вузол «зависає» (SIGSTOP): тайм-аут через heartbeat
        C.ipc.q.clear()
        D.proc.send_signal(signal.SIGSTOP)
        t0 = time.perf_counter()
        down = C.ipc.wait(lambda m: m[0] == "PEER_DOWN" and m[1] == D.id, 40)
        dt = (down.t - t0) if down else None
        record("Т-4", "Втрата вузла", "статус «не в мережі» не пізніше ніж за 20 с",
               f"за {dt:.1f} с" if dt else "не виявлено", bool(dt and dt <= 20))
        D.proc.send_signal(signal.SIGCONT)
        back = C.ipc.wait(lambda m: m[0] == "PEER_UP" and m[1] == D.id, 25)
        record("Т-4б", "Автовідновлення", "після повернення вузла з'єднання відновлюється саме",
               f"відновлено за {back.t - t0:.1f} с від зависання" if back else "не відновлено", bool(back))
    finally:
        for n in (C, D): n.stop()


class Tap:
    """Проксі, який записує весь TCP-трафік між двома вузлами."""
    def __init__(self, listen, target):
        self.data = bytearray()
        self.lock = threading.Lock()
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", listen)); self.srv.listen(5)
        self.target = target
        threading.Thread(target=self._accept, daemon=True).start()
    def _accept(self):
        while True:
            try: c, _ = self.srv.accept()
            except OSError: return
            t = socket.create_connection(self.target)
            for a, b in ((c, t), (t, c)):
                threading.Thread(target=self._pump, args=(a, b), daemon=True).start()
    def _pump(self, a, b):
        while True:
            try: d = a.recv(65536)
            except OSError: break
            if not d: break
            with self.lock: self.data += d
            try: b.sendall(d)
            except OSError: break
        for x in (a, b):
            try: x.close()
            except OSError: pass
    def size(self):
        with self.lock: return len(self.data)
    def snapshot(self):
        with self.lock: return bytes(self.data)
    def close(self): self.srv.close()


def scenario_wire():
    group("Конфіденційність каналу: Т-7")
    F = Node(9106); F.start()
    tap = Tap(9107, ("127.0.0.1", 9106))
    E = Node(9105, ["127.0.0.1", "9107"])       # E підключається до F через прослуховування
    E.start()
    try:
        # Знайти з'єднання, що йде крізь проксі: після Hello у ньому має бути обмін
        E.wait_online(F.id, 15); time.sleep(1)
        secret = "ТАЄМНИЙ-ТЕКСТ-" + "ключ-" * 300      # довге унікальне повідомлення
        before = tap.size()
        E.say(F.id, secret)
        got = F.ipc.wait(msg_in(E.id), 8)
        time.sleep(0.5)
        grown = tap.size() - before
        snap = tap.snapshot()
        found = secret.encode() in snap or "ТАЄМНИЙ-ТЕКСТ".encode() in snap
        delivered = bool(got and got[4] == secret)
        through_tap = grown >= len(secret.encode())
        if not through_tap:
            res, ok = "недостатньо даних: повідомлення пішло повз прослуховування (повторіть запуск)", False
        else:
            res = (f"перехоплено {grown} байт повідомлення, відкритого тексту в трафіку "
                   f"{'ЗНАЙДЕНО' if found else 'немає'}; адресат {'отримав' if delivered else 'НЕ отримав'} текст")
            ok = (not found) and delivered
        hello_visible = b"N9105" in snap
        record("Т-7", "Шифрування", "у перехопленому трафіку немає відкритого тексту", res, ok)
        record("Т-7б", "Що видно в Hello", "ключ і ім'я у рукостисканні відкриті (відоме обмеження)",
               f"ім'я вузла в трафіку {'видно' if hello_visible else 'не видно'}", True)
    finally:
        for n in (E, F): n.stop()
        tap.close()


def scenario_shutdown():
    group("Коректне завершення: Т-8")
    G = Node(9108); G.start()
    t0 = time.perf_counter()
    rc = G.stop(signal.SIGTERM, 5)
    dt = time.perf_counter() - t0
    sock_gone = not os.path.exists(G.sock)
    log = open(G.logfile, encoding="utf8", errors="replace").read()
    clean = "Вузол зупинено" in log
    record("Т-8", "Сигнал SIGTERM", "коректне завершення: код 0, сокет видалено, журнал закрито",
           f"код {rc}, за {dt:.2f} с, сокет {'видалено' if sock_gone else 'ЗАЛИШИВСЯ'}, "
           f"повідомлення про зупинку {'є' if clean else 'немає'}", rc == 0 and sock_gone and clean)


def scenario_load(N=20):
    group(f"Навантаження: Т-9 ({N} вузлів)")
    nodes = [Node(9500 + i) for i in range(N)]
    t0 = time.perf_counter()
    try:
        for n in nodes: n.start()
        started = time.perf_counter()
        # Повна сітка: кожен вузол бачить усіх інших
        deadline = time.time() + 90
        full = False
        while time.time() < deadline:
            if all(len(n.online()) == N - 1 for n in nodes):
                full = True; break
            time.sleep(1)
        mesh_t = time.perf_counter() - started
        record("Т-9", f"Повна сітка з {N} вузлів", f"усі {N} вузлів з'єднані між собою",
               f"сітка з {N*(N-1)//2} з'єднань за {mesh_t:.1f} с після запуску" if full else "сітка не побудована",
               full)
        if not full: return

        # Доставка від першого вузла всім іншим
        src = nodes[0]
        for n in nodes[1:]: src.say(n.id, f"привіт, {n.name}")
        delivered = sum(1 for n in nodes[1:]
                        if n.ipc.wait(lambda m: m[0] == "MSG" and m[1] == src.id and m[4] == f"привіт, {n.name}", 10))
        record("Т-9б", "Розсилка по сітці", "повідомлення доходять до всіх вузлів",
               f"{delivered}/{N-1} вузлів отримали", delivered == N - 1)

        # Ресурси у стані спокою
        for n in nodes: n.ipc.q.clear()
        c0 = [n.cpu_ticks() for n in nodes]; w0 = time.perf_counter()
        time.sleep(10)
        w = time.perf_counter() - w0
        tick = os.sysconf("SC_CLK_TCK")
        cpu = [100.0 * (n.cpu_ticks() - c) / tick / w for n, c in zip(nodes, c0)]
        rss = [n.rss_mb() for n in nodes]
        fds = [n.fd_count() for n in nodes]
        record("Ресурси", "Стан спокою", "завантаження ЦП демона в спокої менше 1 %",
               f"ЦП: середнє {statistics.mean(cpu):.2f} %, макс {max(cpu):.2f} %; "
               f"пам'ять: середнє {statistics.mean(rss):.1f} МБ, макс {max(rss):.1f} МБ; "
               f"дескрипторів: {min(fds)}..{max(fds)}", max(cpu) < 1.0)
    finally:
        codes = [n.stop() for n in nodes]
        record("Т-9в", "Завершення всіх вузлів", "усі демони завершуються з кодом 0",
               f"{sum(1 for c in codes if c == 0)}/{len(codes)} з кодом 0", all(c == 0 for c in codes))


def scenario_valgrind():
    group("Витоки пам'яті та дескрипторів (valgrind)")
    if shutil.which("valgrind") is None:
        record("valgrind", "Перевірка", "valgrind встановлено", "не знайдено: sudo apt install valgrind", False)
        return
    root = os.path.join(TMP, "n9600")
    os.makedirs(root, exist_ok=True)
    vlog = os.path.join(root, "valgrind.txt")
    V = Node(9600, prefix=["valgrind", "--leak-check=full", "--show-leak-kinds=definite,indirect",
                           "--track-fds=yes", f"--log-file={vlog}"])
    W = Node(9601)
    V.start(); W.start()
    try:
        ok = V.wait_online(W.id, 40)
        for i in range(50):                      # обмін повідомленнями в обидва боки
            V.say(W.id, f"v{i}"); W.say(V.id, f"w{i}")
        V.say(W.id, "Я" * 50000)
        got = sum(1 for i in range(50) if W.ipc.wait(lambda m: m[0] == "MSG" and m[4] == f"v{i}", 20))
        V.history(W.id, 20)
        # зміна конфігурації на лету та некоректний пакет
        with open(f"{V.home}/.config/p2pm/node-9600.conf", "w") as f: f.write("ping_interval=2\npeer_timeout=8\n")
        s = socket.create_connection(("127.0.0.1", 9600), 3); s.sendall(bytes([2]) + b"\xff" * 4); s.close()
        time.sleep(3)
        W.stop()                                  # вузол-партнер зникає: розрив з'єднання
        time.sleep(1)
    finally:
        rc = V.stop(signal.SIGTERM, 60)
        W.stop()
    out = open(vlog, encoding="utf8", errors="replace").read()
    def grab(key):
        for l in out.splitlines():
            if key in l: return l.split("==", 2)[-1].strip()
        return "?"
    errs = grab("ERROR SUMMARY:")
    fd_open = [l for l in out.splitlines() if "Open file descriptor" in l and "inherited" not in l]
    if "All heap blocks were freed" in out:        # valgrind пише це, коли витоків немає зовсім
        leaks, leak_ok = "вся динамічна пам'ять звільнена", True
    else:
        leaks = grab("definitely lost:")
        leak_ok = "0 bytes in 0 blocks" in leaks
    record("valgrind", "Витоки пам'яті", "витоків пам'яті і помилок доступу до неї немає",
           f"{leaks}; {errs}; доставлено {got}/50; код завершення {rc}",
           leak_ok and errs.startswith("ERROR SUMMARY: 0 errors") and rc == 0)
    extra = [l for l in fd_open if not any(x in l for x in ("/dev/", "pipe", "valgrind"))]
    record("valgrind-fd", "Незакриті дескриптори", "після завершення відкритих дескрипторів немає",
           "немає незакритих дескрипторів" if not extra else f"{len(extra)} незакритих", not extra)


def write_report():
    lines = ["# Результати тестування p2pd", "",
             f"Дата: {time.strftime('%d.%m.%Y %H:%M')}, система: {os.uname().sysname} {os.uname().release}", "",
             "| № | Перевірка | Вимога | Результат | Висновок |", "|---|---|---|---|---|"]
    for tid, name, req, res, ok in RESULTS:
        lines.append(f"| {tid} | {name} | {req} | {res} | {'пройдено' if ok else 'ПРОВАЛЕНО'} |")
    passed = sum(1 for r in RESULTS if r[4])
    lines += ["", f"Пройдено перевірок: {passed} із {len(RESULTS)}."]
    open(REPORT, "w", encoding="utf8").write("\n".join(lines) + "\n")


def main():
    if not os.path.exists(P2PD):
        sys.exit(f"Не знайдено {P2PD}. Спочатку зберіть проект: cmake --build build")
    if subprocess.run(["pgrep", "-x", "p2pd"], capture_output=True).returncode == 0 and "--force" not in sys.argv:
        sys.exit("Уже запущені p2pd (вони могли б підключитися до тестових вузлів). "
                 "Закрийте їх: pkill p2pd (і зупиніть службу: systemctl --user stop 'p2pd@*'). "
                 "Або запустіть з --force.")
    print(f"Тимчасовий каталог: {TMP}")
    scenarios = (scenario_pair, scenario_discovery, scenario_wire, scenario_shutdown, scenario_load)
    if "--valgrind" in sys.argv:        # лише перевірка витоків
        scenarios = (scenario_valgrind,)
    for sc in scenarios:
        try:
            sc()
        except Exception as e:
            record("ПОМИЛКА", sc.__name__, "сценарій виконується без збоїв", f"{type(e).__name__}: {e}", False)
    write_report()
    failed = [r for r in RESULTS if not r[4]]
    print(f"\nПідсумок: пройдено {len(RESULTS) - len(failed)} із {len(RESULTS)}. Звіт: {REPORT}")
    sys.exit(1 if failed else 0)

main()
