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
    # `common` AVANT la racine, et ce n'est pas un détail : l'arbre aplati
    # contient DEUX fichiers nommés `unicode.h` — celui du tokeniseur, à la
    # racine, et celui que l'amont a ajouté dans `common/` (octobre 2026). Un
    # fichier de `common/jinja` qui écrit `#include "unicode.h"` veut le
    # second ; avec la racine en premier il recevait le premier, et la
    # compilation s'arrêtait sur `common_unicode_cpt_to_utf8` introuvable.
    #
    # Les fichiers de la racine, eux, ne risquent rien : pour une inclusion
    # entre guillemets, le compilateur regarde d'abord le répertoire du
    # fichier qui inclut, donc ils gardent leur propre `unicode.h`.
    ${RNLLAMA_LIB_DIR}/common
    ${RNLLAMA_LIB_DIR}
    ${RNLLAMA_LIB_DIR}/common/jinja
    ${RNLLAMA_LIB_DIR}/ggml-cpu
    ${RNLLAMA_LIB_DIR}/tools/mtmd
    # `codec` : synthèse vocale, apparue en amont après l'écriture de
    # `sync_llamacpp.sh` — qui ne connaît donc pas ce répertoire. `rn-tts.cpp`
    # inclut `codec.h`, d'où l'échec de compilation sans ces chemins.
    ${RNLLAMA_LIB_DIR}/codec/include
    ${RNLLAMA_LIB_DIR}/codec/common
    # `vendor/hash` : sha256.c inclut « rotate-bits/rotate-bits.h » par un
    # chemin relatif à la racine du dossier hash.
    ${RNLLAMA_LIB_DIR}/tools/mtmd/hash
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
file(GLOB MODEL_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/models/*.cpp)
file(GLOB GGML_CPU_C_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/ggml-cpu/*.c)
file(GLOB GGML_CPU_CPP_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/ggml-cpu/*.cpp)
file(GLOB GGML_CPU_AMX_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/ggml-cpu/amx/*.cpp)
file(GLOB LLAMA_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/llama*.cpp)
file(GLOB MTMD_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/tools/mtmd/*.cpp)
file(GLOB MTMD_MODEL_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/tools/mtmd/models/*.cpp)
file(GLOB COMMON_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/common/*.cpp)
file(GLOB JINJA_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/common/jinja/*.cpp)
# Deux dossiers nés en amont après l'instantané d'août : sans eux, l'édition
# de liens réclamait lm_ggml_compute_forward_mul_mat_tiled et les analyseurs
# de discussion (2026-10-24).
file(GLOB TILED_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/ggml-cpu/tiled/*.cpp)
file(GLOB PARSERS_FILES CONFIGURE_DEPENDS ${RNLLAMA_LIB_DIR}/common/parsers/*.cpp)
# `vendor/hash` de l'amont, que mtmd-helper.cpp appelle pour nommer les
# bitmaps par leur empreinte SHA-256.
file(GLOB HASH_FILES CONFIGURE_DEPENDS
    ${RNLLAMA_LIB_DIR}/tools/mtmd/hash/*.cpp
    ${RNLLAMA_LIB_DIR}/tools/mtmd/hash/sha256/*.c)

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
    ${RNLLAMA_LIB_DIR}/ggml.cpp
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
    ${TILED_FILES}
    ${PARSERS_FILES}
    ${HASH_FILES}

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
    
)

# Le tronc commun ci-dessus ne contient AUCUN point d'entrée : deux cibles le
# partagent, le pont JNI de la bibliothèque et l'exécutable natif.
#
# L'exécutable existe parce qu'un processus d'application finit toujours par
# être classé puis sacrifié par Android : mesuré le 2026-10-25, le service du
# modèle dans son propre processus mourait quand même (`importance=400` puis
# `SIGKILL` hors de tout registre d'ActivityManager), 2,8 Go chargés. Un
# exécutable lancé par le clavier n'est pas une application : il garde
# l'`oom_score_adj` hérité au lancement, et n'embarque ni ART ni rien d'Android.
set(SOURCE_FILES_JNI ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/jni.cpp)
set(SOURCE_FILES_SERVEUR ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/serveur.cpp)

find_library(LOG_LIB log)

# Configure une cible DÉJÀ créée : drapeaux processeur, backend OpenCL,
# options d'édition de liens. Partagée par la bibliothèque et l'exécutable,
# pour qu'aucun des deux ne dérive de l'autre.
function(configurer_cible target_name arch cpu_flags)
    # Backend OpenCL (GPU Adreno) : activé pour les cibles dont le nom se
    # termine par `_opencl`, comme dans le CMake de production de
    # cui-llama.rn dont ce bloc est transposé. Une cible SÉPARÉE plutôt qu'un
    # drapeau global : la variante CPU doit rester chargeable telle quelle sur
    # un appareil sans OpenCL utilisable.
    set(ENABLE_OPENCL OFF)
    if (${target_name} MATCHES ".*_opencl$")
        set(ENABLE_OPENCL ON)
    endif ()

    target_link_libraries(${target_name} ${LOG_LIB} android)

    if (ENABLE_OPENCL)
        # Connu d'emblée : il départage les noyaux (génériques pour le serveur
        # natif) et le stub d'édition de liens (chemin absolu pour lui aussi).
        get_target_property(genre_de_cible ${target_name} TYPE)
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

        # Les noyaux spécifiques Adreno : dans la bibliothèque, PAS dans le
        # serveur natif. C'est eux qui plantaient.
        #
        # Enquête du 2026-10-25. Symptôme : sur le GPU, le processus était tué
        # (`SIGKILL`) systématiquement au DEUXIÈME jeton — le premier sort, qui
        # vient de la passe d'invite. Invariant par rapport aux couches
        # déportées (1 à 29), à l'attention éclair, à la taille de lot (64 à
        # 512), à la fenêtre, au cache de noyaux, et aux réglages d'exécution
        # `MM_*`, `XMEM_*`, `DISABLE_FUSION`.
        #
        # Bissection à l'opération près avec `LM_GGML_OPENCL_OPFILTER`, qui
        # refuse au GPU les ops dont le nom correspond :
        #
        #   ROPE, SOFT_MAX, FLASH_ATTN, CPY, GET_ROWS, MUL_MAT_ID → toujours tué
        #   MUL_MAT                                               → ABOUTIT
        #
        # Le noyau fautif est donc la multiplication de matrices, et seulement
        # dans sa version Adreno : compilé sans
        # `LM_GGML_OPENCL_USE_ADRENO_KERNELS`, le serveur mène à bien ses
        # générations avec 4, 15 puis 29 couches sur 29 déportées.
        #
        # Contrepartie mesurée, à ne pas enjoliver : le chemin générique donne
        # 4,4 jetons/s contre 23,7 au processeur sur cet appareil (Qwen2.5-1.5B,
        # chargement déduit). Les noyaux rapides sont justement ceux qui
        # plantent. Le GPU fonctionne, il n'est pas encore rentable ici.
        set(OPENCL_DEFS
            -DLM_GGML_USE_OPENCL
            -DLM_GGML_OPENCL_EMBED_KERNELS
            -DLM_GGML_OPENCL_SOA_Q
            -DLM_GGML_OPENCL_TARGET_VERSION=300)
        # Les noyaux Adreno restent RÉSERVÉS à la bibliothèque JNI.
        #
        # Réessayé le 2026-10-25 sur llama.cpp b11880, qui apportait pourtant
        # « opencl: add several gemm variants for small batches and some
        # fixes » : le processus est toujours tué au deuxième jeton
        # (`SIGKILL`, code 137). Le défaut du noyau `MUL_MAT` version Adreno,
        # isolé par bissection avec `LM_GGML_OPENCL_OPFILTER`, n'est donc pas
        # corrigé en amont. Le chemin générique, lui, mène ses générations à
        # bien avec 29 couches sur 29.
        if (NOT genre_de_cible STREQUAL "EXECUTABLE")
            list(APPEND OPENCL_DEFS -DLM_GGML_OPENCL_USE_ADRENO_KERNELS)
        endif ()
        target_compile_options(${target_name} PRIVATE ${OPENCL_DEFS})

        # On lie contre un STUB de `libOpenCL.so` : à l'exécution, c'est le
        # vrai pilote du téléphone, `/vendor/lib64/libOpenCL.so`, qui est
        # chargé. Le stub ne sert qu'à satisfaire l'éditeur de liens.
        #
        # Deux stubs, selon le genre de cible, et la différence est tout le
        # sujet (mesuré le 2026-10-25) :
        #
        #  - une BIBLIOTHÈQUE est chargée dans un processus d'application, à
        #    qui le linker Android monte l'espace de noms « sphal » : la
        #    dépendance « libOpenCL.so » s'y résout par son nom ;
        #  - un EXÉCUTABLE lancé par l'application n'a pas cet espace de noms.
        #    « libOpenCL.so » n'est alors trouvée nulle part, et ajouter
        #    /vendor/lib64 à LD_LIBRARY_PATH fait chercher au linker TOUTES les
        #    bibliothèques là-dedans, dont libc++.so, dont la copie du
        #    constructeur tire des bibliothèques privées que l'application n'a
        #    pas le droit de mapper (« libbinderdebug.so phdr mmap failed:
        #    Permission denied »).
        #
        # Le stub de `bin/${ANDROID_ABI}-absolu` porte donc comme SONAME le
        # CHEMIN ABSOLU du pilote : le linker charge ce fichier-là, sans rien
        # chercher, et ses propres dépendances (libc++, libc, libm, libdl)
        # viennent du système. Voir scripts/stub-opencl-absolu.sh.
        if (genre_de_cible STREQUAL "EXECUTABLE")
            set(OPENCL_STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../bin/${ANDROID_ABI}-absolu)
        else ()
            set(OPENCL_STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../bin/${ANDROID_ABI})
        endif ()
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


# Les sources propres à une architecture, communes aux deux fabriques.
function(sources_arch arch sortie)
    set(fichiers "")
    if (NOT ${arch} STREQUAL "generic")
        set(fichiers
            ${RNLLAMA_LIB_DIR}/ggml-cpu/arch/${arch}/quants.c
            ${RNLLAMA_LIB_DIR}/ggml-cpu/arch/${arch}/repack.cpp
        )
    endif ()
    set(${sortie} ${fichiers} PARENT_SCOPE)
endfunction()

function(build_library target_name arch cpu_flags)
    sources_arch(${arch} arch_files)
    add_library(${target_name} SHARED ${SOURCE_FILES_JNI} ${arch_files})
    configurer_cible(${target_name} ${arch} "${cpu_flags}")
endfunction()

# L'exécutable porte un nom en `lib*.so` : Android n'autorise l'exécution que
# depuis le répertoire des bibliothèques natives de l'application, et n'y
# extrait que les fichiers qui ressemblent à des bibliothèques. C'en est un
# par le nom seulement.
function(build_executable target_name arch cpu_flags)
    sources_arch(${arch} arch_files)
    add_executable(${target_name} ${SOURCE_FILES_SERVEUR} ${arch_files})
    set_target_properties(${target_name} PROPERTIES
        PREFIX "lib"
        SUFFIX ".so")
    configurer_cible(${target_name} ${arch} "${cpu_flags}")
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
    # Le serveur natif, en deux exécutables. Le GPU est le chemin NORMAL ; le
    # second ne sert que si le pilote du téléphone refuse de se charger dans un
    # processus qui n'est pas une application — ce qui ne se devine pas à la
    # compilation et ne doit pas priver l'utilisateur d'IA locale.
    #
    # Pas un oubli, une mesure (2026-10-25) : le pilote OpenCL du téléphone
    # tire des bibliothèques privées du constructeur. Un processus
    # d'application y accède par l'espace de noms que le linker Android lui
    # monte ; un exécutable nu, non. Le lancer avec `/vendor/lib64` dans
    # LD_LIBRARY_PATH donne « /vendor/lib64/libbinderdebug.so phdr mmap
    # failed: Permission denied » — SELinux, et rien à y faire de ce côté.
    #
    # Le GPU reste accessible par la bibliothèque JNI, dans le service de
    # secours. Et c'est le processeur qui a produit les seuls jetons jamais
    # mesurés sur cet appareil : 3,53 par seconde, là où le GPU n'a jamais
    # tenu jusqu'au premier.
    build_executable("llm_serveur_opencl" "arm" "-march=armv8.2-a+dotprod+i8mm")
    build_executable("llm_serveur" "arm" "-march=armv8.2-a+dotprod+i8mm")
    # Add other variants if needed, mirroring cui-llama.rn
elseif (${ANDROID_ABI} STREQUAL "x86_64")
    # x86_64 target
    build_library("rnllama_x86_64" "x86" "-march=x86-64;-mtune=generic;-msse4.2;-mpopcnt")
endif ()
