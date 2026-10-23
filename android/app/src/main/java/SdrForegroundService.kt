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

class SdrForegroundService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onBind(intent: Intent?): IBinder? {
        return null
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "SDR++ 后台运行", NotificationManager.IMPORTANCE_LOW)
        )

        val openIntent = Intent(this, MainActivity::class.java)
        openIntent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_NEW_TASK)
        
        val pending = PendingIntent.getActivity(this, 0, openIntent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

        val notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("SDR++")
            .setContentText("正在后台运行")
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(pending)
            .setOngoing(true)
            .build()
        startForeground(NOTIFICATION_ID, notification)

        if (wakeLock == null) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SDRPP::BackgroundWakeLock")
            wakeLock?.setReferenceCounted(false)
            wakeLock?.acquire()
        }

        if (wifiLock == null) {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "SDRPP::WifiLock")
            wifiLock?.setReferenceCounted(false)
            wifiLock?.acquire()
        }

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