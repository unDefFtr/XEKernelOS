#!/usr/bin/env python3
"""XEKernelOS 串口冒烟测试 (2026-08-16)

命令主要走串口 TCP；QEMU monitor TCP 注入 PS/2 按键并抓取屏幕。
另以真实 PS/2 RUN/普通输入/Ctrl+C 检查软件扫描码队列的保留与终止行为。

判定点:
  1. boot 到 "tasks ready" (内核完整启动)
  2. 全程无 panic
  3. RUN GFXDEMO.BIN 后 ECHO X 仍能被读到 —— 证明 fork→exec_fd→exit→
     waitpid 唤醒→调度切换 全链路无死锁 (回归: RUN 死锁/僵尸)
  4. CREATE 的文件在磁盘镜像中簇链正确 (回归: FAT 簇重复分配)

用法: python tools/smoke_serial.py
"""
import codecs, hashlib, os, shutil, socket, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BASE)
BLD  = os.path.join(ROOT, 'build')
QEMU = os.environ.get('QEMU', 'qemu-system-i386')
PORT = 7788

CMDS = [
    'LS',
    'CREATE TEST1.TXT HELLO',
    'CREATE TEST2.TXT WORLD',
    'CAT TEST1.TXT',
    'RUN GFXDEMO.BIN',
    'ECHO X',          # RUN 之后 Shell 必须还活着
    'RUN GFXDEMO.BIN', # 第二次 RUN — 回归: 旧版僵尸残留导致首次后不等待
    'ECHO Y',
    'RUN DESKTOP.BIN', # GUI 桌面 → 探针 → sendkey esc 退出
    'ECHO Z',          # ESC 退出后 Shell 恢复 (屏幕对比断言)
    'ECHO W',          # 双保险: 屏幕应继续变化
    'RUN SEGTEST.BIN',
    'ECHO SEG1',
    'RUN SEGTEST.BIN',
    'ECHO SEG2',
    'RUN SPIN.BIN',    # 抢占式调度: 父进程纯死循环 + 子进程心跳打印
    'ECHO KILLOK',     # Ctrl+C 终止自旋进程后 Shell 必须恢复
    'RUN LFSTEST.BIN', # 长文件名 (LFN) 读侧: 列目录 + 长名/短名打开
    'ECHO L',
]

def log(s): print(s, flush=True)

def kill_qemu():
    """强杀残留 QEMU (Windows 用 taskkill; 其他平台忽略)"""
    try:
        subprocess.run(['taskkill', '/F', '/IM', 'qemu-system-i386.exe'],
                       capture_output=True)
    except FileNotFoundError:
        pass

