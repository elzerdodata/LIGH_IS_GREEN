#---------------------------------------------------------------------------------
# ZERODROID Nintendo Switch build Makefile (devkitA64 / libnx / deko3d)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO not set - build inside the devkitpro/devkita64 environment")
endif

include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := ZERODROID
APP_AUTHOR  := Zero Data
APP_VERSION := 0.8.4.4
APP_ICON    := icon.jpg

TARGET   := ZERODROID_v0.8.4.4
BUILD    := build-switch
SOURCES  := src/core src/switch
INCLUDES := src vendor deps/shim/include
ROMFS    := romfs

DEFINES := -DZERODROID_VERSION=\"$(APP_VERSION)\" -D__SWITCH__

ROOTDIR := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
ifneq ($(strip $(wildcard $(ROOTDIR)deps/switch/lib/libpeer.a)),)
SOURCES       += src/switch/stream
EXTRA_LIBDIRS := $(ROOTDIR)deps/switch
STREAM_LIBS   := -lpeer -lsrtp2 -lusrsctp
STREAM_PKGS   := libavcodec libavutil opus
DEFINES       += -DZERODROID_NATIVE_STREAMING=1
export GNX_STREAM_DEPS := $(wildcard $(ROOTDIR)deps/switch/lib/lib*.a)
endif

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g0 -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE) \
            -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -std=gnu++17
ASFLAGS  := -g0 $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g0 $(ARCH) \
            -Wl,-Map,$(notdir $*.map)

PKGCONF := /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-pkg-config
LIBS := $(STREAM_LIBS) \
        $(shell $(PKGCONF) --libs $(STREAM_PKGS) SDL2_ttf SDL2_image sdl2 libcurl) \
        -lmbedtls -lmbedx509 -lmbedcrypto \
        -lEGL -lglapi -ldrm_nouveau -ldeko3d -lnx
LIBDIRS := $(EXTRA_LIBDIRS) $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq ($(strip $(ROMFS)),)
export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

export NROFLAGS += --icon=$(CURDIR)/$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

UAM        := $(DEVKITPRO)/tools/bin/uam
SHADER_OUT := $(CURDIR)/romfs/shaders/video_vsh.dksh \
              $(CURDIR)/romfs/shaders/video_fsh.dksh \
              $(CURDIR)/romfs/shaders/hud_fsh.dksh

.PHONY: all clean shaders

all: shaders $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

shaders: $(SHADER_OUT)

$(CURDIR)/romfs/shaders/%_vsh.dksh: $(CURDIR)/shaders/%_vsh.glsl
	@mkdir -p $(CURDIR)/romfs/shaders
	@echo "uam $(notdir $<)"
	@$(UAM) -s vert -o $@ $<

$(CURDIR)/romfs/shaders/%_fsh.dksh: $(CURDIR)/shaders/%_fsh.glsl
	@mkdir -p $(CURDIR)/romfs/shaders
	@echo "uam $(notdir $<)"
	@$(UAM) -s frag -o $@ $<

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $@ -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).map romfs/shaders

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).nacp:
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
