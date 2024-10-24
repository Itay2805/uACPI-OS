########################################################################################################################
# uACPI-OS
########################################################################################################################

# Nuke built-in rules and variables.
override MAKEFLAGS += -rR

KERNEL			:= kernel

#-----------------------------------------------------------------------------------------------------------------------
# General Config
#-----------------------------------------------------------------------------------------------------------------------

# Are we compiling as debug or not
DEBUG 			?= 1

ifeq ($(DEBUG),1)
OPTIMIZE		?= 0
else
OPTIMIZE		?= 1
endif

#-----------------------------------------------------------------------------------------------------------------------
# Directories
#-----------------------------------------------------------------------------------------------------------------------

BUILD_DIR		:= build
BIN_DIR			:= $(BUILD_DIR)/bin
OBJ_DIR			:= $(BUILD_DIR)/obj

#-----------------------------------------------------------------------------------------------------------------------
# Flags
#-----------------------------------------------------------------------------------------------------------------------

#
# Toolchain
#
CC				:= ccache clang
LD				:= ld.lld

#
# Compiler flags
#
CFLAGS			:= -target x86_64-pc-none-elf
CFLAGS			+= -Wall -Werror -std=gnu11 -fshort-wchar
CFLAGS 			+= -Wno-address-of-packed-member
CFLAGS			+= -mgeneral-regs-only -msse2
CFLAGS			+= -fno-pie -fno-pic -ffreestanding -fno-builtin -static
CFLAGS			+= -mcmodel=kernel -mno-red-zone -mgeneral-regs-only
CFLAGS			+= -nostdlib
CFLAGS			+= -Ikernel -Ibuild/limine -Ilibs/uACPI/include
CFLAGS			+= -flto
CFLAGS			+= -g
CFLAGS			+= -march=x86-64-v3
CFLAGS 			+= -DUACPI_KERNEL_INITIALIZATION

# Debug flags
ifeq ($(DEBUG),1)
CFLAGS			+= -Wno-unused-function -Wno-unused-label -Wno-unused-variable
CFLAGS			+= -D__DEBUG__
else
CFLAGS			+= -DNDEBUG
endif

# Optimization flags
ifeq ($(OPTIMIZE),1)
CFLAGS			+= -Os
endif

#
# Linker flags
#
LDFLAGS			:= -Tkernel/linker.ld -nostdlib -static

#-----------------------------------------------------------------------------------------------------------------------
# Sources
#-----------------------------------------------------------------------------------------------------------------------

# Get list of source files
SRCS 		:= $(shell find kernel -name '*.c')

SRCS		+= libs/uACPI/source/tables.c
SRCS		+= libs/uACPI/source/types.c
SRCS		+= libs/uACPI/source/uacpi.c
SRCS		+= libs/uACPI/source/utilities.c
SRCS		+= libs/uACPI/source/interpreter.c
SRCS		+= libs/uACPI/source/opcodes.c
SRCS		+= libs/uACPI/source/namespace.c
SRCS		+= libs/uACPI/source/stdlib.c
SRCS		+= libs/uACPI/source/shareable.c
SRCS		+= libs/uACPI/source/opregion.c
SRCS		+= libs/uACPI/source/default_handlers.c
SRCS		+= libs/uACPI/source/io.c
SRCS		+= libs/uACPI/source/notify.c
SRCS		+= libs/uACPI/source/sleep.c
SRCS		+= libs/uACPI/source/registers.c
SRCS		+= libs/uACPI/source/resources.c
SRCS		+= libs/uACPI/source/event.c
SRCS		+= libs/uACPI/source/mutex.c
SRCS		+= libs/uACPI/source/osi.c

# The objects/deps
OBJ 		:= $(addprefix $(OBJ_DIR)/,$(SRCS:.c=.c.o))
DEPS		:= $(addprefix $(OBJ_DIR)/,$(SRCS:.c=.c.d))

# Default target.
.PHONY: all
all: $(BIN_DIR)/$(KERNEL).elf

.PHONY: $(BIN_DIR)/kernel.aml
$(BIN_DIR)/kernel.aml:
	$(MAKE) -C acpi

# Get the header deps
-include $(DEPS)

# Link rules for the final kernel executable.
$(BIN_DIR)/$(KERNEL).elf: Makefile kernel/linker.ld $(OBJ)
	@echo LD $@
	@mkdir -p "$$(dirname $@)"
	@$(LD) $(OBJ) $(LDFLAGS) -o $@

# Compilation rules for *.c files.
$(OBJ_DIR)/%.c.o: %.c Makefile $(BUILD_DIR)/limine/limine.h
	@echo CC $@
	@mkdir -p $(@D)
	@$(CC) -MMD $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: distclean
distclean: clean
	rm -rf $(BUILD_DIR)

#-----------------------------------------------------------------------------------------------------------------------
# Quick test
#-----------------------------------------------------------------------------------------------------------------------

$(BUILD_DIR)/limine/limine.h: $(BUILD_DIR)/limine

# Clone and build limine utils
$(BUILD_DIR)/limine:
	mkdir -p $(@D)
	cd $(BUILD_DIR) && git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
	$(MAKE) -C $(BUILD_DIR)/limine

# The name of the image we are building
IMAGE_NAME 	:= $(BIN_DIR)/$(KERNEL)

# Build a limine image with both bios and uefi boot options
.PHONY: $(IMAGE_NAME).hdd
$(IMAGE_NAME).hdd: $(BIN_DIR)/$(KERNEL).elf $(BIN_DIR)/kernel.aml
	mkdir -p $(@D)
	rm -f $(IMAGE_NAME).hdd
	dd if=/dev/zero bs=1M count=0 seek=64 of=$(IMAGE_NAME).hdd
	sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00
	./$(BUILD_DIR)/limine/limine bios-install $(IMAGE_NAME).hdd
	mformat -i $(IMAGE_NAME).hdd@@1M
	mmd -i $(IMAGE_NAME).hdd@@1M ::/EFI ::/EFI/BOOT
	mcopy -i $(IMAGE_NAME).hdd@@1M $(BIN_DIR)/$(KERNEL).elf kernel/limine.conf $(BUILD_DIR)/limine/limine-bios.sys ::/
	mcopy -i $(IMAGE_NAME).hdd@@1M $(BIN_DIR)/kernel.aml ::/
	mcopy -i $(IMAGE_NAME).hdd@@1M $(BUILD_DIR)/limine/BOOTX64.EFI ::/EFI/BOOT

.PHONY: run
run: $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		--enable-kvm \
		-cpu host,+invtsc,+tsc-deadline \
		-machine q35 \
		-m 2G \
		-smp 4 \
		-s \
		-hda $(IMAGE_NAME).hdd \
		-debugcon stdio \
		-no-reboot \
	 	-no-shutdown