def main():
    img  = os.path.join(BLD, 'xekernelos.img')
    disk = os.path.join(BLD, 'disk.img')
    if not (os.path.exists(img) and os.path.exists(disk)):
        log('FAIL: 构建产物缺失, 先 make'); return 1

    data_img = os.path.join(BLD, 'smoke-data.img')
    shutil.copyfile(disk, data_img)
    qcow2 = os.path.join(BLD, 'smoke.qcow2')
    if os.path.exists(qcow2): os.remove(qcow2)
    subprocess.run(['qemu-img', 'convert', '-f', 'raw', '-O', 'qcow2', '-S', '4M',
                    data_img, qcow2], check=True, capture_output=True)

    boot_img = os.path.join(BLD, 'smoke-boot.img')
    shutil.copyfile(img, boot_img)

    accel = os.environ.get('QEMU_ACCEL', 'tcg')
    accel_args = ['-accel', accel]
    if accel == 'kvm':
        accel_args += ['-cpu', 'host']
    p = subprocess.Popen(
        [QEMU, *accel_args,
         '-drive', f'file={qcow2},format=qcow2,if=ide,index=1',
         '-drive', f'file={boot_img},format=raw,if=ide,index=0',
         '-m', '32', '-boot', 'order=c', '-display', 'none', '-no-reboot',
         '-serial', f'tcp:127.0.0.1:{PORT},server=on,wait=on',
         '-monitor', f'tcp:127.0.0.1:{PORT+1},server=on,wait=off'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def monitor_cmd(cmdline):
        """经 TCP monitor 发一条命令"""
        try:
            m = socket.create_connection(('127.0.0.1', PORT + 1), timeout=3)
            m.settimeout(2.0)
            try:
                m.recv(4096)   # banner
            except OSError:
                pass
            m.sendall((cmdline + '\n').encode())
            time.sleep(0.6)
            try:
                m.recv(4096)
            except OSError:
                pass
            m.close()
        except OSError:
            pass

    def sendkey(key):
        """monitor sendkey — PIT drain 会捕获 PS/2 字节进事件流"""
        monitor_cmd(f'sendkey {key}')

    def screendump(name, probe=None, region=None, with_sig=False):
        """截屏: probe=(x,y)→RGB; region=(x0,y0,x1,y1)→暗像素计数
        with_sig: region 模式额外返回区域像素 md5 (时钟走动对比用)"""
        try:
            m = socket.create_connection(('127.0.0.1', PORT + 1), timeout=3)
            m.settimeout(2.0)
            try:
                m.recv(4096)   # banner
            except OSError:
                pass
            path = os.path.join(BLD, name)
            qpath = path.replace('\\', '/')
            m.sendall(f'screendump {qpath}\n'.encode())
            time.sleep(1.5)
            try:
                m.recv(4096)
            except OSError:
                pass
            m.close()
            if not os.path.exists(path):
                return (-1, None) if probe else -1
            with open(path, 'rb') as f:
                f.readline(); w, h = map(int, f.readline().split()); f.readline()
                data = f.read()
            if probe:
                x, y = probe
                i = (y * w + x) * 3
                rgb = (data[i], data[i+1], data[i+2])
                return rgb
            if region:
                x0, y0, x1, y1 = region
                dark = 0
                rbytes = bytearray()
                for y in range(y0, min(y1, h)):
                    base = y * w
                    for x in range(x0, min(x1, w)):
                        i = (base + x) * 3
                        rbytes += data[i:i+3]
                        if data[i] < 80 and data[i+1] < 80 and data[i+2] < 80:
                            dark += 1
                if with_sig:
                    return (dark, hashlib.md5(bytes(rbytes)).hexdigest())
                return dark
            return sum(1 for i in range(0, len(data), 3)
                       if data[i] or data[i+1] or data[i+2])
        except OSError:
            return (-1, None) if probe else -1

    def check_desktop(name):
        """桌面四探针: 三色块 + 时钟文字暗像素。
        返回 (bg, top, bar, clock_dark, label_dark)"""
        p1 = screendump(name + '.bg.ppm',   probe=(500, 400))
        p2 = screendump(name + '.top.ppm',  probe=(500, 10))
        p3 = screendump(name + '.bar.ppm',  probe=(500, 754))
        # 时钟文字: 任务栏右侧 (924..1010, 746..762); 黑字于灰底
        clk = screendump(name + '.clk.ppm', region=(920, 744, 1012, 763))
        # 对照组: 任务栏左侧文字 (10..200)
        lbl = screendump(name + '.lbl.ppm', region=(8, 744, 200, 763))
        log(f'桌面探针: 背景={p1}(0,0,170) 顶栏={p2}(85,85,255) 任务栏={p3}(170,170,170) '
            f'时钟暗px={clk} 左侧文字暗px={lbl}')
        return (p1, p2, p3, clk, lbl)

    try:
        # 连接串口 (wait=on: 连接后 guest 才启动)
        sock = None
        for _ in range(60):
            try:
                sock = socket.create_connection(('127.0.0.1', PORT), timeout=3)
                break
            except OSError:
                time.sleep(0.5)
        if sock is None:
            log('FAIL: 串口连接失败'); return 1
        sock.settimeout(0.2)

        serial = ''
        # 增量 UTF-8 解码: 串口逐字节写, TCP 分片可能切断多字节序列,
        # 逐片 decode 会把中文误报成 U+FFFD 乱码
        decoder = codecs.getincrementaldecoder('utf-8')('replace')

        def drain(dur):
            nonlocal serial
            end = time.time() + dur
            while time.time() < end:
                try:
                    d = sock.recv(4096)
                    if not d: break
                    serial += decoder.decode(d)
                except socket.timeout:
                    pass
                except OSError:
                    break

        # 1. 等 boot 完成 → 开机自动进 GUI
        end = time.time() + 25
        while time.time() < end and 'tasks ready' not in serial:
            drain(0.5)
        if 'tasks ready' not in serial:
            log('FAIL: 25s 内未见 tasks ready, 启动日志:\n' + serial)
            return 1
        log('OK: 内核启动完成')

        # 2. 开机 GUI 探针 (kernel 默认加载 DESKTOP.BIN)
        drain(4.0)
        boot_gui = check_desktop('s0_gui.ppm')
        # 2b. 时钟走动验证: 两次截屏 (间隔≥1.5s+1.6s) 秒位变化 → 像素不同
        clk1 = screendump('s0_clk1.ppm', region=(920, 744, 1012, 763), with_sig=True)
        time.sleep(1.6)
        clk2 = screendump('s0_clk2.ppm', region=(920, 744, 1012, 763), with_sig=True)
        if not isinstance(clk1, tuple): clk1 = (-1, 'x')
        if not isinstance(clk2, tuple): clk2 = (-1, 'y')
        log(f'时钟区域: 第1次 暗px={clk1[0]} 第2次 暗px={clk2[0]} '
            f'({"像素变化" if clk1[1] != clk2[1] else "像素相同"})')
        # 3. ESC 退出 GUI → shell (sendkey 经 PS/2 → PIT drain → 事件流)
        sendkey('esc')
        drain(4.0)
        log(f'屏幕(GUI退出后): {screendump("s0_shell.ppm")} 非黑像素')

        # 4. shell 命令序列
        desktop_probes = None
        for c in CMDS:
            sock.sendall((c + '\r').encode())
            pause = 5.0 if c.startswith('RUN') else 1.8
            drain(pause)
            log(f'>> {c}')
            tag = c.split()[0].lower()
            if c.startswith('RUN DESKTOP'):
                desktop_probes = check_desktop(f's1_{tag}.ppm')
                desktop_base = screendump('s1_desk_base.ppm')  # 桌面基线像素数
                sendkey('esc')          # 退出 GUI 回 shell
                drain(4.0)
            elif c == 'RUN SPIN.BIN':
                # 抢占证明: 父进程是纯 CPU 死循环 (无 syscall), 子进程能打印
                # tick 说明父进程被 PIT 时间片抢占让出了 CPU。
                n_tick = serial.count('SPIN: child tick')
                log(f'SPIN 子进程 tick 行数={n_tick} (若调度非抢占则恒为 0)')
                # 子进程已退出 (只剩自旋父进程) → Ctrl+C 必定命中父进程
                sendkey('ctrl-c')
                drain(3.0)
                log(f'Ctrl+C 之后: {screendump("s1_spin.ppm")} 非黑像素')
            elif c == 'ECHO Z':
                pz = screendump('s1_ez.ppm')
                log(f'屏幕(ECHO Z): {pz} (桌面基线 {desktop_base})')
            elif c == 'ECHO W':
                pw = screendump('s1_ew.ppm')
                log(f'屏幕(ECHO W): {pw}')
            elif c == 'RUN SEGTEST.BIN':
                # Dedicated assertions below; no graphics probe applies.
                pass
            elif c.startswith('RUN'):
                # gfx_demo: CLS 深蓝 + 左上红矩形, 2s 退出
                probe = screendump(f's1_{tag}.ppm', probe=(50, 60))
                log(f'屏幕(RUN) 探针(50,60) RGB={probe} (红矩形区, 期望 ~(170,0,0))')
            else:
                log(f'屏幕({c}): {screendump(f"s1_{tag}.ppm")} 非黑像素')

        drain(2.0)

        # PS/2 路径：RUN 的 Return break 和自旋期间的普通输入不能挡住
        # Ctrl+C。终止后再提交原队列里的 ECHO，验证普通字符没有被丢弃。
        ps2_start = len(serial)
        def type_ps2(text):
            for char in text:
                sendkey({' ': 'spc', '.': 'dot'}.get(char, char))
        type_ps2('run spin.bin')
        sendkey('ret')
        drain(5.0)
        type_ps2('echo ps2ok')
        sendkey('ctrl-c')
        drain(3.0)
        sendkey('ret')
        drain(2.0)
        ps2_serial = serial[ps2_start:]

        # ---- 断言 ----
        ok = True
        for bad in ('PANIC', 'Page Fault', 'kernel_panic', 'Triple', '#DF'):
            if bad in serial:
                log(f'FAIL: 串口日志出现 {bad}'); ok = False
        if ok: log('OK: 无 panic')
        for run_no, echo in enumerate(('SEG1', 'SEG2'), 1):
            run_marker = f'[READ] "RUN SEGTEST.BIN"'
            echo_marker = f'[READ] "ECHO {echo}"'
            starts = [i for i in range(len(serial))
                      if serial.startswith(run_marker, i)]
            start = starts[run_no - 1] if len(starts) >= run_no else -1
            end = serial.find(echo_marker, start + len(run_marker)) if start >= 0 else -1
            if start < 0 or end < 0:
                log(f'FAIL: SEGTEST run {run_no} 或后续 ECHO {echo} 未读到')
                ok = False
                continue
            interval = serial[start:end]
            if any(marker in interval for marker in
                   ('SEGTEST: FAIL', 'SEGEXEC: FAIL')) or any(
                    marker not in interval for marker in
                    ('SEGTEST: CHILD PASS', 'SEGEXEC: PASS', 'SEGTEST: PASS')):
                log(f'FAIL: SEGTEST run {run_no} 缺少本次区间 PASS 或出现 FAIL')
                ok = False
            else:
                log(f'OK: SEGTEST run {run_no} 独立回归标记与 ECHO {echo}')

        # [READ] 回显与 shell 提示符在同一行 (提示符先输出), 用 in 匹配
        rd_lines = [l for l in serial.splitlines() if '[READ]' in l and l.lstrip().startswith('XEKernel')]
        runs = [i for i, l in enumerate(rd_lines) if 'RUN GFXDEMO' in l]
        ex_after = [next((j for j, l in enumerate(rd_lines)
                          if ('ECHO X' in l or 'ECHO Y' in l) and j > i), -1)
                    for i in runs]
        n_exec = serial.count('exec: loaded')
        if not runs:
            log('FAIL: 未收到 RUN 命令 (输入丢失?)'); ok = False
        elif len(runs) < 2:
            log(f'FAIL: RUN 命令只收到 {len(runs)} 次'); ok = False
        elif n_exec < 3:
            log(f'FAIL: exec 只执行 {n_exec} 次 (<3, 含 DESKTOP) — RUN 未等待/未执行')
            ok = False
        elif any(j < 0 for j in ex_after):
            log('FAIL: 某次 RUN 之后 Shell 无响应 — waitpid/僵尸回归!')
            ok = False
        else:
            log(f'OK: {len(runs)} 次 RUN 全部 exec 并等待退出 (僵尸回收回归通过)')

        # 桌面绘制探针 (开机 GUI 与 RUN DESKTOP 各一组)
        exp3 = ((0, 0, 170), (85, 85, 255), (170, 170, 170))
        names3 = ('背景', '顶栏', '任务栏')
        for tag, probes in (('开机', boot_gui), ('RUN', desktop_probes)):
            if probes is None:
                log(f'FAIL: {tag}桌面探针缺失'); ok = False
                continue
            for got, want, nm in zip(probes[:3], exp3, names3):
                if got is None or any(abs(a - b) > 12 for a, b in zip(got, want)):
                    log(f'FAIL: {tag}桌面{nm}探针 {got} ≠ {want}'); ok = False
        if ok:
            log('OK: 桌面绘制正确 (开机 GUI + RUN DESKTOP 三色探针)')

        # 时钟文字探针: RUN DESKTOP 那组 (开机组同样要求)
        for tag, probes in (('开机', boot_gui), ('RUN', desktop_probes)):
            if probes is None: continue
            clk, lbl = probes[3], probes[4]
            if lbl is None or lbl < 30:
                log(f'FAIL: {tag}任务栏左侧文字缺失 (暗px={lbl}) — TEXT ioctl 异常'); ok = False
            elif clk is None or clk < 30:
                log(f'FAIL: {tag}时钟未显示 (暗px={clk}, 左侧文字={lbl}) — SYS_TIME/绘制异常'); ok = False
            else:
                log(f'OK: {tag}时钟显示正常 (暗px={clk})')

        # 时钟走动断言: 两次截屏像素必须变化 (事件循环 SYS_SLEEP→RTC 秒刷新)
        try:
            if clk1[0] < 30 or clk2[0] < 30:
                log(f'FAIL: 时钟区域无文字 (clk1={clk1[0]} clk2={clk2[0]})'); ok = False
            elif clk1[1] == clk2[1]:
                log('FAIL: 时钟两次截屏像素相同 — 事件循环卡死/SYS_SLEEP 未唤醒/时钟不刷新'); ok = False
            else:
                log('OK: 时钟走动 (两次截屏像素变化)')
        except (TypeError, IndexError):
            log('FAIL: 时钟截屏数据缺失'); ok = False

        # ESC 退出 GUI 验证: 屏幕对比法 (ECHO Z/W 的输出画上屏幕即证明
        # shell 收到了命令并执行; [READ] 串口回显与 quit 有竞态不作依据)
        try:
            base_ok = abs(pz - desktop_base) > 1000 or abs(pw - desktop_base) > 1000
            resp_ok = abs(pz - pw) > 100   # Z 与 W 两次输出 → 屏幕持续变化
        except (NameError, TypeError):
            base_ok = resp_ok = False
        if not (base_ok and resp_ok):
            log(f'FAIL: ESC 退出后 Shell 无响应 (Z={pz} W={pw} 基线={desktop_base})')
            ok = False
        else:
            log('OK: ESC 退出 GUI → Shell 响应命令 (屏幕对比通过)')

        if 'signal: killing' in serial:
            log('注意: 有进程被信号杀死: ' +
                '; '.join(l for l in serial.splitlines() if 'signal' in l))

        # ---- M1: 抢占式调度 + SIGINT 默认终止 ----
        n_tick  = serial.count('SPIN: child tick')
        i_start = serial.find('SPIN: start')
        i_done  = serial.find('SPIN: child done')
        i_kill  = serial.find('signal: killing')
        i_ok    = serial.find('[READ] "ECHO KILLOK"')
        i_tl    = serial.find('SPIN: tasks=')
        if i_start < 0:
            log('FAIL: SPIN.BIN 未启动 (RUN 失败?)'); ok = False
        elif i_tl < 0 or 'SPIN: tasks=?' in serial:
            log('FAIL: SYS_TASK_LIST (任务管理器数据源) 从 ring3 调用失败'); ok = False
        elif n_tick < 2 or i_done < 0:
            log(f'FAIL: SPIN 子进程未运行 (tick={n_tick}, done={i_done >= 0}) '
                f'— 抢占式调度失效, 死循环饿死了其他任务'); ok = False
        elif i_kill < 0 or i_kill < i_done:
            log('FAIL: Ctrl+C 未终止自旋进程 (无 signal: killing, 或早于子进程结束)')
            ok = False
        elif i_ok < 0 or i_ok < i_kill:
            log('FAIL: 自旋进程被终止后 Shell 未恢复 (ECHO KILLOK 无回显)')
            ok = False
        else:
            log(f'OK: 抢占式调度 (SPIN 子进程 tick={n_tick}) + Ctrl+C(SIGINT) '
                f'终止自旋进程 + Shell 恢复 + SYS_TASK_LIST 可用')

        # 同一场景经 PS/2 而不是串口启动；断言只看本次独立日志区间。
        ps2_run = ps2_serial.find('[READ] "run spin.bin"')
        ps2_done = ps2_serial.find('SPIN: child done')
        ps2_kill = ps2_serial.find('signal: killing')
        ps2_echo = ps2_serial.find('[READ] "echo ps2ok"')
        ps2_body = ps2_serial.find('\nps2ok', ps2_echo)
        if not (0 <= ps2_run < ps2_done < ps2_kill < ps2_echo < ps2_body
                and ps2_serial.count('SPIN: child tick') >= 2):
            log('FAIL: PS/2 RUN/Ctrl+C/Shell 恢复或普通输入保留失败')
            ok = False
        else:
            log('OK: PS/2 RUN + Ctrl+C 越过队列普通输入 + ECHO 保留并执行')

        # ---- M2: VFAT 长文件名 (LFN) 读侧 ----
        i_lfs   = serial.find('LFSTEST: list')
        i_done2 = serial.find('LFSTEST: done')
        i_long  = serial.find('中文文档.txt')
        i_body  = serial.find('XEK-LFN-CONTENT-OK')
        i_short = serial.find('LFSTEST: short-body')
        i_lread = serial.find('LFSTEST: read=')
        long_ok = i_lread >= 0 and i_lread + 15 < len(serial) and serial[i_lread:i_lread+22].find('read=-') < 0
        if i_lfs < 0 or i_done2 < 0:
            log('FAIL: LFSTEST.BIN 未跑完 (RUN LFSTEST.BIN 失败?)'); ok = False
        elif i_long < 0:
            log('FAIL: 目录列表未出现长名"中文文档.txt" — LFN 组装/UTF-16→UTF-8 失败')
            ok = False
        elif i_body < 0 or not long_ok:
            log('FAIL: 用长名打开/读取失败 (read 行: '
                f'{serial[i_lread:i_lread+24] if i_lread >= 0 else "缺失"})'); ok = False
        elif i_short < 0:
            log('FAIL: 8.3 短名回退打开失败 (LONGFI~1.TXT)'); ok = False
        else:
            log('OK: LFN 读侧 (长名/中文名列出 + 长名打开读正文 + 8.3 短名回退)')

        # ---- 先优雅退出 QEMU (quit 刷写缓存), 强杀仅兜底 ----
        try:
            sock.close()
        except Exception:
            pass
        sock = None
        try:
            m = socket.create_connection(('127.0.0.1', PORT + 1), timeout=3)
            m.settimeout(2.0)
            m.sendall(b'quit\n')
            m.close()
        except OSError:
            pass
        try:
            p.wait(timeout=8)
        except Exception:
            pass
        try:
            if p.poll() is None:
                p.kill(); p.wait(timeout=5)
        except Exception:
            pass
        kill_qemu()
        time.sleep(1.0)

        # 3. FAT 簇完整性 (qcow2 → raw 解析)
        raw = os.path.join(BLD, 'smoke.raw')
        if os.path.exists(raw): os.remove(raw)
        subprocess.run(['qemu-img', 'convert', '-O', 'raw', qcow2, raw],
                       check=True, capture_output=True)
        sys.path.insert(0, BASE)
        from qemu_auto import parse_fat12   # 复用解析器
        files, data_sec, bps, spc = parse_fat12(raw)
        t1 = files.get('TEST1   TXT'); t2 = files.get('TEST2   TXT')
        if not t1 or not t2:
            log(f'FAIL: TEST1/TEST2 目录项缺失: t1={t1} t2={t2}'); ok = False
        else:
            overlap = set(t1[2]) & set(t2[2])
            if overlap:
                log(f'FAIL: 文件簇重叠 {overlap} — FAT 簇重复分配回归!'); ok = False
            else:
                log('OK: TEST1/TEST2 簇无重叠')
            blob = open(raw, 'rb').read()
            def rd(t):
                out = b''
                for cl in t[2]:
                    off = (data_sec + (cl - 2) * spc) * bps
                    out += blob[off:off + spc * bps]
                return out[:t[1]]
            c1 = rd(t1).decode('latin1', 'replace')
            c2 = rd(t2).decode('latin1', 'replace')
            log(f'  TEST1 内容: {c1!r}  TEST2 内容: {c2!r}')
            if 'HELLO' not in c1 or 'WORLD' not in c2:
                log('FAIL: 文件内容损坏'); ok = False
            else:
                log('OK: 文件内容正确')

        print('\n===== 完整串口日志 =====', flush=True)
        for l in serial.splitlines():
            log('  ' + l)

        log('\n=== ' + ('冒烟测试全部通过 ===' if ok else '冒烟测试失败 ==='))
        return 0 if ok else 1
    finally:
        try:
            if sock: sock.close()
        except Exception:
            pass
        try:
            p.kill(); p.wait(timeout=5)
        except Exception:
            pass
        kill_qemu()

if __name__ == '__main__':
    sys.exit(main())
