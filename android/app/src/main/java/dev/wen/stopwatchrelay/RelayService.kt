package dev.wen.stopwatchrelay

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.bluetooth.le.ScanSettings
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import java.security.SecureRandom
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager
import java.util.UUID

class RelayService : Service() {

    companion object {
        private fun L(zh: String, en: String) {
            Log.i("RelayService", en)
            State.add(zh, en)
        }
        @Volatile private var instance: RelayService? = null

        /** Called from the UI: ask the bridge to switch backend agent. */
        fun switchAgent(name: String) {
            instance?.ws?.send("{\"cmd\":\"agent\",\"name\":\"$name\"}")
        }

        private const val TAG = "RelayService"
        private const val DEVICE_NAME = "StopWatch-BLE"

        private val SVC = UUID.fromString("4f5a0001-1e3b-4d8a-9c7e-5a1b2c3d4e5f")
        private val CTRL = UUID.fromString("4f5a0002-1e3b-4d8a-9c7e-5a1b2c3d4e5f")
        private val AUDIO = UUID.fromString("4f5a0003-1e3b-4d8a-9c7e-5a1b2c3d4e5f")
        private val STATUS = UUID.fromString("4f5a0004-1e3b-4d8a-9c7e-5a1b2c3d4e5f")
        private val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val handler = Handler(Looper.getMainLooper())
    /* the bridge uses a self-signed certificate: trust it for this LAN link */
    private val trustAll = object : X509TrustManager {
        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
        override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
    }
    private val http: OkHttpClient = run {
        val ctx = SSLContext.getInstance("TLS")
        ctx.init(null, arrayOf(trustAll), SecureRandom())
        OkHttpClient.Builder()
            .connectTimeout(4, java.util.concurrent.TimeUnit.SECONDS)
            .sslSocketFactory(ctx.socketFactory, trustAll)
            .hostnameVerifier { _, _ -> true }
            .build()
    }

    private var host = ""
    /* try in order: home LAN, Tailscale (works anywhere), adb reverse tunnel */
    /* full URLs, tried in order:
       1) home LAN  2) public IPv6 (works on mobile data, no VPN)
       3) Tailscale  4) adb reverse tunnel */
    private var candidates: List<String> = emptyList()

    /** Turn whatever the user typed into a ws/wss URL.
     *  - full URL (ws:// wss:// http:// https://) is honoured as-is
     *  - bare host        -> wss://host:8443/ws
     *  - host:port        -> wss://host:port/ws
     *  - raw IPv6         -> wss://[v6]:8443/ws
     *  - [v6]:port        -> wss://[v6]:port/ws
     *  The transport therefore follows the address, never the app. */
    private fun toUrl(raw: String): String? {
        var s = raw.trim()
        if (s.isEmpty()) return null
        s = when {
            s.startsWith("wss://") -> s
            s.startsWith("ws://") -> s
            s.startsWith("https://") -> "wss://" + s.removePrefix("https://")
            s.startsWith("http://") -> "ws://" + s.removePrefix("http://")
            else -> {
                val (hostPart, port) = when {
                    s.startsWith("[") -> {
                        val close = s.indexOf(']')
                        val h = s.substring(0, close + 1)
                        val p = if (close + 1 < s.length && s[close + 1] == ':') s.substring(close + 2) else "8443"
                        h to p
                    }
                    s.count { it == ':' } > 1 -> "[$s]" to "8443"          // raw IPv6
                    s.count { it == ':' } == 1 -> s.substringBefore(':') to s.substringAfter(':')
                    else -> s to "8443"
                }
                "wss://$hostPart:$port/ws"
            }
        }
        if (!s.endsWith("/ws")) s = s.trimEnd('/') + "/ws"
        return s
    }

    /** true when the phone is currently on Wi-Fi (so the home LAN address can work) */
    private fun onWifi(): Boolean {
        return try {
            val cm = getSystemService(CONNECTIVITY_SERVICE) as ConnectivityManager
            val net = cm.activeNetwork
            net != null && cm.getNetworkCapabilities(net)
                ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        } catch (_: Exception) {
            false
        }
    }

    private fun buildCandidates(manual: String?): List<String> {
        val explicit = toUrl(manual ?: "")
        if (explicit != null) return listOf(explicit)   /* user decides: no fallback */
        return buildList {
            val lan = prefs.getString("lan", "") ?: ""      /* e.g. 192.168.1.10:8443, set by the user */
            if (onWifi() && lan.isNotBlank()) add(toUrl(lan) ?: return@buildList)
            val ddns = prefs.getString("ddns", "") ?: ""    /* e.g. watch.example.com */
            if (ddns.isNotBlank()) add(toUrl(ddns) ?: return@buildList)
            add("wss://127.0.0.1:8443/ws")               /* adb reverse */
        }
    }
    private var candidateIndex = 0
    private var gatt: BluetoothGatt? = null
    private var audioChar: BluetoothGattCharacteristic? = null
    private var statusChar: BluetoothGattCharacteristic? = null
    private var ctrlChar: BluetoothGattCharacteristic? = null
    private var ws: WebSocket? = null
    private var scanning = false
    private var scanAttempts = 0
    @Volatile private var watchdog: Thread? = null
    private var connecting = false
    private var connected = false
    private var talking = false
    private var dlSeq = 0
    @Volatile private var downlinkCodec = 1
    @Volatile private var activeColor = 0
    @Volatile private var rememberingMac: String? = null
    private val prefs by lazy { getSharedPreferences("relay", MODE_PRIVATE) }
    private var batteryBuf: StringBuilder? = null
    @Volatile private var lastAudioAt = 0L
    @Volatile private var playingUntil = 0L

    private fun voiceBusy(): Boolean =
        System.currentTimeMillis() - lastAudioAt < 1500 ||
        System.currentTimeMillis() < playingUntil

    /** Pull the watch's battery ring (control 0x40) and forward it as CSV.
     *  The firmware only services control commands when the voice channel is
     *  idle, so retry until it is. */
    private fun readBatteryLog(attempt: Int = 0) {
        val ctrl = ctrlChar ?: return
        if (voiceBusy() && attempt < 40) {
            handler.postDelayed({ readBatteryLog(attempt + 1) }, 2000)
            return
        }
        batteryBuf = StringBuilder()
        val started = System.currentTimeMillis()
        val okWrite = writeWithResponse(ctrl, byteArrayOf(0x40))
        L("电池导出：ctrl=${ctrl.uuid.toString().take(8)} write=$okWrite",
          "Battery export: write=$okWrite")
        handler.postDelayed({
            val csv = batteryBuf?.toString() ?: ""
            batteryBuf = null
            ws?.send("{\"type\":\"battery_csv\",\"data\":${org.json.JSONObject.quote(csv)}}")
            L("电力日志已导出（${csv.length} 字符）", "Battery log exported (${csv.length} chars)")
        }, 26000)   // firmware streams the whole ring at 15 ms/line (~22 s for 1440)
    }
    private var startLogged = false
    private var audioCount = 0
    private var lastFrameAt = 0L
    private var running = false

    override fun onBind(intent: Intent?): IBinder? = null

    private val keepAlive = object : Runnable {
        override fun run() {
            if (!running) return
            if (ws == null) connectWs()
            if (gatt == null) startScan()
            handler.postDelayed(this, 30_000)
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        startForeground(1, buildNotification("启动中"))
        handler.postDelayed(keepAlive, 30_000)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        host = intent?.getStringExtra("host") ?: host
        val manual = intent?.getStringExtra("manual") ?: ""
        candidates = buildCandidates(manual)
        candidateIndex = 0
        if (!startLogged) {
            startLogged = true
            L("服务启动（手动地址：${manual.ifBlank { "自动" }}）",
              "Service started (manual: ${manual.ifBlank { "auto" }})")
        }
        if (!running) {
            running = true
            connectWs()
            if (!autoReconnect()) startScan()
            startScanWatchdog()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        instance = null
        running = false
        handler.removeCallbacksAndMessages(null)
        try { ws?.close(1000, "stop") } catch (_: Exception) {}
        try { gatt?.disconnect(); gatt?.close() } catch (_: Exception) {}
        super.onDestroy()
    }

    /* ----------------------------- WebSocket ----------------------------- */

    private fun connectWs() {
        if (!running) return
        val target = candidates[candidateIndex]
        State.ws = "连接中…"
        val req = Request.Builder().url(target).build()
        ws = http.newWebSocket(req, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                State.ws = "已连接"
                L("中继已连接：$target", "Relay connected: $target")
                webSocket.send("{\"cmd\":\"agent_status\"}")
                
                
                notify("中继已连接")
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                // downlink ADPCM frame -> watch
                val c = audioChar ?: return
                val payload = bytes.toByteArray()
                val pkt = ByteArray(4 + payload.size)
                pkt[0] = (dlSeq and 0xFF).toByte()
                pkt[1] = ((dlSeq shr 8) and 0xFF).toByte()
                pkt[2] = downlinkCodec.toByte()
                pkt[3] = 0
                System.arraycopy(payload, 0, pkt, 4, payload.size)
                dlSeq = (dlSeq + 1) and 0xFFFF
                write(c, pkt)
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                
                try {
                    val m = org.json.JSONObject(text)
                    if (m.optString("type") == "codec") {
                        downlinkCodec = m.optInt("id", 1)
                        L("下行编码：${if (downlinkCodec == 2) "Opus 32k" else "ADPCM"}",
                          "Downlink codec: ${if (downlinkCodec == 2) "Opus 32k" else "ADPCM"}")
                        return
                    }
                    if (m.optString("cmd") == "battery") {
                        readBatteryLog()
                        return
                    }
                    when (m.optString("type")) {
                        "agent" -> {
                            val active = m.optString("active")
                            L("后台已切换为 $active", "Backend switched to $active")
                            activeAgent = active
                            /* colour comes from agents.json; the app stays backend-agnostic */
                            if (m.has("color")) activeColor = m.optInt("color", 0)
                            /* B-key switches change the colour on the watch itself
                               (instant); only other sources need an explicit write */
                            if (m.optString("source") != "bkey") sendAgentColor(activeColor)
                        }
                        "asr" -> L("识别：${m.optString("text")}", "Transcript: ${m.optString("text")}")
                        "reply" -> L("回复：${m.optString("text")}", "Reply: ${m.optString("text")}")
                        "speaking" -> L("手表播放中…", "Watch playing…")
                        "done" -> L("播放完毕 ✅", "Playback finished ✅")
                        "saved" -> L("已保存录音 ${m.optDouble("seconds")}s", "Recording saved ${m.optDouble("seconds")}s")
                    }
                } catch (_: Exception) {}
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                State.ws = "断开: ${t.message}"
                L("中继连接失败（$target），换下一个地址", "Relay failed ($target), trying next address")
                candidateIndex = (candidateIndex + 1) % candidates.size
                L("切换地址：${candidates[candidateIndex]}", "Next address: ${candidates[candidateIndex]}")
                if (running) handler.postDelayed({ connectWs() }, 3000)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                State.ws = "已关闭"
                if (running) handler.postDelayed({ connectWs() }, 3000)
            }
        })
    }

    /* -------------------------------- BLE -------------------------------- */

    private fun startScan() {
        if (!running) return
        val mgr = getSystemService(BLUETOOTH_SERVICE) as BluetoothManager
        val adapter: BluetoothAdapter = mgr.adapter ?: run { State.ble = "无蓝牙"; return }
        if (!adapter.isEnabled) { State.ble = "蓝牙未开"; return }
        if (scanning) return
        scanning = true
        State.ble = "搜索中…"
        L("搜索手表中…", "Scanning for watch…")
        scanAttempts++
        /* After a couple of misses, drop the name filter: some Android stacks
           stop matching scan-response names until Bluetooth is toggled. */
        val unfiltered = scanAttempts > 2
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        try {
            if (unfiltered) {
                L("改用无过滤扫描（第 $scanAttempts 次）", "Unfiltered scan (attempt $scanAttempts)")
                val cb = newScanCallback(); activeScanCb = cb
                adapter.bluetoothLeScanner.startScan(null, settings, cb)
            } else {
                val filter = ScanFilter.Builder().setDeviceName(DEVICE_NAME).build()
                val cb = newScanCallback(); activeScanCb = cb
                adapter.bluetoothLeScanner.startScan(listOf(filter), settings, cb)
            }
        } catch (e: Exception) {
            State.ble = "扫描失败：${e.javaClass.simpleName}"
            L("扫描异常：${e.javaClass.simpleName} ${e.message ?: ""}", "Scan error: ${e.message}")
            scanning = false
            scheduleScanRetry()
            return
        }
        handler.postDelayed({
            stopScan()
            /* keep scanning until the watch shows up (it may still be booting) */
            if (gatt == null || !connected) scheduleScanRetry()
        }, 10000)
    }

    /** Always comes back: a dead scan loop used to need an app restart. */
    /** Independent watchdog: Handler callbacks can die silently after a BLE
     *  disconnect, which used to leave the app searching forever. */
    private fun startScanWatchdog() {
        if (watchdog != null) return
        watchdog = Thread {
            var lastScanMs = 0L
            while (running) {
                try {
                    Thread.sleep(3000)
                    val now = System.currentTimeMillis()
                    if (!connected && !connecting) {
                        if (!scanning) {
                            handler.post { startScan() }
                            lastScanMs = now
                        } else if (now - lastScanMs > 14000) {
                            // a scan that never reports anything is wedged
                            handler.post { stopScan(); startScan() }
                            lastScanMs = now
                        }
                    } else {
                        lastScanMs = now
                    }
                } catch (_: InterruptedException) {
                    break
                } catch (e: Exception) {
                    Log.w("RelayService", "watchdog: ${e.message}")
                }
            }
        }.apply { isDaemon = true; start() }
    }

    private fun scheduleScanRetry() {
        handler.postDelayed({ if (running) startScan() }, 3000)
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        try {
            activeScanCb?.let { cb ->
                (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter?.bluetoothLeScanner?.stopScan(cb)
            }
            activeScanCb = null
        } catch (_: Exception) {}
    }

    @Volatile private var activeScanCb: ScanCallback? = null

    private fun newScanCallback(): ScanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (connecting) return          /* one candidate is enough */
            val name = result.scanRecord?.deviceName ?: result.device.name
            if (scanAttempts > 2 && name != DEVICE_NAME && result.device.name != DEVICE_NAME) return
            connecting = true
            L("发现手表 ${result.device.address}", "Watch found ${result.device.address}")
            stopScan()
            connect(result.device)
        }
        override fun onScanFailed(errorCode: Int) {
            State.ble = "扫描失败 $errorCode"
            scanning = false
        }
    }

