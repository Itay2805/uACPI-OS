########################################################################################################################
# uACPI-OS - the acpi part
########################################################################################################################

# Nuke built-in rules and variables.
override MAKEFLAGS += -rR

#-----------------------------------------------------------------------------------------------------------------------
# Directories
#-----------------------------------------------------------------------------------------------------------------------

BUILD_DIR		:= ../build
BIN_DIR			:= $(BUILD_DIR)/bin
OBJ_DIR			:= $(BUILD_DIR)/obj

#-----------------------------------------------------------------------------------------------------------------------
# Sources
#-----------------------------------------------------------------------------------------------------------------------

SRCS 		:= $(shell find . -name '*.py')

$(BIN_DIR)/kernel.aml: $(SRCS) ../compiler/compiler.py
	mkdir -p $(@D)
	python3 ../compiler/compiler.py --root kernel.py $@

