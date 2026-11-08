# Builds the runtime and the generated C (gen/, from `python3 -m recomp f1 GPM2.EXE gen/`).
#   make app       build/Grand Prix Manager 2.app, macOS 11 or later, universal
#   make ppc-app   Mac OS X 10.4/10.5 PowerPC (cross compiler, see the ppc-app section below)
CC       = clang
AR      ?= ar
GEN     ?= ../gen
BUILD   ?= build
CFLAGS  ?= -O1 -g0 -Wall -Wextra -fno-strict-aliasing
INC      = -I. -I$(GEN)

RT_SRC   = machine.c image.c dispatch.c hooks/hooks.c
GEN_SRC  = $(sort $(wildcard $(GEN)/*.c))
LIB_OBJ  = $(addprefix $(BUILD)/lib/rt_,$(RT_SRC:.c=.o)) \
           $(addprefix $(BUILD)/lib/gen_,$(notdir $(GEN_SRC:.c=.o)))

.PHONY: lib app ppc-app clean
lib: $(BUILD)/lib/libgpm2.a

$(BUILD)/lib/libgpm2.a: $(LIB_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

# portable Win32 core and boot; each front end brings its own main
CORE_SRC = $(wildcard win32/*.c) gpm2run.c host/net.c
W32_SRC  = $(CORE_SRC)
CORE_LIB = $(addprefix $(BUILD)/lib/w32_,$(subst /,_,$(CORE_SRC:.c=.o)))
W32_HDR  = $(wildcard win32/*.h host/*.h host/macos/*.h) gpm2run.h machine.h

define W32_RULE
$$(BUILD)/lib/w32_$(subst /,_,$(1:.c=.o)): $(1) $$(W32_HDR)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) $$(INC) -c -o $$@ $$<
endef
$(foreach s,$(W32_SRC),$(eval $(call W32_RULE,$(s))))

$(BUILD)/lib/rt_%.o: %.c machine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
$(BUILD)/lib/gen_%.o: $(GEN)/%.c machine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
clean:
	rm -rf $(BUILD)

# ---- GPM2.app (no Xcode; the bundle is assembled here) ----------------------------------
APP      = $(BUILD)/GPM2.app
# the game's own icon (not committed; without the file, e.g. in CI, the app has no icon)
GAME_DIR ?= $(HOME)/Games/GPM2
ICON_SRC ?= $(firstword $(wildcard $(GAME_DIR)/GPM.ICO))
MAC_SRC  = $(wildcard host/macos/*.m)
MAC_OBJ  = $(addprefix $(BUILD)/lib/mac_,$(notdir $(MAC_SRC:.m=.o)))
ARCHFLAGS ?=
OBJCFLAGS = -fobjc-arc -O1 -g0 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
MINVER   = -mmacosx-version-min=11.0
app: $(APP)
$(BUILD)/lib/mac_%.o: host/macos/%.m $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(OBJCFLAGS) $(ARCHFLAGS) $(INC) -c -o $@ $<
# one binary per architecture (sub-make with its own BUILD) + lipo => universal arm64+x86_64
$(BUILD)/GPM2.%.bin: FORCE
	$(MAKE) BUILD=$(BUILD)/$* CFLAGS="$(CFLAGS) -arch $* $(MINVER)" ARCHFLAGS="-arch $* $(MINVER)" $(BUILD)/$*/GPM2.bin
FORCE:
$(BUILD)/GPM2.bin: $(MAC_OBJ) $(CORE_LIB) $(BUILD)/lib/libgpm2.a
	$(CC) $(OBJCFLAGS) $(ARCHFLAGS) -o $@ $(MAC_OBJ) $(CORE_LIB) $(BUILD)/lib/libgpm2.a \
	    -framework AppKit -framework QuartzCore -framework CoreGraphics -framework AudioToolbox -framework CoreAudio -lm -lpthread
$(APP): $(BUILD)/GPM2.arm64.bin $(BUILD)/GPM2.x86_64.bin host/macos/Info.plist.in
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Resources
	lipo -create -output $@/Contents/MacOS/GPM2 $(BUILD)/arm64/GPM2.bin $(BUILD)/x86_64/GPM2.bin
	cp host/macos/Info.plist.in $@/Contents/Info.plist
	@if [ -n "$(ICON_SRC)" ] && [ -f "$(ICON_SRC)" ] && command -v iconutil >/dev/null 2>&1; then \
	  echo "icon: $(ICON_SRC)"; rm -rf $(BUILD)/GPM2.iconset; \
	  python3 -I ../tools/make_icon.py "$(ICON_SRC)" $(BUILD)/GPM2.iconset $(BUILD)/icon.png && \
	  iconutil -c icns $(BUILD)/GPM2.iconset -o $@/Contents/Resources/GPM2.icns || echo "icon: not generated"; \
	else echo "icon: no GPM.ICO (app without icon)"; fi
	codesign --force --deep --sign - $@
	rm -rf "$(BUILD)/Grand Prix Manager 2.app" && mv $@ "$(BUILD)/Grand Prix Manager 2.app"      # final name with a space; make does not handle spaces in targets well

# ---- GPM2.app for Mac OS X 10.4/10.5 PowerPC, Carbon backend -------------------------------
# make -C runtime ppc-app CC=powerpc-apple-darwin8-gcc CFLAGS="-arch ppc -isysroot <SDK> -mmacosx-version-min=10.4 -O1 -fno-strict-aliasing"
PPC_APP   = $(BUILD)/GPM2-ppc.app
PPC_ICON_SRC ?= ../bin/Gpm2.exe
CARBON_FW = -framework Carbon -framework CoreServices -framework ApplicationServices -framework AudioUnit -framework CoreAudio -lm -lpthread
ppc-app: $(PPC_APP)
$(BUILD)/lib/carbon_host.o: host/carbon/host_carbon.c $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
$(BUILD)/GPM2-carbon.bin: $(BUILD)/lib/carbon_host.o $(CORE_LIB) $(BUILD)/lib/libgpm2.a
	$(CC) $(CFLAGS) -o $@ $^ $(CARBON_FW)
$(PPC_APP): $(BUILD)/GPM2-carbon.bin host/carbon/Info.plist.in
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Resources
	cp $(BUILD)/GPM2-carbon.bin "$@/Contents/MacOS/Grand Prix Manager 2"      # executable name = application menu title
	chmod +x "$@/Contents/MacOS/Grand Prix Manager 2"
	cp host/carbon/Info.plist.in $@/Contents/Info.plist
	printf 'APPL????' > $@/Contents/PkgInfo
	@if command -v python3 >/dev/null 2>&1; then python3 -I ../tools/make_icns.py --plain $(PPC_ICON_SRC) $@/Contents/Resources/GPM2.icns || echo 'icon: not generated'; else echo 'icon: no python3 (generated later in CI)'; fi   # icon from the exe itself (GPM.ICO is identical)
