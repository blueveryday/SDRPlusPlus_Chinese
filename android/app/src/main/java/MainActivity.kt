package org.sdrpp.sdrpp

import android.app.NativeActivity
import android.app.PendingIntent
import android.content.Intent
import android.content.IntentFilter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.usb.*
import android.Manifest
import android.os.Bundle
import android.os.Build
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.util.Log
import android.content.res.AssetManager
import android.text.InputType

import androidx.core.app.ActivityCompat
import androidx.core.content.PermissionChecker
import androidx.core.content.ContextCompat

import java.util.concurrent.LinkedBlockingQueue
import java.io.*

private const val ACTION_USB_PERMISSION = "org.sdrpp.sdrpp.USB_PERMISSION"

private val usbReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (ACTION_USB_PERMISSION == intent.action) {
            synchronized(this) {
                val _this = context as MainActivity
                _this.SDR_device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    _this.SDR_conn = _this.usbManager!!.openDevice(_this.SDR_device)
                    _this.SDR_VID = _this.SDR_device!!.vendorId
                    _this.SDR_PID = _this.SDR_device!!.productId
                    _this.SDR_FD = _this.SDR_conn!!.fileDescriptor
                }
                context.unregisterReceiver(this)
                _this.hideSystemBars()
            }
        }
    }
}

/**
 * 软键盘输入连接。输入法的“组合文本”（拼写中的单词、拼音）会被反复替换，
 * 这里用“先退格删除旧的组合文本，再输入新文本”的方式同步给 ImGui，避免字符重复。
 * 退格和回车分别用 8 和 10 编码，由 C++ 端转换成按键事件。
 */
private class SdrInputConnection(view: View, private val sink: (Int) -> Unit) : BaseInputConnection(view, false) {
    private var composingLen = 0

    private fun replaceComposing(text: CharSequence?) {
        for (i in 0 until composingLen) { sink(8) }
        composingLen = 0
        if (text == null) { return }
        var i = 0
        while (i < text.length) {
            val cp = Character.codePointAt(text, i)
            sink(cp)
            composingLen++
            i += Character.charCount(cp)
        }
    }

    override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
        replaceComposing(text)
        return true
    }

    override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
        replaceComposing(text)
        composingLen = 0
        return true
    }

    override fun finishComposingText(): Boolean {
        composingLen = 0
        return true
    }

    override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
        for (i in 0 until beforeLength) { sink(8) }
        composingLen = Math.max(0, composingLen - beforeLength)
        return true
    }

    override fun sendKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            when (event.keyCode) {
                KeyEvent.KEYCODE_DEL -> sink(8)
                KeyEvent.KEYCODE_ENTER -> sink(10)
                else -> {
                    val u = event.unicodeChar
                    if (u != 0) { sink(u) }
                }
            }
        }
        return true
    }

    override fun performEditorAction(actionCode: Int): Boolean {
        sink(10)
        return true
    }
}

/** 不可见的输入视图，只负责接收软键盘输入并交给 C++ */
private class SdrInputView(context: Context, private val sink: (Int) -> Unit, private val onImeBack: () -> Unit) : View(context) {
    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor(): Boolean {
        return true
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        // 禁止横屏时输入法使用全屏的提取输入框，否则它会盖住整个屏幕，看不到程序里的输入框
        outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN
        return SdrInputConnection(this, sink)
    }

    // 用户按返回键收起键盘时，先通知 Activity 恢复全屏
    override fun onKeyPreIme(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) {
            onImeBack()
        }
        return super.onKeyPreIme(keyCode, event)
    }
}

class MainActivity : NativeActivity() {
    private val TAG: String = "SDR++"
    var usbManager: UsbManager? = null
    var SDR_device: UsbDevice? = null
    var SDR_conn: UsbDeviceConnection? = null
    var SDR_VID: Int = -1
    var SDR_PID: Int = -1
    var SDR_FD: Int = -1

