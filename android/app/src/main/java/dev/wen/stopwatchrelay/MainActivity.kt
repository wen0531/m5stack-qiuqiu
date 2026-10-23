package dev.wen.stopwatchrelay

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.method.ScrollingMovementMethod
import android.view.Gravity
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var status: TextView
    private lateinit var logView: TextView
    private lateinit var scroller: ScrollView

    private val refresh = object : Runnable {
        override fun run() {
            status.text = "蓝牙: ${State.ble}    中继: ${State.ws}"
            val lines = State.snapshot()
            logView.text = lines.joinToString("\n")
            scroller.post { scroller.fullScroll(ScrollView.FOCUS_DOWN) }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val prefs = getSharedPreferences("relay", MODE_PRIVATE)

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 64, 32, 24)
        }

        val title = TextView(this).apply {
            text = "手表中继"
            textSize = 20f
            setTypeface(typeface, Typeface.BOLD)
        }
        status = TextView(this).apply { textSize = 15f; setPadding(0, 8, 0, 12) }

        logView = TextView(this).apply {
            textSize = 13f
            typeface = Typeface.MONOSPACE
            setTextColor(Color.parseColor("#DDDDDD"))
            setBackgroundColor(Color.parseColor("#111111"))
            setPadding(16, 16, 16, 16)
            movementMethod = ScrollingMovementMethod()
        }
        scroller = ScrollView(this).apply {
            addView(logView)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
        }

        val hostEdit = EditText(this).apply {
            hint = "地址（IPv4 / IPv6 / 域名，留空=自动探测）"
            setText(prefs.getString("manual", ""))
            textSize = 13f
        }

        val startBtn = Button(this).apply { text = "启动中继" }
        val stopBtn = Button(this).apply { text = "停止" }
        val clearBtn = Button(this).apply { text = "清空日志" }
        val langBtn = Button(this).apply {
            text = if (State.english) "中" else "EN"
            setOnClickListener {
                State.english = !State.english
                text = if (State.english) "中" else "EN"
                getSharedPreferences("relay", MODE_PRIVATE).edit()
                    .putBoolean("english", State.english).apply()
            }
        }

        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            addView(startBtn, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(stopBtn, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(clearBtn, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(langBtn, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 0.7f))
        }

        val agentRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        for (name in listOf("hermes", "codex", "dsh")) {
            val b = Button(this).apply {
                text = name.uppercase()
                textSize = 12f
                setOnClickListener {
                    RelayService.switchAgent(name)
                    State.add("切换到后台 $name", "Switch backend to $name")
                }
            }
            agentRow.addView(b, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        }

        layout.addView(title)
        layout.addView(status)
        layout.addView(agentRow)
        layout.addView(row)
        layout.addView(hostEdit)
        layout.addView(scroller)
        setContentView(layout)

        State.english = prefs.getBoolean("english", false)

        requestPerms()

        /* opening the app is enough: start (or keep) the relay running */
        val manual = prefs.getString("manual", "") ?: ""
        startForegroundService(Intent(this, RelayService::class.java).putExtra("manual", manual))

        startBtn.setOnClickListener {
            val m = hostEdit.text.toString().trim()
            prefs.edit().putString("manual", m).apply()
            startForegroundService(Intent(this, RelayService::class.java).putExtra("manual", m))
            State.add("手动启动中继（地址：${m.ifBlank { "自动" }}）",
                      "Manual start (address: ${m.ifBlank { "auto" }})")
        }
        stopBtn.setOnClickListener {
            stopService(Intent(this, RelayService::class.java))
            State.add("已停止中继", "Relay stopped")
            State.ble = "已停止"; State.ws = "已停止"
        }
        clearBtn.setOnClickListener { State.clear() }
    }

    override fun onResume() { super.onResume(); handler.post(refresh) }
    override fun onPause() { super.onPause(); handler.removeCallbacks(refresh) }

    private fun requestPerms() {
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) {
            perms += Manifest.permission.BLUETOOTH_SCAN
            perms += Manifest.permission.BLUETOOTH_CONNECT
        }
        if (Build.VERSION.SDK_INT >= 33) perms += Manifest.permission.POST_NOTIFICATIONS
        val missing = perms.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), 1)
    }
}
