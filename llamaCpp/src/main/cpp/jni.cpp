#include <jni.h>
#include <unistd.h>
#include <fcntl.h>

#include <android/log.h>
#include <cstdlib>
#include <ctime>
#include <sys/sysinfo.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "llama.h"
#include "rn-llama.h"
#include "rn-completion.h"
#include "rn-mtmd.hpp"
#include "ggml.h"

#define UNUSED(x) (void)(x)
#define TAG "RNLLAMA_ANDROID_JNI"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,     TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,     TAG, __VA_ARGS__)

static inline int min(int a, int b) {
    return (a < b) ? a : b;
}

// Le journal INTERNE de llama.cpp, renvoyé vers logcat.
//
// Sans lui, le moteur dit tout ce qu'il fait — backend choisi, couches
// déportées sur le GPU, tailles des tampons — et personne ne l'entend : rien
// ne sortait de la bibliothèque. Impossible, depuis l'application, de
// répondre à la seule question qui compte quand on vise l'accélération
// matérielle : les couches sont-elles RÉELLEMENT parties sur le GPU ? On
// pouvait la déduire du temps de chargement et du débit ; on ne pouvait pas
// la constater.
//
// Le retour à la ligne final est retiré : llama.cpp termine ses messages par
// une fin de ligne, logcat en ajoute une, ce qui produirait une ligne vide
// sur deux.
static void rnllama_log_to_logcat(lm_ggml_log_level level, const char * text, void * user_data) {
    UNUSED(user_data);
    if (text == nullptr) return;
    std::string ligne(text);
    while (!ligne.empty() && (ligne.back() == '\n' || ligne.back() == '\r')) {
        ligne.pop_back();
    }
    if (ligne.empty()) return;
    int prio;
    switch (level) {
        case LM_GGML_LOG_LEVEL_ERROR: prio = ANDROID_LOG_ERROR; break;
        case LM_GGML_LOG_LEVEL_WARN:  prio = ANDROID_LOG_WARN;  break;
        case LM_GGML_LOG_LEVEL_DEBUG: prio = ANDROID_LOG_DEBUG; break;
        default:                      prio = ANDROID_LOG_INFO;  break;
    }
    __android_log_print(prio, "RNLlamaEngine", "%s", ligne.c_str());
}

extern "C" {

// Helper method to create a Java HashMap
static inline jobject createHashMap(JNIEnv *env) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID init = env->GetMethodID(hashMapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(hashMapClass, init);
    return hashMap;
}

// Helper method to put a string into a Java HashMap
static inline void putStringHashMap(JNIEnv *env, jobject hashMap, const char *key, const char *value) {
    if (value == nullptr) return;
    
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    jstring jKey = env->NewStringUTF(key);
    jstring jValue = env->NewStringUTF(value);
    
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGW("putStringHashMap: Invalid UTF-8 for key %s", key);
        jValue = env->NewStringUTF("");
    }

    env->CallObjectMethod(hashMap, putMethod, jKey, jValue);
    
    env->DeleteLocalRef(jKey);
    if (jValue) env->DeleteLocalRef(jValue);
}

// Helper method to put an int into a Java HashMap
static inline void putIntHashMap(JNIEnv *env, jobject hashMap, const char *key, int value) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    jstring jKey = env->NewStringUTF(key);

    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID integerConstructor = env->GetMethodID(integerClass, "<init>", "(I)V");
    jobject jValue = env->NewObject(integerClass, integerConstructor, value);

    env->CallObjectMethod(hashMap, putMethod, jKey, jValue);
    
    env->DeleteLocalRef(jKey);
    env->DeleteLocalRef(integerClass);
    env->DeleteLocalRef(jValue);
}

