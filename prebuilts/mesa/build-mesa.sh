#!/bin/bash
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Rebuild the Mesa freedreno prebuilts in lib64/ with the Android NDK.
#
# usage: build-mesa.sh <ndk-dir> <mesa-src-dir> <libdrm-src-dir> <work-dir>
#   e.g. build-mesa.sh ~/android-ndk-r30 ~/mesa-26.2.5 external/libdrm /tmp/mesa
#
set -euo pipefail

NDK=$(realpath "$1") MESA=$(realpath "$2") LIBDRM=$(realpath "$3") W=$4
OUT=$(dirname "$(realpath "$0")")/lib64
T=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
API=35

mkdir -p "$W" && cd "$W"
cat > android-aarch64.cross <<EOF
[binaries]
c = '$T/aarch64-linux-android$API-clang'
cpp = '$T/aarch64-linux-android$API-clang++'
ar = '$T/llvm-ar'
strip = '$T/llvm-strip'
pkg-config = ['pkg-config']

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$W/sysroot/lib/pkgconfig'

[built-in options]
c_args = ['-Wno-error=deprecated-declarations']
cpp_args = ['-Wno-error=deprecated-declarations']
cpp_link_args = ['-static-libstdc++']
c_link_args = ['-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

# libdrm, static, so the Mesa libraries only need system libraries.
meson setup --wipe libdrm-build "$LIBDRM" --cross-file android-aarch64.cross \
	--prefix="$W/sysroot" --libdir=lib -Ddefault_library=static -Dbuildtype=release \
	-Dintel=disabled -Damdgpu=disabled -Dradeon=disabled -Dnouveau=disabled \
	-Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
	-Dexynos=disabled -Dtegra=disabled -Domap=disabled -Dtests=false \
	-Dman-pages=disabled -Dcairo-tests=disabled -Dvalgrind=disabled -Dudev=false
ninja -C libdrm-build install

# Mesa: Gallium freedreno GL/GLES on Android. android-stub links against
# stubs, and the cros_api gralloc backend talks to minigbm's gralloc0 module
# at runtime (ro.hardware.gralloc=minigbm).
meson setup --wipe mesa-build "$MESA" --cross-file android-aarch64.cross \
	--wrap-mode=nofallback --prefix=/vendor --libdir=lib64 \
	-Dbuildtype=release -Db_ndebug=true \
	-Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true \
	-Dandroid-libbacktrace=disabled -Dandroid-strict=false \
	-Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=msm \
	-Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true \
	-Dglx=disabled -Dgbm=disabled -Dllvm=disabled -Dshared-llvm=disabled \
	-Dxmlconfig=disabled -Dzstd=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dlmsensors=disabled -Dgallium-va=disabled \
	-Dgallium-extra-hud=false -Dtools= -Dbuild-tests=false -Dexpat=disabled \
	-Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa
ninja -C mesa-build
rm -rf stage && DESTDIR="$W/stage" ninja -C mesa-build install

for f in libEGL_mesa.so libGLESv1_CM_mesa.so libGLESv2_mesa.so libgallium_dri.so; do
	install -m 0644 "stage/vendor/lib64/$f" "$OUT/$f"
done
