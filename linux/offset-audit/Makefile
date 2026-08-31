CC := g++
SDK ?= ../../../hl2sdk-episode1/hl2sdk-episode1
OUT := sendprop_dump_i486.so

CFLAGS := -m32 -mtune=i686 -march=pentium3 -mmmx -O2 -fPIC -w
CFLAGS += -D_LINUX -DARCH=i486 -DNDEBUG
CFLAGS += -Dstricmp=strcasecmp -D_stricmp=strcasecmp
CFLAGS += -D_vsnprintf=vsnprintf -D_snprintf=snprintf
CFLAGS += -I$(SDK)/public -I$(SDK)/public/tier0 -I$(SDK)/public/tier1
CFLAGS += -I$(SDK)/public/dlls -I$(SDK)/public/engine
CFLAGS += -I$(SDK)/public/vstdlib -I$(SDK)/public/mathlib
CFLAGS += -I$(SDK)/game_shared -I$(SDK)/tier1

all: $(OUT)

$(OUT): sendprop_dump.cpp
	$(CC) $(CFLAGS) -shared -o $@ $< \
		-L$(SDK)/linux_sdk -l:tier0_i486.so -l:vstdlib_i486.so -lm -ldl

clean:
	rm -f $(OUT)

.PHONY: all clean