// Helper method to put a double into a Java HashMap
static inline void putDoubleHashMap(JNIEnv *env, jobject hashMap, const char *key, double value) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    jstring jKey = env->NewStringUTF(key);

    jclass doubleClass = env->FindClass("java/lang/Double");
    jmethodID doubleConstructor = env->GetMethodID(doubleClass, "<init>", "(D)V");
    jobject jValue = env->NewObject(doubleClass, doubleConstructor, value);

    env->CallObjectMethod(hashMap, putMethod, jKey, jValue);
    
    env->DeleteLocalRef(jKey);
    env->DeleteLocalRef(doubleClass);
    env->DeleteLocalRef(jValue);
}

// Helper method to put a boolean into a Java HashMap
static inline void putBooleanHashMap(JNIEnv *env, jobject hashMap, const char *key, bool value) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    jstring jKey = env->NewStringUTF(key);

    jclass booleanClass = env->FindClass("java/lang/Boolean");
    jmethodID booleanConstructor = env->GetMethodID(booleanClass, "<init>", "(Z)V");
    jobject jValue = env->NewObject(booleanClass, booleanConstructor, value);

    env->CallObjectMethod(hashMap, putMethod, jKey, jValue);
    
    env->DeleteLocalRef(jKey);
    env->DeleteLocalRef(booleanClass);
    env->DeleteLocalRef(jValue);
}

// Helper method to create a Java ArrayList
static inline jobject createArrayList(JNIEnv *env) {
    jclass arrayListClass = env->FindClass("java/util/ArrayList");
    jmethodID init = env->GetMethodID(arrayListClass, "<init>", "()V");
    jobject arrayList = env->NewObject(arrayListClass, init);
    return arrayList;
}

// Helper method to add an int to a Java ArrayList
static inline void addIntArrayList(JNIEnv *env, jobject arrayList, int value) {
    jclass arrayListClass = env->FindClass("java/util/ArrayList");
    jmethodID addMethod = env->GetMethodID(arrayListClass, "add", "(Ljava/lang/Object;)Z");

    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID integerConstructor = env->GetMethodID(integerClass, "<init>", "(I)V");
    jobject jValue = env->NewObject(integerClass, integerConstructor, value);

    env->CallBooleanMethod(arrayList, addMethod, jValue);
    
    env->DeleteLocalRef(integerClass);
    env->DeleteLocalRef(jValue);
}

// Helper method to add a double to a Java ArrayList
static inline void addDoubleArrayList(JNIEnv *env, jobject arrayList, double value) {
    jclass arrayListClass = env->FindClass("java/util/ArrayList");
    jmethodID addMethod = env->GetMethodID(arrayListClass, "add", "(Ljava/lang/Object;)Z");

    jclass doubleClass = env->FindClass("java/lang/Double");
    jmethodID doubleConstructor = env->GetMethodID(doubleClass, "<init>", "(D)V");
    jobject jValue = env->NewObject(doubleClass, doubleConstructor, value);

    env->CallBooleanMethod(arrayList, addMethod, jValue);
    
    env->DeleteLocalRef(doubleClass);
    env->DeleteLocalRef(jValue);
}

// Helper method to add a string to a Java ArrayList
static inline void addStringArrayList(JNIEnv *env, jobject arrayList, const char *value) {
    if (value == nullptr) return;
    
    jclass arrayListClass = env->FindClass("java/util/ArrayList");
    jmethodID addMethod = env->GetMethodID(arrayListClass, "add", "(Ljava/lang/Object;)Z");

    jstring jValue = env->NewStringUTF(value);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        jValue = env->NewStringUTF("");
    }

    env->CallBooleanMethod(arrayList, addMethod, jValue);
    
    if (jValue) env->DeleteLocalRef(jValue);
}

// Helper method to add a HashMap to a Java ArrayList
static inline void addHashMapArrayList(JNIEnv *env, jobject arrayList, jobject value) {
    jclass arrayListClass = env->FindClass("java/util/ArrayList");
    jmethodID addMethod = env->GetMethodID(arrayListClass, "add", "(Ljava/lang/Object;)Z");

    env->CallBooleanMethod(arrayList, addMethod, value);
}

