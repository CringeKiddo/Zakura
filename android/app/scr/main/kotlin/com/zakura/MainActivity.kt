package com.zakura

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.media.MediaScannerConnection
import android.provider.MediaStore
import androidx.annotation.NonNull
import androidx.core.content.FileProvider
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

class MainActivity: FlutterActivity() {
    private val CHANNEL = "com.zakura/media"
    private val LEGACY_CHANNEL = "com.aereality/media" // Backwards compatibility alias

    override fun configureFlutterEngine(@NonNull flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        val handler = MethodChannel.MethodCallHandler { call, result ->
            when (call.method) {
                "saveToMovies" -> {
                    val sourcePath = call.argument<String>("sourcePath")
                    val fileName = call.argument<String>("fileName")
                    val subFolder = call.argument<String>("subFolder") ?: "Nulls"
                    val mimeType = call.argument<String>("mimeType") ?: "video/mp4"

                    if (sourcePath == null || fileName == null) {
                        result.error("INVALID_ARGS", "sourcePath or fileName is null", null)
                        return@MethodCallHandler
                    }

                    try {
                        val uri = saveFileToMoviesZakura(sourcePath, fileName, subFolder, mimeType)
                        if (uri != null) {
                            result.success(uri.toString())
                        } else {
                            result.error("SAVE_FAILED", "Failed to insert into MediaStore", null)
                        }
                    } catch (e: Exception) {
                        result.error("EXCEPTION", e.localizedMessage, null)
                    }
                }
                "saveImage" -> {
                    val sourcePath = call.argument<String>("sourcePath")
                    val fileName = call.argument<String>("fileName")
                    val mimeType = call.argument<String>("mimeType") ?: "image/png"

                    if (sourcePath == null || fileName == null) {
                        result.error("INVALID_ARGS", "sourcePath or fileName is null", null)
                        return@MethodCallHandler
                    }

                    try {
                        result.success(saveImageToPictures(sourcePath, fileName, mimeType))
                    } catch (e: Exception) {
                        result.error("EXCEPTION", e.localizedMessage, null)
                    }
                }
                "getExitReasons" -> {
                    try {
                        result.success(collectExitReasons())
                    } catch (e: Exception) {
                        result.success(emptyList<Map<String, Any?>>())
                    }
                }
                "scanFile" -> {
                    val path = call.argument<String>("path")
                    if (path != null) {
                        MediaScannerConnection.scanFile(applicationContext, arrayOf(path), null, null)
                    }
                    result.success(true)
                }
                "installApk" -> {
                    val contentUriString = call.argument<String>("contentUri")
                    val filePath = call.argument<String>("filePath")

                    try {
                        val uri: Uri = if (contentUriString != null) {
                            Uri.parse(contentUriString)
                        } else if (filePath != null) {
                            val apkFile = File(filePath)
                            FileProvider.getUriForFile(this, "${applicationContext.packageName}.fileprovider", apkFile)
                        } else {
                            result.error("INVALID_ARGS", "No URI or path provided", null)
                            return@MethodCallHandler
                        }

                        val intent = Intent(Intent.ACTION_VIEW).apply {
                            setDataAndType(uri, "application/vnd.android.package-archive")
                            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION
                        }
                        startActivity(intent)
                        result.success(true)
                    } catch (e: Exception) {
                        result.error("INSTALL_ERROR", e.localizedMessage, null)
                    }
                }
                else -> result.notImplemented()
            }
        }

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler(handler)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, LEGACY_CHANNEL).setMethodCallHandler(handler)
    }

    // Direct filesystem fallback folder: /storage/emulated/0/Movies/Zakura/[subFolder]
    private fun zakuraMoviesDir(subFolder: String): File {
        val movies = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES)
        if (!movies.exists()) movies.mkdirs()
        val zakura = File(movies, "Zakura")
        if (!zakura.exists()) zakura.mkdirs()
        val target = File(zakura, subFolder)
        if (!target.exists()) target.mkdirs()
        return target
    }

    private fun copyToFolder(sourceFile: File, fileName: String, subFolder: String): Uri? {
        val target = File(zakuraMoviesDir(subFolder), fileName)
        FileInputStream(sourceFile).use { input ->
            FileOutputStream(target).use { out -> input.copyTo(out) }
        }
        MediaScannerConnection.scanFile(applicationContext, arrayOf(target.absolutePath), null, null)
        return Uri.fromFile(target)
    }

    private fun insertViaMediaStore(
        collection: Uri,
        sourceFile: File,
        fileName: String,
        mimeType: String,
        relativePath: String
    ): Uri? {
        val resolver = applicationContext.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, fileName)
            put(MediaStore.MediaColumns.MIME_TYPE, mimeType)
            put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath)
            put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val uri = resolver.insert(collection, values) ?: return null
        try {
            resolver.openOutputStream(uri)?.use { out ->
                FileInputStream(sourceFile).use { input -> input.copyTo(out) }
            } ?: throw IllegalStateException("openOutputStream returned null")
            values.clear()
            values.put(MediaStore.MediaColumns.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
            return uri
        } catch (e: Exception) {
            try { resolver.delete(uri, null, null) } catch (_: Exception) {}
            throw e
        }
    }

    private fun saveFileToMoviesZakura(sourcePath: String, fileName: String, subFolder: String, mimeType: String): Uri? {
        val sourceFile = File(sourcePath)
        if (!sourceFile.exists()) return null

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val isVideo = mimeType.startsWith("video/")
            val primary = if (isVideo) MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
                          else MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
            val relPath = Environment.DIRECTORY_MOVIES + "/Zakura/" + subFolder
            try {
                val uri = insertViaMediaStore(primary, sourceFile, fileName, mimeType, relPath)
                if (uri != null) return uri
            } catch (e: Exception) {
                android.util.Log.w("Zakura", "MediaStore save failed, falling back to direct copy: ${e.message}")
            }
            return copyToFolder(sourceFile, fileName, subFolder)
        } else {
            return copyToFolder(sourceFile, fileName, subFolder)
        }
    }

    private fun saveImageToPictures(sourcePath: String, fileName: String, mimeType: String): Map<String, String> {
        val sourceFile = File(sourcePath)
        if (!sourceFile.exists()) throw IllegalStateException("Source image does not exist: $sourcePath")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val images = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
            val uri = insertViaMediaStore(images, sourceFile, fileName, mimeType, Environment.DIRECTORY_PICTURES + "/Zakura")
                ?: throw IllegalStateException("MediaStore insert returned null")
            return mapOf("uri" to uri.toString(), "location" to "Pictures/Zakura")
        }

        val pictures = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES)
        val dir = File(pictures, "Zakura")
        if (!dir.exists()) dir.mkdirs()
        val target = File(dir, fileName)
        FileInputStream(sourceFile).use { input ->
            FileOutputStream(target).use { out -> input.copyTo(out) }
        }
        MediaScannerConnection.scanFile(applicationContext, arrayOf(target.absolutePath), null, null)
        return mapOf("uri" to Uri.fromFile(target).toString(), "location" to "Pictures/Zakura")
    }

    private fun exitReasonName(reason: Int): String = when (reason) {
        0 -> "UNKNOWN"
        1 -> "EXIT_SELF"
        2 -> "SIGNALED"
        3 -> "LOW_MEMORY"
        4 -> "CRASH"
        5 -> "CRASH_NATIVE"
        6 -> "ANR"
        else -> "REASON_$reason"
    }

    private fun collectExitReasons(): List<Map<String, Any?>> {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return emptyList()
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val infos = am.getHistoricalProcessExitReasons(packageName, 0, 4)
        val out = ArrayList<Map<String, Any?>>()
        for (info in infos) {
            val m = HashMap<String, Any?>()
            m["reason"] = info.reason
            m["reasonName"] = exitReasonName(info.reason)
            m["timestamp"] = info.timestamp
            m["status"] = info.status
            m["importance"] = info.importance
            m["description"] = info.description ?: ""
            m["pssKb"] = info.pss
            m["rssKb"] = info.rss
            out.add(m)
        }
        return out
    }
}
