# Makefile for UMVC3Hook - cross-compile Windows DLL with MinGW on Linux
# Targets: Release x64 (.asi), Debug x64

.DEFAULT_GOAL := all

CXX       := x86_64-w64-mingw32-g++-win32
CC        := x86_64-w64-mingw32-gcc-win32
WINDRES   := x86_64-w64-mingw32-windres
DLLTOOL   := x86_64-w64-mingw32-dlltool
AR        := x86_64-w64-mingw32-ar
STRIP     := x86_64-w64-mingw32-strip
GIT_COMMIT := $(shell git rev-parse HEAD 2>/dev/null || printf unknown)
GIT_DIRTY := $(if $(shell git status --porcelain --untracked-files=normal 2>/dev/null),1,0)
GIT_DIR := $(shell git rev-parse --git-dir 2>/dev/null)
GIT_HEAD_REF := $(shell git symbolic-ref -q HEAD 2>/dev/null)
GIT_COMMIT_DEPS := $(if $(GIT_DIR),$(GIT_DIR)/HEAD $(if $(GIT_HEAD_REF),$(GIT_DIR)/$(GIT_HEAD_REF)))
VERSION    := 0.1.0
VERSION_RC := $(shell printf '%s.0' '$(VERSION)' | tr . ,)

SRCDIR    := UMVC3Hook
ENETDIR   := enet-1.3.18
BUILDDIR  := build
OBJDIR    := $(BUILDDIR)/obj

# Output: .asi is a renamed DLL loaded by Ultimate ASI Loader
OUTPUT    := Timestone.asi
UNSTRIPPED_OUTPUT := Timestone-unstripped.asi
UPDATER_OUTPUT := TimestoneUpdater.exe
PACKAGE_OUTPUT := Timestone.zip
LOADER_DLL := dinput8.dll
MAP_FILE  := Timestone.map

# --- Source files (from UMVC3Hook.vcxproj, Release|x64) ---

CPP_SRC   := \
	$(SRCDIR)/dllmain.cpp \
	$(SRCDIR)/IniReader.cpp \
	$(SRCDIR)/sigscan.cpp \
	$(SRCDIR)/pch.cpp \
	$(SRCDIR)/code/umvc3.cpp \
	$(SRCDIR)/code/umvc3menu.cpp \
	$(SRCDIR)/code/updater.cpp \
	$(SRCDIR)/code/LaunchMenu.cpp \
	$(SRCDIR)/code/eSettingsManager.cpp \
	$(SRCDIR)/code/eNotifManager.cpp \
	$(SRCDIR)/code/UMVC3Camera.cpp \
	$(SRCDIR)/code/mk10utils.cpp \
	$(SRCDIR)/code/framestepper.cpp \
	$(SRCDIR)/code/dtieditor.cpp \
	$(SRCDIR)/code/trainingboot.cpp \
	$(SRCDIR)/code/chrsel_imgui.cpp \
	$(SRCDIR)/code/escmenu_imgui.cpp \
	$(SRCDIR)/code/controller.cpp \
	$(SRCDIR)/code/autotest.cpp \
	$(SRCDIR)/code/diag.cpp \
	$(SRCDIR)/code/rollback_snapdump.cpp \
	$(SRCDIR)/code/exception.cpp \
	$(SRCDIR)/code/rollback.cpp \
	$(SRCDIR)/code/rollback_cleanup.cpp \
	$(SRCDIR)/code/rollback_diagnostics.cpp \
	$(SRCDIR)/code/rollback_effect.cpp \
	$(SRCDIR)/code/rollback_hooks.cpp \
	$(SRCDIR)/code/rollback_memory.cpp \
	$(SRCDIR)/code/rollback_model_policy.cpp \
	$(SRCDIR)/code/rollback_restore.cpp \
	$(SRCDIR)/code/rollback_resim.cpp \
	$(SRCDIR)/code/rollback_runtime.cpp \
	$(SRCDIR)/code/rollback_tick.cpp \
	$(SRCDIR)/code/rollback_profile.cpp \
	$(SRCDIR)/code/rollback_rng.cpp \
	$(SRCDIR)/code/rollback_shot.cpp \
	$(SRCDIR)/code/rollback_snapshot.cpp \
	$(SRCDIR)/code/concrete_snapshot.cpp \
	$(SRCDIR)/code/netplay.cpp \
	$(SRCDIR)/code/steam_lobby.cpp \
	$(SRCDIR)/code/steam_transport.cpp \
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
DEP_FILES := $(CPP_OBJ:.o=.d) $(C_OBJ:.o=.d) $(ENET_OBJ_FULL:.o=.d)
# An object without its compiler-generated dependency manifest is unsafe to
# reuse: header layout changes would otherwise be invisible to make.
MISSING_DEP_FILES := $(filter-out $(wildcard $(DEP_FILES)),$(DEP_FILES))
MISSING_DEP_OBJS  := $(MISSING_DEP_FILES:.d=.o)

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

