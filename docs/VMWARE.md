# 在 VMware 上运行 XEKernelOS

QEMU 之外的目标平台。启动链（MBR → stage2 → 内核）只依赖 BIOS 中断  
（INT 13h 读盘 / INT 10h VBE / INT 15h E820），因此 VMware 只要按  
**BIOS 固件 + IDE 硬盘** 配置就能直接跑。SATA/NVMe 与 EFI 都不支持  
（内核没有 AHCI / GOP 驱动）。

## 1. 生成 vmdk 与 vmx

```bash
source tools/env.sh          # MSYS2 clang64: nasm/clang/lld/qemu-img
make                         # 产出 build/xekernelos.img, build/disk.img
python tools/mkvmdk.py       # → build/vmware/{XEKernelOS.vmdk, -data.vmdk, .vmx}
```

若 VMware/BIOS 对小于 8MB 的"硬盘"表现异常（识别不到、不进启动流程），  
先把镜像补齐再转换：

```bash
python tools/mkvmdk.py --pad-mb 64
```

## 2. 打开虚拟机

用 VMware Workstation / Player 直接打开 `build/vmware/XEKernelOS.vmx`。  
模板已经写好全部关键配置，不需要在 GUI 里改硬件：

| 配置             | 值                      | 原因                                            |
| -------------- | ---------------------- | --------------------------------------------- |
| `firmware`     | `bios`                 | stage2 用 INT 10h/13h 实模式调用，EFI 下无这些服务         |
| `ide0:0`       | `XEKernelOS.vmdk`      | 启动盘：MBR + stage2 + 内核（必须是第一块盘，BIOS 从它引导）      |
| `ide0:1`       | `XEKernelOS-data.vmdk` | 数据盘：FAT12 卷（用户看到的文件系统）                        |
| `memsize`      | 256MB                  | 内核最低 31MB（内核堆固定在 16–31MB），低于此值会明确报错停机         |
| `mks.enable3d` | FALSE                  | 关 3D，保证走传统 VBE 线性帧缓冲（内核只认 linear framebuffer） |
| `serial0`      | 文件 `serial.log`        | 内核日志走 COM1，**排障第一现场**                         |

> 如果自己新建虚拟机：磁盘类型必须选 **IDE**（新版本 GUI 默认给 SATA/NVMe，  
> 需要手工在 .vmx 里加 `ide0:0.present = "TRUE"` 之类的条目）；  
> 固件必须选 **BIOS**（不要 EFI）；内存 ≥ 64MB。

## 3. 预期结果

1. VMware 开机约 3 秒后进入图形桌面（1024×768，顶部/底部任务栏 + 6 个图标）。
2. 按 `ESC` 退出桌面，进入用户态 Shell（`XEKernel@Xek\>`）。
3. `serial.log` 里应能看到探测结果：

```
mm: detected RAM 00000040MB       ← 探测到 256MB, 但 mm 上限截到 64MB (见 §5.2)
VBE: fb=0xE8000000 w=1024 h=0768 bpp=32 pitch=4096
ata: drv0 ident=K sec0=<MBR 字节> fat=n
ata: drv1 ident=K sec0=<EB 3C 90> fat=Y
ata: data drive = 1
```

（VMware SVGA 的线性帧缓冲在 `0xE8000000`，QEMU 是 `0xFD000000` —— 两者都动态读取，  
不要硬编码。VMware 还会把 `svga.vramSize` 改写成 128MB。）

## 4. 排障

QEMU 与 VMware 的差异集中在三处，代码里都做了适配并打了日志：

