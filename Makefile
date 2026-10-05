# WarpPDFView: a PDF viewer for AmigaOS 3.x on Picasso96 screens, built with
# vbcc on top of MuPDF.  make -j8 the first time: MuPDF is ~330 files.

# ---- Project ----
VIEWER      := WarpPDFView
BUILD_DIR   := build
# 'make install' copies the binary here, when the directory exists (e.g. the
# shared folder of an emulator).
TARGET_DIR  ?= ~/Documents/Amiga/amiga_uae_disk/

# ---- CPU ----
# FPU build: MuPDF is float throughout.  68060 by default; CPU=68040 for the
# other floor (a separate build dir, the two must not share objects).
CPU         ?= 68060
MATHLIB     := $(if $(filter 68040,$(CPU)),-lm040,-lm060)
# OPT=2 is an experiment: see the -O1 note at CFLAGS.
OPT         ?= 1
OBJ_DIR     := $(BUILD_DIR)/obj-$(CPU)-O$(OPT)
SUFFIX      := $(if $(filter 68040,$(CPU)),_040)$(if $(filter-out 1,$(OPT)),_O$(OPT))
VIEW_BIN    := $(BUILD_DIR)/$(VIEWER)$(SUFFIX)

# ---- SDK / Includes ----
# All overridable on the command line, e.g. make P96_SDK=/path/to/Include.
SDK_DIR     ?= /opt/amiga_sdk
NDK_INC_C   ?= $(SDK_DIR)/NDK3.2R4/Include_H
# The Picasso96 developer includes (libraries/Picasso96.h, and vbcc's
# inline/Picasso96_protos.h) and the MUI 3.8 developer includes.  They come
# after the NDK: the P96 SDK's own proto/ and pragmas/ are for other compilers.
# The defaults are where they sit in the tree this repo is developed in.
P96_SDK     ?= ../p96_gfx_driver/Picasso96Develop/Include
MUI_SDK     ?= ../WarpDiag/mui38dev/MUI/Developer/C/Include

# ---- Toolchain ----
CC      := vc
RM      := rm -rf
MKDIR   := mkdir -p

# ---- Flags ----
# -O1, not -O2: vbcc 0.9h at -O2 miscompiles MuPDF.  pdf_read_start_xref()'s
# do { ... i += 9; ... } while (i-- > 0) loop loses the in-body change to i,
# so every file's startxref parses as 0 and each open falls back to a full
# repair scan.  One confirmed case in 330 files - there may be more.
OPTFLAG := -O$(OPT)
CFLAGS  = +aos68k_std -cpu=$(CPU) -fpu=$(CPU) -c99 $(OPTFLAG) -dontwarn=153 -dontwarn=214 -dontwarn=208 -dontwarn=81
LIBS    := -lamiga -ldebug $(MATHLIB)
LDFLAGS := $(LIBS)

# MuPDF itself: paths, feature set, sources, object rules.
include mupdf.mk

INC_FLAGS   := -Isrc/common $(MUPDF_INC) -I$(NDK_INC_C) -I$(P96_SDK) -I$(MUI_SDK)
DEF_FLAGS   := $(MUPDF_DEFS) $(addprefix -D,$(DEFINES))

# ---- Sources ----
VIEW_SRC  := $(wildcard src/common/*.c) $(wildcard compat/*.c) $(wildcard src/viewer/*.c)
VIEW_OBJS := $(patsubst %.c,$(OBJ_DIR)/app/%.o,$(VIEW_SRC))

.PHONY: all app clean distclean install ftp

# 'all' does not clean first: MuPDF does not change between app builds.
all: app install

app: check-mupdf $(VIEW_BIN)

$(VIEW_BIN): $(VIEW_OBJS) $(MUPDF_OBJS)
	@echo '------  LINK $@ ------'
	$(CC) $(CFLAGS) $(VIEW_OBJS) $(MUPDF_OBJS) $(LDFLAGS) -o $@

$(OBJ_DIR)/app/%.o: %.c
	@$(MKDIR) $(@D)
	$(CC) $(CFLAGS) $(INC_FLAGS) $(DEF_FLAGS) -c $< -o $@

# The app only; the MuPDF objects stay.
clean:
	@echo '------  CLEAN  ------'
	$(RM) $(BUILD_DIR)/$(VIEWER) $(BUILD_DIR)/$(VIEWER)_* $(BUILD_DIR)/obj-*/app

distclean:
	$(RM) $(BUILD_DIR)

install: $(VIEW_BIN)
	@if [ -d $(TARGET_DIR) ]; then echo '------ INSTALL ------'; cp $(VIEW_BIN) $(TARGET_DIR); fi

# Upload to a live Amiga.  The FTP login comes from the AMIGA_FTP_* environment
# variables or tools/ftp_config.local (git-ignored, see the .example next to it).
FTP_DIR ?= SYS:WarpApps
ftp: app
	python3 tools/amiput.py $(VIEW_BIN) -d $(FTP_DIR)