    private fun connect(device: BluetoothDevice) {
        State.ble = "连接中…"
        try {
            gatt?.close()
            gatt = device.connectGatt(this, false, gattCb, BluetoothDevice.TRANSPORT_LE)
            rememberingMac = device.address
            prefs.edit().putString("mac", device.address).apply()
        } catch (e: SecurityException) {
            State.ble = "连接异常"
        }
    }

    /** Known watch? Let Android hold a background connection. This survives the
     *  watch rebooting, which the flaky scan path never did reliably. */
    private fun autoReconnect(): Boolean {
        val mac = rememberingMac ?: prefs.getString("mac", null) ?: return false
        val adapter = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter ?: return false
        if (!adapter.isEnabled) return false
        return try {
            val dev = adapter.getRemoteDevice(mac)
            State.ble = "后台连接中…"
            L("按 MAC 后台重连 $mac", "Auto-reconnecting to $mac")
            gatt?.close()
            gatt = dev.connectGatt(this, true, gattCb, BluetoothDevice.TRANSPORT_LE)
            true
        } catch (e: Exception) {
            L("自动重连失败：${e.javaClass.simpleName}", "Auto-reconnect failed: ${e.message}")
            false
        }
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                State.ble = "已连接，发现服务…"
                connecting = false
                connected = true
                scanAttempts = 0
                L("蓝牙已连上", "Bluetooth connected")
                try { g.discoverServices() } catch (_: SecurityException) {}
            } else {
                State.ble = "未连接（重试中）"
                L("蓝牙断开，2 秒后重连", "Bluetooth lost, retry in 2s")
                connecting = false
                if (running) handler.postDelayed({ startScan() }, 2000)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(SVC) ?: run { State.ble = "服务未找到"; return }
            audioChar = svc.getCharacteristic(AUDIO)
            statusChar = svc.getCharacteristic(STATUS)
            ctrlChar = svc.getCharacteristic(CTRL)
            try {
                g.requestMtu(400)
            } catch (_: SecurityException) {}
            enableNotify(g, audioChar)
            enableNotify(g, statusChar)
            State.ble = "已连接"
            L("手表服务就绪（音频=${audioChar != null} 状态=${statusChar != null}）", "Watch services ready (audio=${audioChar != null}, status=${statusChar != null})")
            applyCurrentAgentColour()
            notify("手表已连接")
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            L("MTU=$mtu", "MTU=$mtu")
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray) {
            handle(g, ch.uuid, value)
        }

