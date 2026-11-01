package com.rawr.camera.location

import androidx.annotation.Keep
import com.rawr.camera.location.LocationPolicy.MAX_AGE_NANOS
import java.util.concurrent.atomic.AtomicReference

/** Named fields are read by JNI; keep their names in minified builds. */
@Keep
@ConsistentCopyVisibility
data class CaptureGps internal constructor(
    val latitude: Double,
    val longitude: Double,
    val altitude: Double?,
    val accuracy: Double?,
    val fixTimeMillis: Long,
    val providerCode: Int = 0
) {
    override fun toString(): String = "CaptureGps"
}

internal enum class LocationPrecision { NONE, COARSE, FINE }

internal class Observation(val provider: String?, val measuredNanos: Long, val uncertainty: Double?, val gps: CaptureGps) {
    fun eligibleAt(nowNanos: Long): Boolean = nowNanos - measuredNanos <= MAX_AGE_NANOS
}

/** One immutable publication: local gates, last classified access, collection identity and one candidate. */
internal data class LocationState(
    val enabled: Boolean = false,
    val foreground: Boolean = false,
    val precision: LocationPrecision = LocationPrecision.NONE,
    val suppressed: Boolean = false,
    val identity: Long = 0,
    val candidate: Observation? = null
) {
    val usable: Boolean get() = enabled && precision != LocationPrecision.NONE && !suppressed

    fun admissible(nowNanos: Long): CaptureGps? = candidate?.takeIf { usable && it.eligibleAt(nowNanos) }?.gps

    fun missesRefreshTarget(nowNanos: Long): Boolean {
        val current = candidate?.takeIf { it.eligibleAt(nowNanos) } ?: return true
        if (nowNanos - current.measuredNanos > LocationPolicy.REFRESH_AGE_NANOS) return true
        if (precision != LocationPrecision.FINE) return false
        val uncertainty = current.uncertainty ?: return true
        return uncertainty > LocationPolicy.REFRESH_FINE_UNCERTAINTY_METERS
    }
}

/**
 * Single candidate: known accuracy first, then recency, then tolerated same-provider worsening.
 * Selection rule adapted from bjzhou/PhotonCamera (Apache-2.0), see THIRD_PARTY_NOTICES.md.
 */
internal fun acceptsObservation(current: Observation?, incoming: Observation, nowNanos: Long): Boolean {
    val incumbent = current?.takeIf { it.eligibleAt(nowNanos) } ?: return true
    val next = incoming.uncertainty
    val previous = incumbent.uncertainty
    if (next != null && (previous == null || next < previous)) return true
    if (incoming.measuredNanos <= incumbent.measuredNanos) return false
    if (next == previous) return true
    return next != null && previous != null && incoming.provider != null && incoming.provider == incumbent.provider &&
        next - previous <= LocationPolicy.SAME_PROVIDER_WORSENING_METERS
}

internal class GpsSnapshotStore {
    private val ref = AtomicReference(LocationState())
    val state: LocationState get() = ref.get()

    fun setEnabled(value: Boolean): Boolean = update {
        if (it.enabled == value) it else it.copy(enabled = value, identity = it.identity + 1,
            candidate = if (value) it.candidate else null)
    }

    fun setForeground(value: Boolean): Boolean = update {
        if (it.foreground == value) it else it.copy(foreground = value, identity = it.identity + 1)
    }

    /** A null [suppressed] keeps the last effective decision, used while no foreground check is possible. */
    fun classify(precision: LocationPrecision, suppressed: Boolean?) = update {
        val changed = precision != it.precision
        it.copy(precision = precision, suppressed = suppressed ?: it.suppressed,
            identity = if (changed) it.identity + 1 else it.identity,
            candidate = if (precision < it.precision) null else it.candidate)
    }

    fun suppress() = update { it.copy(suppressed = true) }

    /** Drops an expired candidate; returns the remaining original-age window of a retained one. */
    fun expire(nowNanos: Long): Long? {
        while (true) {
            val current = ref.get()
            val candidate = current.candidate ?: return null
            if (candidate.eligibleAt(nowNanos)) return MAX_AGE_NANOS - (nowNanos - candidate.measuredNanos)
            if (ref.compareAndSet(current, current.copy(candidate = null))) return null
        }
    }

    fun offer(identity: Long, provider: String?, measuredNanos: Long, nowNanos: Long, latitude: Double, longitude: Double,
              altitude: Double?, uncertainty: Double?, fixTimeMillis: Long): Boolean {
        if (measuredNanos < 0 || measuredNanos > nowNanos || nowNanos - measuredNanos > MAX_AGE_NANOS) return false
        if (!latitude.isFinite() || latitude !in -90.0..90.0 || !longitude.isFinite() || longitude !in -180.0..180.0) return false
        if (uncertainty != null && (!uncertainty.isFinite() || uncertainty < 0.0)) return false
        val incoming = Observation(provider, measuredNanos, uncertainty,
            CaptureGps(latitude, longitude, altitude?.takeIf { it.isFinite() }, uncertainty, fixTimeMillis,
                when (provider) { "gps" -> 1; "network" -> 2; "fused" -> 3; else -> 0 }))
        while (true) {
            val current = ref.get()
            if (!current.enabled || current.precision == LocationPrecision.NONE || current.identity != identity ||
                !acceptsObservation(current.candidate, incoming, nowNanos)) return false
            if (ref.compareAndSet(current, current.copy(candidate = incoming))) return true
        }
    }

    private inline fun update(transform: (LocationState) -> LocationState): Boolean {
        while (true) {
            val current = ref.get()
            val next = transform(current)
            if (next == current) return false
            if (ref.compareAndSet(current, next)) return true
        }
    }
}
