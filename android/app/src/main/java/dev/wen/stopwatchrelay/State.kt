package dev.wen.stopwatchrelay

import java.text.SimpleDateFormat
import java.util.ArrayDeque
import java.util.Date
import java.util.Locale

/** Shared status + rolling log (bilingual) for the activity. */
object State {
    @Volatile var ble: String = "未连接"
    @Volatile var ws: String = "未连接"
    @Volatile var english: Boolean = false

    private const val MAX = 300
    private val lines = ArrayDeque<Pair<String, String>>()   // (zh, en)
    private val fmt = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

    @Synchronized
    fun add(zh: String, en: String) {
        val stamp = fmt.format(Date())
        lines.addLast("$stamp  $zh" to "$stamp  $en")
        while (lines.size > MAX) lines.removeFirst()
    }

    @Synchronized
    fun snapshot(): List<String> = lines.map { if (english) it.second else it.first }

    @Synchronized
    fun clear() { lines.clear() }
}
