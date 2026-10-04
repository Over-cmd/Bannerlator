#!/usr/bin/env bash
# Cross-compile the PulseAudio 17.0 stack for Android arm64, updated from BrunoSX's layout.
# Compila dependencias base usando Autotools y la pila principal usando Meson + Ninja.
# Inputs (env): NDK_PATH. Output: $OUT/ (client libs + daemon + core modules) for arm64.
set -euo pipefail

ARCH=arm64
BUILDCHAIN=aarch64-linux-android
API=26
PA_VER=17.0
LIBTOOL_VER=2.4.6
LIBSNDFILE_VER=1.0.31

BASE_DIR="$PWD"
SRC_DIR="$BASE_DIR/.src"
ROOT_DIR="$BASE_DIR/root-$ARCH"
OUT="$BASE_DIR/output/$ARCH"
: "${NDK_PATH:?set NDK_PATH to the Android NDK root}"

export PATH="$ROOT_DIR/bin:$PATH"
export PKG_CONFIG_PATH="$ROOT_DIR/lib/pkgconfig"

# Flags heredados para corregir problemas de sintaxis C en dependencias antiguas con Clang moderno
LEGACY_C="-Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=incompatible-function-pointer-types -Wno-error=incompatible-pointer-types -Wno-error=deprecated-non-prototype"
export CFLAGS="-O2 -I$ROOT_DIR/include $LEGACY_C"
export CPPFLAGS="-I$ROOT_DIR/include"
export LDFLAGS="-L$ROOT_DIR/lib"

# Bionic quirk overrides para las configuraciones por Autotools (libltdl y libsndfile)
export ALLOW_UNRESOLVED_SYMBOLS=1
export ac_cv_func_mkfifo=yes
export ac_cv_func_getuid=no
export ax_cv_PTHREAD_PRIO_INHERIT=no
export ac_cv_header_glob_h=no
export ac_cv_func_malloc_0_nonnull=yes
export ac_cv_func_realloc_0_nonnull=yes
export ac_cv_lib_ltdl_lt_dladvise_init=yes
export ac_cv_header_execinfo_h=no

TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin"
export CC="$TOOLCHAIN/${BUILDCHAIN}${API}-clang"
export CXX="$TOOLCHAIN/${BUILDCHAIN}${API}-clang++"
export AR="$TOOLCHAIN/llvm-ar" RANLIB="$TOOLCHAIN/llvm-ranlib" STRIP="$TOOLCHAIN/llvm-strip"
test -x "$CC" || { echo "CC not found: $CC"; ls "$TOOLCHAIN" | grep -i clang | head; exit 1; }

mkdir -p "$SRC_DIR" "$ROOT_DIR"

# Función de descarga compartida
fetch() {
  local dest="$1"; shift
  local url
  for url in "$@"; do
    echo "fetch $url"
    if curl -fsSL --retry 3 --retry-delay 2 -A "Mozilla/5.0 (X11; Linux x86_64)" -o "$dest" "$url"; then
      return 0
    fi
    echo "  ...mirror failed, trying next"
  done
  echo "ERROR: all mirrors failed for $dest"; return 1
}

# --- Compilación de libltdl (Requerido para la carga dinámica de módulos) ---
if [ ! -e "$ROOT_DIR/include/ltdl.h" ]; then
  cd "$SRC_DIR"
  
  URL_L1="https://mirrors.kernel.org"
  URL_L1="$URL_L1/gnu/libtool"
  URL_L1="$URL_L1/libtool-$LIBTOOL_VER.tar.gz"
  
  URL_L2="https://ftp.gnu.org"
  URL_L2="$URL_L2/gnu/libtool"
  URL_L2="$URL_L2/libtool-$LIBTOOL_VER.tar.gz"

  [ -f "libtool-$LIBTOOL_VER.tar.gz" ] || \
    fetch "libtool-$LIBTOOL_VER.tar.gz" \
    "$URL_L1" \
    "$URL_L2"
    
  rm -rf "libtool-$LIBTOOL_VER"
  tar xf "libtool-$LIBTOOL_VER.tar.gz"
  cd "libtool-$LIBTOOL_VER"
  rm -rf "build-$ARCH"
  mkdir -p "build-$ARCH"
  cd "build-$ARCH"
  
  ../configure \
    --host=$BUILDCHAIN \
    --prefix="$ROOT_DIR" \
    --enable-ltdl-install \
    --enable-shared \
    HELP2MAN=/bin/true \
    MAKEINFO=/bin/true
    
  make -j"$(nproc)"
  make install
  
  test -e "$ROOT_DIR/include/ltdl.h" || \
    { echo "ltdl.h still missing"; exit 1; }
