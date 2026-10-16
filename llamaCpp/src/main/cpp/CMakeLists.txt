cmake_minimum_required(VERSION 3.10)

project("llamacppkt")

set(CMAKE_CXX_STANDARD 17)
set(RNLLAMA_LIB_DIR ${CMAKE_SOURCE_DIR}/lib)

# Bridage mémoire (2026-09-25). Cette compilation était tuée par
# `systemd-oomd` : 10,4 Go de pic sur une machine de 16 Go, toutes les
# variantes de llama.cpp étant liées EN MÊME TEMPS avec `-flto`. L'édition de
# liens LTO charge le bitcode du programme entier en mémoire — sept en
# parallèle ne tiennent pas. Ce sont les LIENS qui explosent, pas les
# compilations : on sérialise donc les premiers (pool à 1) en laissant les
# secondes parallèles, ce qui coûte peu de temps et rend la compilation
# reproductible au lieu d'être tuée à mi-parcours.
set_property(GLOBAL PROPERTY JOB_POOLS link_pool=1 compile_pool=4)
set(CMAKE_JOB_POOL_LINK link_pool)
set(CMAKE_JOB_POOL_COMPILE compile_pool)

include_directories(
    ${RNLLAMA_LIB_DIR}
    ${RNLLAMA_LIB_DIR}/common
    ${RNLLAMA_LIB_DIR}/common/jinja
    ${RNLLAMA_LIB_DIR}/ggml-cpu
    ${RNLLAMA_LIB_DIR}/tools/mtmd
    # `codec` : synthèse vocale, apparue en amont après l'écriture de
    # `sync_llamacpp.sh` — qui ne connaît donc pas ce répertoire. `rn-tts.cpp`
    # inclut `codec.h`, d'où l'échec de compilation sans ces chemins.
    ${RNLLAMA_LIB_DIR}/codec/include
    ${RNLLAMA_LIB_DIR}/codec/common
)

