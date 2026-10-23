package dev.wen.stopwatchrelay

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Start the relay automatically after a reboot. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED &&
            intent.action != "android.intent.action.QUICKBOOT_POWERON") return
        val prefs = context.getSharedPreferences("relay", Context.MODE_PRIVATE)
        val manual = prefs.getString("manual", "") ?: ""
        context.startForegroundService(Intent(context, RelayService::class.java).putExtra("manual", manual))
    }
}
