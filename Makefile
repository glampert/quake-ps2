# ============================================================================
#  Quake PS2 - build system
# ----------------------------------------------------------------------------
#  Produces build/<config>/quake.elf for the PS2 EE, using the modern ps2dev
#  toolchain (mips64r5900el-ps2-elf-*). Built on the PS2SDK sample makefiles.
#
#    make             -> debug build -> build/debug/quake.elf   (VSCode: Shift+Cmd+B)
#    make release     -> optimized   -> build/release/quake.elf
#    make run         -> build + launch in PCSX2  (VSCode: F5)
#    make release run -> the same, with the release build
#    make tools       -> build host tools (unpak, musenc, symbolize) into build/tools/
#    make music       -> encode id1/music/trackNN.wav into the trackNN.adp files the game streams
#    make clean       -> remove build artifacts (both configs)
#    make clean_vu    -> remove only assembled VU microprograms
#
#  Header dependencies are tracked automatically (-MMD), so editing a header
#  rebuilds just the affected objects; no more `make clean` after header edits.
# ============================================================================

# Toolchain / tool locations (override from the environment if needed):
PS2DEV ?= /Users/guilherme/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
PCSX2  ?= /Applications/PCSX2.app/Contents/MacOS/PCSX2

# ----------------------------------------------------------------------------
#  Build configuration
# ----------------------------------------------------------------------------
#
#  Picked by the `release` goal, or by BUILD= on the command line:
#
#      make          -> debug   -O2, DWARF info, asserts and debug-only code IN
#      make release  -> release -O3, no DWARF, asserts and debug-only code OUT
#
#  Each config owns its object tree under build/<config>/, so alternating
#  between the two neither mixes objects built with different flags nor forces
#  a full rebuild.

ifneq ($(filter release,$(MAKECMDGOALS)),)
    BUILD := release
else
    BUILD ?= debug
endif

ifeq ($(filter $(BUILD),debug release),)
    $(error BUILD must be 'debug' or 'release', got '$(BUILD)')
endif

# Strip the linked ELF - in BOTH configs, since the debug build is what gets
# iterated on and DWARF dominates its size. Costs nothing at runtime: the PS2
# loader only reads program headers, which strip leaves alone. The symbols are
# not lost, they stay in quake_unstripped.elf beside it (feed that one to
# addr2line to resolve a crash address). STRIP_ELF=0 turns this off and ships
# the unstripped ELF as quake.elf instead.
STRIP_ELF ?= 1

SRC_DIR    = src
BUILD_DIR  = build
OUTPUT_DIR = $(BUILD_DIR)/$(BUILD)

# The SDK link rule (Makefile.eeglobal_cpp) produces $(EE_BIN) with full symbols;
# $(GAME_ELF) is the binary that actually runs, stripped out of it below.
EE_BIN   = $(OUTPUT_DIR)/quake_unstripped.elf
GAME_ELF = $(OUTPUT_DIR)/quake.elf

# ----------------------------------------------------------------------------
#  Source files
# ----------------------------------------------------------------------------

# New PS2 backend, modern C++:
PS2_CXX_SRC =                         \
	ps2/system/main.cpp               \
	ps2/system/sys.cpp                \
	ps2/system/iop_boot.cpp           \
	ps2/system/heap.cpp               \
	ps2/math/vec_mat.cpp              \
	ps2/net/net.cpp                   \
	ps2/input/input.cpp               \
	ps2/audio/snd.cpp                 \
	ps2/audio/cd_audio.cpp            \
	ps2/renderer/gs.cpp               \
	ps2/renderer/vram.cpp             \
	ps2/renderer/texture.cpp          \
	ps2/renderer/scrap_atlas.cpp      \
	ps2/renderer/profile.cpp          \
	ps2/renderer/vu1.cpp              \
	ps2/renderer/cmd_buffer.cpp       \
	ps2/renderer/render_system.cpp    \
	ps2/renderer/clip.cpp             \
	ps2/renderer/vid.cpp              \
	ps2/renderer/draw.cpp             \
	ps2/renderer/texmgr.cpp           \
	ps2/renderer/refresh.cpp          \
	ps2/renderer/overlays.cpp         \
	ps2/tests/draw_cube.cpp           \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/pipeline_dump.cpp       \
	ps2/debug/exception_handler.cpp   \
	ps2/debug/profile.cpp

