"""Build FAT12 disk image with demo files for XEKernelOS."""

import struct
import os
import datetime

SECTOR_SIZE = 512
DISK_SIZE = 1474560  # 1.44 MB

# BPB fields (must match run.py)
BPS = 512
SPC = 1
RESERVED = 1
NUM_FAT = 2
ROOT_ENTS = 224
FAT_SIZE = 9
SECTORS_PER_TRACK = 18
NUM_HEADS = 2

ROOT_SEC = RESERVED + NUM_FAT * FAT_SIZE          # 19
DATA_SEC = ROOT_SEC + (ROOT_ENTS * 32 + BPS - 1) // BPS  # 33
ROOT_SECTORS = DATA_SEC - ROOT_SEC                   # 14

# Demo files: (8.3 name, content_bytes)
DEMO_FILES = []

# 长文件名文件: (长名 UTF-8, 8.3 短名 11 字节 ASCII, 内容) → 额外生成 LFN 槽
LONG_FILES = []

def add_text(name_83, text):
    DEMO_FILES.append((name_83, text.encode('utf-8')))

def add_binary(name_83, path):
    with open(path, 'rb') as f:
        DEMO_FILES.append((name_83, f.read()))

def add_text_long(long_name, name_83, text):
    LONG_FILES.append((long_name, name_83, text.encode('utf-8')))

add_text("README  TXT", "欢迎使用 XEKernelOS！\n\n这是一个示例文件。\n你可以用 CAT README.TXT 查看我。\n")
add_text("HELLO   TXT", "Hello from XEKernelOS!\n\n系统基于 x86 32 位保护模式。\n支持 FAT12 文件系统。\n")
add_text("DEMO    TXT", "XEKernelOS 演示文件\n==================\n\n创建文件: CREATE test.txt 内容\n查看文件: CAT test.txt\n删除文件: RM test.txt\n复制文件: CP a.txt b.txt\n重命名:   MV old.txt new.txt\n创建目录: MKDIR mydir\n删除目录: RMDIR mydir\n\n祝你使用愉快！\n")

# 长文件名 / 中文名 (VFAT LFN) — 供 LFSTEST.BIN 与文件管理器验证读侧 LFN
add_text_long("中文文档.txt", "ZHWDOC~1TXT",
              "XEK-LFN-CONTENT-OK 这是中文长文件名的正文。\n")
add_text_long("LongFileName.txt", "LONGFI~1TXT",
              "XEK-LFN-LONGNAME-OK long file name body.\n")

# Batch script demo
script_dir = os.path.dirname(__file__)
demo_bat = os.path.join(script_dir, 'demo.bat')
if os.path.exists(demo_bat):
    with open(demo_bat, 'rb') as f:
        DEMO_FILES.append(("DEMO    BAT", f.read()))

# Binary user program
hello_bin = os.path.join(os.path.dirname(__file__), '..', 'build', 'hello.bin')
if os.path.exists(hello_bin):
    add_binary("HELLO   BIN", hello_bin)

# ELF test program
test_elf = os.path.join(os.path.dirname(__file__), '..', 'build', 'test_elf.elf')
if os.path.exists(test_elf):
    add_binary("TEST_ELFELF", test_elf)
else:
    print(f"Warning: {test_elf} not found, skipping")

# User demo program
demo_bin = os.path.join(os.path.dirname(__file__), '..', 'build', 'demo.bin')
if os.path.exists(demo_bin):
    add_binary("DEMO    BIN", demo_bin)
else:
    print(f"Warning: {demo_bin} not found, skipping")

# GUI desktop program
desktop_bin = os.path.join(os.path.dirname(__file__), '..', 'build', 'desktop.bin')
if os.path.exists(desktop_bin):
    add_binary("DESKTOP BIN", desktop_bin)
else:
    print(f"Warning: {desktop_bin} not found, skipping")

# Preemption / signal test program
spin_bin = os.path.join(os.path.dirname(__file__), '..', 'build', 'spin.bin')
if os.path.exists(spin_bin):
    add_binary("SPIN    BIN", spin_bin)
else:
    print(f"Warning: {spin_bin} not found, skipping")

# LFN read-path test program
lfstest_bin = os.path.join(os.path.dirname(__file__), '..', 'build', 'lfstest.bin')
if os.path.exists(lfstest_bin):
    add_binary("LFSTEST BIN", lfstest_bin)