| 症状                                                              | 原因                                                                                                                                                                            | 日志/处理                                                                                                                                                               |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 开机黑屏，无任何输出                                                      | 视频模式没选到                                                                                                                                                                       | stage2 现在**枚举 VBE 模式列表**（不再硬编码 QEMU 的 `0x4144`），按 32bpp > 24bpp、1024×768 加权打分。若一个可用模式都没有，会**切回文本模式打印原因**：`no usable VBE mode (need 24/32bpp, >=640x480, linear FB)` |
| `VBE int 10h/4F00 failed`                                       | 机器/固件没有 VBE                                                                                                                                                                   | 需要支持 VBE 2.0 的 BIOS；VMware 都支持                                                                                                                                      |
| 卡在 `stage2` 之后，VMware 日志出现 `I/O out of range` / `out of bounds` | **内核加载循环曾依赖 BIOS 回填 AX**：调用前 `AX=0x42<<8\|请求扇区数`，SeaBIOS 成功时把 AH 清 0（所以 QEMU 正常），VMware 不清 → `sub cx,ax` 下溢、`add [dap_lba],eax` 每轮跳 1.7 万扇区 → 读到盘尾越界（实测 `ide0:0 numIOs=1647`） | 已修：只在**请求值**上推进循环，不看 BIOS 返回值；目标地址改 32 位线性值换算 `seg:off`（顺带去掉内核 >130KB 时的 offset 溢出）                                                                                 |
| 有引导标记但停在 `kernel`                                               | 内核读盘失败                                                                                                                                                                        | MBR/stage2 现在都逐行打印进度：`XEKernelOS MBR` → `stage2` → `kernel` → `vbe` → `pm`；最后一行就是失败点                                                                                |
| `ERR: Disk!`                                                    | 启动盘读不到                                                                                                                                                                        | boot.asm 现在用 BIOS 传入的 DL（不再硬编码 0x80）；确认 `.vmdk` 在 IDE 0:0                                                                                                           |
| 进不了 Shell、文件操作失败                                                | 数据盘没定位到                                                                                                                                                                       | `ata: drvN ... fat=Y/n` 会逐盘打印。内核自动选带 FAT 卷的那块（默认沿用从盘，主盘有 FAT 卷时切主盘）                                                                                                 |
| `mm: FATAL - RAM below kernel heap end`                         | 内存 < 31MB                                                                                                                                                                     | 把虚拟机内存调到 ≥ 64MB                                                                                                                                                     |
| 桌面能出但鼠标不动                                                       | PS/2 鼠标不被转发                                                                                                                                                                   | 关掉 `usb.present`（已默认关），改用 PS/2；VMware 默认给 BIOS 客户机提供 PS/2                                                                                                           |
| BIOS 画面后只有光标闪烁、连 `XEKernelOS MBR` 都没有                           | BIOS 未执行我们的引导扇区                                                                                                                                                               | MBR 现在带一张**活动分区表项**（旧版本分区表全 0，部分 BIOS 直接判为不可引导）；另外每次读盘前加了磁盘控制器 reset。若仍如此，检查虚拟机设置里硬盘是否在 **IDE 0:0**、启动顺序是否 Hard Drive 优先                                            |

## 5. 与 QEMU 的差异（实测结论）

| 项        | QEMU          | VMware                   | 状态                                                                                  |
| -------- | ------------- | ------------------------ | ----------------------------------------------------------------------------------- |
| VBE 模式号  | 专属 `0x4144`   | 各自编号不同                   | ✅ 改为枚举 + 打分；已用「强制 24bpp 回退」验证过非硬编码路径                                                |
| 物理内存     | 硬编码 64MB      | 视虚拟机设置                   | ✅ stage2 用 E820 探测写入 `0x514`；`-m 32` 实测报 31MB（SeaBIOS 为 ACPI 保留 128KB）              |
| 数据盘位置    | 从盘 `0xF0`     | 可能主盘                     | ✅ 逐盘 IDENTIFY + 读扇区 0 判 FAT BPB，自动选择                                                |
| 内核加载循环   | SeaBIOS 回填 AL | **不回填**                  | ✅ 曾因此读到盘尾越界（`ide0:0 numIOs=1647` + `I/O out of range`）。已改为按「本次请求扇区数」推进，不依赖 BIOS 返回值 |
| MBR 分区表  | 全 0 也能引导      | **要求有活动分区项**             | ✅ 已补一张活动分区表项（引导代码本身不读它）                                                             |
| 串口日志     | TCP           | 文件 `serial.log`（逐次写入即落盘） | ✅ 可用；已据此定位多个问题                                                                      |
| 24bpp 模式 | 可用            | 常用                       | ✅ gfx 本就支持；鼠标光标读写已按 bpp 处理                                                          |


### 5.1 2026-10-05 排查记录（历史结论，已由 §5.4 修正）

> 当时把故障归因于 VMware 特权栈切换并使用 Ring0 绕过；该归因错误。
> 后续隔离取证证明 CPU 已完成 Ring3→Ring0 切栈，失败发生在数据段尚未建立的
> C 分发器内。当前默认 `g_ring0_mode=false`、`g_ring3_irq_on=true`；
> 修复和回归见 §5.4。下文保留当时的取证过程，不作为当前运行限制。

现象：约 2/3 概率在 `loader: flat binary 38964B` 之后弹  
"virtual CPU ... shutdown state"（即 guest 三重故障），点 OK 重启后能正常进桌面。

已逐项排除（探针都在 ring0，异常可正常投递，不会静默）：

| 检查                                           | 结果                                                     |
| -------------------------------------------- | ------------------------------------------------------ |
| 页目录 / ESP0 / 入口 / 用户栈 / CR3 / EFLAGS         | 与 QEMU 完全一致                                            |
| TSS ESP0 目标页可写                               | ✅                                                      |
| 用户栈顶页可写                                      | ✅                                                      |
| `int 0x80` 门（`off/sel/flags`）                | ✅ 与 QEMU 逐位一致                                          |
| ring0 走 `int 0x80`（门 + 分发链路）                 | ✅ 正常返回 pid                                             |
| GDT 中 TSS 描述符 base/limit、TSS 内 ESP0/SS0、`tr` | ✅ 全部正确                                                 |
| 关键异常/定时器向量 IDT 门（0/6/8/13/14/0x20）           | ✅ 全部正确                                                 |
| TSS 页与页目录是否同页                                | ✅ 不同页                                                  |
| 串口日志被缓冲截断                                    | ✅ 排除（两次失败日志字节数不同，非同一缓冲边界）                              |
| **进 ring3 时关闭中断（IF=0）**                      | ❌ **仍然 triple fault** → 与中断投递无关 （⚠️ **此条作废，见 §5.1.1**） |

