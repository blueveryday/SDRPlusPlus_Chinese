package org.sdrpp.sdrpp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.IBinder
import android.os.PowerManager

/**
 * 前台服务：屏幕熄灭或程序切到后台后保持进程运行。
 * - 常驻通知，让系统不会回收进程
 * - 持有 CPU 唤醒锁，保证接收和解调线程不被休眠
 * - 持有 Wi-Fi 锁，保证 SpyServer 等网络信号源在熄屏后不断流
 */
class SdrForegroundService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onBind(intent: Intent?): IBinder? {
        return null
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // 通知渠道（Android 8.0 及以上必需，本程序最低版本为 9.0）
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "SDR++ 后台运行", NotificationManager.IMPORTANCE_LOW)
        )

        // 点击通知回到程序
        val openIntent = Intent(this, MainActivity::class.java)
        openIntent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_NEW_TASK)
        val pending = PendingIntent.getActivity(this, 0, openIntent, 0)

        val notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("SDR++")
            .setContentText("正在后台运行")
            .setSmallIcon(R.drawable.ic_stat_sdr)
            .setContentIntent(pending)
            .setOngoing(true)
            .build()
        startForeground(NOTIFICATION_ID, notification)

        // 唤醒锁
        if (wakeLock == null) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SDRPP::BackgroundWakeLock")
            wakeLock?.setReferenceCounted(false)
            wakeLock?.acquire()
        }

        // Wi-Fi 锁
        if (wifiLock == null) {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "SDRPP::WifiLock")
            wifiLock?.setReferenceCounted(false)
            wifiLock?.acquire()
        }

        // 被系统回收后不自动重启：没有界面时重启服务没有意义
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        if (wakeLock?.isHeld == true) { wakeLock?.release() }
        if (wifiLock?.isHeld == true) { wifiLock?.release() }
        wakeLock = null
        wifiLock = null
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL_ID = "sdrpp_background"
        private const val NOTIFICATION_ID = 1
    }
}