    private var inputView: SdrInputView? = null
    private val unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1)
        }
    }

    fun hideSystemBars() {
        val decorView = window.decorView
        val uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        decorView.systemUiVisibility = uiOptions
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        hideSystemBars()

        checkAndAsk(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        checkAndAsk(Manifest.permission.READ_EXTERNAL_STORAGE)

        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        val permissionIntent = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION), PendingIntent.FLAG_IMMUTABLE)
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        registerReceiver(usbReceiver, filter)

        val devList = usbManager!!.deviceList
        for ((name, dev) in devList) {
            usbManager!!.requestPermission(dev, permissionIntent)
        }

        checkAndAsk(Manifest.permission.INTERNET)

        // Android 13 及以上需要用户允许通知，前台服务的通知和状态栏图标才会显示
        if (Build.VERSION.SDK_INT >= 33) {
            checkAndAsk("android.permission.POST_NOTIFICATIONS")
        }

        super.onCreate(savedInstanceState)

        ContextCompat.startForegroundService(this, Intent(this, SdrForegroundService::class.java))
        SdrFilePicker.init(this)

        inputView = SdrInputView(this, { code -> unicodeCharacterQueue.offer(code) }, { onImeDismissed() })
        addContentView(inputView, ViewGroup.LayoutParams(1, 1))
    }

    override fun onDestroy() {
        stopService(Intent(this, SdrForegroundService::class.java))
        super.onDestroy()
    }

    override fun onResume() {
        hideSystemBars()
        super.onResume()
    }

    // 输入期间临时显示系统栏：沉浸模式下 adjustResize 需要系统栏可见才能可靠生效
    private fun showSystemBarsTemporarily() {
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
    }

    fun showSoftInput() {
        runOnUiThread {
            val view = inputView ?: return@runOnUiThread

            // 全屏窗口不会随键盘缩小，所以输入期间临时取消全屏，并让窗口随键盘调整大小，
            // 这样键盘弹出后程序界面缩小到键盘上方，输入框不会被遮住
            showSystemBarsTemporarily()
            window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE or WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)

            view.requestFocus()
            view.postDelayed({
                val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                imm.restartInput(view)
                imm.showSoftInput(view, InputMethodManager.SHOW_FORCED)
            }, 100)
        }
    }

    fun hideSoftInput() {
        runOnUiThread {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.hideSoftInputFromWindow(inputView?.windowToken, 0)
            onImeDismissed()
        }
    }

    // 键盘收起后恢复全屏沉浸模式
    private fun onImeDismissed() {
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING or WindowManager.LayoutParams.SOFT_INPUT_STATE_HIDDEN)
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
        hideSystemBars()
    }

    // 供原生代码调用：打开系统文件选择器（导入）
    fun openFilePicker() {
        runOnUiThread { SdrFilePicker.openDocument() }
    }

    // 供原生代码调用：让用户选择保存位置，并把缓存文件复制过去（导出）
    fun saveFilePicker(cachePath: String, defaultName: String) {
        runOnUiThread { SdrFilePicker.saveDocument(cachePath, defaultName) }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        SdrFilePicker.onActivityResult(requestCode, resultCode, data)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    fun createIfDoesntExist(path: String) {
        val folder = File(path)
        if (!folder.exists() && !folder.mkdirs()) {
            Log.e(TAG, "Could not create folder with path $path")
        }
    }

    fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc)
        var ecount = 0
        for (fp in flist) {
            val lpath = "$local/$fp"
            val rpath = "$rsrc/$fp"
            Log.w(TAG, "Extracting '$rpath' to '$lpath'")
            createIfDoesntExist(local)
            val ext = extractDir(aman, lpath, rpath)
            if (ext == 0) {
                val _os = FileOutputStream(lpath)
                val _is = aman.open(rpath)
                val ilen = _is.available()
                val fbuf = ByteArray(ilen)
                _is.read(fbuf, 0, ilen)
                _os.write(fbuf)
                _os.close()
                _is.close()
            }
            ecount++
        }
        return ecount
    }

    fun getAppDir(): String {
        val fdir = filesDir.absolutePath
        extractDir(assets, "$fdir/res", "res")
        createIfDoesntExist("$fdir/modules")
        return fdir
    }
}