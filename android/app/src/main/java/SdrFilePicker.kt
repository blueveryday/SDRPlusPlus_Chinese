package org.sdrpp.sdrpp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import java.io.File
import java.io.FileOutputStream

object SdrFilePicker {
    private const val REQ_OPEN = 1001
    private const val REQ_SAVE = 1002
    private var activity: Activity? = null
    private const val TAG = "SdrFilePicker"

    fun init(act: Activity) {
        activity = act
    }

    fun openDocument() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json" 
        }
        activity?.startActivityForResult(intent, REQ_OPEN)
    }

    fun saveDocument(defaultName: String) {
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json"
            putExtra(Intent.EXTRA_TITLE, defaultName)
        }
        activity?.startActivityForResult(intent, REQ_SAVE)
    }

    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (resultCode != Activity.RESULT_OK || data == null) {
            if (requestCode == REQ_OPEN) nativeOnResultOpen("")
            if (requestCode == REQ_SAVE) nativeOnResultSave("")
            return
        }

        val uri = data.data
        if (requestCode == REQ_OPEN) {
            val path = copyToCache(uri)
            nativeOnResultOpen(path ?: "")
        } else if (requestCode == REQ_SAVE) {
            nativeOnResultSave(uri.toString())
        }
    }

    private fun copyToCache(uri: Uri?): String? {
        if (uri == null) return null
        return try {
            val act = activity ?: return null
            val inputStream = act.contentResolver.openInputStream(uri)
            val tempFile = File(act.cacheDir, "picked_${System.currentTimeMillis()}.json")
            FileOutputStream(tempFile).use { output ->
                inputStream?.use { input ->
                    input.copyTo(output)
                }
            }
            tempFile.absolutePath
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }

    external fun nativeOnResultOpen(path: String)
    external fun nativeOnResultSave(uriString: String)
}