# Doug Lea's allocator: vendored third-party C, left as C on purpose (see the
# note at the top of dlmalloc.c). Everything else of ours is C++.
PS2_C_SRC = ps2/system/dlmalloc/dlmalloc.c

# QuakeSpasm's C, statically linked. Its OpenGL renderer is not here: the PS2
# backend implements the renderer's public surface (draw.h, render.h,
# gl_texmgr.h, vid.h) instead. The gl_*/r_* files listed are the ones that hold
# engine logic - gl_model.c's BSP loading feeds the server too - built with
# their OpenGL halves cut out.
ENGINE_C_SRC = \
	quake/chase.c      quake/cl_demo.c    quake/cl_input.c   quake/cl_main.c   \
	quake/cl_parse.c   quake/cl_tent.c    quake/cmd.c        quake/common.c    \
	quake/console.c    quake/crc.c        quake/cvar.c       quake/cfgfile.c   \
	quake/host.c       quake/host_cmd.c   quake/keys.c       quake/mathlib.c   \
	quake/menu.c       quake/net_main.c   quake/net_loop.c   quake/pr_cmds.c   \
	quake/pr_edict.c   quake/pr_exec.c    quake/sbar.c       quake/snd_dma.c   \
	quake/snd_mem.c    quake/snd_mix.c    quake/sv_main.c    quake/sv_move.c   \
	quake/sv_phys.c    quake/sv_user.c    quake/view.c       quake/wad.c       \
	quake/world.c      quake/zone.c       quake/strlcat.c    quake/strlcpy.c   \
	quake/gl_model.c   quake/gl_refrag.c  quake/gl_rlight.c  quake/gl_screen.c \
	quake/gl_fog.c     quake/r_part.c

C_SRC   = $(PS2_C_SRC) $(ENGINE_C_SRC)
CXX_SRC = $(PS2_CXX_SRC)

# Backend sources that run at load time or not at all in a normal frame: asset
# parsing, IOP module boot, device setup, the debug screen printer. None of them
# are on the per-frame path, so they are built for size instead of speed, which
# is RAM the levels get to use instead.
SIZE_OPT_CXX_SRC =                    \
	ps2/renderer/texture.cpp          \
	ps2/renderer/scrap_atlas.cpp      \
	ps2/system/iop_boot.cpp           \
	ps2/renderer/vid.cpp              \
	ps2/tests/draw_cube.cpp           \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/pipeline_dump.cpp       \
	ps2/debug/exception_handler.cpp   \
	ps2/debug/profile.cpp

SIZE_OPT_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(SIZE_OPT_CXX_SRC:.cpp=.o))

C_OBJS   = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(C_SRC:.c=.o))
CXX_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(CXX_SRC:.cpp=.o))

# VU microprograms: vclpp -> openvcl -> dvp-as
# Each .vcl assembles into .vudata with <name>_CodeStart/_CodeEnd link symbols
# (see PS2_DECLARE_VU_MICROPROGRAM in ps2/renderer/vu1.h).
# None of the EE compiler flags reach this toolchain, so the output is identical
# in both configs: build it once into build/vu/ and share it.
VCL_PATH  = $(SRC_DIR)/ps2/renderer/vu1progs
VCL_FILES = textured_triangles.vcl particles.vcl
VU_OBJS   = $(addprefix $(BUILD_DIR)/vu/, $(VCL_FILES:.vcl=.o))

