# KosmicKrisp (Mesa's Vulkan driver for Metal) built from source with the framework's patches (patches/mesa).
# Shared by the x86-64 target (recomp.mk) and the win32 target (win32/win32.mk, for its DXVK d3d9 backend).
# Needs FW, BUILD, MACOS_MIN. `make kosmickrisp` stages the driver, loader and ICD json in $(BUILD)/kosmickrisp.
# Needs: brew install llvm libclc spirv-llvm-translator; the Vulkan SDK in /usr/local (static SPIRV-Tools).
MESA_DIR ?= external/mesa
MESA_REF ?= main
KK_STAGE = $(BUILD)/kosmickrisp
# The driver targets the app's minimum macOS, like the runtime: without it, clang targets the build Mac's macOS,
# its @available(macOS 27, *) checks compile to "always", and on macOS 26 Metal throws on the first texture view
# (-[MTLTextureViewDescriptor setMinLOD:]: unrecognized selector). Meson keeps these from the first setup:
# remove $(MESA_DIR)/build after changing MACOS_MIN.
KK_MIN = -mmacosx-version-min=$(MACOS_MIN)
kosmickrisp:
	@test -d $(MESA_DIR) || git clone --depth 1 --branch $(MESA_REF) https://gitlab.freedesktop.org/mesa/mesa.git $(MESA_DIR)
	@test -x external/mesa-venv/bin/python || { /opt/homebrew/bin/python3 -m venv external/mesa-venv && external/mesa-venv/bin/pip install -q mako packaging pyyaml; }
	@test -d $(MESA_DIR)/build || MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN) PATH="/opt/homebrew/opt/llvm/bin:$(CURDIR)/external/mesa-venv/bin:$$PATH" meson setup $(MESA_DIR)/build $(MESA_DIR) \
	    --prefix=$(CURDIR)/external/kk-install --buildtype=release -Db_ndebug=true -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
	    -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static --pkg-config-path=/usr/local/lib/pkgconfig \
	    -Dc_args=$(KK_MIN) -Dcpp_args=$(KK_MIN) -Dobjc_args=$(KK_MIN) -Dc_link_args=$(KK_MIN) -Dcpp_link_args=$(KK_MIN) -Dobjc_link_args=$(KK_MIN)
	@for p in $(abspath $(FW))/patches/mesa/*.patch; do \
	    git -C $(MESA_DIR) apply --reverse --check "$$p" 2>/dev/null && continue; \
	    git -C $(MESA_DIR) apply "$$p" && echo "applied $$(basename $$p)" || exit 1; \
	done
	MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN) PATH="/opt/homebrew/opt/llvm/bin:$(CURDIR)/external/mesa-venv/bin:$$PATH" ninja -C $(MESA_DIR)/build install
	rm -rf $(KK_STAGE) && mkdir -p $(KK_STAGE)/drirc.d
	cp external/kk-install/lib/libvulkan_kosmickrisp.dylib $(KK_STAGE)/
	cp /usr/local/lib/libvulkan.1.dylib $(KK_STAGE)/
	printf '{\n    "file_format_version": "1.0.1",\n    "ICD": { "library_path": "./libvulkan_kosmickrisp.dylib", "api_version": "1.4.0" }\n}\n' > $(KK_STAGE)/kosmickrisp_icd.json
	cp $(MESA_DIR)/src/util/00-mesa-defaults.conf $(MESA_DIR)/src/kosmickrisp/vulkan/00-kk-defaults.conf $(KK_STAGE)/drirc.d/
	git -C $(MESA_DIR) log -1 --format='mesa %H (%cd)' > $(KK_STAGE)/VERSION
	cp $(MESA_DIR)/docs/license.rst $(KK_STAGE)/LICENSE-mesa.rst
	printf 'libvulkan_kosmickrisp.dylib: KosmicKrisp from Mesa (https://mesa3d.org), MIT license, see LICENSE-mesa.rst.\nlibvulkan.1.dylib: the Khronos Vulkan loader (https://github.com/KhronosGroup/Vulkan-Loader), Apache-2.0 license.\n' > $(KK_STAGE)/THIRD_PARTY.txt
	@cat $(KK_STAGE)/VERSION