else:
    print(f"Warning: {lfstest_bin} not found, skipping")

# Segment context inheritance and exec-reset regression.
for name_83, filename in (("SEGTEST BIN", "segtest.bin"),
                          ("SEGEXEC BIN", "segexec.bin")):
    binary = os.path.join(os.path.dirname(__file__), '..', 'build', filename)
    if os.path.exists(binary):
        add_binary(name_83, binary)
    else:
        print(f"Warning: {binary} not found, skipping")

# User shell as launchable program (GUI Terminal icon)
ushell_bin = os.path.join(os.path.dirname(__file__), '..', 'build', 'ushell.bin')
if os.path.exists(ushell_bin):
    add_binary("USHELL  BIN", ushell_bin)
else:
    print(f"Warning: {ushell_bin} not found, skipping")

# Graphics demo programs
gfx_demo = os.path.join(os.path.dirname(__file__), '..', 'build', 'gfx_demo.bin')
if os.path.exists(gfx_demo):
    add_binary("GFXDEMO BIN", gfx_demo)

bounce = os.path.join(os.path.dirname(__file__), '..', 'build', 'bounce.bin')
if os.path.exists(bounce):
    add_binary("BOUNCE  BIN", bounce)

testgfx = os.path.join(os.path.dirname(__file__), '..', 'build', 'testgfx.bin')
if os.path.exists(testgfx):
    add_binary("TESTGFX BIN", testgfx)


def name_to_83(name_str):
    """Convert "README  TXT" to 11 bytes."""
    b = bytearray(11)
    for i in range(11):
        b[i] = ord(' ')
    for i, ch in enumerate(name_str):
        if i < 11:
            b[i] = ord(ch)
    return bytes(b)


def lfn_checksum(short83):
    """短名 11 字节 → LFN 校验和 (微软规范)."""
    s = 0
    for b in short83:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


LFN_CHAR_OFFS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)

def make_lfn_slots(long_name, short83):
    """长名 → LFN 目录项列表 (物理顺序: N|0x40, N-1, ..., 1)."""
    raw = long_name.encode('utf-16-le')
    chars = [raw[i] | (raw[i + 1] << 8) for i in range(0, len(raw), 2)]
    n = (len(chars) + 12) // 13
    if n > 20:
        raise ValueError(f'长名过长: {long_name}')
    cksum = lfn_checksum(short83)
    slots = []
    for i in range(n, 0, -1):                 # 物理顺序从高序号开始
        chunk = chars[(i - 1) * 13: i * 13]
        chunk = chunk + [0x0000] * (13 - len(chunk))
        e = bytearray(32)
        e[0] = i | (0x40 if i == n else 0)
        e[11] = 0x0F                          # LFN 属性
        e[12] = 0
        e[13] = cksum
        struct.pack_into('<H', e, 26, 0)      # 簇号必须为 0
        for k, ch in enumerate(chunk):
            struct.pack_into('<H', e, LFN_CHAR_OFFS[k], ch)
        slots.append(bytes(e))
    return slots


