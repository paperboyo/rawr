package com.rawr.camera.location

import org.junit.Assert.*
import org.junit.Test

class GpsSnapshotStoreTest {
    private val second = 1_000_000_000L

    private fun observation(provider: String?, seconds: Long, uncertainty: Double?) =
        Observation(provider, seconds * second, uncertainty, CaptureGps(0.0, 0.0, null, uncertainty, 1L))

    private fun ready(precision: LocationPrecision = LocationPrecision.FINE) = GpsSnapshotStore().apply {
        setEnabled(true)
        setForeground(true)
        classify(precision, false)
    }

    private fun GpsSnapshotStore.offer(seconds: Long, now: Long, uncertainty: Double?, provider: String? = "gps",
                                       latitude: Double = 0.0) =
        offer(state.identity, provider, seconds * second, now * second, latitude, 0.0, null, uncertainty, 1L)

    @Test fun selectionPrefersKnownAccuracyThenRecencyThenSameProviderTolerance() {
        data class Case(val name: String, val current: Observation, val incoming: Observation, val accepts: Boolean)
        val now = 200L * second
        for (case in listOf(
            Case("smaller known wins even when older", observation("network", 100, 20.0), observation("gps", 90, 5.0), true),
            Case("known outranks unknown", observation("gps", 100, null), observation("network", 90, 900.0), true),
            Case("unknown never displaces known", observation("gps", 90, 900.0), observation("gps", 100, null), false),
            Case("newer equal known", observation("gps", 90, 5.0), observation("network", 100, 5.0), true),
            Case("newer both unknown", observation("gps", 90, null), observation("network", 100, null), true),
            Case("equal time needs better accuracy", observation("gps", 100, 5.0), observation("gps", 100, 5.0), false),
            Case("equal time better accuracy", observation("gps", 100, 5.0), observation("network", 100, 4.0), true),
            Case("same provider 200 m boundary", observation("fused", 90, 5.0), observation("fused", 100, 205.0), true),
            Case("same provider beyond 200 m", observation("fused", 90, 5.0), observation("fused", 100, 205.1), false),
            Case("different provider worse", observation("gps", 90, 5.0), observation("network", 100, 6.0), false),
            Case("older worse", observation("gps", 100, 5.0), observation("gps", 90, 6.0), false),
            Case("expired incumbent cannot block", observation("gps", 79, 1.0), observation("network", 199, 1000.0), true)
        )) assertEquals(case.name, case.accepts, acceptsObservation(case.current, case.incoming, now))
        assertTrue(acceptsObservation(null, observation(null, 100, null), now))
    }

    @Test fun singleCandidateIntentionallyForgetsRejectedAlternative() {
        val store = ready()
        assertTrue(store.offer(0, 0, 5.0))
        assertFalse(store.offer(110, 110, 1000.0, provider = "network"))
        assertNotNull(store.state.admissible(120 * second))
        assertNull("The rejected fresher alternative is not retained", store.state.admissible(121 * second))
    }

    @Test fun offerValidatesBeforeSelectionAndPublishesOnlyCurrentIdentity() {
        val store = ready()
        assertFalse("future", store.offer(11, 10, 5.0))
        assertFalse("negative", store.offer(-1, 10, 5.0))
        assertFalse("too old", store.offer(0, 121, 5.0))
        assertFalse("latitude", store.offer(10, 10, 5.0, latitude = 90.1))
        assertFalse("NaN latitude", store.offer(10, 10, 5.0, latitude = Double.NaN))
        for (uncertainty in listOf(-0.1, Double.NaN, Double.POSITIVE_INFINITY)) assertFalse(store.offer(10, 10, uncertainty))
        assertNull(store.state.candidate)
        assertTrue("zero is known, not missing", store.offer(0, 120, 0.0))
        val stale = store.state.identity
        store.setForeground(false)
        assertFalse(store.offer(stale, "gps", 119 * second, 120 * second, 1.0, 1.0, null, null, 1L))
        assertNotNull("Pause retains the original-aged candidate", store.state.candidate)
        store.setForeground(true)
        store.classify(LocationPrecision.COARSE, false)
        assertNull("A classified downgrade clears precise data", store.state.candidate)
        assertNotEquals(stale, store.state.identity)
        assertTrue(store.offer(119, 120, 2000.0))
        store.classify(LocationPrecision.COARSE, true)
        assertNull("Effective denial suppresses use", store.state.admissible(120 * second))
        store.classify(LocationPrecision.COARSE, false)
        assertNotNull("Restoration reuses the retained candidate", store.state.admissible(120 * second))
        store.setEnabled(false)
        assertNull(store.state.candidate)
        assertFalse(store.offer(120, 120, 1.0))
    }

    @Test fun refreshTargetAndExpiryUseOriginalAge() {
        val fine = ready()
        fine.offer(0, 0, 50.0)
        assertFalse(fine.state.missesRefreshTarget(30 * second))
        assertTrue(fine.state.missesRefreshTarget(30 * second + 1))
        fine.offer(1, 1, 50.1)
        assertTrue("Fine target needs known uncertainty within 50 m", fine.state.missesRefreshTarget(1 * second))
        val coarse = ready(LocationPrecision.COARSE)
        coarse.offer(0, 0, 2000.0)
        assertFalse("Approximate uncertainty alone does not miss the coarse target", coarse.state.missesRefreshTarget(30 * second))
        assertTrue("Unknown uncertainty misses the fine target", ready().apply { offer(0, 0, null) }.state.missesRefreshTarget(0))
        assertEquals(20 * second, coarse.expire(100 * second))
        assertNotNull(coarse.state.candidate)
        assertNull(coarse.expire(120 * second + 1))
        assertNull(coarse.state.candidate)
    }

    @Test fun selectedProviderTravelsWithFrozenRecord() {
        for ((provider, code) in listOf("gps" to 1, "network" to 2, "fused" to 3,
            "passive" to 0, "custom" to 0, "" to 0, null to 0)) {
            val store = ready()
            assertTrue(store.offer(10, 10, 5.0, provider))
            val admitted = requireNotNull(store.state.admissible(10 * second))
            assertFalse(store.offer(11, 11, 1000.0, "other"))
            store.setEnabled(false)
            assertEquals(provider, code, admitted.providerCode)
        }
    }

    @Test fun eligibilityAndFrozenRecord() {
        val store = ready()
        val now = 100 * second
        assertTrue(store.offer(store.state.identity, "gps", now, now, 0.0, 0.0, 0.0, null, 1793059200123L))
        val admitted = requireNotNull(store.state.admissible(now))
        assertSame(admitted, store.state.admissible(now + 120 * second))
        assertNull(store.state.admissible(now + 120 * second + 1))
        store.setEnabled(false)
        assertNull(store.state.admissible(now))
        assertEquals(0.0, admitted.latitude, 0.0)
        assertEquals(0.0, admitted.longitude, 0.0)
        assertEquals(0.0, requireNotNull(admitted.altitude), 0.0)
        assertNull(admitted.accuracy)
        assertEquals(1793059200123L, admitted.fixTimeMillis)
        assertEquals("CaptureGps", admitted.toString())
    }
}