# Keep the release ISA portable across netplay peers. In particular, do not use
# -march=native or -ffast-math: host-specific instructions and relaxed floating
# point semantics are both inappropriate for deterministic rollback simulation.
PERF_FLAGS := -O3 -flto=auto -fuse-linker-plugin -fipa-pta \
	-fdevirtualize-at-ltrans -fomit-frame-pointer \
	-ffunction-sections -fdata-sections
CXXFLAGS  := $(PERF_FLAGS) -std=c++20 -fpermissive -MMD -MP $(DEFINES) $(INCLUDES) -DUMVC3HOOK_VERSION=\"$(VERSION)\" -DUMVC3HOOK_GIT_COMMIT=\"$(GIT_COMMIT)\" -DUMVC3HOOK_GIT_DIRTY=$(GIT_DIRTY) -Wall -Wno-unused-variable -Wno-unused-function
CFLAGS    := $(PERF_FLAGS) -MMD -MP $(DEFINES) $(INCLUDES) -Wall -Wno-unused-variable
LDFLAGS   := -shared -static-libgcc -static-libstdc++ -flto=auto \
	-fuse-linker-plugin -Wl,--gc-sections -Wl,-O2 -Wl,-Map,$(MAP_FILE)
STRIP_ASI := 1
LIBS      := \
	-ld3d9 \
	-ld3dcompiler \
	-lkernel32 \
	-luser32 \
	-lgdi32 \
	-ldbghelp \
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

.PHONY: all clean release debug fix-includes force-missing-dependency-rebuild

force-missing-dependency-rebuild:

$(MISSING_DEP_OBJS): force-missing-dependency-rebuild

all:
	@if $(MAKE) --no-print-directory -q $(PACKAGE_OUTPUT); then \
		echo "=== No changes: release outputs are up to date ==="; \
	else \
		$(MAKE) --no-print-directory release; \
	fi

# ---------- Release build ----------

release: CXXFLAGS += -DNDEBUG
release: fix-includes $(OBJDIR) $(PACKAGE_OUTPUT)

# ---------- Debug build ----------

debug: CXXFLAGS  := -O0 -g -std=c++20 -MMD -MP $(DEFINES) $(INCLUDES) -Wall
debug: CFLAGS    := -O0 -g -MMD -MP $(DEFINES) $(INCLUDES) -Wall
debug: DEFINES   := -D_DEBUG -DUMVC3HOOK_EXPORTS -D_WINDOWS -D_USRDLL -D_CRT_SECURE_NO_WARNINGS
debug: STRIP_ASI := 0
debug: fix-includes $(OBJDIR) $(OUTPUT)

# ---------- Link ----------