// Helper method to put a Java ArrayList into a Java HashMap
static inline void putArrayListHashMap(JNIEnv *env, jobject hashMap, const char *key, jobject value) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    jstring jKey = env->NewStringUTF(key);

    env->CallObjectMethod(hashMap, putMethod, jKey, value);
    
    env->DeleteLocalRef(jKey);
}

// Helper method to put a Java HashMap into a Java HashMap
static inline void putHashMapHashMap(JNIEnv *env, jobject hashMap, const char *key, jobject value) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    jstring jKey = env->NewStringUTF(key);

    env->CallObjectMethod(hashMap, putMethod, jKey, value);
    
    env->DeleteLocalRef(jKey);
}

std::unordered_map<long, rnllama::llama_rn_context *> context_map;

JNIEXPORT jlong JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_initContextWithFd(
        JNIEnv *env,
        jobject thiz,
        jint model_fd,
        jboolean embedding,
        jint n_ctx,
        jint n_batch,
        jint n_threads,
        jint n_gpu_layers,
        jboolean use_mlock,
        jboolean use_mmap,
        jboolean vocab_only,
        jstring lora_str,
        jfloat lora_scaled,
        jfloat rope_freq_base,
        jfloat rope_freq_scale,
        jint mmproj_fd,
        jintArray image_fds
) {
    UNUSED(thiz);

    // Posé avant toute initialisation : les lignes qui nous intéressent
    // (« offloaded N/M layers to GPU ») sont écrites PENDANT le chargement.
    // Réinstallé à chaque appel plutôt qu'une fois pour toutes : c'est
    // idempotent, et ça évite de dépendre d'un `JNI_OnLoad` absent ici.
    llama_log_set(rnllama_log_to_logcat, nullptr);

    common_params defaultParams;

    defaultParams.vocab_only = vocab_only;
    if (vocab_only) defaultParams.warmup = false;

    if (model_fd < 0) {
        LOGW("Invalid model_fd < 0");
        return 0;
    }

    int dupfd = dup(model_fd);
    if (dupfd == -1) {
        LOGW("dup(model_fd=%d) failed errno=%d (%s)",
             model_fd, errno, strerror(errno));
        return 0;
    }
    close(model_fd);

    char fdString[32];
    snprintf(fdString, 32, "%d", dupfd);
    defaultParams.model.path = fdString;

    defaultParams.embedding = embedding;
    defaultParams.n_ctx = n_ctx;
    defaultParams.n_batch = n_batch;

    int max_threads = std::thread::hardware_concurrency();
    int auto_threads = max_threads == 4 ? 2 : std::min(4, max_threads);
    defaultParams.cpuparams.n_threads =
            n_threads > 0 ? n_threads : auto_threads;

    defaultParams.n_gpu_layers = n_gpu_layers;
    // llama.cpp a remplacé les deux booléens `use_mmap`/`use_mlock` par un
    // seul `load_mode`. On traduit ici plutôt que côté Kotlin : l'API de la
    // bibliothèque garde ses deux paramètres, les appelants ne changent pas.
    //
    // `MLOCK` mérite d'être souligné : il épingle les poids en RAM. Sur un
    // téléphone sous pression mémoire, le modèle est sinon paginé hors RAM et
    // relu depuis le stockage à chaque jeton — mesuré sur cet appareil :
    // 0,47 jeton/s au lieu de 7,36, avec 106 000 défauts de page majeurs par
    // minute.
    if (use_mmap && use_mlock) {
        defaultParams.load_mode = LLAMA_LOAD_MODE_MMAP_MLOCK;
    } else if (use_mmap) {
        defaultParams.load_mode = LLAMA_LOAD_MODE_MMAP;
    } else if (use_mlock) {
        defaultParams.load_mode = LLAMA_LOAD_MODE_MLOCK;
    } else {
        defaultParams.load_mode = LLAMA_LOAD_MODE_NONE;
    }

    const char *lora_chars = env->GetStringUTFChars(lora_str, nullptr);
    if (lora_chars && lora_chars[0] != '\0') {
        defaultParams.lora_adapters.push_back({lora_chars, lora_scaled, "", "", nullptr});
    }

    if (mmproj_fd >= 0) {
        int dup_mmproj_fd = dup(mmproj_fd);
        if (dup_mmproj_fd != -1) {
            char mmproj_path[32];
            snprintf(mmproj_path, 32, "%d", dup_mmproj_fd);
            defaultParams.mmproj.path = mmproj_path;
            LOGI("mmproj set to FD: %s", defaultParams.mmproj.path.c_str());
        }
        close(mmproj_fd);
    }

    defaultParams.rope_freq_base = rope_freq_base;
    defaultParams.rope_freq_scale = rope_freq_scale;

    auto llama = new rnllama::llama_rn_context();
    bool ok = llama->loadModel(defaultParams);

    if (ok) {
        context_map[(long) llama->ctx] = llama;
        if (!defaultParams.mmproj.path.empty()) {
            LOGI("Initializing multimodal with mmproj: %s", defaultParams.mmproj.path.c_str());
            bool mm_ok = llama->initMultimodal(defaultParams.mmproj.path, n_gpu_layers > 0);
            LOGI("Multimodal initialization result: %s", mm_ok ? "success" : "failed");
            LOGI("Context multimodal enabled check: %s", llama->isMultimodalEnabled() ? "yes" : "no");
        }
    } else {
        delete llama;
    }

    env->ReleaseStringUTFChars(lora_str, lora_chars);
    return ok ? reinterpret_cast<jlong>(llama->ctx) : 0;
}

