package com.astrix.phone

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.SweepGradient
import android.graphics.Typeface
import android.view.View

// Кольцо загрузки как на главной ZAVIRIA на ПК: дорожка, дуга в цветах логотипа (фиолетовый → синий), число в центре
class RingView(context: Context, private val ink: Int, private val track: Int) : View(context) {
    private var value = 0f
    private var shown = 0f
    private val density = resources.displayMetrics.density
    private val arc = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND; strokeWidth = 7 * density }
    private val back = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 7 * density; color = track }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = ink; textAlign = Paint.Align.CENTER; typeface = Typeface.DEFAULT_BOLD; textSize = 15 * density }
    private val box = RectF()

    fun set(percent: Double) { value = percent.toFloat().coerceIn(0f, 100f); invalidate() }

    override fun onDraw(canvas: Canvas) {
        val pad = arc.strokeWidth / 2 + density
        box.set(pad, pad, width - pad, height - pad)
        canvas.drawArc(box, 0f, 360f, false, back)
        shown += (value - shown) * 0.2f // плавно доезжает до нового значения
        if (shown > 0.5f) {
            val sweep = SweepGradient(width / 2f, height / 2f, intArrayOf(Color.rgb(150, 110, 255), Color.rgb(70, 130, 250), Color.rgb(150, 110, 255)), null)
            sweep.setLocalMatrix(Matrix().apply { setRotate(-90f, width / 2f, height / 2f) })
            arc.shader = sweep
            canvas.drawArc(box, -90f, 360f * shown / 100f, false, arc)
        }
        canvas.drawText("${value.toInt()}%", width / 2f, height / 2f - (text.descent() + text.ascent()) / 2, text)
        if (kotlin.math.abs(value - shown) > 0.3f) postInvalidateOnAnimation()
    }
}
