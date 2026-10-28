#!/usr/bin/env bash
# Fabrique le stub d'édition de liens dont le SONAME est le CHEMIN ABSOLU du
# pilote OpenCL du téléphone.
#
# Pourquoi (mesuré le 2026-10-25) : un exécutable lancé par l'application
# n'hérite pas de l'espace de noms que le linker Android monte pour un
# processus d'application. Sa dépendance « libOpenCL.so » n'est donc trouvée
# nulle part :
#
#   CANNOT LINK EXECUTABLE: library "libOpenCL.so" not found
#
# Ajouter /vendor/lib64 à LD_LIBRARY_PATH « répare » ça et casse tout le
# reste : le linker y cherche AUSSI libc++.so, et la copie du constructeur
# tire des bibliothèques privées que l'application n'a pas le droit de mapper :
#
#   "/vendor/lib64/libbinderdebug.so" phdr mmap failed: Permission denied
#
# Or /vendor/lib64/libOpenCL.so porte l'étiquette `same_process_hal_file`,
# que l'application PEUT mapper, et ne dépend que de libc++, libc, libm et
# libdl. Il suffit donc que la dépendance soit inscrite en chemin absolu : le
# linker charge ce fichier-là sans rien chercher, et le reste vient du système.
#
# Les symboles sont ceux que ggml-opencl appelle vraiment, relevés dans la
# table dynamique de la variante GPU.
set -euo pipefail

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/29.0.13846066}"
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
SORTIE="$(dirname "$0")/../bin/arm64-v8a-absolu"
PILOTE="/vendor/lib64/libOpenCL.so"

SYMBOLES=(
    clBuildProgram clCreateBuffer clCreateBufferWithProperties clCreateCommandQueue
    clCreateContext clCreateImage clCreateKernel clCreateProgramWithBinary
    clCreateProgramWithSource clCreateSubBuffer clEnqueueBarrierWithWaitList
    clEnqueueCopyBuffer clEnqueueFillBuffer clEnqueueMapBuffer
    clEnqueueMarkerWithWaitList clEnqueueNDRangeKernel clEnqueueReadBuffer
    clEnqueueUnmapMemObject clEnqueueWriteBuffer clFinish clFlush clGetDeviceIDs
    clGetDeviceInfo clGetKernelSubGroupInfo clGetKernelWorkGroupInfo
    clGetPlatformIDs clGetPlatformInfo clGetProgramBuildInfo clGetProgramInfo
    clReleaseEvent clReleaseKernel clReleaseMemObject clReleaseProgram
    clSetKernelArg clWaitForEvents
)

mkdir -p "$SORTIE"
source_c="$(mktemp /tmp/stub_opencl_XXXX.c)"
trap 'rm -f "$source_c"' EXIT
{
    echo "/* Engendré par scripts/stub-opencl-absolu.sh — ne pas modifier. */"
    for s in "${SYMBOLES[@]}"; do
        echo "void $s(void) {}"
    done
} > "$source_c"

"$CLANG" --target=aarch64-linux-android26 -shared -fPIC -O0 \
    -Wl,-soname,"$PILOTE" \
    -o "$SORTIE/libOpenCL.so" "$source_c"

echo "stub écrit : $SORTIE/libOpenCL.so (soname = $PILOTE, ${#SYMBOLES[@]} symboles)"
