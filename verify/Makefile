# make -C verify [all|stock|poke|clean]: the stock differential's tools, in verify/build
#   all    phv (the verifier), phv_seed and phv_pre (phv with the special-mode patches), urandom_fixed.so, seedapi
#   stock  the stock reference (stock/build.sh: NLE 0.9.0's sdist + attknum, CMake Release, gcc) and phd_stock.so, the digest
#          compiled against stock's headers and linked to it
#   poke   LIBDIR=DIR POKE=FILE: the state-perturbation control for the engine library in DIR, against this checkout's headers
MAKEFLAGS += -r
ifeq ($(origin CC),default)
CC = gcc
endif
ROOT := $(abspath ..)
B = build
S = stock/build
SRC = $(S)/src/nle-0.9.0
OPT = -O2 -g -march=x86-64-v3
INC = -I$(ROOT)/include -I$(ROOT)/src/third_party/fcontext/include -Isrc
# the defines both engines are built with (the engine adds the pool and TLS model, NLE 0.9.0 seeding and tiles)
NHDEFS = -DDEFAULT_WINDOW_SYS='"rl"' -DDLB -DGCC_WARN -DHACKDIR='"/nonexistent"' -DNOCLIPPING -DNOCWD_ASSUMPTIONS -DNOMAIL \
  -DNOTPARMDECL
sha16 = $(shell cat $(1) | sha256sum | cut -c1-16)

.PHONY: all stock poke clean
all: $(B)/phv $(B)/phv_seed $(B)/phv_pre $(B)/urandom_fixed.so $(B)/seedapi
stock: $(S)/ref/libnethack.so $(B)/phd_stock.so

$(B)/phv: src/phv.c src/keylog.h $(ROOT)/include/ph_digest.h $(ROOT)/include/nletypes.h
	@mkdir -p $(B)
	$(CC) $(OPT) -Wall -Wno-unused-function -Wno-format-truncation $(INC) -rdynamic -o $@ $< -ldl

$(B)/phv_%.c: src/phv.c src/phv_%.patch
	@mkdir -p $(B)
	patch -s -o $@ src/phv.c < src/phv_$*.patch

$(B)/phv_seed $(B)/phv_pre: $(B)/%: $(B)/%.c src/keylog.h $(ROOT)/include/ph_digest.h $(ROOT)/include/nletypes.h
	$(CC) $(OPT) -w $(INC) -rdynamic -o $@ $< -ldl

$(B)/urandom_fixed.so: src/urandom_fixed.c
	@mkdir -p $(B)
	$(CC) -O2 -fPIC -shared -o $@ $< -ldl

$(B)/seedapi: src/seedapi.c $(ROOT)/include/nletypes.h
	@mkdir -p $(B)
	$(CC) -O2 -w $(INC) -o $@ $< -ldl -lpthread

$(S)/ref/libnethack.so: stock/build.sh stock/attknum.patch refs.toml
	stock/build.sh

# the id covers the digest's source and the adapter's, as the engine's build does for its side
$(B)/phd_stock.so: src/phd_stock.c src/appearance_canon.h $(ROOT)/src/ph_digest.c $(ROOT)/include/ph_digest.h $(S)/ref/libnethack.so
	@mkdir -p $(B)
	$(CC) $(OPT) -fPIC $(NHDEFS) -DNLE_ALLOW_SEEDING -DNLE_USE_TILES -DPHD_SRC='"$(ROOT)/src/ph_digest.c"' \
	  -DPHD_SRC_SHA_C=0x$(call sha16,$(ROOT)/src/ph_digest.c)ULL -DPHD_SRC_SHA_H=0x$(call sha16,$(ROOT)/include/ph_digest.h)ULL \
	  -DPHS_ADAPTER_SHA=0x$(call sha16,src/phd_stock.c src/appearance_canon.h)ULL -I$(SRC)/include -I$(S)/cmake/include \
	  -I$(ROOT)/include -Isrc -Wno-deprecated-declarations -c -o $(B)/phd_stock.o src/phd_stock.c
	$(CC) -shared -o $@ $(B)/phd_stock.o -L$(S)/ref -l:libnethack.so

poke:
	@test -n "$(LIBDIR)" -a -n "$(POKE)" || { echo "make poke LIBDIR=DIR POKE=FILE"; exit 2; }
	@mkdir -p $(dir $(POKE))
	$(CC) -O1 -fPIC -shared -march=x86-64-v3 $(NHDEFS) -DNH_POOL_ALLOC -DNH_TLS_INITIAL_EXEC -I$(ROOT)/include -I$(LIBDIR)/include \
	  -I$(ROOT)/src/third_party/libtmt -I$(ROOT)/src/third_party/fcontext/include -o $(POKE).tmp src/phv_poke.c -L$(LIBDIR) -lpufferhack \
	  -Wl,--no-undefined && mv $(POKE).tmp $(POKE)

clean:
	rm -rf $(B)
