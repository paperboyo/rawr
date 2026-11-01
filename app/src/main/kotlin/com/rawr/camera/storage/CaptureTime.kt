package com.rawr.camera.storage

import androidx.annotation.Keep
import java.util.TimeZone

/** Field names are read by JNI and must survive release shrinking. */
@Keep
internal data class CaptureTime(val wallClockUnixMillis: Long, val utcOffsetMinutes: Int) {
    init {
        require(wallClockUnixMillis in 0..253402300799999L)
        require(utcOffsetMinutes in -1080..1080)
        require(wallClockUnixMillis / 1_000 + utcOffsetMinutes * 60L <= 253402300799L)
    }

    companion object {
        fun snapshot(
            wallClockUnixMillis: Long = System.currentTimeMillis(),
            timeZone: TimeZone = TimeZone.getDefault()
        ): CaptureTime = CaptureTime(wallClockUnixMillis, timeZone.getOffset(wallClockUnixMillis) / 60_000)
    }
}