> ⚠️ **上表最后一行的证据无效**：所谓的「IF=0 测试」改的是  
> `task.cpp:219 task_start_user()`，而它**全工程没有任何调用点**（死代码）；  
> `task_launch_user()` 只有 `task.h` 的声明、没有定义也没有调用。  
> 也就是说那次测试从未真正执行过，**中断因素并未被排除**。已记入  
> `docs/REVIEW_2026-10-05.md`。

结论：故障点在 `iret` 特权切换本身，且 guest 侧状态已逐项验证有效、ring0 异常路径可用，  
因此**高度怀疑是 VMware 在 Windows Hypervisor Platform（Hyper-V）后端上对  
「PSE 4MB 页 + ring3 特权切换」的处理问题**（`vmware.log` 里有  
`Syncing WHP TSCs`，说明它走的是 WHP 而非 VMware 原生 monitor）。

#### 5.1.1 2026-10-05 实测：卡点确认，并指向宿主的 ULM 兼容模式

**卡点精确到一条指令。** 本次在 VMware Workstation 26.0.0（`F:\VMware\`）实测，  
`serial.log` 的**最后一行**就是 `enter_user:` 那条，之后再无任何输出：

```
loader: flat binary 42268B
launch: pd=0x010EE24C
launch: task pid=00000001 kstack=0x010EEC70
enter_user: cr3=0x0004F000 esp0=0x010F0C70 entry=0x00400000 ustack=0x00440000
                                       ← 到这里为止，下面本该是 ring3 sys=00000009
```

即死在 `enter_user_mode()` 末尾的 `pushl … / iret` 上（其后第一件事就该是桌面发的  
`SYS_GETFB`）。同时**屏幕和串口都没有任何 panic / `user #PF` / `signal: killing` 输出**  
—— 说明异常根本没投递到我们的 IDT，是 `iret` 特权切换本身不可恢复。

**guest 状态与 QEMU 逐值相同。** 用 QEMU 的 VMware SVGA 显卡跑同一份镜像，得到的  
`pd / kstack / cr3 / esp0 / entry / ustack` **和上面 VMware 的值一字不差**  
（连堆指针都相同，说明 kmalloc 布局完全一致）。也就是说**不是我们这几行 C++ 的状态问题**。

**唯一确认的 guest 差异是显存地址**：VMware `fb=0xE8000000`，QEMU `fb=0xFD000000`  
（→ 页目录里 4MB PSE 页落在 PDE[928] vs PDE[1012]）。这一项在 §5.2 的重跑里没被覆盖。

**宿主侧证据（决定性）：**

```
vmware.log:65   IOPL_Init: Hyper-V detected by CPUID
vmware.log:298  Monitor Mode: ULM
vmware.log:944  Syncing WHP TSCs took 43 us.
```

`Monitor Mode: ULM` = VMware **没有**用自己的原生 monitor（BT/VT-x），而是退到了  
Hyper-V 之上的兼容模式 —— 因为 Windows hypervisor 占着 VMX root，VMware 拿不到 VT-x。  
宿主实测状态（`bcdedit` / `Get-WindowsOptionalFeature`）：

| 项                                   | 值           | 含义                               |
| ----------------------------------- | ----------- | -------------------------------- |
| `hypervisorlaunchtype`              | `Auto`      | Windows hypervisor **在开机时加载**    |
| `VirtualMachinePlatform`            | **Enabled** | WSL2 依赖它 → 这就是 hypervisor 被拉起的原因 |
| `Hyper-V-All` / `Microsoft-Hyper-V` | Disabled    | 没装完整 Hyper-V                     |
| `HypervisorPlatform`（WHP）           | Disabled    | WHP 功能没开，但 hypervisor 已在跑        |

**首选判定实验（最可能直接解决，5 分钟、可逆）：**

```
# 管理员 cmd / PowerShell：让 Windows hypervisor 别加载
bcdedit /set hypervisorlaunchtype off
# 重启（必须），然后直接开 XEKernelOS.vmx
# 验证：vmware.log 里应变成 Monitor Mode: VT / BT，而不是 ULM

# 测完恢复（WSL2 需要它）
bcdedit /set hypervisorlaunchtype auto
```

