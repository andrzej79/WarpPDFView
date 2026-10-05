# MuPDF, built for AmigaOS with vbcc: the paths, the feature set, the source
# lists and the object rules.  Included by this repo's Makefile; another
# program can include it too, to link against the same MuPDF objects:
#
#   WPDF_DIR := path/to/WarpPDFView
#   include $(WPDF_DIR)/mupdf.mk
#
# The including Makefile sets CPU, OPT, CFLAGS, CC and MKDIR first.  Objects go
# to $(WPDF_DIR)/build/obj-$(CPU)-O$(OPT)/mupdf, shared by everything that
# includes this.

WPDF_DIR    ?= .

# Not in the repo: ./fetch_mupdf.sh downloads it into thirdparty/ and applies
# patches/ (git-ignored, like every build input that comes from outside).
MUPDF_VER   := 1.28.5
MUPDF_DIR   := $(WPDF_DIR)/thirdparty/mupdf-$(MUPDF_VER)-source
MUPDF_OBJ   := $(WPDF_DIR)/build/obj-$(CPU)-O$(OPT)/mupdf

# compat/ first: it supplies the POSIX headers vbcc's libc lacks.
MUPDF_INC   := $(addprefix -I, \
	$(WPDF_DIR)/compat \
	$(MUPDF_DIR)/include \
	$(MUPDF_DIR)/generated \
	$(MUPDF_DIR)/thirdparty/freetype/include \
	$(MUPDF_DIR)/scripts/freetype \
	$(MUPDF_DIR)/thirdparty/libjpeg \
	$(MUPDF_DIR)/scripts/libjpeg \
	$(MUPDF_DIR)/thirdparty/zlib \
	$(MUPDF_DIR)/thirdparty/jbig2dec)

# PDF only; no JavaScript, no HTML engine (harfbuzz is C++), no ICC (lcms2),
# no JPEG 2000 yet; CJK and Noto fallback fonts left out (TOFU), the base 14
# kept.  Each is a later decision, not a limitation of the port.  NDEBUG as in
# MuPDF's own release build: without it every lock call is checked
# (FITZ_DEBUG_LOCKING) - a few percent of a page on a 68060.
MUPDF_DEFS  := $(addprefix -D, \
	FZ_ENABLE_XPS=0 FZ_ENABLE_SVG=0 FZ_ENABLE_CBZ=0 FZ_ENABLE_IMG=0 \
	FZ_ENABLE_HTML=0 FZ_ENABLE_HTML_ENGINE=0 FZ_ENABLE_EPUB=0 FZ_ENABLE_FB2=0 \
	FZ_ENABLE_MD=0 FZ_ENABLE_MOBI=0 FZ_ENABLE_OFFICE=0 FZ_ENABLE_TXT=0 \
	FZ_ENABLE_JS=0 FZ_ENABLE_BROTLI=0 FZ_ENABLE_BARCODE=0 \
	FZ_ENABLE_DOCX_OUTPUT=0 FZ_ENABLE_ODT_OUTPUT=0 FZ_ENABLE_OCR_OUTPUT=0 \
	FZ_ENABLE_JPX=0 FZ_ENABLE_ICC=0 FZ_ENABLE_HYPHEN=0 \
	TOFU TOFU_CJK \
	HAVE_STDINT_H \
	NDEBUG)

# vc hands its arguments to the compiler through a shell, which eats one level
# of quotes: a string-valued define needs two.
MUPDF_FT_DEFS := -DFT2_BUILD_LIBRARY \
	'-DFT_CONFIG_MODULES_H=\"slimftmodules.h\"' \
	'-DFT_CONFIG_OPTIONS_H=\"slimftoptions.h\"'

# The lists match MuPDF's own Makelists, less what the feature set above
# leaves out.  libjpeg: the encoder too, which output-jpeg.c links against.
MUPDF_SRC := \
	$(wildcard $(MUPDF_DIR)/source/fitz/*.c) \
	$(wildcard $(MUPDF_DIR)/source/pdf/*.c) \
	$(wildcard $(MUPDF_DIR)/generated/resources/fonts/urw/*.c) \
	$(addprefix $(MUPDF_DIR)/thirdparty/freetype/src/, \
	base/ftbase.c base/ftbbox.c base/ftbitmap.c base/ftdebug.c base/ftfstype.c \
	base/ftgasp.c base/ftglyph.c base/ftinit.c base/ftstroke.c base/ftsynth.c \
	base/ftsystem.c base/fttype1.c cff/cff.c cid/type1cid.c psaux/psaux.c \
	pshinter/pshinter.c psnames/psnames.c raster/raster.c sfnt/sfnt.c \
	smooth/smooth.c truetype/truetype.c type1/type1.c) \
	$(addprefix $(MUPDF_DIR)/thirdparty/libjpeg/, \
	jaricom.c jcomapi.c jdapimin.c jdapistd.c jdarith.c jdatasrc.c jdcoefct.c \
	jdcolor.c jddctmgr.c jdhuff.c jdinput.c jdmainct.c jdmarker.c jdmaster.c \
	jdmerge.c jdpostct.c jdsample.c jdtrans.c jerror.c jidctflt.c jidctfst.c \
	jidctint.c jmemmgr.c jquant1.c jquant2.c jutils.c \
	jcapimin.c jcapistd.c jcarith.c jccoefct.c jccolor.c jcdctmgr.c jchuff.c \
	jcinit.c jcmainct.c jcmarker.c jcmaster.c jcparam.c jcprepct.c jcsample.c \
	jdatadst.c jfdctflt.c jfdctfst.c jfdctint.c) \
	$(addprefix $(MUPDF_DIR)/thirdparty/zlib/, \
	adler32.c compress.c crc32.c deflate.c inffast.c inflate.c inftrees.c \
	trees.c uncompr.c zutil.c) \
	$(filter-out %/jbig2dec.c %/jbig2_image_pbm.c %/jbig2_image_png.c, \
	$(wildcard $(MUPDF_DIR)/thirdparty/jbig2dec/jbig2*.c))

MUPDF_OBJS := $(patsubst $(MUPDF_DIR)/%.c,$(MUPDF_OBJ)/%.o,$(MUPDF_SRC))

.PHONY: check-mupdf
check-mupdf:
	@test -d $(MUPDF_DIR) || { echo "MuPDF sources missing - run $(WPDF_DIR)/fetch_mupdf.sh"; exit 1; }

$(MUPDF_OBJ)/thirdparty/freetype/%.o: $(MUPDF_DIR)/thirdparty/freetype/%.c
	@$(MKDIR) $(@D)
	@echo "  CC $<"
	@$(CC) $(CFLAGS) $(MUPDF_INC) $(MUPDF_DEFS) $(MUPDF_FT_DEFS) -c $< -o $@ > $@.log 2>&1 || { cat $@.log; exit 1; }

$(MUPDF_OBJ)/%.o: $(MUPDF_DIR)/%.c
	@$(MKDIR) $(@D)
	@echo "  CC $<"
	@$(CC) $(CFLAGS) $(MUPDF_INC) $(MUPDF_DEFS) -c $< -o $@ > $@.log 2>&1 || { cat $@.log; exit 1; }

# The one confirmed -O2 casualty stays at -O1 in an OPT=2 build.
$(MUPDF_OBJ)/source/pdf/pdf-xref.o: OPTFLAG := -O1
