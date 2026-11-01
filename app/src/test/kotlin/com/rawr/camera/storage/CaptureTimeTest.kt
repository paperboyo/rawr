package com.rawr.camera.storage

import java.time.Instant
import java.util.TimeZone
import org.junit.Assert.*
import org.junit.Test

class CaptureTimeTest {
    @Test fun offsetIsResolvedAtTheCapturedInstantIncludingDaylightSaving() {
        val zone = TimeZone.getTimeZone("America/New_York")
        val winter = CaptureTime.snapshot(Instant.parse("2026-01-01T12:00:00Z").toEpochMilli(), zone)
        val summer = CaptureTime.snapshot(Instant.parse("2026-07-01T12:00:00Z").toEpochMilli(), zone)
        assertEquals(-300, winter.utcOffsetMinutes)
        assertEquals(-240, summer.utcOffsetMinutes)
    }

    @Test fun snapshotAndFileNameIgnoreLaterTimeZoneChanges() {
        val original = TimeZone.getDefault()
        try {
            TimeZone.setDefault(TimeZone.getTimeZone("GMT+05:45"))
            val instant = Instant.parse("2026-05-01T04:34:56.042Z").toEpochMilli()
            val snapshot = CaptureTime.snapshot(instant)
            TimeZone.setDefault(TimeZone.getTimeZone("GMT-07:00"))
            assertEquals(345, snapshot.utcOffsetMinutes)
            assertEquals(instant, snapshot.wallClockUnixMillis)
            assertEquals("RAWR_20260501_101956_042", CaptureFileNames.baseName(snapshot))
        } finally {
            TimeZone.setDefault(original)
        }
    }
}