fi

# --- Compilación de libsndfile (Soporte de archivos de audio básico) ---
if [ ! -e "$ROOT_DIR/lib/libsndfile.so" ]; then
  cd "$SRC_DIR"
  
  URL_S1="https://github.com"
  URL_S1="$URL_S1/libsndfile/libsndfile"
  URL_S1="$URL_S1/releases/download/$LIBSNDFILE_VER"
  URL_S1="$URL_S1/libsndfile-$LIBSNDFILE_VER.tar.bz2"

  [ -f "libsndfile-$LIBSNDFILE_VER.tar.bz2" ] || \
    fetch "libsndfile-$LIBSNDFILE_VER.tar.bz2" \
    "$URL_S1"
    
  rm -rf "libsndfile-$LIBSNDFILE_VER"
  tar xf "libsndfile-$LIBSNDFILE_VER.tar.bz2"
  cd "libsndfile-$LIBSNDFILE_VER"
  
  ./configure \
    --host=$BUILDCHAIN \
    --prefix="$ROOT_DIR" \
    --disable-external-libs \
    --disable-alsa \
    --disable-sqlite \
    --disable-static \
    --enable-shared
    
  make -j"$(nproc)"
  make install
fi

# --- PulseAudio 17.0 (Compilación mediante Meson + Ninja usando el código clonado del YAML) ---
cd "$BASE_DIR"
SRC_17_DIR="${BASE_DIR}/src-17.0"
BUILD_MESON_DIR="${BASE_DIR}/build-meson"

echo "=== Iniciando la compilación de PulseAudio 17.0 con Meson ==="

# 1. Limpieza y creación de directorios
rm -rf "${BUILD_MESON_DIR}" "${OUT}"
mkdir -p "${OUT}/modules"
mkdir -p "${ROOT_DIR}/include"

# CREACIÓN DEL ARCHIVO VIRTUAL (ENLACE SIMBÓLICO) para Android
echo "-> Creando librería virtual libintl.so..."
SYSROOT_LIB="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/${API}"
ln -sf "${SYSROOT_LIB}/libc.so" "${ROOT_DIR}/lib/libintl.so"

# Generar archivo de cabecera libintl.h vacío para Clang
echo "-> Creando archivo de cabecera virtual libintl.h..."
cat << EOF > "${ROOT_DIR}/include/libintl.h"
#ifndef LIBINTL_H
#define LIBINTL_H
#define gettext(String) (String)
#define dgettext(Domain,String) (String)
#define dcgettext(Domain,String,Type) (String)
#define textdomain(Domain) ((char*) (Domain))
#define bindtextdomain(Domain,Directory) ((char*) (Domain))
#define bind_textdomain_codeset(Domain,Codeset) ((char*) (Domain))
#endif
EOF

# Generar archivo de configuración cruzada
# CORREGIDO: Añadido [built-in options] para inyectar flags globales en Meson
cat << EOF > "${BASE_DIR}/android_arm64.txt"
[binaries]
c = '${CC}'
cpp = '${CXX}'
ar = '${AR}'
strip = '${STRIP}'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['-I${ROOT_DIR}/include', '-DENABLE_NLS=0', '-DHAVE_GETTEXT=0', '-DHAVE_BACKTRACE=0', '-DHAVE_EXECINFO_H=0', '-Dpthread_mutexattr_setprotocol(a,b)=0', '-DPTHREAD_PRIO_INHERIT=0', '-DPTHREAD_PRIO_NONE=0']
c_link_args = ['-L${ROOT_DIR}/lib', '-lintl']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'arm64-v8a'
endian = 'little'
EOF

# 2. Configuración con Meson y desactivación de dependencias innecesarias
cd "${SRC_17_DIR}"