⚠️ **代价**：`hypervisorlaunchtype off` 期间 **WSL2 起不来**（本机 WSL Ubuntu 上的  
qemu-mcp-server 会一起停），Windows Sandbox / VBS-HVCI / Credential Guard 也不工作。  
关掉它只是**一次重启的开关**，随时 `auto` 回去，不改功能开关，风险很低。

同类案例（VMware 社区，对话框文案与 `IOPL_Init: Hyper-V detected by CPUID` +  
`Monitor Mode: ULM` 完全同签名）：答复明确指出「宿主开着 Hyper-V/WSL2/VBS 是  
问题根源」，提问者**关掉 VBS/Hyper-V 后该 VM 恢复正常**：  
<https://community.broadcom.com/communities/community-home/digestviewer/viewthread?GroupId=7171\\\&MessageKey=bcaaca79-066d-4f52-b364-8e11f8453e62\\\&CommunityKey=fb707ac3-9412-4fad-b7af-018f5da56d9f>

**如果关掉后仍然 triple fault**，那说明确实是 guest 侧问题，此时按 §5.1 表里  
「PSE 4MB 页 + ring3」这条线做二分（把用户页目录里 0–64MB 的 PSE 页统一改成 4KB 页，  
消除「同一页目录里 PSE 与 4KB 混合」），并配合一个「ring3 探针程序」定位。

**务实提醒**：VM 三重故障 = 虚拟机直接断电，所以在当前宿主配置下  
**XEKernelOS 在 VMware 上是完全不可用的**（不是"某些功能不可用"，是开机即死）。  
等不及改宿主的话，先用 QEMU（`make run`，本仓已验证全绿）。

#### 5.1.2 2026-10-05 第二轮：宿主换 monitor 后仍然失败 → 转到 guest 侧取证

**宿主侧结论已闭环（不是宿主的原因）**：`bcdedit /set hypervisorlaunchtype off` + 重启后  
`HypervisorPresent=False`、`Monitor Mode` 从 `ULM` 变成 **`CPL0`**（原生 monitor 生效），  
**但崩溃点和之前一模一样**，`serial.log` 仍然停在 `enter_user:` 那一行。  
（副作用：这段时间 WSL2 起不来；**已于 2026-10-05 19:23 `bcdedit /set hypervisorlaunchtype auto`
恢复**，下次重启后 WSL2 / qemu-mcp-server 回来。）

于是转回 guest 侧。注意一个之前所有人都忽略的前提：**`loader: flat binary 42268B`  
只证明了长度对，内容从来没校验过**。如果 ATA PIO 把 42KB 的用户程序读花了，  
garbage 里随便一条 `iret`/`sysret` 就能在**零输出**的情况下三重故障 —— 这正好解释  
「没有任何异常投递」「卡点每次都在同一处」「QEMU 完全不复现」。所以在内核里加了两组插桩：

**A. 装载完整性指纹**（`loader.cpp`，读到 0x400000 后立刻算）：

```
img: head=<头16字节hex> fnv_file=0x... fnv_map=0x... map_sz=0x...
```

**B. `iret` 前完整取证转储**（`user.cpp`，`pd->load()` 之后、压栈之前）：

```
pre-iret: esp=... cr0=... cr4=... eflags=...
pre-iret: gdtr=... lim=... idtr=... lim=... tr=... tss=...
pre-iret: GDT[4] SS/0x23 = <8字节原始值>      ← 旧排查从没验过这两个
pre-iret: GDT[5] CS/0x2B = <8字节原始值>
pre-iret: GDT[6] TSS/0x30= <8字节原始值>
pre-iret: map EIP/USTK/KSTK/GDT/IDT/TSS  pde=... pte=... PA=...
                                              ↑ 在**当前用户页目录**下走一遍页表
```

**QEMU 基准（已存档 `build/qemu_probe_baseline.txt`，全部正确）**：

```
img: head=5589E553575681ECB80500008D9D3CFF fnv_file=0xA2237416 fnv_map=0xCA3B7416 map_sz=0x0001A51C
pre-iret: esp=0x0009F8E3 cr0=0x80000011 cr4=0x00000010 eflags=0x00000012
pre-iret: gdtr=0x000102FF lim=0x00000037 idtr=0x0003BB10 lim=0x000007FF tr=0x00000030 tss=0x0004E000
pre-iret: GDT[4] SS/0x23 = FFFF000000F2CF00      (DPL=3/W=1/G=1  正确)
pre-iret: GDT[5] CS/0x2B = FFFF000000FACF00      (DPL=3/exec/G=1 正确)
pre-iret: GDT[6] TSS/0x30= 670000E0048B4000      (limit=103 base=0x4E000 busy 正确)
pre-iret: map EIP  0x00400000 pde=0x00050007 pte=0x00400007 PA=0x00400000
pre-iret: map USTK 0x00440000 pde=0x00050007 pte=0x00000000  !!PTE-NOT-PRESENT   ← 预期的
pre-iret: map KSTK(当前 esp) pde=0x000000E3 PA=0x0009F8E3 (4MB)
pre-iret: map GDT  pde=0x000000E3 PA=0x000102FF (4MB)
pre-iret: map IDT  pde=0x000000E3 PA=0x0003BB10 (4MB)
pre-iret: map TSS  pde=0x000000E3 PA=0x0004E000 (4MB)
```