# Sources de `codec`, reprises du CMake de production de cui-llama.rn.
file(GLOB CODEC_FILES CONFIGURE_DEPENDS
    ${RNLLAMA_LIB_DIR}/codec/src/*.cpp
    ${RNLLAMA_LIB_DIR}/codec/src/batch/*.cpp
    ${RNLLAMA_LIB_DIR}/codec/src/models/*.cpp
    ${RNLLAMA_LIB_DIR}/codec/src/ops/*.cpp
    ${RNLLAMA_LIB_DIR}/codec/src/runtime/*.cpp
    ${RNLLAMA_LIB_DIR}/codec/src/lm/*.cpp
    ${RNLLAMA_LIB_DIR}/codec/common/*.cpp
)
# `tts_runner{,_flow}.cpp` sont la boucle de référence de `codec.cpp` : ils
# tirent des aides `codec_example_*` qui ne sont pas livrées ici. `rn-tts`
# conduit sa propre boucle, on les écarte donc — comme le fait l'amont.
list(REMOVE_ITEM CODEC_FILES
    ${RNLLAMA_LIB_DIR}/codec/common/tts_runner.cpp
    ${RNLLAMA_LIB_DIR}/codec/common/tts_runner_flow.cpp
)

# Collect source files using glob patterns
file(GLOB MODEL_FILES ${RNLLAMA_LIB_DIR}/models/*.cpp)
file(GLOB GGML_CPU_C_FILES ${RNLLAMA_LIB_DIR}/ggml-cpu/*.c)
file(GLOB GGML_CPU_CPP_FILES ${RNLLAMA_LIB_DIR}/ggml-cpu/*.cpp)
file(GLOB GGML_CPU_AMX_FILES ${RNLLAMA_LIB_DIR}/ggml-cpu/amx/*.cpp)
file(GLOB LLAMA_FILES ${RNLLAMA_LIB_DIR}/llama*.cpp)
file(GLOB MTMD_FILES ${RNLLAMA_LIB_DIR}/tools/mtmd/*.cpp)
file(GLOB MTMD_MODEL_FILES ${RNLLAMA_LIB_DIR}/tools/mtmd/models/*.cpp)
file(GLOB COMMON_FILES ${RNLLAMA_LIB_DIR}/common/*.cpp)
file(GLOB JINJA_FILES ${RNLLAMA_LIB_DIR}/common/jinja/*.cpp)

# Source files for the shared library
set(
    SOURCE_FILES
    # Core GGML files
    ${RNLLAMA_LIB_DIR}/ggml.c
    ${RNLLAMA_LIB_DIR}/ggml-alloc.c
    ${RNLLAMA_LIB_DIR}/ggml-backend.cpp
    ${RNLLAMA_LIB_DIR}/ggml-backend-dl.cpp
    # Nouveau fichier amont : les sources ggml sont listées à la main
    # ici (pas globbées), donc tout ajout en amont doit être repris.
    ${RNLLAMA_LIB_DIR}/ggml-backend-meta.cpp
    ${RNLLAMA_LIB_DIR}/ggml-backend-reg.cpp
    ${RNLLAMA_LIB_DIR}/ggml-opt.cpp
    ${RNLLAMA_LIB_DIR}/ggml-threading.cpp
    ${RNLLAMA_LIB_DIR}/ggml-quants.c
    ${RNLLAMA_LIB_DIR}/gguf.cpp

    # GGML CPU files (globbed)
    ${GGML_CPU_C_FILES}
    ${GGML_CPU_CPP_FILES}
    ${GGML_CPU_AMX_FILES}

    # Llama files (globbed)
    ${LLAMA_FILES}
    ${RNLLAMA_LIB_DIR}/unicode-data.cpp
    ${RNLLAMA_LIB_DIR}/unicode.cpp

    # Common utilities
    ${COMMON_FILES}

    ${RNLLAMA_LIB_DIR}/anyascii.c

    # Jinja template engine
    ${JINJA_FILES}

    # Multimodal support (globbed)
    ${MTMD_MODEL_FILES}
    ${MTMD_FILES}

    # React Native llama APIs (used by our JNI)
    ${RNLLAMA_LIB_DIR}/rn-llama.cpp
    ${RNLLAMA_LIB_DIR}/rn-completion.cpp
    ${RNLLAMA_LIB_DIR}/rn-tts.cpp
    ${CODEC_FILES}
    ${RNLLAMA_LIB_DIR}/rn-slot.cpp
    ${RNLLAMA_LIB_DIR}/rn-slot-manager.cpp

    # Model implementations (globbed)
    ${MODEL_FILES}
    
    # Our JNI wrapper
    ${CMAKE_SOURCE_DIR}/jni.cpp
)

find_library(LOG_LIB log)

function(build_library target_name arch cpu_flags)
    # Backend OpenCL (GPU Adreno) : activé pour les cibles dont le nom se
    # termine par `_opencl`, comme dans le CMake de production de
    # cui-llama.rn dont ce bloc est transposé. Une cible SÉPARÉE plutôt qu'un
    # drapeau global : la variante CPU doit rester chargeable telle quelle sur
    # un appareil sans OpenCL utilisable.
    set(ENABLE_OPENCL OFF)
    if (${target_name} MATCHES ".*_opencl$")
        set(ENABLE_OPENCL ON)
    endif ()

    if (NOT ${arch} STREQUAL "generic")
        set(SOURCE_FILES_ARCH
            ${RNLLAMA_LIB_DIR}/ggml-cpu/arch/${arch}/quants.c
            ${RNLLAMA_LIB_DIR}/ggml-cpu/arch/${arch}/repack.cpp
        )
    endif ()

    add_library(
            ${target_name}
            SHARED
            ${SOURCE_FILES}
            ${SOURCE_FILES_ARCH}
    )

    target_link_libraries(${target_name} ${LOG_LIB} android)

    if (ENABLE_OPENCL)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/OpenCL-Headers)

        # Les noyaux sont embarqués dans le binaire (pas de fichier .cl à
        # installer à côté) : `embed_kernel.py` transforme chaque .cl en
        # en-tête C. On les prend TOUS — `ggml-opencl.cpp` n'inclut que ceux
        # dont il a besoin, les autres ne coûtent qu'un fichier généré.
        file(GLOB GGML_OPENCL_KERNEL_FILES ${RNLLAMA_LIB_DIR}/ggml-opencl/kernels/*.cl)
        set(GGML_OPENCL_KERNEL_HEADERS "")
        foreach (cl ${GGML_OPENCL_KERNEL_FILES})
            get_filename_component(kname ${cl} NAME_WE)
            # Écarte les résidus AppleDouble (._foo.cl), qui ne sont pas du
            # code source valide.
            if (kname MATCHES "^\\._" OR kname STREQUAL "")
                continue()
            endif ()
            set(out ${CMAKE_CURRENT_BINARY_DIR}/${kname}.cl.h)
            add_custom_command(
                OUTPUT ${out}
                COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${RNLLAMA_LIB_DIR}/ggml-opencl/kernels
                        ${Python3_EXECUTABLE} ${RNLLAMA_LIB_DIR}/ggml-opencl/kernels/embed_kernel.py
                        ${cl} ${out}
                DEPENDS ${cl} ${RNLLAMA_LIB_DIR}/ggml-opencl/kernels/embed_kernel.py
                COMMENT "Embedding OpenCL kernel: ${kname}.cl")
            list(APPEND GGML_OPENCL_KERNEL_HEADERS ${out})
        endforeach ()

        target_sources(${target_name} PRIVATE
            ${RNLLAMA_LIB_DIR}/ggml-opencl/ggml-opencl.cpp
            ${RNLLAMA_LIB_DIR}/ggml-opencl/cl-program-cache.cpp
            ${GGML_OPENCL_KERNEL_HEADERS})
        target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

        target_compile_options(${target_name} PRIVATE
            -DLM_GGML_USE_OPENCL
            -DLM_GGML_OPENCL_USE_ADRENO_KERNELS
            -DLM_GGML_OPENCL_EMBED_KERNELS
            -DLM_GGML_OPENCL_SOA_Q
            -DLM_GGML_OPENCL_TARGET_VERSION=300)

        # On lie contre un STUB de `libOpenCL.so` (produit par
        # `scripts/build-opencl.sh` de cui-llama.rn) : à l'exécution, c'est le
        # vrai pilote du téléphone, `/vendor/lib64/libOpenCL.so`, qui est
        # chargé. Le stub ne sert qu'à satisfaire l'éditeur de liens.
        set(OPENCL_STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../bin/${ANDROID_ABI})
        if (EXISTS ${OPENCL_STUB_DIR}/libOpenCL.so)
            target_link_directories(${target_name} PRIVATE ${OPENCL_STUB_DIR})
            target_link_libraries(${target_name} OpenCL)
        else ()
            message(FATAL_ERROR
                "libOpenCL.so introuvable dans ${OPENCL_STUB_DIR} — voir scripts/build-opencl.sh")
        endif ()
    endif ()

    if (${arch} STREQUAL "generic")
        target_compile_options(${target_name} PRIVATE -DLM_GGML_CPU_GENERIC)
    endif ()

    target_compile_options(${target_name} PRIVATE 
        -DLM_GGML_USE_CPU 
        -DLM_GGML_USE_CPU_REPACK 
        -pthread 
        ${cpu_flags} 
        -fvectorize 
        -ffp-model=fast 
        -fno-finite-math-only 
        -flto 
        -D_GNU_SOURCE
        -DRNLLAMA_USE_FD_FILE
    )

    if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -DRNLLAMA_ANDROID_ENABLE_LOGGING)
    endif ()

    target_compile_options(${target_name} PRIVATE -O3 -DNDEBUG)
    target_compile_options(${target_name} PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    target_compile_options(${target_name} PRIVATE -ffunction-sections -fdata-sections)

    target_link_options(${target_name} PRIVATE -Wl,--gc-sections)
    target_link_options(${target_name} PRIVATE -Wl,--exclude-libs,ALL)
    target_link_options(${target_name} PRIVATE -flto)
endfunction()


# Default target (no specific CPU features)
build_library("rnllama" "generic" "")

if (${ANDROID_ABI} STREQUAL "arm64-v8a")
    # ARM64 targets
    build_library("rnllama_v8" "arm" "-march=armv8-a")
    build_library("rnllama_v8_2" "arm" "-march=armv8.2-a")
    build_library("rnllama_v8_2_dotprod" "arm" "-march=armv8.2-a+dotprod")
    build_library("rnllama_v8_2_i8mm" "arm" "-march=armv8.2-a+i8mm")
    build_library("rnllama_v8_2_dotprod_i8mm" "arm" "-march=armv8.2-a+dotprod+i8mm")
    # Variante GPU : mêmes instructions ARM que ci-dessus, PLUS le backend
    # OpenCL. Elle s'ajoute aux variantes CPU au lieu de les remplacer, pour
    # qu'un appareil sans OpenCL exploitable garde un chemin qui fonctionne.
    build_library("rnllama_v8_2_dotprod_i8mm_opencl" "arm" "-march=armv8.2-a+dotprod+i8mm")
    # Add other variants if needed, mirroring cui-llama.rn
elseif (${ANDROID_ABI} STREQUAL "x86_64")
    # x86_64 target
    build_library("rnllama_x86_64" "x86" "-march=x86-64;-mtune=generic;-msse4.2;-mpopcnt")
endif ()