$(OUTPUT): $(ALL_OBJ)
	$(CXX) $(LDFLAGS) -o $(UNSTRIPPED_OUTPUT) $^ $(LIBS)
	cp $(UNSTRIPPED_OUTPUT) $@.tmp
	@if [ "$(STRIP_ASI)" = "1" ]; then $(STRIP) --strip-all $@.tmp; fi
	mv $@.tmp $@
	@echo "=== Built: $(OUTPUT) ==="

$(UPDATER_OUTPUT): tools/timestone_updater.cpp Makefile
	$(CXX) -Os -flto -ffunction-sections -fdata-sections -std=c++20 \
		-DWIN32_LEAN_AND_MEAN -DNOMINMAX \
		-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
		-DTIMESTONE_VERSION=\"$(VERSION)\" \
		-static-libgcc -static-libstdc++ -mwindows \
		-Wl,--gc-sections -Wl,-s -o $@ $< \
		-lwinhttp -lbcrypt -lcomctl32 -lshell32 -lversion -lole32 -loleaut32 -luuid
	@echo "=== Built: $(UPDATER_OUTPUT) ==="

$(PACKAGE_OUTPUT): $(LOADER_DLL) $(OUTPUT) $(UPDATER_OUTPUT)
	rm -f $@
	zip -j -0 $@ $^
	@echo "=== Built: $(PACKAGE_OUTPUT) ==="

# ---------- Compile C++ ----------

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Recompile when HEAD or the current branch advances.
$(OBJDIR)/code/exception.o: $(GIT_COMMIT_DEPS)

# These objects display or compare the release version supplied by Make.
$(OBJDIR)/dllmain.o $(OBJDIR)/code/LaunchMenu.o \
	$(OBJDIR)/code/umvc3menu.o $(OBJDIR)/code/updater.o \
	$(OBJDIR)/code/steam_lobby.o: Makefile

# ---------- Compile C ----------

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Compile enet C ----------

$(OBJDIR)/%.o: $(ENETDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Resource file ----------

$(BUILDDIR)/UMVC3Hook.rc: Makefile
	@mkdir -p $(@D)
	@printf '%s\n' \
		'VS_VERSION_INFO VERSIONINFO' \
		'' \
		'FILEVERSION     $(VERSION_RC)' \
		'PRODUCTVERSION  $(VERSION_RC)' \
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
		'            VALUE "FileDescription", "Timestone Rollback"' \
		'            VALUE "FileVersion", "$(VERSION)"' \
		'            VALUE "InternalName", "Timestone"' \
		'            VALUE "OriginalFilename", "Timestone.asi"' \
		'            VALUE "ProductName", "Timestone"' \
		'            VALUE "ProductVersion", "$(VERSION)"' \
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

-include $(DEP_FILES)

# ---------- Clean ----------

clean:
	rm -rf $(BUILDDIR) $(OUTPUT) $(UNSTRIPPED_OUTPUT) $(UPDATER_OUTPUT) $(PACKAGE_OUTPUT) $(MAP_FILE)

# ---------- Install (example path - adjust to your UMVC3 install) ----------

INSTALL_DIR ?= ../ULTIMATE MARVEL VS. CAPCOM 3

install: $(OUTPUT) $(UPDATER_OUTPUT)
	cp "$(OUTPUT)" "$(INSTALL_DIR)/"
	cp "$(UPDATER_OUTPUT)" "$(INSTALL_DIR)/"

# ---------- Help ----------

help:
	@echo "Targets:"
	@echo "  all         - Build release (default)"
	@echo "  release     - Release build and Timestone.zip package"
	@echo "  debug       - Debug build (-O0 -g)"
	@echo "  clean       - Remove build artifacts"
	@echo "  install     - Copy .asi to UMVC3 game directory"
	@echo ""
	@echo "Variables:"
	@echo "  INSTALL_DIR - UMVC3 install path (default: ~/.steam/...)"
	@echo ""
	@echo "Dependencies:"
	@echo "  apt install g++-mingw-w64-x86-64 mingw-w64"