JNIEXPORT jobject JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_loadModelDetails(
        JNIEnv *env,
        jobject thiz,
        jlong context_ptr
) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    auto llama = it->second;

    int count = llama_model_meta_count(llama->model);
    auto meta = createHashMap(env);
    for (int i = 0; i < count; i++) {
        char key[256];
        llama_model_meta_key_by_index(llama->model, i, key, sizeof(key));
        char val[2048];
        llama_model_meta_val_str_by_index(llama->model, i, val, sizeof(val));

        putStringHashMap(env, meta, key, val);
    }

    auto result = createHashMap(env);

    char desc[1024];
    llama_model_desc(llama->model, desc, sizeof(desc));
    putStringHashMap(env, result, "desc", desc);
    putDoubleHashMap(env, result, "size", llama_model_size(llama->model));
    putDoubleHashMap(env, result, "nParams", (double)llama_model_n_params(llama->model));
    putBooleanHashMap(env, result, "isChatTemplateSupported", llama->validateModelChatTemplate(true, nullptr));
    putHashMapHashMap(env, result, "metadata", meta);

    return reinterpret_cast<jobject>(result);
}

JNIEXPORT jstring JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_getFormattedChat(
        JNIEnv *env,
        jobject thiz,
        jlong context_ptr,
        jobjectArray messages,
        jstring chat_template
) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    return env->NewStringUTF(""); 
}

JNIEXPORT jobject JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_loadSession(
        JNIEnv *env,
        jobject thiz,
        jlong context_ptr,
        jstring path
) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    auto llama = it->second;
    const char *path_chars = env->GetStringUTFChars(path, nullptr);

    auto result = createHashMap(env);
    size_t n_token_count_out = 0;
    if (!llama_state_load_file(llama->ctx, path_chars, nullptr, 0, &n_token_count_out)) {
        env->ReleaseStringUTFChars(path, path_chars);
        putStringHashMap(env, result, "error", "Failed to load session");
        return reinterpret_cast<jobject>(result);
    }
    env->ReleaseStringUTFChars(path, path_chars);

    putIntHashMap(env, result, "tokens_loaded", (int)n_token_count_out);
    putStringHashMap(env, result, "prompt", "");
    return reinterpret_cast<jobject>(result);
}