def build_disk(output_path):
    img = bytearray(DISK_SIZE)
    now = datetime.datetime.now()

    def make_fat_time():
        """Encode current time as FAT12 time (hh:mm:ss/2)."""
        return (now.hour << 11) | (now.minute << 5) | (now.second // 2)

    def make_fat_date():
        """Encode current date as FAT12 date (y-1980:mon:day)."""
        return ((now.year - 1980) << 9) | (now.month << 5) | now.day

    # ---- BPB ----
    img[0:3] = b'\xEB\x3C\x90'
    img[3:11] = b'XEKERNEL'
    struct.pack_into('<H', img, 11, BPS)
    img[13] = SPC
    struct.pack_into('<H', img, 14, RESERVED)
    img[16] = NUM_FAT
    struct.pack_into('<H', img, 17, ROOT_ENTS)
    struct.pack_into('<H', img, 19, 2880)
    img[21] = 0xF0
    struct.pack_into('<H', img, 22, FAT_SIZE)
    struct.pack_into('<H', img, 24, SECTORS_PER_TRACK)
    struct.pack_into('<H', img, 26, NUM_HEADS)
    struct.pack_into('<I', img, 28, 0)
    img[32] = 0x29
    struct.pack_into('<I', img, 33, 0x20250720)
    img[38:49] = b'XEKernelOS '
    img[54:61] = b'FAT12   '

    # ---- FAT tables ----
    fat = bytearray(FAT_SIZE * BPS)
    fat[0:3] = b'\xF0\xFF\xFF'   # entries 0,1 reserved
    fat2 = bytearray(fat)
    img[RESERVED * BPS: RESERVED * BPS + len(fat)] = fat
    img[(RESERVED + FAT_SIZE) * BPS: (RESERVED + FAT_SIZE) * BPS + len(fat2)] = fat2

    # ---- Write files ----
    current_cluster = 2  # first data cluster
    root_dir = bytearray(ROOT_SECTORS * BPS)
    dir_entry_idx = 0  # separate counter for root directory entries

    def set_fat_entry(cl, value):
        """Write a 12-bit FAT entry for cluster cl."""
        off = cl + cl // 2
        if cl & 1:
            # Odd cluster: upper 12 bits of the 16-bit word
            old = fat[off]
            fat[off] = (old & 0x0F) | ((value & 0x0F) << 4)
            fat[off + 1] = (value >> 4) & 0xFF
        else:
            # Even cluster: lower 12 bits
            fat[off] = value & 0xFF
            fat[off + 1] = (fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F)

    # 统一写入序列: 普通文件 = [8.3 项]; 长名文件 = [LFN 槽..., 8.3 项]
    groups = []
    for name_str, content_bytes in DEMO_FILES:
        groups.append(([name_to_83(name_str)], content_bytes))
    for long_name, name_83, content_bytes in LONG_FILES:
        s83 = name_to_83(name_83)
        groups.append((make_lfn_slots(long_name, s83) + [s83], content_bytes))

    for entries, content_bytes in groups:
        size = len(content_bytes)

        # Number of clusters needed
        cluster_size = SPC * BPS
        num_clusters = (size + cluster_size - 1) // cluster_size

        # Allocate cluster chain
        start_cluster = current_cluster
        prev_cluster = 0
        for i in range(num_clusters):
            cl = current_cluster
            if prev_cluster:
                set_fat_entry(prev_cluster, cl)
            prev_cluster = cl
            current_cluster += 1
        set_fat_entry(prev_cluster, 0xFFF)  # end of chain

        # Write file data across allocated clusters
        data_base = DATA_SEC * BPS
        for i, off in enumerate(range(0, size, cluster_size)):
            cl = start_cluster + i
            cl_off = (cl - 2) * cluster_size
            chunk = content_bytes[off: off + cluster_size]
            img[data_base + cl_off: data_base + cl_off + len(chunk)] = chunk

        # LFN 槽 (在短目录项之前, 物理顺序)
        for lfn in entries[:-1]:
            off = dir_entry_idx * 32
            dir_entry_idx += 1
            root_dir[off: off + 32] = lfn

        # Create root directory entry
        name_bytes = entries[-1]
        entry_offset = dir_entry_idx * 32
        dir_entry_idx += 1
        entry = bytearray(32)
        entry[0:11] = name_bytes
        entry[11] = 0x20  # archive attribute
        struct.pack_into('<H', entry, 22, make_fat_time())  # time
        struct.pack_into('<H', entry, 24, make_fat_date())  # date
        struct.pack_into('<H', entry, 26, start_cluster)
        struct.pack_into('<I', entry, 28, size)
        root_dir[entry_offset: entry_offset + 32] = entry

    # Write root directory
    img[ROOT_SEC * BPS: ROOT_SEC * BPS + len(root_dir)] = root_dir
    # Write FATs back
    img[RESERVED * BPS: RESERVED * BPS + len(fat)] = fat
    img[(RESERVED + FAT_SIZE) * BPS: (RESERVED + FAT_SIZE) * BPS + len(fat2)] = fat

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(img)

    print(f"Built {output_path} ({len(DEMO_FILES)} files, {os.path.getsize(output_path)} bytes)")

    for name_str, content_bytes in DEMO_FILES:
        s = name_str.strip()
        name = s[:8].rstrip()
        ext = s[8:].rstrip() if len(s) > 8 else ''
        display = name + (('.' + ext) if ext else '')
        print(f"  {display:20s} {len(content_bytes)} bytes")
    for long_name, name_83, content_bytes in LONG_FILES:
        print(f"  {long_name:20s} {len(content_bytes)} bytes  (LFN, 短名 {name_83.strip()})")


if __name__ == '__main__':
    out = os.path.join(os.path.dirname(__file__), '..', 'build', 'disk.img')
    build_disk(out)
