package org.nehuatl.llamacpp

import android.content.ContentResolver
import android.net.Uri
import android.os.Build
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.File
import java.io.FileReader
import java.io.IOException
import java.util.Random
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.absoluteValue

class LlamaAndroid(val resolver: ContentResolver) {

    companion object {
        private const val NAME = "RNLlama"

        init {
            Log.d(NAME, "Primary ABI: ${Build.SUPPORTED_ABIS[0]}")
            if (isArm64V8a()) {
                val cpuFeatures = getCpuFeatures()
                Log.d(NAME, "CPU features: $cpuFeatures")

                val hasDotProd = cpuFeatures.contains("dotprod") || cpuFeatures.contains("asimddp")
                val isAtLeastArmV82 = cpuFeatures.contains("asimd") && cpuFeatures.contains("crc32") && cpuFeatures.contains("aes")
                val hasI8mm = cpuFeatures.contains("i8mm")

                when {
                    isAtLeastArmV82 && hasDotProd && hasI8mm -> {
                        // Variante GPU d'abord : elle contient le backend
                        // OpenCL (Adreno) EN PLUS des mêmes noyaux ARM que la
                        // variante CPU. Elle ne force rien — c'est
                        // `n_gpu_layers` qui décide ensuite si le calcul part
                        // sur le GPU (>0) ou reste sur le processeur (0). Une
                        // seule bibliothèque permet donc de comparer les deux
                        // sans réinstaller.
                        //
                        // Le repli n'est pas théorique : cette variante est
                        // liée à `libOpenCL.so`, absente des appareils sans
                        // pilote OpenCL. `System.loadLibrary` y échoue par
                        // `UnsatisfiedLinkError` — on retombe alors sur la
                        // variante CPU, qui est exactement ce qui était
                        // chargé jusqu'ici.
                        val chargee = runCatching {
                            System.loadLibrary("rnllama_v8_2_dotprod_i8mm_opencl")
                            "librnllama_v8_2_dotprod_i8mm_opencl.so (OpenCL disponible)"
                        }.getOrElse { erreur ->
                            Log.w(NAME, "OpenCL indisponible, repli processeur", erreur)
                            System.loadLibrary("rnllama_v8_2_dotprod_i8mm")
                            "librnllama_v8_2_dotprod_i8mm.so (processeur seul)"
                        }
                        Log.d(NAME, "Loading $chargee")
                    }
                    isAtLeastArmV82 && hasDotProd -> {
                        Log.d(NAME, "Loading librnllama_v8_2_dotprod.so")
                        System.loadLibrary("rnllama_v8_2_dotprod")
                    }
                    isAtLeastArmV82 && hasI8mm -> {
                        Log.d(NAME, "Loading librnllama_v8_2_i8mm.so")
                        System.loadLibrary("rnllama_v8_2_i8mm")
                    }
                    isAtLeastArmV82 -> {
                        Log.d(NAME, "Loading librnllama_v8_2.so")
                        System.loadLibrary("rnllama_v8_2")
                    }
                    else -> {
                        Log.d(NAME, "Loading librnllama_v8.so")
                        System.loadLibrary("rnllama_v8")
                    }
                }
            } else if (isX86_64()) {
                Log.d(NAME, "Loading librnllama_x86_64.so")
                System.loadLibrary("rnllama_x86_64")
            } else {
                Log.d(NAME, "Loading default librnllama.so")
                System.loadLibrary("rnllama")
            }
        }

        private fun isArm64V8a(): Boolean = Build.SUPPORTED_ABIS[0] == "arm64-v8a"
        private fun isX86_64(): Boolean = Build.SUPPORTED_ABIS[0] == "x86_64"

        private fun getCpuFeatures(): String {
            val file = File("/proc/cpuinfo")
            val stringBuilder = StringBuilder()
            try {
                BufferedReader(FileReader(file)).use { bufferedReader ->
                    var line: String?
                    while (bufferedReader.readLine().also { line = it } != null) {
                        if (line!!.startsWith("Features")) {
                            stringBuilder.append(line)
                            break
                        }
                    }
                }
                return stringBuilder.toString()
            } catch (e: IOException) {
                Log.w(NAME, "Couldn't read /proc/cpuinfo", e)
                return ""
            }
        }
    }

    private val contexts = ConcurrentHashMap<Int, LlamaContext>()
    private var llamaContextLimit = 1

    fun setContextLimit(limit: Int) {
        llamaContextLimit = limit
    }

