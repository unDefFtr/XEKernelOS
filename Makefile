# XEKernelOS Makefile — C++ Build
#   Boot:      boot.asm + stage2.asm (NASM)
#   ISR asm:   isr.asm (NASM elf32)
#   Kernel:    C++ sources

NASM     = nasm
CXX      = clang++
LD       = ld.lld
OBJCOPY  = llvm-objcopy
TARGET   = i686-elf

SRCDIR   = src
BLDDIR   = build

BOOT_SRC   = $(SRCDIR)/boot/boot.asm
STAGE2_SRC = $(SRCDIR)/boot/stage2.asm
ISR_ASM    = $(SRCDIR)/kernel/isr.asm
LINKER_SRC = $(SRCDIR)/linker.ld

# All C++ sources
CXX_SRCS := $(SRCDIR)/kernel/kernel.cpp \
            $(SRCDIR)/kernel/isr.cpp \
            $(SRCDIR)/kernel/idt.cpp \
            $(SRCDIR)/kernel/mm.cpp \
            $(SRCDIR)/kernel/paging.cpp \
            $(SRCDIR)/kernel/user.cpp \
            $(SRCDIR)/kernel/panic.cpp \
            $(SRCDIR)/kernel/syscall.cpp \
            $(SRCDIR)/kernel/task.cpp \
            $(SRCDIR)/kernel/loader.cpp \
            $(SRCDIR)/kernel/elf.cpp \
            $(SRCDIR)/drivers/keyboard.cpp \
            $(SRCDIR)/drivers/pic.cpp \
            $(SRCDIR)/drivers/pit.cpp \
            $(SRCDIR)/drivers/mouse.cpp \
            $(SRCDIR)/drivers/ata.cpp \
            $(SRCDIR)/drivers/bcache.cpp \
            $(SRCDIR)/drivers/input.cpp \
            $(SRCDIR)/drivers/gfx.cpp \
            $(SRCDIR)/drivers/serial.cpp \
            $(SRCDIR)/drivers/font_cn_load.cpp \
            $(SRCDIR)/fs/fat12.cpp \
            $(SRCDIR)/fs/ext2.cpp \
            $(SRCDIR)/fs/vfs.cpp \
            $(SRCDIR)/fs/ramdisk.cpp \
            $(SRCDIR)/shell/shell.cpp \
            $(SRCDIR)/lib/heap.cpp \
            $(SRCDIR)/lib/strutil.cpp \
            $(SRCDIR)/lib/uaccess.cpp \
            $(SRCDIR)/lib/cpprt.cpp

CXX_OBJS = $(patsubst $(SRCDIR)/%.cpp,$(BLDDIR)/%.o,$(CXX_SRCS))

BOOT_BIN   = $(BLDDIR)/boot.bin
STAGE2_BIN = $(BLDDIR)/stage2.bin
ISR_OBJ    = $(BLDDIR)/isr.o
KERNEL_ELF = $(BLDDIR)/kernel.elf
KERNEL_BIN = $(BLDDIR)/kernel.bin
IMG        = $(BLDDIR)/xekernelos.img
HELLO_BIN  = $(BLDDIR)/hello.bin
GFXDEMO_BIN = $(BLDDIR)/gfx_demo.bin
BOUNCE_BIN  = $(BLDDIR)/bounce.bin
TESTGFX_BIN = $(BLDDIR)/testgfx.bin

USER_ASM   = $(SRCDIR)/user/hello.asm
GFXDEMO_ASM = $(SRCDIR)/user/gfx_demo.asm
BOUNCE_ASM  = $(SRCDIR)/user/bounce.asm
TESTGFX_ASM = $(SRCDIR)/user/testgfx.asm

# User-space demo program (flat binary)
DEMO_SRC   = $(SRCDIR)/user/demo.cpp
DEMO_ELF   = $(BLDDIR)/demo.elf
DEMO_BIN   = $(BLDDIR)/demo.bin

# GUI desktop program (flat binary)
DESKTOP_SRC = $(SRCDIR)/user/desktop.cpp
DESKTOP_ELF = $(BLDDIR)/desktop.elf
DESKTOP_BIN = $(BLDDIR)/desktop.bin

# Preemption test program (flat binary)
SPIN_SRC = $(SRCDIR)/user/spin.cpp
SPIN_ELF = $(BLDDIR)/spin.elf
SPIN_BIN = $(BLDDIR)/spin.bin

# LFN (long file name) read-path test program (flat binary)
LFSTEST_SRC = $(SRCDIR)/user/lfstest.cpp
LFSTEST_ELF = $(BLDDIR)/lfstest.elf
LFSTEST_BIN = $(BLDDIR)/lfstest.bin

# Hardware segment-boundary regression and exec probe share one flat source.
SEGTEST_BIN = $(BLDDIR)/segtest.bin
SEGEXEC_BIN = $(BLDDIR)/segexec.bin

