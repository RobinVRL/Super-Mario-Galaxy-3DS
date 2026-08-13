# Super Mario Galaxy 3DS bring-up target for devkitARM/libctru.
#
# This file is deliberately single-pass. Invoke it from an empty or existing
# build directory, for example:
#   make -C build -f ../Makefile TOPDIR=..
# Keeping TOPDIR relative also avoids leaking a workspace path containing spaces
# into compiler and linker arguments.

.SUFFIXES:
.DEFAULT_GOAL := all

ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set; install devkitPro's 3ds-dev group")
endif

ifneq ($(notdir $(CURDIR)),build)
$(error "Run make from a build directory: make -C build -f ../Makefile TOPDIR=..")
endif

TOPDIR ?= ..
include $(DEVKITARM)/3ds_rules

TARGET       := smg3ds-bringup
SOURCE_DIRS  := $(TOPDIR)/source $(TOPDIR)/source/petari \
                $(TOPDIR)/external/DolRecomp/src/cpu
INCLUDE_DIRS := $(TOPDIR)/include $(TOPDIR)/external/DolRecomp/src
ROMFS_DIR    := $(TOPDIR)/romfs

GENERATED_DIR       := $(TOPDIR)/generated
GENERATED_C         := $(GENERATED_DIR)/generated.c
GENERATED_H         := $(GENERATED_DIR)/generated.h
GENERATED_CHUNK_DIR := $(GENERATED_DIR)/generated_chunks
GENERATED_ACTUAL_CHUNKS := $(sort $(notdir $(wildcard $(GENERATED_CHUNK_DIR)/*.c)))
GENERATED_ANY := $(strip $(wildcard $(GENERATED_C)) \
                          $(wildcard $(GENERATED_H)) \
                          $(GENERATED_ACTUAL_CHUNKS))

# DolRecomp records every required split source in generated.c. Compare that
# inventory with the directory contents so stale chunks from another DOL cannot
# be linked accidentally.
ifneq ($(GENERATED_ANY),)
ifeq ($(wildcard $(GENERATED_C)),)
$(error "Generated state is incomplete: generated/generated.c is missing; rerun tools/configure.ps1")
endif
ifeq ($(wildcard $(GENERATED_H)),)
$(error "Generated state is incomplete: generated/generated.h is missing; rerun tools/configure.ps1")
endif
ifeq ($(GENERATED_ACTUAL_CHUNKS),)
$(error "Generated state is incomplete: generated/generated_chunks contains no C sources")
endif
GENERATED_LISTED_CHUNKS := $(sort $(shell sed -n 's/\r$$//;s|^// generated_chunks/\([^/]*\.c\)$$|\1|p' $(GENERATED_C)))
ifneq ($(GENERATED_LISTED_CHUNKS),$(GENERATED_ACTUAL_CHUNKS))
$(error "Generated state is incoherent: generated.c and generated_chunks disagree; rerun tools/configure.ps1")
endif
SOURCE_DIRS += $(GENERATED_DIR) $(GENERATED_CHUNK_DIR)
INCLUDE_DIRS += $(GENERATED_DIR)
GENERATED_DEFINE := -DSMG3DS_WITH_GENERATED=1
endif

ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=3dsx.specs -g $(ARCH) \
               -Wl,-Map,$(TARGET).map,--gc-sections
LIBS        := -lcitro3d -lctru -lm
LIBDIRS     := $(DEVKITPRO)/libctru

APP_TITLE       := SMG3DS Bring-up
APP_DESCRIPTION := DolRecomp runtime probe for 3DS/Azahar
APP_AUTHOR      := SMG3DS contributors

export OUTPUT   := $(TARGET)
export VPATH    := $(SOURCE_DIRS)
export DEPSDIR  := .

CFILES           := $(foreach dir,$(SOURCE_DIRS),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES         := $(foreach dir,$(SOURCE_DIRS),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES           := $(foreach dir,$(SOURCE_DIRS),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES        := $(foreach dir,$(SOURCE_DIRS),$(notdir $(wildcard $(dir)/*.v.pica)))
SOURCE_BASENAMES := $(CFILES) $(CPPFILES) $(SFILES) $(PICAFILES)
ifneq ($(words $(SOURCE_BASENAMES)),$(words $(sort $(SOURCE_BASENAMES))))
$(error "Two source files share a basename; the flat build object namespace would collide")
endif

ifeq ($(strip $(CPPFILES)),)
export LD       := $(CC)
else
export LD       := $(CXX)
endif

export OFILES   := $(PICAFILES:.v.pica=.shbin.o) \
                   $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export HFILES   := $(PICAFILES:.v.pica=_shbin.h)
export INCLUDE  := $(foreach dir,$(INCLUDE_DIRS),-I"$(dir)") \
                   $(foreach dir,$(LIBDIRS),-I"$(dir)/include") -I.
CFLAGS      := -g -Wall -Wextra -O2 -mword-relocations \
               -ffunction-sections -fdata-sections $(ARCH) \
               $(INCLUDE) -include smg3ds/petari_overrides.h \
               -D__3DS__ $(GENERATED_DEFINE)
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L"$(dir)/lib")
export _3DSXDEPS := $(OUTPUT).smdh
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh --romfs="$(ROMFS_DIR)"

.PHONY: all clean

all: $(OUTPUT).3dsx

clean:
	@echo clean ...
	@rm -f *.o *.d *.lst $(OUTPUT).3dsx $(OUTPUT).smdh \
	       $(OUTPUT).elf $(OUTPUT).map

$(OUTPUT).3dsx: $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf: $(OFILES)

pica200_renderer.o: pica200_vshader_shbin.h

.PRECIOUS: %.shbin

%.shbin.o %_shbin.h: %.shbin
	@echo $(notdir $<)
	@$(bin2o)

# Newlib defines uint32_t as unsigned long on ARM; upstream's diagnostics use
# unsigned-int format specifiers. Keep project warnings enabled and isolate this
# harmless upstream-only warning to its object.
cpu.o: CFLAGS += -Wno-format

-include *.d
