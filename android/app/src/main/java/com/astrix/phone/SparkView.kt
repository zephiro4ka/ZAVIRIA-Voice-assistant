package com.astrix.phone

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Shader
import android.view.View

// Мини-график последних замеров: плавная линия и мягкая заливка под ней
class SparkView(context: Context, private val color: Int) : View(context) {
    private var values: List<Float> = emptyList()
    private var top = 100f
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.2f * resources.displayMetrics.density; strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND; this.color = color }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val dot = Paint(Paint.ANTI_ALIAS_FLAG).apply { this.color = color }

    /** [max] — верх шкалы; null — по самому большому значению (для сети). */
    fun set(history: List<Float>, max: Float? = 100f) {
        values = history
        top = max ?: ((history.maxOrNull() ?: 1f) * 1.2f).coerceAtLeast(1f)
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (values.size < 2) return
        val w = width.toFloat(); val h = height.toFloat()
        val pad = line.strokeWidth
        val step = w / (values.size - 1)
        fun yOf(v: Float) = h - pad - (v.coerceIn(0f, top) / top) * (h - pad * 2)
        val path = Path()
        var px = 0f; var py = yOf(values[0])
        path.moveTo(px, py)
        for (i in 1 until values.size) { // сглаживание: кривая через середины отрезков
            val x = i * step; val y = yOf(values[i])
            path.quadTo(px, py, (px + x) / 2, (py + y) / 2)
            px = x; py = y
        }
        path.lineTo(px, py)
        val area = Path(path).apply { lineTo(w, h); lineTo(0f, h); close() }
        fill.shader = LinearGradient(0f, 0f, 0f, h, Color.argb(110, Color.red(color), Color.green(color), Color.blue(color)), Color.argb(0, Color.red(color), Color.green(color), Color.blue(color)), Shader.TileMode.CLAMP)
        canvas.drawPath(area, fill)
        canvas.drawPath(path, line)
        canvas.drawCircle(px - pad / 2, py, line.strokeWidth * 1.6f, dot)
    }
}
