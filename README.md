# XEKernelOS

一个从零手写的 **x86 32 位保护模式操作系统内核**，严格遵循自定开发宪章 5 条准则。

---

## 特性

| 分类 | 功能 |
|------|------|
| **启动** | MBR → Stage2 三段启动，实模式→保护模式切换 |
| **图形** | VBE 1024×768×32bpp 线性帧缓冲，8×16 ASCII + 16×16 中文字体(27826字) |
| **中断** | PIC 重映射(0x20/0x28)，48 条目 IDT + ISR 分发器 |
| **定时器** | PIT 100Hz |
| **输入** | PS/2 键盘轮询，PS/2 鼠标驱动(灵敏度 1x+1.5x加速) |
| **内存** | 位图物理页分配器(4KB)，PSE 4MB 大页分页 |
| **磁盘** | ATA PIO 扇区读写(28-bit LBA) |
| **文件系统** | FAT12 双驱动：内核(bcache) + 用户态(ufs.cpp裸扇区) |
| **用户态** | Ring 3 + TSS，抢占式 O(1) 动态优先级调度 |
| **Shell** | 内核 Shell(19命令) + 用户 Shell(14命令)，双色中文界面 |

---

## 开发宪章 (5条准则)

| # | 准则 | 实现 |
|---|------|------|
| 1 | **接口统一** — 万物皆 fd | GFX→fd0 write/ioctl |
| 2 | **对象状态机** — fd 对应 C++ 类 | file/pipe/fb 四类型 |
| 3 | **响应优先** — 动态优先级 | O(1)调度+IRQ提升+衰减 |
| 4 | **令牌权限** — uint32_t caps | 继承+只减不增 |
| 5 | **内核边界** — FS 在用户态 | ufs.cpp完整FAT12读写 |

---

## 项目结构

```
src/
├── boot/              引导层 (NASM)
├── kernel/             内核核心 (C++)
│   ├── kernel.cpp, isr.cpp, idt.cpp, mm.cpp
│   ├── paging.cpp, task.cpp, syscall.cpp
│   ├── loader.cpp, user.cpp, panic.cpp
├── drivers/            硬件驱动 (C++)
│   ├── gfx.cpp, keyboard.cpp, mouse.cpp
│   ├── ata.cpp, bcache.cpp, pic.cpp, pit.cpp
│   ├── serial.cpp, font8x16.h, font_cn.h, font_cn_load.cpp
├── fs/                 文件系统
│   ├── fat12.cpp       内核 FAT12 (bcache)
│   └── ramdisk.cpp     内存文件系统
├── shell/              Shell
│   └── shell.cpp       内核 Shell (Ring 0)
├── user/               用户态 (Ring 3)
│   ├── ushell.cpp      用户 Shell
│   ├── ufs.cpp         用户态 FAT12 完整读写驱动
│   ├── usys.h          用户态 syscall 包装
│   └── user.ld         ��户态链接脚本
├── lib/                公共库
│   ├── types.h, ports.h, heap.cpp
│   ├── list.h, strutil.cpp, cpprt.cpp
└── linker.ld           内核链接脚本
```

---

## 构建运行

### Nix 开发环境

安装 Nix 并启用 `nix-command`、`flakes` 后，在项目根目录执行：

```bash
nix develop
make          # 编译内核、用户程序和磁盘镜像
make run      # QEMU 图形界面启动，需要可用的图形会话
```

也可以直接执行命令，无需进入交互式 shell：

```bash
nix develop --command make
nix develop --command python tools/desktop_shot.py  # 无图形会话的启动检查，输出 build/desktop_shot.png
```

如果 `flake.nix`、`flake.lock` 尚未加入 Git 跟踪，将上述 `nix develop` 改为 `nix develop path:.`。
未全局启用 Flake 时，可使用 `nix --extra-experimental-features 'nix-command flakes' develop path:.`。

环境提供 Make、NASM、Clang、LLD、LLVM 工具（含 `llvm-objcopy`）、Python 3（含 Pillow）和 QEMU（含 `qemu-img`）。
使用未包装的 Clang，避免宿主编译器包装器向 `i686-elf` 裸机构建注入参数；无需单独安装交叉 GCC。
同时设置 `QEMU`、`QEMU_IMG`，供默认使用 Windows 路径的辅助脚本使用。

首次进入 shell 时，Nix 下载并校验 GNU Unifont 16.0.04，在缺少字体输入时创建
`tools/unifont.hex.gz` 到 Nix store 的符号链接；已有字体文件不会被覆盖。
该路径已被 Git 忽略，请从项目根目录进入 shell，以便构建脚本找到字体。

`flake.lock` 固定 Nixpkgs 版本；首次使用需要联网获取依赖。更新依赖使用 `nix flake update`。
提供 x86_64/aarch64 的 Linux 和 macOS shell；实际构建及 QEMU 启动验证覆盖 x86_64 Linux。

### 手动安装依赖

```bash
# 依赖: Make, NASM, Clang++(i686-elf), LLD, llvm-objcopy, Python 3, QEMU
# 还需准备 tools/unifont.hex.gz（GNU Unifont 的 gzip 压缩 hex 字体）
make
make run
```

---

## Shell 命令

### 内核 Shell (`XEKernel\>`)
```
HELP     INFO     MEM      CLEAR    LS       CAT
CD       MKDIR    RMDIR    CP       MV       RM
CREATE   MOUSE    RUN      ECHO     REBOOT   SHUTDOWN
DISK ID  DISK R   DISK W   USERSH
```

### 用户 Shell (`XEKernel@Xek\路径>`)
```
HELP     CLEAR    LS       CD       ECHO     MKDIR
RMDIR    RM       MV       CREATE   TIME     TMP
RUN      EXIT
```

LS 输出格式: `类型 DIR/空格 大小(9位右齐) 日期(YYYY-MM-DD HH:MM) 文件名`

---

## License

MIT