CXXFLAGS = -target $(TARGET) -ffreestanding -nostdlib -Wall -Wextra -O1 \
           -fno-exceptions -fno-rtti -fno-use-cxa-atexit -std=c++17 \
           -mno-sse -mno-mmx -mno-sse2 -I $(SRCDIR)
LDFLAGS = -m elf_i386

.PHONY: all run clean disk-img

DISK_IMG = $(BLDDIR)/disk.img
EXT2_IMG = $(BLDDIR)/ext2.img

TEST_ELF_ASM = $(SRCDIR)/user/test_elf.asm
TEST_ELF_O   = $(BLDDIR)/test_elf.o
TEST_ELF     = $(BLDDIR)/test_elf.elf

# User-space shell (embedded in kernel)
USHELL_SRC   = $(SRCDIR)/user/ushell.cpp
USHELL_ELF   = $(BLDDIR)/ushell.elf
USHELL_BIN   = $(BLDDIR)/ushell.bin
USHELL_HDR   = $(SRCDIR)/user/ushell_blob.h

all: $(IMG) $(HELLO_BIN) $(TEST_ELF) $(USHELL_HDR) $(FONT_BIN) $(DISK_IMG) $(DEMO_BIN) $(DESKTOP_BIN) $(SPIN_BIN) $(LFSTEST_BIN) $(GFXDEMO_BIN) $(BOUNCE_BIN) $(TESTGFX_BIN) $(SEGTEST_BIN) $(SEGEXEC_BIN)

# ... existing targets ...

# Write font_cn.bin to disk.img at LBA 2048
FONT_BIN = $(BLDDIR)/font_cn.bin
$(FONT_BIN): tools/gen_font_bin.py tools/unifont.hex.gz | $(BLDDIR)
	python tools/gen_font_bin.py

$(BOOT_BIN): $(BOOT_SRC) | $(BLDDIR)
	$(NASM) -f bin $< -o $@

$(STAGE2_BIN): $(STAGE2_SRC) | $(BLDDIR)
	$(NASM) -f bin $< -o $@

$(ISR_OBJ): $(ISR_ASM) | $(BLDDIR)
	$(NASM) -f elf32 -w-label-orphan $< -o $@