# Shared macro/constant includes. The programs include these by bare name, which
# vclpp finds next to the including file (and through -I $(VCL_PATH) as well);
# they are prerequisites here because the pattern rule below would not otherwise
# see them change.
VCL_INCS  = $(wildcard $(VCL_PATH)/*.i)

# The openvcl/dvp-as output checks every VU build runs (see the rule below). They
# come in as a git submodule (https://github.com/glampert/vu-checker), so other
# PS2 projects can share them.
VU_CHECK = $(SRC_DIR)/tools/vu-checker/check_vu_code.py

# vclpp is not part of the ps2dev distribution: it comes in as a git submodule
# (https://github.com/glampert/vclpp) and is built by its own Makefile, so every
# VU build runs the pinned version rather than whatever is on PATH - an older
# vclpp silently mangles macro bodies that have comments in them. vclpp has a
# submodule of its own, parse-utils (https://github.com/glampert/parse-utils),
# whose lexer it is built with.
VCLPP_PATH        = $(SRC_DIR)/tools/vclpp
VCLPP_PARSE_UTILS = $(VCLPP_PATH)/external/parse-utils
VCLPP             = $(BUILD_DIR)/tools/vclpp

# miniz (https://github.com/richgel999/miniz), the deflate codec the save games are
# compressed with. A git submodule like vclpp; only the raw deflate/inflate and
# CRC-32 sources are built, straight from the submodule. Its headers include a
# miniz_export.h that miniz's CMake would generate: the one in MINIZ_CFG_PATH stands
# in for it and also carries the build configuration, so the library and every file
# including miniz.h agree on it (struct sizes depend on TDEFL_LESS_MEMORY).
MINIZ_PATH     = $(SRC_DIR)/tools/miniz
MINIZ_CFG_PATH = $(SRC_DIR)/ps2/save/miniz_cfg
MINIZ_SRC      = miniz.c miniz_tdef.c miniz_tinfl.c
MINIZ_OBJS     = $(addprefix $(OUTPUT_DIR)/miniz/, $(MINIZ_SRC:.c=.o))

# Standalone command line tools: the C++ ones under src/tools/host, built with the
# HOST C++ compiler (not the EE toolchain) since they run on the development
# machine, and the Python ones under src/tools/scripts. Being host binaries they
# are config-independent, so they live outside build/<config>/.
HOST_TOOLS_PATH = $(SRC_DIR)/tools/host
SCRIPTS_PATH    = $(SRC_DIR)/tools/scripts
TOOLS_CXX_BINS  = $(addprefix $(BUILD_DIR)/tools/, unpak musenc)
TOOLS_PY_BINS   = $(addprefix $(BUILD_DIR)/tools/, symbolize)
TOOLS_BINS      = $(TOOLS_CXX_BINS) $(TOOLS_PY_BINS)
HOST_CXX       ?= c++
HOST_CXXFLAGS  ?= -std=gnu++20 -O2 -Wall

# IOP/IRX modules embedded into the ELF: HDD/PFS and BDM USB storage, booted
# by ps2/system/iop_boot.cpp when the game data isn't on host:,
# the USB keyboard driver started on demand by ps2/input/keyboard.cpp, and the
# sound driver pair (libsd under audsrv) started by ps2/audio/audsrv_device.cpp.
IRX_PATH  = $(PS2SDK)/iop/irx
IRX_FILES = iomanX.irx fileXio.irx \
            ps2dev9.irx ps2atad.irx ps2hdd.irx ps2fs.irx \
            bdm.irx bdmfs_fatfs.irx usbd.irx usbmass_bd.irx \
            ps2kbd.irx libsd.irx audsrv.irx

IRX_OBJS  = $(addprefix $(OUTPUT_DIR)/irx/, $(IRX_FILES:.irx=.o))

EE_OBJS = $(C_OBJS) $(CXX_OBJS) $(VU_OBJS) $(IRX_OBJS) $(MINIZ_OBJS)
DEPS    = $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d) $(MINIZ_OBJS:.o=.d)

# ----------------------------------------------------------------------------
#  Compiler / linker flags (appended to the SDK defaults from Makefile.eeglobal)
# ----------------------------------------------------------------------------

# Per-config flags. EE_OPTFLAGS and EE_DBGINFOFLAGS are the SDK's own knobs:
# Makefile.eeglobal_cpp defaults them with ?= (to -O2 and -gdwarf-2 -gz), so
# whatever is set here wins - including setting the debug info to empty.
#
# PS2_QUAKE_DEBUG, PS2_QUAKE_ASSERTS and PS2_QUAKE_PROFILE are always defined to 0 or 1,
# never undefined.
ifeq ($(BUILD),release)
    EE_OPTFLAGS     = -O3
    EE_DBGINFOFLAGS =
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=0 -DNDEBUG
else
    EE_OPTFLAGS     = -O2
    EE_DBGINFOFLAGS = -gdwarf-2 -gz
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=1 -DPS2_QUAKE_ASSERTS=1 -DPS2_QUAKE_PROFILE=1
endif

COMMON_DEFS = -DPS2_QUAKE $(CONFIG_DEFS)

EE_INCS += -I$(SRC_DIR)

# The C side of the build is QuakeSpasm, the vendored dlmalloc and the bin2c IRX
# blobs - everything of ours is C++. QuakeSpasm is C11: common.h's q_min/q_max/CLAMP
# use _Generic there, and GCC 15 would otherwise compile it as C23. It keeps the
# SDK's -Wall and is not held to the backend's -Werror set.
#
# -fsingle-precision-constant: QuakeSpasm writes its float constants unsuffixed
# (x * 0.5), which C makes double - and the EE has no double FPU, so every one of
# those would turn a float expression into libgcc soft-float calls. The backend's
# C++ needs no such flag: it uses f suffixes, and -Wdouble-promotion enforces them.
#
EE_CFLAGS += -std=gnu11 -fno-strict-aliasing -fsingle-precision-constant $(COMMON_DEFS) -MMD -MP

# Strict, portable, warnings-as-errors for the new C++ backend (applies ONLY to
# our .cpp - QuakeSpasm's C above stays lenient). The set targets portability and
# undefined behaviour: value-changing/alignment/format hazards, accidental
# float->double promotion (the EE has no hardware doubles), shadowing, VLAs, and
# GCC's near-zero-false-positive logic/duplicate-branch checks.
# -Wconversion/-Wsign-conversion flag every implicit value-, sign- or precision-
# changing conversion (all backend code must cast intentionally); SDK/STL library
# conversions are silenced via -isystem below, so only our own code is enforced.
EE_CXX_WARNFLAGS = -Wall -Wextra -Werror \
	-Wshadow -Wdouble-promotion -Wconversion -Wsign-conversion \
	-Wformat=2 -Wno-format-nonliteral -Wundef -Wpointer-arith \
	-Wcast-align -Wwrite-strings -Wredundant-decls -Wnull-dereference \
	-Wnon-virtual-dtor -Woverloaded-virtual -Wvla \
	-Wlogical-op -Wduplicated-cond -Wduplicated-branches

# Reclassify the PS2SDK headers as system headers for C++ so their own warnings
# (e.g. redundant redeclarations in kernel.h) don't trip our -Werror. The same
# dirs are still added via -I by Makefile.eeglobal; GCC then ignores the -I copy
# and treats them as system. Our own headers stay under -Isrc (warnings enforced).
EE_CXX_SYSINCS = -isystem $(PS2SDK)/ee/include -isystem $(PS2SDK)/common/include \
	-isystem $(MINIZ_PATH) -isystem $(MINIZ_CFG_PATH)

# Lean, embedded C++ for the new backend.
EE_CXXFLAGS += -std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-strict-aliasing $(COMMON_DEFS) \
	$(EE_CXX_WARNFLAGS) $(EE_CXX_SYSINCS) \
	-MMD -MP

# -leedebug supplies the level 1 exception vector that src/ps2/debug/exception_handler.cpp
# hangs its post-mortem off. It contributes nothing to a release build - the whole
# handler is behind PS2_QUAKE_DEBUG - but the linker only pulls in what is referenced,
# so leaving it on the line for both configs costs nothing.
EE_LIBS += -lkernel -ldraw -lgraph -lpacket2 -ldma -lpad -lkbd -laudsrv -lpatches -lfileXio -lmc -leedebug

# ----------------------------------------------------------------------------
#  Rules
# ----------------------------------------------------------------------------

.PHONY: all release run tools music clean clean_vu compiledb

all: $(GAME_ELF) tools

# `release` only selects the config (see BUILD above); the build itself is `all`.
release: all

# Records which STRIP_ELF setting the current $(GAME_ELF) was made with. make
# compares timestamps, not recipes, so without this a `make STRIP_ELF=0` over an
# already-built tree would leave the stripped ELF in place and report nothing to
# do. Flipping the flag switches to a marker that does not exist yet, which
# re-makes the ELF below.
STRIP_MARKER = $(OUTPUT_DIR)/.strip_elf-$(STRIP_ELF)

$(STRIP_MARKER):
	@mkdir -p $(dir $@)
	@rm -f $(OUTPUT_DIR)/.strip_elf-*
	@touch $@

# The runnable ELF, made from the symbol-carrying one the SDK link rule builds.
ifeq ($(STRIP_ELF),0)
$(GAME_ELF): $(EE_BIN) $(STRIP_MARKER)
	cp -f $< $@
else
$(GAME_ELF): $(EE_BIN) $(STRIP_MARKER)
	$(EE_STRIP) --strip-all -o $@ $<
endif

# Out-of-tree object rules. These static-pattern rules take precedence over the
# generic %.o rules from Makefile.eeglobal so objects land under build/<config>/
# mirroring the src/ tree. ($(EE_BIN) link rule comes from Makefile.eeglobal_cpp.)
#
# One rule per language: the C one is only reached by QuakeSpasm and dlmalloc,
# every backend source goes through the C++ one.
$(C_OBJS): $(OUTPUT_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

# dlmalloc is configured with MORECORE_CANNOT_TRIM (see dlmalloc.c), which leaves its
# sYSTRIm helper unused. Vendored code: the warning is silenced for that one file
# rather than for QuakeSpasm as well.
$(OUTPUT_DIR)/$(SRC_DIR)/ps2/system/dlmalloc/dlmalloc.o: EE_CFLAGS += -Wno-unused-function

$(CXX_OBJS): $(OUTPUT_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(EE_CXX) $(EE_CXXFLAGS) $(EE_INCS) $(CXX_OPTFLAGS_FOR) -c $< -o $@

# Per-object optimization override for the cold sources listed above. EE_CXXFLAGS
# already carries $(EE_OPTFLAGS) from Makefile.eeglobal; appending -Os after it
# wins, since the last -O on the command line is the one GCC applies. Target-
# specific variables are inherited by the rule above, so only these objects see it.
$(SIZE_OPT_OBJS): CXX_OPTFLAGS_FOR = -Os

# miniz, straight out of the submodule. Always -O3, in the debug config too: deflating
# a level's state is a few hundred KB of work on a slow CPU, done while the player waits.
# Its warnings are not ours to fix.
$(MINIZ_OBJS): $(OUTPUT_DIR)/miniz/%.o: $(MINIZ_PATH)/%.c
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) -O3 -w -I$(MINIZ_PATH) -I$(MINIZ_CFG_PATH) -c $< -o $@

# A clone without the submodule has no miniz sources, so the rule above has nothing to
# build from; this one stops with the fix instead. It never runs while the files exist.
$(MINIZ_PATH)/%.c:
	@echo "$(MINIZ_PATH) is empty - run 'git submodule update --init'"; exit 1

# The vclpp submodule, through its own Makefile. That one recompiles on every
# run, so the up-to-date check is made here instead: it runs again only when the
# submodule's sources or Makefile change, as checking out a new commit does.
# BIN_TARGET puts the binary under build/tools/ rather than in the submodule's
# work tree, where git would report it as untracked content. Every VU program
# depends on the binary, so a new vclpp rebuilds them all.
$(VCLPP): $(wildcard $(VCLPP_PATH)/Makefile $(VCLPP_PATH)/src/*.cpp $(VCLPP_PATH)/src/*.hpp $(VCLPP_PARSE_UTILS)/*.hpp)
	@test -f $(VCLPP_PATH)/Makefile && test -f $(VCLPP_PARSE_UTILS)/lexer.hpp || \
		{ echo "$(VCLPP_PATH) is incomplete - run 'git submodule update --init --recursive'"; exit 1; }
	@mkdir -p $(dir $@)
	@$(MAKE) --no-print-directory -C $(VCLPP_PATH) CXX=$(HOST_CXX) BIN_TARGET=$(abspath $@)

# The vu-checker submodule. The script needs no build step, so this rule only
# runs when the file is missing - an uninitialized submodule. Every VU program
# depends on it, so a new vu-checker checks them all again.
$(VU_CHECK):
	@echo "$(patsubst %/,%,$(dir $@)) is incomplete - run 'git submodule update --init --recursive'"; exit 1

# VU1 microprograms.
# The checks in check_vu_code.py are not optional, and every one of them exists
# because the toolchain fails silently. openvcl allocates VI registers by
# liveness and gets it wrong on control flow past a single counted loop - it
# hands a live register to a temporary, with no diagnostic, and the microprogram
# then runs away or reads garbage. It pads a clip flag or Q read for latency only
# within a basic block. dvp-as truncates an immediate that does not fit its field
# and says nothing, so a constant one larger than the instruction can hold
# becomes a different constant. All of them reach the screen rather than the
# build log.
# The register allocation check needs a second openvcl run with -c for the source
# names, and the branch check reads the object, so the checks run once dvp-as is
# done and a failure deletes the object - otherwise the next make would take the
# bad object as up to date. The 'operand out of range' warnings from dvp-as are
# expected; see the branch check for why they are harmless.
$(BUILD_DIR)/vu/%.o: $(VCL_PATH)/%.vcl $(VCL_INCS) $(VCLPP) $(VU_CHECK)
	@mkdir -p $(dir $@)
	$(VCLPP) -I $(VCL_PATH) -Wundef -Werror -j $< $(basename $@).pp.vcl
	openvcl -o $(basename $@).vsm $(basename $@).pp.vcl
	@openvcl -c -o $(basename $@).c.vsm $(basename $@).pp.vcl
	dvp-as $(basename $@).vsm -o $@
	@python3 $(VU_CHECK) $@ || { rm -f $@; exit 1; }

# IOP modules embedded via bin2c.
$(OUTPUT_DIR)/irx/%.o: $(IRX_PATH)/%.irx
	@mkdir -p $(dir $@)
	bin2c $< $(basename $@).c $(notdir $(basename $@))_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(basename $@).c -o $@

# Host tools: each is a single self-contained .cpp compiled straight to a binary.
tools: $(TOOLS_BINS)

$(TOOLS_CXX_BINS): $(BUILD_DIR)/tools/%: $(HOST_TOOLS_PATH)/%.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_CXXFLAGS) $< -o $@

# musenc shares the ADPCM decoder with the game, so it plays back exactly what it measured.
$(BUILD_DIR)/tools/musenc: $(SRC_DIR)/ps2/audio/spu_adpcm.h

# The soundtrack for the CD audio module (ps2/audio/cd_audio.cpp): every trackNN.wav in
# MUSIC_DIR, any case, is encoded to a lowercase trackNN.adp beside it, skipping the ones
# already newer than both their .wav and the encoder. The .wav files are only the source;
# the game never reads them, so they needn't go onto the USB stick.
MUSIC_DIR ?= id1/music

music: $(BUILD_DIR)/tools/musenc
	@found=0; \
	for wav in $(MUSIC_DIR)/[Tt]rack*.wav; do \
		[ -e "$$wav" ] || continue; \
		found=1; \
		adp="$(MUSIC_DIR)/$$(basename "$$wav" .wav | tr 'A-Z' 'a-z').adp"; \
		if [ "$$adp" -nt "$$wav" ] && [ "$$adp" -nt $(BUILD_DIR)/tools/musenc ]; then continue; fi; \
		$(BUILD_DIR)/tools/musenc "$$wav" "$$adp" || exit 1; \
	done; \
	[ $$found = 1 ] || { echo "No trackNN.wav files in $(MUSIC_DIR)/"; exit 1; }

# Script tools are published into build/tools/ under the same extensionless names
# as the compiled ones, so everything in there is invoked the same way.
$(TOOLS_PY_BINS): $(BUILD_DIR)/tools/%: $(SCRIPTS_PATH)/%.py
	@mkdir -p $(dir $@)
	cp -f $< $@
	@chmod +x $@

# PCSX2 exposes the ELF's directory as host:, so the game data must be reachable
# as build/<config>/id1. A symlink back to the repo's id1/ does it.
$(OUTPUT_DIR)/id1:
	@mkdir -p $(dir $@)
	ln -sfn $(abspath id1) $@

# The game's command line, handed to the ELF through PCSX2's -gameargs, e.g.
# make run RUN_ARGS="-heapsize 20480". +commands need the registered data: on the shareware pak
# QuakeSpasm ignores them, as id's Quake did, so script a session with id1/autoexec.cfg instead.
RUN_ARGS ?=

run: all $(OUTPUT_DIR)/id1
	$(PCSX2) -batch -elf $(abspath $(GAME_ELF))$(if $(RUN_ARGS), -gameargs "$(RUN_ARGS)")

# Regenerate compile_commands.json so the editor's IntelliSense uses the exact
# per-file compile flags. Run after adding/removing source files.
compiledb:
	@$(MAKE) -Bnk | python3 $(SCRIPTS_PATH)/gen_compile_commands.py

# Both configs, not just the selected one.
clean:
	rm -rf $(BUILD_DIR)/debug $(BUILD_DIR)/release $(BUILD_DIR)/vu $(BUILD_DIR)/tools

clean_vu:
	rm -rf $(BUILD_DIR)/vu

-include $(DEPS)

# Pull in the PS2SDK toolchain definitions and the C++ link rule. These provide
# EE_CC/EE_CXX/EE_STRIP, the -D_EE/-G0 defaults (the optimization and debug-info
# ones are set per-config above), EE_LDFLAGS (linkfile, max-page-size) and the
# `$(EE_BIN): $(EE_OBJS)` link recipe (links with g++).
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal_cpp
