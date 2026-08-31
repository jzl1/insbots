# Linux i386 build for insbots 0.20.0
#
# Requires the local hl2sdk-episode1 tree and a 32-bit-capable g++ toolchain:
#   apt install g++-multilib gcc-multilib
#
CC := g++
SDK := ../../../hl2sdk-episode1/hl2sdk-episode1
SRC := ../source/src
BUILD := build
OUT := ../insbots_i486.so

ARCH := -m32 -mtune=i686 -march=pentium3 -mmmx -O2
DEFS := -D_LINUX -DARCH=i486 -DNDEBUG -DINSBOT_PROFILER
DEFS += -Dstricmp=strcasecmp -D_stricmp=strcasecmp -D_strnicmp=strncasecmp -Dstrnicmp=strncasecmp
DEFS += -Dstrcmpi=strcasecmp -D_vsnprintf=vsnprintf -D_snprintf=snprintf
DEFS += -D_strdup=strdup -D_alloca=alloca
BUILD_TIME ?= $(shell date -u '+%b %d %Y %H:%M:%S')
DEFS += -DBUILD_TIMESTAMP="\"$(BUILD_TIME)\""
WARN := -w -fno-strict-aliasing
INC := -I$(SRC)
INC += -I$(SDK)/public -I$(SDK)/public/tier0 -I$(SDK)/public/tier1 -I$(SDK)/public/dlls
INC += -I$(SDK)/public/engine -I$(SDK)/public/vstdlib -I$(SDK)/public/mathlib
INC += -I$(SDK)/game_shared -I$(SDK)/tier1
CFLAGS := $(ARCH) $(DEFS) $(WARN) $(INC) -fPIC

OBJS := $(BUILD)/sdk_glue.o $(BUILD)/cfgvar.o $(BUILD)/nav.o $(BUILD)/objective.o
OBJS += $(BUILD)/voice.o $(BUILD)/profile.o $(BUILD)/bot.o $(BUILD)/insbots.o $(BUILD)/convar.o

all: $(OUT)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC)/%.cpp | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/convar.o: $(SDK)/tier1/convar.cpp | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJS)
	$(CC) -m32 -shared -o $@ $(OBJS) -L$(SDK)/linux_sdk -l:tier0_i486.so -l:vstdlib_i486.so -lm -ldl

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