JNIEXPORT jint JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_saveSession(
        JNIEnv *env,
        jobject thiz,
        jlong context_ptr,
        jstring path,
        jint size
) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return -1;
    auto llama = it->second;

    const char *path_chars = env->GetStringUTFChars(path, nullptr);

    if (!llama_state_save_file(llama->ctx, path_chars, nullptr, 0)) {
        env->ReleaseStringUTFChars(path, path_chars);
        return -1;
    }

    env->ReleaseStringUTFChars(path, path_chars);
    return 0;
}

JNIEXPORT jobject JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_doCompletion(
        JNIEnv *env,
        jobject thiz,
        jlong context_ptr,
        jstring prompt,
        jstring grammar,
        jfloat temperature,
        jint n_threads,
        jint n_predict,
        jint n_probs,
        jint penalty_last_n,
        jfloat penalty_repeat,
        jfloat penalty_freq,
        jfloat penalty_present,
        jfloat mirostat,
        jfloat mirostat_tau,
        jfloat mirostat_eta,
        jboolean penalize_nl,
        jint top_k,
        jfloat top_p,
        jfloat min_p,
        jfloat xtc_t,
        jfloat xtc_p,
        jfloat tfs_z,
        jfloat typical_p,
        jint seed,
        jobjectArray stop,
        jboolean ignore_eos,
        jobjectArray logit_bias,
        jintArray image_fds,
        jobject partialCompletionCallback
) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    auto llama = it->second;

    if (llama->completion == nullptr) return nullptr;

    llama->completion->rewind();

    const char* prompt_chars = env->GetStringUTFChars(prompt, nullptr);
    llama->params.prompt = prompt_chars;

    llama->params.sampling.seed = (seed == -1) ? time(NULL) : seed;
    llama->params.sampling.temp = temperature;
    llama->params.sampling.top_k = top_k;
    llama->params.sampling.top_p = top_p;
    llama->params.sampling.min_p = min_p;
    llama->params.n_predict = n_predict;

    llama->completion->initSampling();
    
    std::vector<std::string> images;
    if (image_fds != nullptr) {
        jsize len = env->GetArrayLength(image_fds);
        jint *fds = env->GetIntArrayElements(image_fds, nullptr);
        for (jsize i = 0; i < len; i++) {
            int dup_img_fd = dup(fds[i]);
            if (dup_img_fd != -1) {
                char img_path[32];
                snprintf(img_path, 32, "%d", dup_img_fd);
                images.push_back(img_path);
            }
            close(fds[i]);
        }
        env->ReleaseIntArrayElements(image_fds, fds, 0);
    }
    
    LOGI("doCompletion: prompt='%s', images=%zu, multimodal_enabled=%s", prompt_chars, images.size(), llama->isMultimodalEnabled() ? "yes" : "no");
    
    try {
        llama->completion->loadPrompt(images);
        llama->completion->beginCompletion();

        jclass cb_class = env->GetObjectClass(partialCompletionCallback);
        jmethodID onPartialCompletion = env->GetMethodID(cb_class, "onPartialCompletion", "(Ljava/util/Map;)V");

        size_t sent_count = 0;
        while (llama->completion->has_next_token && !llama->completion->is_interrupted) {
            auto token_output = llama->completion->doCompletion();
            if (token_output.tok == -1) break;

            if (llama->completion->incomplete) continue;

            size_t pos = std::min(sent_count, llama->completion->generated_text.size());
            std::string to_send = llama->completion->generated_text.substr(pos);
            sent_count += to_send.size();

            if (!to_send.empty()) {
                auto tokenResult = createHashMap(env);
                putStringHashMap(env, tokenResult, "token", to_send.c_str());
                env->CallVoidMethod(partialCompletionCallback, onPartialCompletion, tokenResult);
                env->DeleteLocalRef(tokenResult);
            }
        }
        
        llama->completion->endCompletion();
    } catch (const std::exception& e) {
        LOGW("doCompletion: Caught exception: %s", e.what());
    } catch (...) {
        LOGW("doCompletion: Caught unknown exception");
    }

    env->ReleaseStringUTFChars(prompt, prompt_chars);
    return createHashMap(env);
}