（`USTK 0x440000` 那一行 NOT-PRESENT 是**正确**的：栈区映射的是 `0x430000–0x43FFFF`，  
`0x440000` 是栈顶「开区间」，第一次 push 落在 `0x43FFFC` 才是已映射页。）

**怎么用**：在 VMware 上跑一次，把 `serial.log` 里 `img:` 和 `pre-iret:` 这些行  
和上面逐行对比：

| 对比结果                                      | 结论                                                                |
| ----------------------------------------- | ----------------------------------------------------------------- |
| `img:` 指纹和基准**不一致**                       | 用户程序**读盘被破坏** → 去查 ATA PIO 读路径（这是首要嫌疑）                            |
| `pre-iret:` 里出现 `!!NOT-PRESENT` 或描述符与基准不同 | guest 侧真的有问题 → 按那一行定位                                             |
| **全部逐值一致，然后仍然断电**                         | guest 状态可证明是对的 → 故障在 CPU/VMware 前端的 `iret` 或"刚进 ring3 立刻投递中断"这条路上 |

第三种情况下，下一个实验是「进 ring3 时用 IF=0」—— 因为 `load_flat_binary` 里 `cli`  
之后的十几毫秒（串口打印本身就要 ~10ms/百字节）里 PIT 必然已经把 IRQ0 挂在 PIC 的  
IRR 上，`iret` 一开 IF，CPU 会在**第一条用户指令之前**立刻投递它。  
（⚠️ 注意：§5.1 表里那条「IF=0 测试」是**死代码**，从未真正跑过，见上方警告。）

建议的 A/B 验证（任一即可判定）：

1. 关闭 Windows 的「虚拟机平台 / Hyper-V」（`bcdedit /set hypervisorlaunchtype off` + 重启），  
   让 VMware 用原生 monitor 再试；
2. 换一个宿主验证同一份镜像：用 `VBoxManage convertfromraw build/xekernelos.img a.vhd`  
   在 VirtualBox 里跑，或直接往真机 U 盘写入；
3. 若确认是宿主问题，本内核侧无需改动 —— 也欢迎提供 VMware 版本号与  
   `vmware.log` 中故障前后的完整片段继续定位。

**注意**：QEMU 侧全流程（含串口冒烟 10 项断言）始终全绿，上述问题只出现在 VMware/WHP 上。

#### 5.1.3 2026-10-05 第三~六轮：逐项排除的完整记录（当前状态）

**已排除清单** —— 每一条都是用插桩或替换变量实测掉的，不是推测：

| 假设                          | 怎么排除的                                        | 结果                                                                                 |
| --------------------------- | -------------------------------------------- | ---------------------------------------------------------------------------------- |
| 宿主 hypervisor（ULM/WHP 兼容模式） | `bcdedit /set hypervisorlaunchtype off` + 重启 | `Monitor Mode: ULM` → **`CPL0`**、`HypervisorPresent=False`，**现象不变**                |
| 用户程序读盘被破坏                   | 装入后立刻算头 16 字节 + 两个 FNV-1a 指纹                 | 与 QEMU 逐位一致（`fnv_file=0xA2237416 fnv_map=0xCA3B7416`）                              |
| GDT 描述符非法                   | 转储 GDT[4]/[5]/[6] 的原始 8 字节                   | `FFFF000000F2CF00` / `FFFF000000FACF00` / `670000E0048B4000`，与 QEMU 完全一致，DPL=3 合法  |
| 页表映射错                       | 在**当前用户页目录**下逐个走页表                           | EIP `pde=0x50007 pte=0x400007`；KSTK/GDT/IDT/TSS 落在 PDE[0] 的 4MB 页（`pde=0xE3`）；逐值一致 |
| TSS ESP0 目标页没映射             | 新增转储条目                                       | `map ESP0(TSS栈顶) pde=0x010000E3 PA=0x010F0C70 (4MB)` → **映射正常**                    |
| 挂起 IRQ0 在 `iret` 后被立刻投递     | `g_ring3_irq_on=false`（IF=0 进 ring3）         | **仍然 `Triple fault.`**                                                             |

> **排障方法论**：判定"崩没崩"要读 `vmware.log` 里的 **`Triple fault.`** 行，  
> **不要看弹窗** —— 那个对话框一旦勾过 "Do not show this message again" 就不再出现，  
> 很容易把"崩了"误判成"只是卡住了"（这次踩过两次）。

**ring3 桩实验**（零内核改动 —— 直接替换 `build/desktop.bin` 头部，  
原文件备份为 `build/desktop.bin.orig`）：