# PARCHE CRÍTICO ANTI-BACKTRACE: Desactiva el volcado de error incompatible con Android API 26
echo "-> Aplicando parche de compatibilidad para backtrace en log.c..."
sed -i 's/#ifdef HAVE_EXECINFO_H/#if 0/g' src/pulsecore/log.c

# NUEVO PARCHE CRÍTICO ANTI-PRIO-INHERIT: Desactiva la herencia de prioridad de hilos no soportada por Android Bionic
echo "-> Aplicando parche de hilos para mutex-posix.c..."
sed -i 's/#ifdef PTHREAD_PRIO_INHERIT/#if 0/g' src/pulsecore/mutex-posix.c

meson setup "${BUILD_MESON_DIR}" \
  --cross-file="${BASE_DIR}/android_arm64.txt" \
  --prefix="${ROOT_DIR}" \
  --buildtype=release \
  -Dc_args="-Dpthread_mutexattr_setprotocol\(a,b\)=0 -DPTHREAD_PRIO_INHERIT=0 -DPTHREAD_PRIO_NONE=0" \
  -Dc_link_args="-L${ROOT_DIR}/lib -lintl" \
  -Ddatabase=simple \
  -Dbluez5=disabled \
  -Ddoxygen=false \
  -Dtests=false \
  -Ddaemon=true \
  -Dclient=true \
  -Dalsa=disabled \
  -Dglib=disabled \
  -Dgtk=disabled \
  -Davahi=disabled \
  -Djack=disabled \
  -Dasyncns=disabled \
  -Ddbus=disabled \
  -Dudev=disabled \
  -Dopenssl=disabled \
  -Dwebrtc-aec=disabled \
  -Dspeex=disabled \
  -Dorc=disabled

# 3. Compilar e instalar en el directorio raíz temporal
echo "-> Compilando el código con Ninja..."
ninja -C "${BUILD_MESON_DIR}"

echo "-> Instalando binarios en el prefijo temporal..."
ninja -C "${BUILD_MESON_DIR}" install

# 4. --- Recolectar el conjunto binario (Mapeo exacto del layout de Bannerlator) ---
echo "-> Organizando y empaquetando archivos de salida (.so) para el APK..."

# Renombrar el ejecutable principal del demonio para que Bannerlator lo cargue como librería dinámica nativa
cp -a "$ROOT_DIR/bin/pulseaudio"                               "$OUT/libpulseaudio.so"

# Copiar las librerías base centrales generadas por la versión 17.0
cp -a "$ROOT_DIR"/lib/pulseaudio/libpulsecommon-*.so           "$OUT/"
cp -a "$ROOT_DIR"/lib/pulseaudio/libpulsecore-*.so             "$OUT/"
cp -a "$ROOT_DIR/lib/libpulse.so"                              "$OUT/libpulse.so"

# Copiar las dependencias base de la Parte 1
cp -a "$ROOT_DIR/lib/libsndfile.so"                            "$OUT/libsndfile.so"
cp -a "$ROOT_DIR/lib/libltdl.so"                               "$OUT/libltdl.so"

# Copiar los protocolos de comunicación interna a la subcarpeta de módulos
cp -a "$ROOT_DIR"/lib/pulseaudio/modules/libprotocol-native.so           "$OUT/modules/"
cp -a "$ROOT_DIR"/lib/pulseaudio/modules/module-native-protocol-unix.so  "$OUT/modules/"

# Asegurar la correcta recolección de los módulos de tuberías (Esencial para la compatibilidad del micrófono)
for m in module-pipe-source module-pipe-sink; do
  src=$(echo "$ROOT_DIR"/lib/pulseaudio/modules/$m.so)
  if [ -f "$src" ]; then
    cp -a "$src" "$OUT/modules/"
  else
    echo "ERROR: No se encontró el módulo crítico $m.so en la ruta $src"
    exit 1
  fi
done

echo "=== ¡Pila de PulseAudio 17.0 construida con éxito! ==="
echo "Contenido en $OUT:"
ls -la "$OUT" "$OUT/modules"

# Exponer la ruta del código fuente de PulseAudio 17.0 para el script secundario build-module.sh
echo "${SRC_17_DIR}" > "$BASE_DIR/.pa_src_path"
