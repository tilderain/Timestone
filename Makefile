# Makefile for UMVC3Hook - cross-compile Windows DLL with MinGW on Linux
# Targets: Release x64 (.asi), Debug x64

CXX       := x86_64-w64-mingw32-g++-win32
CC        := x86_64-w64-mingw32-gcc-win32
WINDRES   := x86_64-w64-mingw32-windres
DLLTOOL   := x86_64-w64-mingw32-dlltool
AR        := x86_64-w64-mingw32-ar
GIT_COMMIT := $(shell git rev-parse HEAD 2>/dev/null || printf unknown)
GIT_DIR := $(shell git rev-parse --git-dir 2>/dev/null)
GIT_HEAD_REF := $(shell git symbolic-ref -q HEAD 2>/dev/null)
GIT_COMMIT_DEPS := $(if $(GIT_DIR),$(GIT_DIR)/HEAD $(if $(GIT_HEAD_REF),$(GIT_DIR)/$(GIT_HEAD_REF)))

SRCDIR    := UMVC3Hook
ENETDIR   := enet-1.3.18
BUILDDIR  := build
OBJDIR    := $(BUILDDIR)/obj

# Output: .asi is a renamed DLL loaded by Ultimate ASI Loader
OUTPUT    := UMVC3Hook.asi

# --- Source files (from UMVC3Hook.vcxproj, Release|x64) ---

CPP_SRC   := \
	$(SRCDIR)/dllmain.cpp \
	$(SRCDIR)/IniReader.cpp \
	$(SRCDIR)/sigscan.cpp \
	$(SRCDIR)/pch.cpp \
	$(SRCDIR)/code/umvc3.cpp \
	$(SRCDIR)/code/umvc3menu.cpp \
	$(SRCDIR)/code/LaunchMenu.cpp \
	$(SRCDIR)/code/eSettingsManager.cpp \
	$(SRCDIR)/code/eNotifManager.cpp \
	$(SRCDIR)/code/UMVC3Camera.cpp \
	$(SRCDIR)/code/mk10utils.cpp \
	$(SRCDIR)/code/framestepper.cpp \
	$(SRCDIR)/code/dtieditor.cpp \
	$(SRCDIR)/code/trainingboot.cpp \
	$(SRCDIR)/code/controller.cpp \
	$(SRCDIR)/code/diag.cpp \
	$(SRCDIR)/code/exception.cpp \
	$(SRCDIR)/code/rollback.cpp \
	$(SRCDIR)/code/netplay.cpp \
	$(SRCDIR)/imgui/imgui.cpp \
	$(SRCDIR)/imgui/imgui_demo.cpp \
	$(SRCDIR)/imgui/imgui_draw.cpp \
	$(SRCDIR)/imgui/imgui_tables.cpp \
	$(SRCDIR)/imgui/imgui_widgets.cpp \
	$(SRCDIR)/imgui/imgui_impl_dx9.cpp \
	$(SRCDIR)/imgui/imgui_impl_dx11.cpp \
	$(SRCDIR)/imgui/imgui_impl_win32.cpp \
	$(SRCDIR)/kiero/kiero.cpp \
	$(SRCDIR)/utils/Patterns.cpp

C_SRC     := \
	$(SRCDIR)/kiero/minhook/src/hook.c \
	$(SRCDIR)/kiero/minhook/src/buffer.c \
	$(SRCDIR)/kiero/minhook/src/trampoline.c \
	$(SRCDIR)/kiero/minhook/src/hde/hde32.c \
	$(SRCDIR)/kiero/minhook/src/hde/hde64.c

RC_SRC    := $(BUILDDIR)/UMVC3Hook.rc

# --- Object files ---

