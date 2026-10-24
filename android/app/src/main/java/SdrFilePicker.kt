package org.sdrpp.sdrpp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.util.Log
import android.widget.Toast
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

/**
 * 系统文件选择器（导入 / 导出频率管理器书签等）。
 * 原生代码不直接访问这个类，而是通过 MainActivity.openFilePicker() / saveFilePicker() 转发：
 * 原生线程里用 FindClass 只能使用系统类加载器，找不到应用自己的类。
 */
object SdrFilePicker {
    private const val REQ_OPEN = 1001
    private const val REQ_SAVE = 1002
    private const val TAG = "SdrFilePicker"
    private var activity: Activity? = null

    // 导出时：原生代码已写好的缓存文件路径，选好保存位置后复制过去
    private var pendingSavePath: String? = null

    fun init(act: Activity) {
        activity = act
    }

    fun openDocument() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            // 很多文件管理器把 .json 标成 text/plain 或 application/octet-stream，只写 application/json 会让文件灰掉选不了
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/json", "text/plain", "application/octet-stream"))
        }
        activity?.startActivityForResult(intent, REQ_OPEN)
    }

    fun saveDocument(cachePath: String, defaultName: String) {
        pendingSavePath = cachePath
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json"
            putExtra(Intent.EXTRA_TITLE, defaultName)
        }
        activity?.startActivityForResult(intent, REQ_SAVE)
    }

    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != REQ_OPEN && requestCode != REQ_SAVE) { return }

        val uri = data?.data
        if (resultCode != Activity.RESULT_OK || uri == null) {
            if (requestCode == REQ_OPEN) nativeOnResultOpen("")
            if (requestCode == REQ_SAVE) nativeOnResultSave("")
            return
        }

        if (requestCode == REQ_OPEN) {
            val path = copyToCache(uri)
            if (path == null) { toast("导入失败：无法读取所选文件") }
            nativeOnResultOpen(path ?: "")
        } else {
            val ok = copyFromCache(uri)
            toast(if (ok) "导出成功" else "导出失败：无法写入所选位置")
            nativeOnResultSave(if (ok) uri.toString() else "")
        }
    }

    // 把用户选择的文件复制到应用缓存目录，供原生代码按普通路径读取
    private fun copyToCache(uri: Uri): String? {
        return try {
            val act = activity ?: return null
            val tempFile = File(act.cacheDir, "picked_${System.currentTimeMillis()}.json")
            val input = act.contentResolver.openInputStream(uri) ?: return null
            input.use { src ->
                FileOutputStream(tempFile).use { out -> src.copyTo(out) }
            }
            tempFile.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "copyToCache failed", e)
            null
        }
    }

    // 把原生代码写好的缓存文件复制到用户选择的位置
    private fun copyFromCache(uri: Uri): Boolean {
        return try {
            val act = activity ?: return false
            val srcPath = pendingSavePath ?: return false
            // "wt" 会先清空目标文件，避免覆盖已有文件时残留旧内容
            val output = act.contentResolver.openOutputStream(uri, "wt") ?: return false
            output.use { out ->
                FileInputStream(srcPath).use { src -> src.copyTo(out) }
            }
            true
        } catch (e: Exception) {
            Log.e(TAG, "copyFromCache failed", e)
            false
        }
    }

    private fun toast(text: String) {
        activity?.let { act -> Toast.makeText(act, text, Toast.LENGTH_SHORT).show() }
    }

    external fun nativeOnResultOpen(path: String)
    external fun nativeOnResultSave(uriString: String)
}
