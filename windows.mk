# windows.mk — Windows/MinGW build overrides
# Usage (from an MSYS2 MinGW64 shell):
#   make -f Makefile.cbm -f windows.mk <target>
#
# Problems solved:
#   1. IS_GCC / IS_MINGW shell detection in Makefile.cbm uses pipe chains that
#      GnuWin32/MSYS2 sh cannot reliably execute — the variables stay empty and
#      all Windows-specific logic (TRE, WIN32_LIBS, wrap flags, GCC warning
#      suppression) is silently skipped.  We force-set both here.
#   2. MinGW GCC does not ship AddressSanitizer/UBSan on Windows.
#      SANITIZE= disables it (Makefile.cbm already documents this override).
#   3. zlib (-lz) is not on the default linker search path.  The MSYS2
#      MinGW64 zlib package drops libz.a into /mingw64/lib.
#   4. On Linux/macOS the test binary uses ASan malloc interception so the
#      mi_* symbols in mem.c/diagnostics.c are never reached at runtime.
#      On Windows (no ASan) they are called directly — mimalloc must be linked.

# ── Compiler ─────────────────────────────────────────────────────────────────
override CC  = gcc
override CXX = g++

# ── Platform detection (fixes broken shell-pipe detection) ───────────────────
# Force both flags so every ifeq($(IS_MINGW),yes) / ifeq($(IS_GCC),yes) block
# in Makefile.cbm activates correctly.
override IS_MINGW = yes
override IS_GCC   = yes

# ── Sanitizers (not supported by MinGW GCC) ──────────────────────────────────
SANITIZE =

# ── GCC-specific warning suppressions (IS_GCC detection was failing) ─────────
GCC_ONLY_FLAGS = -Wno-format-truncation -Wno-stringop-truncation \
                 -Wno-stringop-overflow -Wno-free-nonheap-object \
                 -Wno-alloc-size-larger-than -Wno-unused-result

# ── Linker: add MSYS2 MinGW64 lib dir so -lz resolves ───────────────────────
# Prerequisite: pacman -S mingw-w64-x86_64-zlib
#
# LDFLAGS / LDFLAGS_TEST are rebuilt here to prepend -L/mingw64/lib.
# WIN32_LIBS and MIMALLOC_WRAP_FLAGS are set by Makefile.cbm once IS_MINGW=yes
# is seen, so they expand correctly when these lines are evaluated.
LDFLAGS      = -L/mingw64/lib -lm -lstdc++ -lpthread -lz $(WIN32_LIBS) $(STATIC_FLAGS) $(MIMALLOC_WRAP_FLAGS) $(ELF_HARDENING_FLAGS)
LDFLAGS_TEST = -L/mingw64/lib -lm -lstdc++ -lpthread -lz $(WIN32_LIBS) $(MIMALLOC_WRAP_FLAGS)

# ── mimalloc for test builds ─────────────────────────────────────────────────
# On Windows mem.c/diagnostics.c call mi_* directly (no ASan interception),
# so we compile mimalloc with test flags (-DMI_OVERRIDE=0) and link it.
MIMALLOC_OBJ = $(BUILD_DIR)/mimalloc_test.o

$(MIMALLOC_OBJ): $(MIMALLOC_SRC) | $(BUILD_DIR)
	$(CC) $(MIMALLOC_CFLAGS_TEST) -c -o $@ $<

# ── Override test-foundation to include mimalloc + TRE on Windows ────────────
# TRE_OBJ_TEST is set by Makefile.cbm's ifeq($(IS_MINGW),yes) block once
# IS_MINGW=yes is forced above.
$(BUILD_DIR)/test-foundation: $(TEST_FOUNDATION_SRCS) $(FOUNDATION_SRCS) $(MIMALLOC_OBJ) $(TRE_OBJ_TEST) $(PROJECT_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS_TEST) -o $@ $(TEST_FOUNDATION_SRCS) $(FOUNDATION_SRCS) $(MIMALLOC_OBJ) $(TRE_OBJ_TEST) $(LDFLAGS_TEST)

# ── Application Memory MCP Target ────────────────────────────────────────────
WIN_LOCAL_BIN := $(shell if [ -n "$$USERPROFILE" ]; then cygpath -u "$$USERPROFILE"; else echo "$$HOME"; fi)/.local/bin

$(BUILD_DIR)/application-memory-mcp.exe: $(BUILD_DIR)/codebase-memory-mcp
	cp -f $(BUILD_DIR)/codebase-memory-mcp.exe $@
	mkdir -p $(WIN_LOCAL_BIN)
	cp -f $@ $(WIN_LOCAL_BIN)/application-memory-mcp.exe 2>/dev/null || true

$(BUILD_DIR)/system-memory-mcp.exe: $(BUILD_DIR)/codebase-memory-mcp
	cp -f $(BUILD_DIR)/codebase-memory-mcp.exe $@

# Hook 'cbm' so running make with cbm always creates and installs application-memory-mcp.exe
cbm: $(BUILD_DIR)/application-memory-mcp.exe
	@echo "Built: $(BUILD_DIR)/application-memory-mcp.exe"
	@echo "Installed to: $(WIN_LOCAL_BIN)/application-memory-mcp.exe"

# Convenient aliases
app: cbm
amm: cbm

