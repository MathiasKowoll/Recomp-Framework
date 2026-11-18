# FFmpeg (third_party/ffmpeg, a git submodule), built as minimal static LGPL libraries for the Windows Media
# Format SDK replacement (runtime/wmvcore.c): the ASF demuxer, the WMV / VC-1 / WMA decoders, swscale and
# swresample. Included by win32.mk; `make ffmpeg` builds $(FFMPEG_OUT)/lib/*.a.
FFMPEG_SRC   = $(W32)/../third_party/ffmpeg
FFMPEG_BUILD = $(BUILD)/ffmpeg-build
FFMPEG_OUT   = $(abspath $(BUILD))/ffmpeg
FFMPEG_LIBS  = $(FFMPEG_OUT)/lib/libavformat.a $(FFMPEG_OUT)/lib/libavcodec.a $(FFMPEG_OUT)/lib/libswscale.a \
               $(FFMPEG_OUT)/lib/libswresample.a $(FFMPEG_OUT)/lib/libavutil.a
FFMPEG_INC   = -I$(FFMPEG_OUT)/include
FFMPEG_DECODERS = wmv1,wmv2,wmv3,wmv3image,vc1,vc1image,msmpeg4v3,wmav1,wmav2,wmapro,wmavoice,wmalossless,pcm_s16le

ffmpeg: $(FFMPEG_OUT)/lib/libavcodec.a
$(FFMPEG_OUT)/lib/libavcodec.a:
	@test -f $(FFMPEG_SRC)/configure || git -C $(W32)/.. submodule update --init third_party/ffmpeg
	@mkdir -p $(FFMPEG_BUILD)
	cd $(FFMPEG_BUILD) && $(abspath $(FFMPEG_SRC))/configure --prefix=$(FFMPEG_OUT) --arch=aarch64 --cc=clang \
	    --extra-cflags="-mmacosx-version-min=$(MACOS_MIN)" --extra-ldflags="-mmacosx-version-min=$(MACOS_MIN)" \
	    --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-network \
	    --disable-autodetect --disable-everything --disable-avdevice --disable-avfilter \
	    --enable-demuxer=asf --enable-decoder=$(FFMPEG_DECODERS) --enable-parser=vc1 --enable-swscale --enable-swresample
	$(MAKE) -C $(FFMPEG_BUILD) -j$$(sysctl -n hw.ncpu) install

.PHONY: ffmpeg