CPP_OBJ   := $(CPP_SRC:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
C_OBJ     := $(C_SRC:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
ENET_OBJ  := $(ENETDIR)/callbacks.o \
             $(ENETDIR)/compress.o \
             $(ENETDIR)/host.o \
             $(ENETDIR)/list.o \
             $(ENETDIR)/packet.o \
             $(ENETDIR)/peer.o \
             $(ENETDIR)/protocol.o \
             $(ENETDIR)/win32.o
ENET_OBJ_FULL := $(addprefix $(OBJDIR)/, $(notdir $(ENET_OBJ)))
RC_OBJ    := $(OBJDIR)/UMVC3Hook.res

ALL_OBJ   := $(CPP_OBJ) $(C_OBJ) $(ENET_OBJ_FULL) $(RC_OBJ)

# --- Include paths ---

INCLUDES  := \
	-I$(SRCDIR) \
	-I$(SRCDIR)/code \
	-I$(SRCDIR)/imgui \
	-I$(SRCDIR)/kiero \
	-I$(SRCDIR)/kiero/minhook/include \
	-I$(SRCDIR)/utils \
	-I$(ENETDIR)/include \
	-I$(BUILDDIR)/include_fix

# --- Preprocessor defines (Release) ---

DEFINES   := \
	-DNDEBUG \
	-DUMVC3HOOK_EXPORTS \
	-D_WINDOWS \
	-D_USRDLL \
	-D_CRT_SECURE_NO_WARNINGS \
	-D_CRT_NONSTDC_NO_WARNINGS \
	-D_MEMORY_NO_CRT \
	-D_WIN32_WINNT=0x0601 \
	-DWINVER=0x0601

# --- Compiler flags ---

CXXFLAGS  := -O2 -std=c++20 -fpermissive $(DEFINES) $(INCLUDES) -DUMVC3HOOK_GIT_COMMIT=\"$(GIT_COMMIT)\" -Wall -Wno-unused-variable -Wno-unused-function
CFLAGS    := -O2 $(DEFINES) $(INCLUDES) -Wall -Wno-unused-variable
LDFLAGS   := -shared -static-libgcc -static-libstdc++
LIBS      := \
	-ld3d9 \
	-ld3dcompiler \
	-lxinput \
	-lkernel32 \
	-luser32 \
	-lgdi32 \
	-lpsapi \
	-lcomctl32 \
	-lcomdlg32 \
	-ladvapi32 \
	-lshell32 \
	-lole32 \
	-loleaut32 \
	-luuid \
	-lwinmm \
	-lws2_32 \
	-lversion

# ##############################################################################
# Targets
# ##############################################################################

.PHONY: all clean release debug fix-includes

all:
	@if $(MAKE) --no-print-directory -q $(OUTPUT); then \
		echo "=== No changes: $(OUTPUT) is up to date ==="; \
	else \
		$(MAKE) --no-print-directory release; \
	fi

# ---------- Release build ----------

release: CXXFLAGS += -DNDEBUG
release: fix-includes $(OBJDIR) $(OUTPUT)

# ---------- Debug build ----------

debug: CXXFLAGS  := -O0 -g -std=c++20 $(DEFINES) $(INCLUDES) -Wall
debug: CFLAGS    := -O0 -g $(DEFINES) $(INCLUDES) -Wall
debug: DEFINES   := -D_DEBUG -DUMVC3HOOK_EXPORTS -D_WINDOWS -D_USRDLL -D_CRT_SECURE_NO_WARNINGS
debug: fix-includes $(OBJDIR) $(OUTPUT)

# ---------- Link ----------

$(OUTPUT): $(ALL_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "=== Built: $(OUTPUT) ==="

# ---------- Compile C++ ----------

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Recompile only when HEAD or the current branch advances.
$(OBJDIR)/code/exception.o: $(GIT_COMMIT_DEPS)

# ---------- Compile C ----------

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Compile enet C ----------

$(OBJDIR)/%.o: $(ENETDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Resource file ----------

$(BUILDDIR)/UMVC3Hook.rc:
	@mkdir -p $(@D)
	@printf '%s\n' \
		'VS_VERSION_INFO VERSIONINFO' \
		'' \
		'FILEVERSION     1,0,0,0' \
		'PRODUCTVERSION  1,0,0,0' \
		'FILEFLAGSMASK   0x3fL' \
		'FILEFLAGS       0x0L' \
		'FILEOS          0x40004L' \
		'FILETYPE        0x2L' \
		'FILESUBTYPE     0x0L' \
		'BEGIN' \
		'    BLOCK "StringFileInfo"' \
		'    BEGIN' \
		'        BLOCK "040904E4"' \
		'        BEGIN' \
		'            VALUE "FileDescription", "UMVC3 Hook"' \
		'            VALUE "FileVersion", "1.0.0.0"' \
		'            VALUE "InternalName", "UMVC3Hook"' \
		'            VALUE "OriginalFilename", "UMVC3Hook.asi"' \
		'            VALUE "ProductName", "UMVC3 Hook"' \
		'            VALUE "ProductVersion", "1.0.0.0"' \
		'        END' \
		'    END' \
		'    BLOCK "VarFileInfo"' \
		'    BEGIN' \
		'        VALUE "Translation", 0x409, 1252' \
		'    END' \
		'END' > $@

$(RC_OBJ): $(BUILDDIR)/UMVC3Hook.rc $(SRCDIR)/resource.h | $(INCLUDE_FIX_STAMP)
	@mkdir -p $(@D)
	$(WINDRES) --include-dir $(SRCDIR) --include-dir $(INCLUDE_FIX_DIR) -O coff -i $< -o $@

# ---------- Include case fix ----------
# MinGW headers on Linux are lowercase; some source files include Windows.h / Psapi.h
# We create symlinks so the case-sensitive compiler finds them.

INCLUDE_FIX_DIR := $(BUILDDIR)/include_fix
INCLUDE_FIX_STAMP := $(INCLUDE_FIX_DIR)/.stamp

fix-includes: $(INCLUDE_FIX_STAMP)

$(INCLUDE_FIX_STAMP):
	@mkdir -p $(INCLUDE_FIX_DIR)
	@test -L $(INCLUDE_FIX_DIR)/Windows.h 2>/dev/null || \
		ln -sf /usr/x86_64-w64-mingw32/include/windows.h $(INCLUDE_FIX_DIR)/Windows.h
	@test -L $(INCLUDE_FIX_DIR)/Psapi.h 2>/dev/null || \
		ln -sf /usr/x86_64-w64-mingw32/include/psapi.h $(INCLUDE_FIX_DIR)/Psapi.h
	@test -L $(INCLUDE_FIX_DIR)/XInput.h 2>/dev/null || \
		ln -sf /usr/x86_64-w64-mingw32/include/xinput.h $(INCLUDE_FIX_DIR)/XInput.h
	@touch $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

# ---------- Clean ----------

clean:
	rm -rf $(BUILDDIR) $(OUTPUT)

# ---------- Install (example path - adjust to your UMVC3 install) ----------

INSTALL_DIR ?= ../ULTIMATE MARVEL VS. CAPCOM 3

install: $(OUTPUT)
	cp "$(OUTPUT)" "$(INSTALL_DIR)/"

# ---------- Help ----------

help:
	@echo "Targets:"
	@echo "  all         - Build release (default)"
	@echo "  release     - Release build, optimized"
	@echo "  debug       - Debug build (-O0 -g)"
	@echo "  clean       - Remove build artifacts"
	@echo "  install     - Copy .asi to UMVC3 game directory"
	@echo ""
	@echo "Variables:"
	@echo "  INSTALL_DIR - UMVC3 install path (default: ~/.steam/...)"
	@echo ""
	@echo "Dependencies:"
	@echo "  apt install g++-mingw-w64-x86-64 mingw-w64"
