package org.sdrpp.sdrpp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.graphics.BitmapFactory
import android.graphics.Color
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

        // 旧版本使用的是低重要性渠道，部分手机系统会隐藏它的状态栏图标；渠道重要性创建后无法修改，所以换新渠道
        nm.deleteNotificationChannel(OLD_CHANNEL_ID)

        // 默认重要性 + 关闭声音和振动：状态栏会显示图标，但不会打扰用户
        val channel = NotificationChannel(CHANNEL_ID, "SDR++ 后台运行", NotificationManager.IMPORTANCE_DEFAULT)
        channel.setSound(null, null)
        channel.enableVibration(false)
        channel.setShowBadge(false)
        nm.createNotificationChannel(channel)

        val openIntent = Intent(this, MainActivity::class.java)
        openIntent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_NEW_TASK)
        
        val pending = PendingIntent.getActivity(this, 0, openIntent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

        val notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("SDR++ CE")
            .setContentText("正在后台运行，点击返回程序")
            .setSmallIcon(R.drawable.ic_stat_logo)
            .setLargeIcon(BitmapFactory.decodeResource(resources, R.drawable.ic_notify_large))
            .setColor(Color.parseColor("#FF8A2A"))
            .setCategory(Notification.CATEGORY_SERVICE)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setShowWhen(false)
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
        private const val OLD_CHANNEL_ID = "sdrpp_background"
        private const val CHANNEL_ID = "sdrpp_background_v2"
        private const val NOTIFICATION_ID = 1
    }
}