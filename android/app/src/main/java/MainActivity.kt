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
import android.view.View
import android.view.inputmethod.InputMethodManager
import android.util.Log
import android.content.res.AssetManager
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.widget.EditText

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
                    
                    // Save SDR info
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

class MainActivity : NativeActivity() {
    private val TAG: String = "SDR++"
    var usbManager: UsbManager? = null
    var SDR_device: UsbDevice? = null
    var SDR_conn: UsbDeviceConnection? = null
    var SDR_VID: Int = -1
    var SDR_PID: Int = -1
    var SDR_FD: Int = -1

    private var hiddenEditText: EditText? = null

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
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

        super.onCreate(savedInstanceState)

        ContextCompat.startForegroundService(this, Intent(this, SdrForegroundService::class.java))
        
        SdrFilePicker.init(this)

        hiddenEditText = EditText(this).apply {
            alpha = 0f
            inputType = InputType.TYPE_CLASS_TEXT or 
                        InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                        InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
                override fun afterTextChanged(s: Editable?) {
                    if (!s.isNullOrEmpty()) {
                        for (char in s) {
                            unicodeCharacterQueue.offer(char.toInt())
                        }
                        s.clear()
                    }
                }
            })

            setOnKeyListener { _, keyCode, event ->
                if (event.action == android.view.KeyEvent.ACTION_DOWN) {
                    if (keyCode == android.view.KeyEvent.KEYCODE_DEL) {
                        unicodeCharacterQueue.offer(8)
                        return@setOnKeyListener true
                    } else if (keyCode == android.view.KeyEvent.KEYCODE_ENTER) {
                        unicodeCharacterQueue.offer(10) // 回车键 ASCII
                        return@setOnKeyListener true
                    }
                }
                false
            }
        }

        addContentView(hiddenEditText, android.view.ViewGroup.LayoutParams(1, 1))
    }

    override fun onDestroy() {
        stopService(Intent(this, SdrForegroundService::class.java))
        super.onDestroy()
    }

    override fun onResume() {
        hideSystemBars()
        super.onResume()
    }

    fun showSoftInput() {
        hiddenEditText?.requestFocus()
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.showSoftInput(hiddenEditText, InputMethodManager.SHOW_IMPLICIT)
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(hiddenEditText?.windowToken, 0)
        hideSystemBars()
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
        var success = true
        if (!folder.exists()) {
            success = folder.mkdirs()
        }
        if (!success) {
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
        val aman = assets
        extractDir(aman, "$fdir/res", "res")
        createIfDoesntExist("$fdir/modules")
        return fdir
    }
}