package com.example.msiscanner

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import java.util.concurrent.Executors

class MainActivity : ComponentActivity() {

    private lateinit var previewView: PreviewView
    private lateinit var resultView: TextView
    private lateinit var detailView: TextView
    private val analysisExecutor = Executors.newSingleThreadExecutor()
    private var checksumMode = ChecksumMode.AUTO
    private var lastText: String? = null
    private var lastShownAt = 0L

    private companion object {
        const val MIN_DIGITS = 7
        const val MAX_DIGITS = 10
    }

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                startCamera()
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildUi())
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            startCamera()
        } else {
            permissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    override fun onDestroy() {
        analysisExecutor.shutdown()
        super.onDestroy()
    }

    private fun buildUi(): FrameLayout {
        val root = FrameLayout(this)
        previewView = PreviewView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        root.addView(previewView)

        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 24, 32, 48)
            setBackgroundColor(Color.argb(170, 0, 0, 0))
        }

        val modeSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(
                this@MainActivity,
                android.R.layout.simple_spinner_dropdown_item,
                ChecksumMode.entries.map { it.label },
            )
            setSelection(ChecksumMode.entries.indexOf(checksumMode))
            onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
                override fun onItemSelected(
                    parent: android.widget.AdapterView<*>?,
                    view: android.view.View?,
                    position: Int,
                    id: Long,
                ) {
                    checksumMode = ChecksumMode.entries[position]
                }

                override fun onNothingSelected(parent: android.widget.AdapterView<*>?) = Unit
            }
        }
        panel.addView(modeSpinner)

        resultView = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 30f)
            gravity = Gravity.CENTER_HORIZONTAL
        }
        panel.addView(resultView)

        detailView = TextView(this).apply {
            setTextColor(Color.LTGRAY)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            gravity = Gravity.CENTER_HORIZONTAL
        }
        panel.addView(detailView)

        root.addView(panel, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM,
        ))
        return root
    }

    private fun startCamera() {
        val future = ProcessCameraProvider.getInstance(this)
        future.addListener({
            val provider = future.get()
            val preview = Preview.Builder().build().also {
                it.surfaceProvider = previewView.surfaceProvider
            }
            val analysis = ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .build()
            analysis.setAnalyzer(analysisExecutor) { proxy -> analyze(proxy) }
            provider.unbindAll()
            provider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis)
        }, ContextCompat.getMainExecutor(this))
    }

    private fun analyze(proxy: ImageProxy) {
        try {
            val plane = proxy.planes.getOrNull(0) ?: return
            val buffer = plane.buffer
            val result = MsiNative.decode(
                y = buffer,
                offset = buffer.position(),
                width = proxy.width,
                height = proxy.height,
                rowStride = plane.rowStride,
                mode = checksumMode,
                minDigits = MIN_DIGITS,
                maxDigits = MAX_DIGITS,
            ) ?: return
            val now = System.currentTimeMillis()
            if (result.text != lastText || now - lastShownAt > 1500L) {
                lastText = result.text
                lastShownAt = now
                runOnUiThread { showResult(result) }
            }
        } finally {
            proxy.close()
        }
    }

    private fun showResult(result: MsiScanResult) {
        resultView.text = result.text
        resultView.setTextColor(
            when (result.checksumStatus) {
                "valid" -> Color.rgb(80, 220, 120)
                "invalid" -> Color.rgb(255, 120, 120)
                else -> Color.WHITE
            }
        )
        detailView.text = when (result.checksumStatus) {
            "valid" -> "checksum valid \u00b7 ${result.meta}"
            "invalid" -> "checksum INVALID \u00b7 ${result.meta}"
            "n/a" -> "checksum not applicable \u00b7 ${result.meta}"
            else -> result.meta
        }
    }
}