- **桩 A**：`mov eax,1` → `mov ebx,msg` → `mov ecx,16` → `int 0x80` → `jmp $`  
  → VMware 上**仍然 `Triple fault.`**，串口既无 `ring3 sys=` 也无 `RING3-STUB-OK!`。  
  **桩的第一条指令是 `mov`，`mov` 不可能出错** ⇒ 崩溃点只剩两个：  
  ① **`iret` 本身**；② **紧接的 `int 0x80` 的 ring3→ring0 特权栈切换**。
- **桩 B**：前 2 字节改成 `EB FE`（纯 `jmp $`，既不碰内存也不碰 syscall）  
  → 用来把上面两者**分离开**。

**桩 B 的判读**：

| `vmware.log`                     | 含义                                                          | 下一步                                                       |
| -------------------------------- | ----------------------------------------------------------- | --------------------------------------------------------- |
| **没有 `Triple fault.`**（画面冻住是预期的） | **`iret` 成功进了 ring3** ⇒ 元凶是 **`int 0x80` 的 ring3→ring0 投递** | 深挖 TSS 特权栈切换；也正好解释"桌面第一个 syscall 之后就死"                    |
| **有 `Triple fault.`**            | **`iret` 本身在 VMware 前端上失败**                                 | 改用 `retf`（远返回）进 ring3；或把用户页目录的页粒度统一成 4KB（消除 PSE 与 4KB 混用） |

**收尾状态（2026-10-05 19:23 已全部归位）**：

- `build/desktop.bin` **已还原**（`desktop.bin.orig` 备份保留，桩已移除）。
- `g_ring0_mode = true`（Ring0 兼容模式，VMware 上靠它才能出桌面）；
  `g_ring3_irq_on = false`（IF=0，Ring0 模式下无实际影响）。
- `hypervisorlaunchtype` **已改回 `auto`** → 下次重启后宿主 hypervisor 恢复，
  WSL2 / qemu-mcp-server 可用（**两种 monitor 模式下本 bug 表现完全一致，恢复它不影响复现**）。
- 渲染问题（图标名/标题栏文字/三大金刚键/右键菜单/拖影/`CAT` 乱码）**已定位并修复**，
  根因是 ring0 模式下自加的 `r->user_esp` 补丁踩坏程序栈上的 ioctl 结构体（详见
  `docs/ISSUES_2026-10-05.md`），**与 VMware 无关**（QEMU 上同样复现）。

#### 5.1.4 结论与 Ring0 兼容模式（2026-10-05 当日收口）

**桩 B（纯 `jmp $`）的结果是决定性的**：VMware 上 **没有 `Triple fault.`**，  
CPU 在 ring3 空转、VM 一直活着。而桩 A（多一次 `int 0x80`）必崩。两者只差那一次  
**ring3→ring0 的特权级切换式中断投递**。至此结论闭合：

| 环节 | VMware 实测 |
|---|---|
| `iret` 进 ring3 | ✅ **成功** |
| ring3→ring0 的 `int 0x80` 投递（IDT 门 + TSS 切栈） | ❌ **三重故障** |

而这条投递路径需要的四样东西 —— IDT 门、TSS 描述符、`SS0=0x10`、ESP0 目标页映射  
—— **全部与 QEMU 逐值相同且验证正确**。也就是说：**同样的表、同样的状态，  
QEMU 能投递，VMware 直接三重故障。**

> 当时推断“没有 `ring3 sys=` 就说明没有完成特权切换”，此推断无效：
> 在它之前还有 `c_isr_handler()` 的 `r->vec` 读取，空 DS 会先触发 #GP。
> 2026-10-07 隔离取证已确认切栈成功，见 §5.4。

**当时的绕过：Ring0 兼容模式**（现在仅保留诊断开关，默认 `false`）。

原理：不再用 `iret` 进 ring3，而是**远跳转** `ljmpl *(CS=0x18)`，程序以 **ring0** 运行。  
这样后续 `int 0x80` 发生在**同级**，CPU 不会做特权栈切换 —— 正好绕开故障点。

```c
if (g_ring0_mode) {
    ... fj.sel = 0x18;
    __asm__ volatile("movl %0, %%esp\n\tmovl %1, %%eax\n\tljmpl *(%%eax)\n\t" ...);
}
```

**实测（QEMU）**：桌面正常渲染、时钟在走、无 panic、VM 不会被三重故障打死。

**代价（必须知道）**：

| 项 | 说明 |
|---|---|
| 内存保护 | **没有**。程序跑在 ring0，可直接访问内核。这只是一个兼容/演示模式。 |
| `RUN <程序>` 不可用 | **结构性限制**：CPL0 下 `iretd` 不弹 SS/ESP，而本内核的任务切换正是靠 ring3 帧里的 SS/ESP 恢复目标任务的应用栈 → fork 出的子进程必然跑飞。已让 `sys_fork` 在这种情况下**干净返回 -1**（不再 panic）。 |
| 完整功能 | 要看完整 ring3 行为请用 **QEMU**（`make run`，本仓全绿）或 VirtualBox / 真机。 |