JNIEXPORT void JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_stopCompletion(
        JNIEnv *env, jobject thiz, jlong context_ptr) {
    UNUSED(env);
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return;
    auto llama = it->second;
    if (llama->completion) llama->completion->is_interrupted = true;
}

JNIEXPORT jboolean JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_isPredicting(
        JNIEnv *env, jobject thiz, jlong context_ptr) {
    UNUSED(env);
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return false;
    auto llama = it->second;
    if (llama->completion) return llama->completion->is_predicting;
    return false;
}

JNIEXPORT jobject JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_tokenize(
        JNIEnv *env, jobject thiz, jlong context_ptr, jstring text) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    auto llama = it->second;

    const char *text_chars = env->GetStringUTFChars(text, nullptr);
    auto result = llama->tokenize(text_chars, {});
    env->ReleaseStringUTFChars(text, text_chars);

    jobject list = createArrayList(env);
    for (const auto &tok : result.tokens) {
        addIntArrayList(env, list, tok);
    }
    return list;
}

JNIEXPORT jstring JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_detokenize(
        JNIEnv *env, jobject thiz, jlong context_ptr, jintArray tokens) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    auto llama = it->second;

    jsize tokens_len = env->GetArrayLength(tokens);
    jint *tokens_ptr = env->GetIntArrayElements(tokens, 0);
    std::vector<llama_token> toks;
    for (int i = 0; i < tokens_len; i++) {
        toks.push_back(tokens_ptr[i]);
    }
    env->ReleaseIntArrayElements(tokens, tokens_ptr, 0);

    auto text = rnllama::tokens_to_str(llama->ctx, toks.cbegin(), toks.cend());
    
    jstring jText = env->NewStringUTF(text.c_str());
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        jText = env->NewStringUTF("");
    }
    return jText;
}

JNIEXPORT jboolean JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_isEmbeddingEnabled(
        JNIEnv *env, jobject thiz, jlong context_ptr) {
    UNUSED(env);
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return false;
    auto llama = it->second;
    return llama->params.embedding;
}

JNIEXPORT jobject JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_embedding(
        JNIEnv *env, jobject thiz, jlong context_ptr, jstring text) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    auto llama = it->second;

    const char *text_chars = env->GetStringUTFChars(text, nullptr);
    llama->params.prompt = text_chars;

    if (llama->completion) {
        auto result = llama->completion->embedding(llama->params);
        env->ReleaseStringUTFChars(text, text_chars);
        jobject list = createArrayList(env);
        for (const auto &val : result) {
            addDoubleArrayList(env, list, (double)val);
        }
        return list;
    }
    env->ReleaseStringUTFChars(text, text_chars);
    return createArrayList(env);
}

JNIEXPORT jstring JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_bench(
        JNIEnv *env,
        jobject thiz,
        jlong context_ptr,
        jint pp,
        jint tg,
        jint pl,
        jint nr
) {
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return nullptr;
    auto llama = it->second;
    if (llama->completion) {
        std::string result = llama->completion->bench(pp, tg, pl, nr);
        return env->NewStringUTF(result.c_str());
    }
    return env->NewStringUTF("[]"); 
}

JNIEXPORT void JNICALL
Java_org_nehuatl_llamacpp_LlamaContext_freeContext(
        JNIEnv *env, jobject thiz, jlong context_ptr) {
    UNUSED(env);
    UNUSED(thiz);
    auto it = context_map.find((long) context_ptr);
    if (it == context_map.end()) return;
    auto llama = it->second;
    context_map.erase((long) llama->ctx);
    delete llama;
}

} // extern "C"