    fun startEngine(
        params: Map<String, Any>,
        tokenCallback: ((String) -> Unit)
    ): Map<String, Any>? {
        return try {
            if (contexts.size >= llamaContextLimit) {
                throw Exception("Context limit reached")
            }
            val id = Random().nextInt().absoluteValue
            Log.d(NAME, "Checking if GGUF: ${params["model"]}")
            if (!isGGUF(Uri.parse(params["model"] as String))) {
                Log.e(NAME, "File is not in GGUF format: ${params["model"]}")
                throw IllegalArgumentException("File is not in GGUF format")
            }
            Log.d(NAME, "GGUF check passed")
            val llamaContext = LlamaContext(id, params)
            if (llamaContext.context == 0L) {
                throw Exception("Failed to initialize context")
            }
            llamaContext.setTokenCallback(tokenCallback)
            contexts[id] = llamaContext
            mapOf(
                "contextId" to id,
                "gpu" to false,
                "reasonNoGPU" to "Currently not supported",
                "model" to llamaContext.modelDetails
            )
        } catch (e: Exception) {
            Log.e(NAME, "Error initializing context", e)
            null
        }
    }

    fun releaseContext(id: Int) {
        contexts[id]!!.release()
        contexts.remove(id)
    }

    fun isGGUF(uri: Uri): Boolean {
        val ggufHeader = byteArrayOf(0x47, 0x47, 0x55, 0x46)
        return try {
            Log.d(NAME, "isGGUF: Opening stream for $uri")
            resolver.openInputStream(uri)?.use { input ->
                val header = ByteArray(4)
                val read = input.read(header)
                if (read != 4) {
                    Log.w(NAME, "isGGUF: Failed to read 4 bytes, read $read")
                    return false
                }
                val isGguf = header.contentEquals(ggufHeader)
                Log.d(NAME, "isGGUF: result=$isGguf")
                isGguf
            } ?: false
        } catch (err: Exception) {
            Log.e(NAME, "isGGUF: Error reading stream", err)
            false
        }
    }

    fun getFormattedChat(
        id: Int, messages: List<Map<String, Any>>, chatTemplate: String
    ): Flow<String> = flow {
        try {
            val context = contexts[id] ?: throw Exception("Context not found")
            val result = context.getFormattedChat(messages, chatTemplate)
            emit(result)
        } catch (e: Exception) {
            Log.e(NAME, "Error formatting chat", e)
        }
    }.flowOn(Dispatchers.IO)

    fun loadSession(id: Int, path: String): Flow<Map<String, Any>> = flow {
        try {
            val context = contexts[id] ?: throw Exception("Context not found")
            emit(context.loadSession(path))
        } catch (e: Exception) {
            Log.e(NAME, "Error loading session", e)
        }
    }.flowOn(Dispatchers.IO)

    fun saveSession(id: Int, path: String, size: Int): Flow<Int> = flow {
        try {
            val context = contexts[id] ?: throw Exception("Context not found")
            emit(context.saveSession(path, size))
        } catch (e: Exception) {
            Log.e(NAME, "Error saving session", e)
            emit(-1)
        }
    }.flowOn(Dispatchers.IO)

    fun launchCompletion(id: Int, params: Map<String, Any>): Map<String, Any>?  {
        Log.i(NAME, "launchCompletion: completion $id of $params")
        return try {
            val context = contexts[id] ?: throw Exception("Context not found")
            if (context.isPredicting()) throw Exception("Context is busy")
            context.completion(params)
        } catch (e: Exception) {
            Log.e(NAME, "Error during completion", e)
            null
        }
    }

    suspend fun stopCompletion(id: Int) = withContext(Dispatchers.IO) {
        val context = contexts[id] ?: throw Exception("Context not found")
        context.stopCompletion()
    }

    fun tokenize(id: Int, text: String): Flow<Map<String, Any>> = flow {
        try {
            val context = contexts[id] ?: throw Exception("Context not found")
            emit(mapOf("tokens" to context.tokenize(text)))
        } catch (e: Exception) {
            Log.e(NAME, "Error tokenizing text", e)
        }
    }.flowOn(Dispatchers.IO)

    fun detokenize(id: Int, tokens: List<Int>): Flow<String> = flow {
        try {
            val context = contexts[id] ?: throw Exception("Context not found")
            emit(context.detokenize(tokens))
        } catch (e: Exception) {
            Log.e(NAME, "Error detokenizing tokens", e)
        }
    }.flowOn(Dispatchers.IO)

    fun embedding(id: Int, text: String): Flow<Map<String, Any>> = flow {
        try {
            val context = contexts[id] ?: throw Exception("Context not found")
            emit(context.getEmbedding(text))
        } catch (e: Exception) {
            Log.e(NAME, "Error getting embedding", e)
        }
    }.flowOn(Dispatchers.IO)
}
