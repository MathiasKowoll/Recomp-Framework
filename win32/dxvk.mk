# DXVK (third_party/dxvk, a git submodule) built natively for macOS as the d3d9 backend of the win32 target:
# Direct3D 9 -> Vulkan -> Metal (KosmicKrisp). For testing and prototyping recompiled D3D9 games; the bridge is
# runtime/d3d9_bridge.cpp. Included by win32.mk; `make dxvk` builds $(BUILD)/dxvk/libdxvk_d3d9.dylib.
DXVK_SRC   = $(W32)/../third_party/dxvk
DXVK_BUILD = $(BUILD)/dxvk-build
DXVK_OUT   = $(BUILD)/dxvk
DXVK_LIB   = $(DXVK_OUT)/libdxvk_d3d9.dylib
DXVK_INC   = -I$(DXVK_SRC)/include -I$(DXVK_SRC)/include/native/windows -I$(DXVK_SRC)/include/native/directx

dxvk: $(DXVK_LIB)
$(DXVK_LIB): $(wildcard $(W32)/../patches/dxvk/*.patch)
	@test -f $(DXVK_SRC)/meson.build || git -C $(W32)/.. submodule update --init --recursive third_party/dxvk
	@for p in $(abspath $(W32)/../patches/dxvk)/*.patch; do \
	    git -C $(DXVK_SRC) apply --reverse --check "$$p" 2>/dev/null && continue; \
	    git -C $(DXVK_SRC) apply "$$p" && echo "applied $$(basename $$p)" || exit 1; \
	done
	@test -d $(DXVK_BUILD) || meson setup $(DXVK_BUILD) $(DXVK_SRC) --buildtype=release -Denable_d3d9=true -Denable_d3d8=false \
	    -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false -Dnative_sdl3=disabled -Dnative_sdl2=disabled -Dnative_glfw=disabled
	ninja -C $(DXVK_BUILD) src/d3d9/libdxvk_d3d9.0.dylib
	@mkdir -p $(DXVK_OUT)
	cp $(DXVK_BUILD)/src/d3d9/libdxvk_d3d9.0.dylib $(DXVK_LIB)
	install_name_tool -id @rpath/libdxvk_d3d9.dylib $(DXVK_LIB)

.PHONY: dxvk
