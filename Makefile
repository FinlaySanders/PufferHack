# libpufferhack.so and its game data without cmake, bison, flex or network: make [lib|data|install|clean|verify]
# The parsers (util/*_yacc.c, util/*_lex.c, include/*_comp.h), bzip2 and fcontext are checked in.
MAKEFLAGS += -r
ifeq ($(origin CC),default)
CC = gcc
endif
ifeq ($(origin CXX),default)
CXX = g++
endif
ifeq ($(origin AR),default)
AR = gcc-ar
endif
HOSTCC ?= $(CC)
HOSTCFLAGS ?= -O2
CFLAGS ?= -fno-semantic-interposition -march=x86-64-v3 -fno-stack-protector -fcf-protection=none -U_FORTIFY_SOURCE \
  -D_FORTIFY_SOURCE=0 -fno-stack-clash-protection -O3 -DNDEBUG
CXXFLAGS ?= $(CFLAGS)
BUILD ?= build
PREFIX ?= /usr/local
DATA_DIR ?= $(abspath $(BUILD))/dat
HACKDIR ?= $(DATA_DIR)
NLE_BUILD_EPOCH ?= 1788913779

# LTO is required: mon.c always-inlines functions defined in other files
LTOFLAGS = -flto=auto -fno-fat-lto-objects
WARN = -Wno-deprecated-non-prototype -Wno-unused-variable
DEFS = -DDEFAULT_WINDOW_SYS='"rl"' -DDLB -DGCC_WARN -DHACKDIR='"$(HACKDIR)"' -DNOCLIPPING -DNOCWD_ASSUMPTIONS -DNOMAIL \
  -DNOTPARMDECL
INCS = -Iinclude -I$(BUILD)/include -Isrc/third_party/libtmt -Isrc/third_party/fcontext/include
O = $(BUILD)/obj
U = $(BUILD)/util
GEN = $(BUILD)/include/gen.stamp