> **2026-10-05 补充**：初次在 VMware 上看到桌面时出现的「窗口标题栏文字/三大金刚键  
> 不显示、图标名不显示、右键菜单不弹、拖动有拖影」**不是 VMware 的问题**，  
> 而是 Ring0 兼容模式里一处补丁错误（往同级帧不存在的 `user_esp` 槽位写值，  
> 踩坏了程序正在构造的 ioctl 结构体）。已在 `isr.cpp` 修正，QEMU 截图与 ring3  
> 基准逐像素一致。完整记录见 `docs/ISSUES_2026-10-05.md`。  
> 冒烟回归里现在只剩 `RUN` 系列失败（即上表的结构性限制）。

当前默认已是真正 Ring3，无需开启兼容模式；完整上下文修复见 §5.4。

### 5.2 2026-10-05 复验：QEMU 侧仍无法复现

用 QEMU 的 **VMware SVGA 显卡**（`-vga vmware`）与 256MB 内存重跑，试图在 QEMU 里复现  
VMware 的 iret 三重故障：

```
qemu-system-i386 -m 256 -vga vmware -boot order=c \
  -drive if=ide,index=0,format=raw,file=build/xekernelos.img \
  -drive if=ide,index=1,format=raw,file=build/disk.img \
  -display none -no-reboot -serial file:build/qemu_svga.log
```

结果：**未复现**。串口日志显示 ring3 正常进入并持续运行到超时（50s）：

```
loader: flat binary 42268B
launch: pd=0x010EE24C
launch: task pid=00000001 kstack=0x010EEC70
enter_user: cr3=0x0004F000 esp0=0x010F0C70 entry=0x00400000 ustack=0x00440000
ring3 sys=00000009 pid=00000001      ← GETFB
ring3 sys=00000004 pid=00000001      ← OPEN
ring3 sys=0000002E pid=00000001 ...  ← IOCTL 绘图，持续
```

注：QEMU 的 VMware SVGA 把线性帧缓冲放在 0xFD000000（不是 VMware 的 0xE0000000），  
因此这一项环境差异**没有被覆盖**；但 ring3 入口、TSS、页表、IDT 这条链路在 QEMU 侧  
（含 SVGA 路径）是稳定的，与 §5.1 的判断一致 —— 故障点更可能在宿主 hypervisor。

另注：`mm` 会把探测到的 RAM **上限截到 64MB**（`MEM_TOP_MAX`，内核恒等映射只有  
前 64MB），所以虚拟机给 256MB 与给 64MB 对内核是等价的 —— 调大内存不改变行为。

### 5.3 本机 vmrun 无法使用（VMware Workstation 26.0.0）

本机 `F:\VMware\vmrun.exe` 是 **32 位** 程序，依赖同为 32 位的 `vix.dll`（存在），  
但同目录的 `vmwarebase.dll` **只有 x64 版本** —— 缺 32 位兼容层，运行即报  
`0xC000007B`（STATUS_INVALID_IMAGE_FORMAT）。`vmplayer.exe` 同为 32 位，同样受影响。

结论：这台机器上**没法用命令行自动开关虚拟机**，请手动打开  
`build/vmware/XEKernelOS.vmx`（GUI 的 `vmware.exe` 是 x64，正常）。若后续要做  
CI / 自动化开关机，需要换用 x64 的接口或补装 32 位组件。

### 5.4 2026-10-07 Ring3 段上下文修复

隔离取证的首次 IRQ0 已从 CS=0x2B 切到 SS=0x10 的任务内核栈，但
DS/ES/FS/GS 全为空；C 分发器首次经 DS 读取 `r->vec` 触发 #GP(0)。
关闭用户 IF 后，`int 0x80` 同样故障。QEMU/TCG 对空 DS 的行为不能证明
硬件正确性，也不能把异常处理器里的再次 #GP 直接称作 #DF。

统一帧协议：

| 字段 | 字节偏移 |
|---|---:|
| GS / FS / ES / DS | 0 / 4 / 8 / 12 |
| edi / esi / ebp / _esp | 16 / 20 / 24 / 28 |
| ebx / edx / ecx / eax | 32 / 36 / 40 / 44 |
| vec / err_code | 48 / 52 |
| eip / cs / eflags | 56 / 60 / 64 |
| user_esp / user_ss（仅跨特权） | 68 / 72 |

Ring0 活帧只有 68B，不能读写不存在的用户尾部；Ring3 完整帧为 76B。
ISR 用零扩展的 32 位槽保存段，调用 C 前建立四段=0x10 并 `cld`；
唯一的 `isr_return` 出口恢复保存段和原 EFLAGS，不强行修正用户 null selector。
调度和 fork 继承真实段状态；exec 与直接 Ring3 入口建立四段=0x23。
`_esp` 与任务保存的 `esp` 指向帧内 `vec`，`popa` 跳过该槽，不用它切栈。
TSS 描述符 flags 修正为 0x00；默认保留真正 Ring3、IRQ 与抢占调度。