        @Deprecated("old API")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            handle(g, ch.uuid, ch.value ?: return)
        }
    }

    private fun handle(g: BluetoothGatt, uuid: UUID, data: ByteArray) {
        if (uuid == STATUS) {
            if (data.isEmpty()) return
            L("状态事件 0x%02X".format(data[0].toInt() and 0xFF), "Status event 0x%02X".format(data[0].toInt() and 0xFF))
            when (data[0].toInt() and 0xFF) {
                0x10 -> {
                    talking = true
                    ws?.send("{\"cmd\":\"start\"}")
                    
                    notify("录音中…")
                }
                0x11 -> {
                    talking = false
                    ws?.send("{\"cmd\":\"stop\"}")
                    
                    notify("等待回答…")
                }
                0x20 -> {
                    L("B 键：切换后台", "B key: cycle backend")
                    ws?.send("{\"cmd\":\"agent_next\"}")
                }
                0x12 -> {
                    playingUntil = System.currentTimeMillis() + 8000
                    L("手表播放中…", "Watch playing…")
                }
                0x13 -> L("播放完毕 ✅", "Playback finished ✅")
            }
            return
        }
        if (uuid == AUDIO && data.size >= 4 && (data[2].toInt() and 0xFF) == 0x7F) {
            if (batteryBuf != null) {
                batteryBuf!!.append(String(data, 4, data.size - 4, Charsets.UTF_8))
            }
            return
        }
        if (uuid == AUDIO && data.size >= 4 && (data[2].toInt() and 0xFF) == 0x7E) {
            when (data[3].toInt() and 0xFF) {
                0x13 -> {
                    L("播放被打断，通知电脑停止发送", "Playback interrupted, stopping the bridge")
                    playingUntil = 0
                    ws?.send("{\"cmd\":\"stop_play\"}")
                }
                0x20 -> {
                    L("B 键：切换后台", "B key: cycle backend")
                    ws?.send("{\"cmd\":\"agent_next\"}")
                }
            }
            return
        }
        if (uuid == AUDIO && data.size > 4 && (data[2].toInt() and 0xFF) == 1) {
            audioCount++
            lastFrameAt = System.currentTimeMillis()
            lastAudioAt = System.currentTimeMillis()
            if (audioCount % 50 == 1) L("发送中… 已传 $audioCount 帧", "Sending… $audioCount frames")
            /* if the status notification was missed, the first audio frame is
               also a valid "talk started" signal */
            if (!talking) {
                talking = true
                ws?.send("{\"cmd\":\"start\"}")
                L("开始录音（收到第一帧音频）", "Recording started (first audio frame)")
                startStopWatchdog()
            }
            ws?.send(ByteString.of(*data.copyOfRange(4, data.size)))
        }
    }

    /* the status notification is unreliable on this phone, so end the session
       when audio frames stop arriving */
    private fun startStopWatchdog() {
        handler.postDelayed(object : Runnable {
            override fun run() {
                if (!talking) return
                val idle = System.currentTimeMillis() - lastFrameAt
                if (idle > 900) {
                    talking = false
                    ws?.send("{\"cmd\":\"stop\"}")
                    L("录音结束，等待回复…", "Recording done, waiting for reply…")
                } else {
                    handler.postDelayed(this, 250)
                }
            }
        }, 250)
    }

    var activeAgent: String = "hermes"

    private fun applyCurrentAgentColour() = sendAgentColor(activeColor)

    /* Colour commands are idempotent, so redundancy beats acknowledgement:
       send the same value a few times instead of tracking GATT callbacks
       (a lost callback used to block every later colour). */
    private fun sendAgentColor(idx: Int) {
        for (delay in longArrayOf(0, 350, 800, 1400)) {
            handler.postDelayed({
                val c = ctrlChar ?: return@postDelayed
                writeNoRsp(c, byteArrayOf(0x50, idx.toByte()))
            }, delay)
        }
    }

    private fun writeWithResponse(ch: BluetoothGattCharacteristic, data: ByteArray): Boolean {
        val g = gatt ?: return false
        return try {
            if (Build.VERSION.SDK_INT >= 33) {
                // API 33+ returns a status code, not a boolean
                g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == 0
            } else {
                @Suppress("DEPRECATION")
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                ch.value = data
                @Suppress("DEPRECATION")
                return g.writeCharacteristic(ch)
            }
        } catch (_: SecurityException) {
            false
        }
    }

    private fun writeNoRsp(ch: BluetoothGattCharacteristic, data: ByteArray) {
        val g = gatt ?: return
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
            } else {
                @Suppress("DEPRECATION")
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                @Suppress("DEPRECATION")
                ch.value = data
                @Suppress("DEPRECATION")
                g.writeCharacteristic(ch)
            }
        } catch (_: SecurityException) {
        }
    }

    private fun enableNotify(g: BluetoothGatt, ch: BluetoothGattCharacteristic?) {
        ch ?: return
        try {
            g.setCharacteristicNotification(ch, true)
            val d = ch.getDescriptor(CCCD) ?: return
            if (Build.VERSION.SDK_INT >= 33) {
                if (ch.uuid == AUDIO) L("订阅通知：音频", "Subscribed: audio")
                else L("订阅通知：状态", "Subscribed: status")
                g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(d)
            }
        } catch (e: SecurityException) {
            State.ble = "订阅失败"
        }
    }

    private fun write(ch: BluetoothGattCharacteristic, data: ByteArray) {
        val g = gatt ?: return
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
            } else {
                @Suppress("DEPRECATION")
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                @Suppress("DEPRECATION")
                ch.value = data
                @Suppress("DEPRECATION")
                g.writeCharacteristic(ch)
            }
        } catch (_: SecurityException) {}
    }

    /* ---------------------------- notification ---------------------------- */

    private fun buildNotification(text: String): Notification {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(NotificationChannel("relay", "手表中继", NotificationManager.IMPORTANCE_LOW))
        }
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, "relay") else Notification.Builder(this)
        return b.setContentTitle("手表中继运行中")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()
    }

    private fun notify(text: String) {
        try {
            val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
            nm.notify(1, buildNotification(text))
        } catch (_: Exception) {}
    }
}