LIBSRC = $(sort $(wildcard src/*.c) $(wildcard win/tty/*.c) win/rl/winrl.cc)
OBJS = $(patsubst %,$(O)/%.o,$(basename $(LIBSRC)))
FCOBJS = $(addprefix $(O)/src/third_party/fcontext/,source/stack.o asm/make_x86_64_sysv_elf_gas.o asm/jump_x86_64_sysv_elf_gas.o)
TPLIBS = $(O)/libfcontext.a $(O)/libtmt.a

SPECIAL = $(addsuffix .des,bigroom castle endgame gehennom knox medusa mines oracle sokoban tower yendor)
QUEST = $(addsuffix .des,Arch Barb Caveman Healer Knight Monk Priest Ranger Rogue Samurai Tourist Valkyrie Wizard)
DATHELP = help hh cmdhelp keyhelp history opthelp wizhelp
DATIN = $(DATHELP) $(SPECIAL) $(QUEST) bogusmon.txt data.base dungeon.def engrave.txt epitaph.txt oracles.txt quest.txt \
  rumors.fal rumors.tru tribute license symbols
DATDLB = $(DATHELP) dungeon tribute bogusmon data engrave epitaph oracles options quest.dat rumors
TOOLS = $(addprefix $(U)/,makedefs dgn_comp lev_comp dlb mkcanon)
HDRS = $(wildcard include/*.h)

.PHONY: all lib data install clean test verify
all: lib data
lib: $(BUILD)/libpufferhack.so
data: $(BUILD)/dat/nhdat

# '+' hands make's jobserver to the LTO link (else -flto=auto runs one job per hardware thread)
$(BUILD)/libpufferhack.so: $(OBJS) $(TPLIBS)
	+$(CXX) -fPIC $(CXXFLAGS) $(LTOFLAGS) -Wl,-Bsymbolic-functions -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc \
	  -Wl,--wrap=realloc -shared -Wl,-soname,libpufferhack.so -o $@ $(OBJS) -lm $(TPLIBS)

# three archives in CMake's order: the same members and link as its build
$(O)/libfcontext.a: $(FCOBJS)
$(O)/libtmt.a: $(O)/src/third_party/libtmt/tmt.o
$(TPLIBS):
	rm -f $@ && $(AR) qc $@ $^

# the digest's definition id covers its own source
PHD_SHA_C := $(shell sha256sum src/ph_digest.c | cut -c1-16)
PHD_SHA_H := $(shell sha256sum include/ph_digest.h | cut -c1-16)
$(O)/src/ph_digest.o: CPPFLAGS += -DPHD_SRC_SHA_C=0x$(PHD_SHA_C)ULL -DPHD_SRC_SHA_H=0x$(PHD_SHA_H)ULL
$(O)/src/pufferhack.o: CPPFLAGS += -DPH_DATA_DIR='"$(DATA_DIR)"'

$(O)/%.o: %.c | $(GEN)
	@mkdir -p $(@D)
	$(CC) $(DEFS) -DNH_POOL_ALLOC -DNH_TLS_INITIAL_EXEC $(INCS) $(CPPFLAGS) $(CFLAGS) $(LTOFLAGS) -fPIC $(WARN) -MMD -MP \
	  -c $< -o $@

$(O)/%.o: %.cc | $(GEN)
	@mkdir -p $(@D)
	$(CXX) $(DEFS) -DNH_POOL_ALLOC -DNH_TLS_INITIAL_EXEC $(INCS) $(CPPFLAGS) $(CXXFLAGS) -std=gnu++14 $(LTOFLAGS) -fPIC \
	  $(WARN) -MMD -MP -c $< -o $@

$(O)/src/third_party/%.o: src/third_party/%.c
	@mkdir -p $(@D)
	$(CC) $(TPDEFS) $(CFLAGS) $(TPSTD) $(LTOFLAGS) -fPIC $(WARN) -c $< -o $@

$(O)/src/third_party/%.o: src/third_party/%.S
	@mkdir -p $(@D)
	$(CC) $(TPDEFS) -O3 -DNDEBUG -fPIC $(WARN) -c $< -o $@

$(O)/src/third_party/libtmt/%.o: TPDEFS = $(DEFS)
$(O)/src/third_party/libtmt/%.o: TPSTD = -std=gnu11
$(O)/src/third_party/fcontext/%.o: TPDEFS = -DBOOST_CONTEXT_EXPORT= -Isrc/third_party/fcontext/include/fcontext

# host tools; gc-sections drops nh_ctx_gen.c's unused reference into mon.c (CMake needs LTO for that)
TOOLCC = $(HOSTCC) $(HOSTCFLAGS) $(DEFS) -Iinclude -I$(BUILD)/include $(WARN) -ffunction-sections -Wl,--gc-sections
$(U)/makedefs: util/makedefs.c src/monst.c src/objects.c $(HDRS)
	@mkdir -p $(@D)
	$(TOOLCC) -DNLE_OBJECTS_GLOBAL -DNLE_BUILD_EPOCH=$(NLE_BUILD_EPOCH) -o $@ $(filter %.c,$^)
$(U)/mkcanon: util/mkcanon.c src/objects.c $(HDRS)
	@mkdir -p $(@D)
	$(TOOLCC) -o $@ $<
$(U)/dgn_comp: util/dgn_main.c util/panic.c src/alloc.c util/dgn_yacc.c util/dgn_lex.c $(HDRS) $(GEN)
	$(TOOLCC) -o $@ $(filter %.c,$^)
$(U)/lev_comp: util/lev_main.c util/panic.c src/decl.c src/nh_ctx_gen.c src/nh_ctx_fixup.c src/monst.c src/objects.c \
  src/alloc.c src/drawing.c util/lev_yacc.c util/lev_lex.c $(HDRS) $(GEN)
	$(TOOLCC) -DNLE_OBJECTS_GLOBAL -DNLE_LEVEL_COMPILER -o $@ $(filter %.c,$^)
$(U)/dlb: util/dlb_main.c src/dlb.c util/panic.c src/alloc.c $(HDRS) $(GEN)
	$(TOOLCC) -DNLE_OBJECTS_GLOBAL -o $@ $(filter %.c,$^)

# makedefs -v writes ../include/date.h and ../dat/options, so it runs here only
$(GEN): $(U)/makedefs $(U)/mkcanon
	@mkdir -p $(BUILD)/include $(BUILD)/dat
	cd $(U) && ./makedefs -v && ./makedefs -o && ./makedefs -p && ./mkcanon ../include/appearance_canon.h
	@touch $@

$(BUILD)/dat/nhdat: $(addprefix dat/,$(DATIN)) $(TOOLS) $(GEN)
	cp $(addprefix dat/,$(DATIN)) $(BUILD)/dat/
	cd $(BUILD)/dat && for o in s d r q h e; do ../util/makedefs -$$o || exit 1; done && ../util/dgn_comp dungeon.pdf && \
	  ../util/lev_comp $(SPECIAL) && ../util/lev_comp $(QUEST) && touch perm record logfile xlogfile && \
	  LC_ALL=C ../util/dlb cf nhdat $(DATDLB) $$(LC_ALL=C ls *.lev)

install: all
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/share/nethackdir
	install -m 755 $(BUILD)/libpufferhack.so $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/pufferhack.h $(DESTDIR)$(PREFIX)/include/
	cp -p $(BUILD)/dat/* $(DESTDIR)$(PREFIX)/share/nethackdir/

clean:
	rm -rf $(BUILD)

# the stock differential's tools and the stock reference (cmake and the NLE 0.9.0 sdist): verify/README.md
verify: all
	$(MAKE) -C verify all stock

-include $(OBJS:.o=.d)

test: all
	rm -rf $(BUILD)/test && mkdir -p $(BUILD)/test && cp -r $(BUILD)/dat $(BUILD)/test/hack
	printf 'OPTIONS=name:Agent,role:Val,race:human,gender:female,align:neutral,!autopickup,nobones,nolegacy,disclose:-i -a -v -g -c -o\n' > $(BUILD)/test/nhrc
	$(CC) -O1 -Wall -Wextra -Iinclude -o $(BUILD)/test/smoke tests/smoke.c -L$(BUILD) -lpufferhack -Wl,-rpath,$(abspath $(BUILD))
	touch $(BUILD)/test/stamp && ls -d $${TMPDIR:-/dev/shm}/pufferhack-* 2>/dev/null | sort > $(BUILD)/test/vardirs.before; true
	TMPDIR= $(BUILD)/test/smoke $(abspath $(BUILD))/dat $(BUILD)/test/nhrc
	test -z "$$(find $(BUILD)/dat -newer $(BUILD)/test/stamp)" || { echo "FAIL: the game wrote into the data directory"; exit 1; }
	ls -d /dev/shm/pufferhack-* /tmp/pufferhack-* 2>/dev/null | sort | comm -13 $(BUILD)/test/vardirs.before - | \
	  { ! grep . || { echo "FAIL: a private game directory was left behind"; exit 1; }; }
	printf 'OPTIONS=name:Agent,role:Val,race:human,gender:female,align:neutral,nosuchoption\n' > $(BUILD)/test/badrc
	$(CC) -O1 -Iinclude -Isrc/third_party/fcontext/include -Itests -o $(BUILD)/test/badoptions tests/badoptions.c -L$(BUILD) -lpufferhack \
	  -Wl,-rpath,$(abspath $(BUILD))
	$(BUILD)/test/badoptions $(BUILD)/test/hack $(BUILD)/test/badrc
	$(CC) -O1 -Wno-format-truncation -Iinclude -Isrc/third_party/fcontext/include -Iverify/src -o $(BUILD)/test/teardown \
	  tests/teardown.c -L$(BUILD) -lpufferhack -Wl,-rpath,$(abspath $(BUILD))
	$(BUILD)/test/teardown $(abspath $(BUILD))/dat $(abspath $(BUILD))/test tests/teardown.keys
	$(CC) $(DEFS) $(INCS) -O1 $(WARN) -o $(BUILD)/test/rndargs tests/rndargs.c src/rnd.c src/isaac64.c
	$(BUILD)/test/rndargs
	cp -r $(BUILD)/dat $(BUILD)/test/hackB
	$(CC) -O1 -Iinclude -Isrc/third_party/fcontext/include -Itests -o $(BUILD)/test/caches tests/caches.c -L$(BUILD) -lpufferhack \
	  -Wl,-rpath,$(abspath $(BUILD))
	$(BUILD)/test/caches $(BUILD)/test/hack $(BUILD)/test/hackB $(BUILD)/test
	$(CC) -O1 -Iinclude -Isrc/third_party/fcontext/include -Itests -o $(BUILD)/test/latebind tests/latebind.c -L$(BUILD) -lpufferhack \
	  -Wl,-rpath,$(abspath $(BUILD))
	$(BUILD)/test/latebind $(BUILD)/test/hack
	$(CC) -O1 -Iinclude -Isrc/third_party/fcontext/include -Itests -o $(BUILD)/test/ckptopts tests/ckptopts.c -L$(BUILD) -lpufferhack \
	  -Wl,-rpath,$(abspath $(BUILD))
	$(BUILD)/test/ckptopts $(BUILD)/test/hack
	$(CC) -O1 -Iinclude -Isrc/third_party/fcontext/include -Itests -o $(BUILD)/test/ckpt_save tests/ckpt_save.c -L$(BUILD) -lpufferhack \
	  -Wl,-rpath,$(abspath $(BUILD))
	$(BUILD)/test/ckpt_save $(BUILD)/test/hack
	$(CC) -O1 -Iinclude -Isrc/third_party/fcontext/include -Itests -o $(BUILD)/test/ckpt_history tests/ckpt_history.c -L$(BUILD) -lpufferhack \
	  -Wl,-rpath,$(abspath $(BUILD))
	$(BUILD)/test/ckpt_history $(BUILD)/test/hack