`make -B` 验证完整头文件迁移；内核规则生成 `.d` 依赖，后续布局变更会重编译
`panic.cpp` 等消费者。`SEGTEST.BIN`/`SEGEXEC.BIN` 用同一 NASM 源构建，
断言 null 段 syscall 原样返回、用户 DF 保留、fork/sleep 段继承、wait/reaper
与 exec 段重置。`tools/smoke_serial.py` 每次运行独立检查 PASS/FAIL 和后续 ECHO。

硬件复验必须使用隔离镜像：KVM (`QEMU_ACCEL=kvm`) 和 TCG (`tcg`) 顺序冒烟；
VMware 使用独立目录的 BIOS+IDE 测试机，不覆盖 `build/vmware`，观察实际控制台、
两次 SEGTEST、两次 GFXDEMO、SPIN 子进程完成后 Ctrl+C 与 `ECHO RING3OK`。
另外单独验证 IF=0 的首批 GETFB/OPEN/IOCTL，以及故意空 DS 访存触发的
`KERNEL PANIC: #GP`；后者按既有 panic 策略停机，不要求恢复 Shell。
成功日志不得混入故障探针；只停止本次新建 VM，不用需要 guest Tools 的
`vmrun captureScreen`。本节与 §5.3 的 Windows `vmrun` 限制不同，Linux 验证可用。

#### 控制台 Ctrl+C 队列修复

真实 PS/2 启动 SPIN 会留下 Return break；旧 `Keyboard::ctrl_c()` 只检查队首，
该字节或普通输入会挡住后面的 Ctrl+C，导致串口启动的 smoke 通过而 VMware
键盘启动的 SPIN 无法终止。用户批准追加此修复。

PIT 在 IF=0 下扫描整个软件环形队列，只移除按时间顺序确认为 Ctrl+C 的 C make，
原地保留其余扫描码和 Ctrl press/release 的顺序。`getchar()` 消费 Ctrl 转换时
更新 `ctrl_`；该字段表示队首之前的状态，PIT 使用局部状态扫描，不提前修改它。
否则重扫旧普通 C 或漏掉已被字符读取消费的 Ctrl release，会丢字或泄漏控制状态。
该实现无动态分配，保留普通输入，不更改 SIGINT、IRQ 或抢占策略。

现有 smoke 追加独立 PS/2 区间：键盘输入 `run spin.bin`，等待 child 完成，
排入 `echo ps2ok` 但不提交，Ctrl+C 后再 Return。该区间必须依次出现 RUN、
child done、signal killing、准确的 ECHO 读取和正文，不能使用串口 SPIN 的标记
满足断言。本机 KVM 已复现修复前无法终止，修复后该场景及原有 smoke 全部通过。

#### 本机硬件验收记录

2026-10-07，Linux x64，VMware Workstation 25.0.1 build 25219725；
正式配置保持 `g_ring0_mode=false`、`g_ring3_irq_on=true`。

- 强制完整重编译成功；反汇编核对 ISR 保存/恢复及两个直接 Ring3 入口。
- KVM 完整 smoke 退出码 0；两次段回归独立通过，桌面中文图标和时钟实际可见，
  原有文件、LFN、fork/exec/wait、抢占与新增 PS/2 输入保留回归通过。
- KVM 完成后顺序运行 TCG 完整 smoke，退出码 0；两者均输出
  `=== 冒烟测试全部通过 ===`，并各自通过新增的 PS/2 队列回归。
- VMware 隔离机实际控制台时钟跨越 14:31:12 → 14:31:15；两组段回归、两次
  GFXDEMO、SPIN 的三次 child tick/完成均通过。随后 Ctrl+C 发出 SIGINT，
  唤醒 Shell；串口和屏幕均显示 `ECHO RING3OK` 正文，后续 `echo afterok` 响应。
  本次隔离机当前及存在的轮转日志无 `Triple fault.`。
- KVM 和 VMware 的独立 IF=0 诊断均完成首批 GETFB/OPEN/IOCTL，未发生 C 入口 #GP。
- KVM 和 VMware 的独立空 DS 访存探针均在串口及实际画面报告
  `KERNEL PANIC: #GP`，`EIP=0x00400004`、`CS=0x0000002B`；按原策略停机，
  VMware 日志无 `Triple fault.`。

成功运行、IF=0 和故意异常使用不同镜像与日志；只关闭本次创建的测试 VM，
未覆盖用户原有 VMware 镜像或停止用户原有 VM/QEMU。截图及串口证据保留在
本次隔离目录的 `accepted/`、`if0/`、`fault/` 下。


