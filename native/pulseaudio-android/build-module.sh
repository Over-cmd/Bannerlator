#!/usr/bin/env bash
# Compila nuestro módulo optimizado module-aaudio-sink.so contra el árbol de PA 17.0 creado por build-stack.sh.
# Adaptado para el entorno moderno de Meson + Android NDK.
set -euo pipefail

ARCH=arm64
BUILDCHAIN=aarch64-linux-android
API=26
BASE_DIR="$PWD"
ROOT_DIR="$BASE_DIR/root-$ARCH"
OUT="$BASE_DIR/output/$ARCH"
: "${NDK_PATH:?set NDK_PATH}"

# Leer la ruta del código fuente expuesta por build-stack.sh
PA_SRC="$(cat "$BASE_DIR/.pa_src_path")"
test -d "$PA_SRC" || { echo "PA source not found ($PA_SRC) — run build-stack.sh first"; exit 1; }

BUILD_MESON_DIR="${BASE_DIR}/build-meson"

TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TOOLCHAIN/${BUILDCHAIN}${API}-clang"

# Flags de compatibilidad heredados por paridad
LEGACY_C="-Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types"

mkdir -p "$OUT/modules"

echo "=== Compilando module-aaudio-sink.so para PulseAudio 17.0 ==="

# Invocación directa del compilador Clang del NDK
$CC -O2 -shared $LEGACY_C \
  -DPACKAGE_VERSION=\"17.0\" \
  -DHAVE_CONFIG_H \
  -I"${BUILD_MESON_DIR}" \
  -I"${PA_SRC}/src" \
  -I"${ROOT_DIR}/include" \
  -L"${ROOT_DIR}/lib/pulseaudio" \
  -L"${ROOT_DIR}/lib" \
  -lpulsecore-17.0 \
  -lpulsecommon-17.0 \
  -lpulse \
  -laaudio \
  -o "$OUT/modules/module-aaudio-sink.so" \
  "$BASE_DIR/pulseaudio-module/module-aaudio-sink.c"

# Optimizar el tamaño de la librería eliminando símbolos no requeridos para el APK
"$TOOLCHAIN/llvm-strip" --strip-unneeded "$OUT/modules/module-aaudio-sink.so" || true

echo "=== Módulo construido con éxito -> $OUT/modules/module-aaudio-sink.so ==="
file "$OUT/modules/module-aaudio-sink.so"