$(BLDDIR)/%.o: $(SRCDIR)/%.cpp | $(BLDDIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# shell.o also depends on the embedded user-shell blob
$(BLDDIR)/shell/shell.o: $(USHELL_HDR)

$(KERNEL_ELF): $(ISR_OBJ) $(CXX_OBJS) $(LINKER_SRC)
	$(LD) $(LDFLAGS) -T $(LINKER_SRC) $(ISR_OBJ) $(CXX_OBJS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN) build_img.py
	python build_img.py $(BLDDIR)
	@echo "=== Image ==="
	@ls -la $@
	@echo "=== Sections ==="
	@echo "  Boot:    512B"
	@echo "  Stage2:  $$(wc -c < $(STAGE2_BIN))B"
	@echo "  Kernel:  $$(wc -c < $(KERNEL_BIN))B"

$(BLDDIR):
	mkdir -p $(BLDDIR)

$(HELLO_BIN): $(USER_ASM) | $(BLDDIR)
	$(NASM) -f bin $< -o $@
	@echo "  User hello: $$(wc -c < $@)B"

$(GFXDEMO_BIN): $(GFXDEMO_ASM) | $(BLDDIR)
	$(NASM) -f bin $< -o $@
	@echo "  GFX demo: $$(wc -c < $@)B"

$(BOUNCE_BIN): $(SRCDIR)/user/bounce.asm | $(BLDDIR)
	$(NASM) -f bin $< -o $@
	@echo "  Bounce: $$(wc -c < $@)B"

$(TESTGFX_BIN): $(SRCDIR)/user/testgfx.asm | $(BLDDIR)
	$(NASM) -f bin $< -o $@
	@echo "  TestGFX: $$(wc -c < $@)B"


$(SEGTEST_BIN): $(SRCDIR)/user/segtest.asm | $(BLDDIR)
	$(NASM) -f bin $< -o $@

$(SEGEXEC_BIN): $(SRCDIR)/user/segtest.asm | $(BLDDIR)
	$(NASM) -DEXEC_PROBE=1 -f bin $< -o $@
$(TEST_ELF_O): $(TEST_ELF_ASM) | $(BLDDIR)
	$(NASM) -f elf32 $< -o $@

$(TEST_ELF): $(TEST_ELF_O)
	$(LD) $(LDFLAGS) -Ttext=0x400000 $< -o $@
	@echo "  ELF test: $$(wc -c < $@)B"

# User-space shell — compile & embed in kernel
$(USHELL_ELF): $(USHELL_SRC) $(SRCDIR)/user/ufs.cpp $(SRCDIR)/user/ufs.h $(SRCDIR)/user/user.ld | $(BLDDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR)/user -c $(USHELL_SRC) -o $(BLDDIR)/ushell.o
	$(CXX) $(CXXFLAGS) -I$(SRCDIR)/user -c $(SRCDIR)/user/ufs.cpp -o $(BLDDIR)/ufs.o
	$(LD) $(LDFLAGS) -T $(SRCDIR)/user/user.ld $(BLDDIR)/ushell.o $(BLDDIR)/ufs.o -o $@

$(USHELL_BIN): $(USHELL_ELF)
	$(OBJCOPY) -O binary $< $@

$(USHELL_HDR): $(USHELL_BIN)
	python tools/binary_to_header.py $< ushell_blob > $@

# User demo program — uses libc.h
$(DEMO_ELF): $(DEMO_SRC) $(SRCDIR)/user/libc.h $(SRCDIR)/user/usys.h $(SRCDIR)/user/user.ld | $(BLDDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR)/user -c $< -o $(BLDDIR)/demo.o
	$(LD) $(LDFLAGS) -T $(SRCDIR)/user/user.ld $(BLDDIR)/demo.o -o $@

$(DEMO_BIN): $(DEMO_ELF)
	$(OBJCOPY) -O binary $< $@

# GUI desktop — self-contained (no libc.h)
$(DESKTOP_ELF): $(DESKTOP_SRC) $(SRCDIR)/user/user.ld | $(BLDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $(BLDDIR)/desktop.o
	$(LD) $(LDFLAGS) -T $(SRCDIR)/user/user.ld $(BLDDIR)/desktop.o -o $@

$(DESKTOP_BIN): $(DESKTOP_ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "  Desktop: $$(wc -c < $@)B"

# Preemption test — pure spin parent + heartbeat child
$(SPIN_ELF): $(SPIN_SRC) $(SRCDIR)/user/usys.h $(SRCDIR)/user/user.ld | $(BLDDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR)/user -c $< -o $(BLDDIR)/spin.o
	$(LD) $(LDFLAGS) -T $(SRCDIR)/user/user.ld $(BLDDIR)/spin.o -o $@

$(SPIN_BIN): $(SPIN_ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "  Spin: $$(wc -c < $@)B"

# LFN read-path test
$(LFSTEST_ELF): $(LFSTEST_SRC) $(SRCDIR)/user/usys.h $(SRCDIR)/user/user.ld | $(BLDDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR)/user -c $< -o $(BLDDIR)/lfstest.o
	$(LD) $(LDFLAGS) -T $(SRCDIR)/user/user.ld $(BLDDIR)/lfstest.o -o $@

$(LFSTEST_BIN): $(LFSTEST_ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "  LFStest: $$(wc -c < $@)B"

# Build FAT12 disk image with CJK font + ext2 injected
DISK_SIZE = 4194304  # 4MB
$(DISK_IMG): $(HELLO_BIN) $(TEST_ELF) $(FONT_BIN) $(DEMO_BIN) $(DESKTOP_BIN) $(SPIN_BIN) $(LFSTEST_BIN) $(USHELL_BIN) $(GFXDEMO_BIN) $(BOUNCE_BIN) $(TESTGFX_BIN) $(SEGTEST_BIN) $(SEGEXEC_BIN) $(EXT2_IMG) tools/mkdisk.py
	python tools/mkdisk.py
	@echo "Expanding disk to 4MB and injecting font at LBA 2048..."
	@python -c "import os; sz=os.path.getsize('$@'); open('$@','ab').write(b'\x00'*($(DISK_SIZE)-sz)); f=open('$(FONT_BIN)','rb').read(); d=open('$@','r+b'); d.seek(2048*512); d.write(f); print(f'Font {len(f)}B injected')"
	@echo "Injecting ext2 at LBA 4096..."
	@python -c "import os; f=open('$(EXT2_IMG)','rb').read(); d=open('$@','r+b'); d.seek(4096*512); d.write(f); print(f'ext2 {len(f)}B injected')"

# Build ext2 test image
$(EXT2_IMG): tools/mkfs_ext2.py | $(BLDDIR)
	python tools/mkfs_ext2.py

disk-img: $(DISK_IMG)

run: $(IMG) $(DISK_IMG)
	qemu-system-i386 -hda $(IMG) -hdb $(DISK_IMG) -m 32 -boot order=c -serial stdio

run-debug: $(IMG) $(DISK_IMG)
	qemu-system-i386 -hda $(IMG) -hdb $(DISK_IMG) -m 32 -boot order=c -serial stdio -monitor stdio -d cpu_reset,int

clean:
	rm -rf $(BLDDIR)

-include $(CXX_OBJS:.o=.d)
