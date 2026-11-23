# Mothlight UI module. Builds inside either engine tree (scripts/build.sh copies it there).
MODULE_NAME = moth_ui
SRCS = moth_ui.o panel.o qbar.o themes.o ui_gfx.o ui_text.o budget.o profiles.o

ifneq ($(wildcard ../Makefile.modules),)
    # Crop Mood engine (2023-era build system)
    MODULE_OBJS = $(SRCS)
    include ../Makefile.modules
else
    # magiclantern_simplified dev
    TOP_DIR = ../..
    MODULE_OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS))
    include $(TOP_DIR)/modules/Makefile
    .DEFAULT_GOAL := $(BUILD_DIR)/module_complete
endif
