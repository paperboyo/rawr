# Renderer editing pipeline

## Source Location

JPEG development and DNG recompression/downscaling preserve supported GPS from
one coherent source directory, independently of live Location Tagging. They never
use the phone's current location. Supported fields are coordinates/references,
datum, altitude/reference, UTC fix date/time, horizontal error, processing method,
speed/units and travel track/reference. Source rational pairs and EXIF version
context are retained; legacy altitude is not guessed or converted. Ambiguous
records are omitted, and malformed optional groups do not prevent image processing.

Live capture GPS crosses JNI as the immutable `CaptureGps` record with named
fields; `@Keep` preserves the native field contract in minified builds and its
text representation omits location values.

JPEG EXIF must fit the 65,533-byte APP1 payload limit. Oversized metadata fails
the JPEG output; optional text and GPS are not silently trimmed to make it fit.

Recent Rawr DNGs are the primary workflow. No extra media-location permission or
preservation control is added. External providers can supply redacted bytes, which
Rawr cannot restore. Owned-media access and actual phone exports still require
device acceptance testing; host tests cover the native preservation contract.

## Recipes and resolution

A new imported DNG starts with a snapshot of the user's photo settings and look.
Its embedded capture recipe remains available as the original. An unfinished
capture starts with its saved recipe. Existing Renderer jobs restore their own
draft; edits and resets have no preferences writer attached. Inactive tone modes,
AgX edits, LUT profiles, and film presets are serialized with the job.

200MP sources are reduced to a Bayer image at half width and half height before
any demosaic, for all output resolutions. The default and maximum output is the
resulting approximately 50MP image. Native 25MP and 12.5MP inputs keep their native
working dimensions. The original source remains available for decoding/binning;
there is no 200MP RGB preparation path for supported 200MP geometry.

## Preview work

The first overview uses a bounded Bayer RGB proxy. Automatic refinement builds
the selected demosaic cache. Subsequent interactive overviews use that cache,
including the selected demosaic, rather than reverting to the initial proxy.

The scheduler is event-driven and conflates pending changes. JPEG quality,
subsampling, and editor-only metadata do not invalidate the preview. Refinement
starts after 350ms of inactivity, after the latest overview has completed. One
scheduler owns both qualities; they cannot independently compete for the native
lane. New recipe edits cancel obsolete refinement. Viewport/pane changes do not
cancel RAW preparation. Fitted refinement uses a session high-water display size quantized to 128-pixel
steps. Shrinking the pane reuses the larger image; growth only requests a larger
preview reduction, not new binning/demosaic. Bitmap publication checks the recipe,
waveform generation, visibility, and (for zoomed detail) viewport.

Interactive frames fit the display up to a 1280-pixel long edge. Settled
full-frame processing follows the measured fitted image size, capped at 2MP and
further reduced by memory admission. The full-frame linear reduction is retained.

Zoom detail is sensor-anchored: 512px tiles at a power-of-two sampling level,
with up to four neighboring tiles prefetched. A bounded LRU (up to 64MiB, less
under memory pressure) reuses tiles across pans. A new visible layer publishes
only when all its required tiles are available. Old completed detail can move
with the image while new pixels are prepared. EXIF maps both tile pixels and
placement; Canvas draws shared floating-point edges without independent integer
layout rounding. Native filtering retains the existing 128px neighborhood.

Film Sim, highlight reconstruction and denoise have region-wide dependencies or broad kernels. They use
one grid-anchored, cached viewport patch, with its expanded processing size
admitted under the same memory budget. Independently processing those looks in
small tiles would introduce statistical seams; a shared full-frame statistics
contract is needed before enabling their multi-tile path. These previews remain
approximations of full-size export, as before.

Heap/system headroom bounds bitmap/cache admission and a conservative processing
allowance. Memory callbacks release detail caches without recycling published
bitmaps. This is not a complete scratch estimator for full-source demosaic.
Temporary crop reductions remain transient on disk; their displayed results
are cached in memory. Navigation image, waveform and detail publication share
one recipe/generation identity. Stale native results cannot update a newer frame.

The renderer preserves its tone, post-processing, and film engines across frames.
Previously it called `reset()` before each render, destroying those engines.
Linear-stage output is reused when source geometry, WB, reconstruction, denoise,
FCC, and defringe dependencies match. Coloropp also invalidates on exposure.
The mutable linear working image is retained and is not overwritten on reuse.

Previews remain resolution-dependent approximations for spatial effects and
reconstruction statistics. Export processes the selected full output with its
own recipe snapshot. A viewport margin limits edge artifacts; it does not make
local reconstruction statistics identical to a full-frame export. UltraHDR
encoding is export-only.

## Editor layout and lifecycle

The toolbar, workspace, preview canvas, and control pane are named composables
with explicit state inputs. The selected stage keys the entire control viewport,
including its clipping layer; the saveable-state holder lives outside that key.
Switching stages replaces page layout nodes while restoring saved scroll/expansion
state. Tone modes also have distinct composition identities. Toolbar and tab hit regions are siblings of the
clipped preview and scrolling controls. Preview gesture handlers only cover the
canvas. There are no native Android Views or Surface/TextureView callbacks in
the editor display path: overview and refinement use Compose Images, and detail uses a Compose Canvas. This trades a bounded ~1MP overview readback for a single display owner.
Native GPU processing, linear-result reuse, and GPU waveform generation remain.

The non-interactive preview status overlay reports proxy building, 200-to-50MP binning,
demosaic, resize, or look processing. The first refinement still needs a full
working-source cache (50MP for a 200MP import); only post-processing is bounded
to the displayed preview size. Native bin/cache and working-RAW preparation
have separate timing logs. Do not interpret the status as export progress.

Session destruction occurs outside the JNI handle-registry lock. UI-facing
cancel/progress calls only borrow the active engine while holding that short
lock, preventing a concurrent close from transferring GPU destruction onto
the UI thread.

Validation: scheduler/geometry/cache tests cover rapid edits, single-flight work,
pane resizing, zoom/pan, export pause/resume, hidden completion, waveform refresh,
EXIF placement, memory admission, complete-layer publication, and eviction.
`RendererEditorContent` is the actual UI without native session dependencies.
Seven shared pointer tests, in portrait and landscape, exercise tabs during preview work, compact inspection tools, Film Sim quick switches, right-gutter scroll,
gesture isolation, toolbar actions, and pane expansion. The same suite runs under
Robolectric and Android instrumentation. Host results do not establish phone GPU
responsiveness or first-import duration.

## Effect controls

Quad Filter and Fast Median controls have been removed from Renderer and Settings.
Old preference keys are ignored and new recipes omit the flags. The renderer JNI
also ignores flags in older recipes, so saved jobs cannot silently re-enable it.
The native filter library remains available for independent development/testing.

Renderer always supplies its camera color matrix to the post processor, selecting
Lab defringe. Strength is active. Edge Threshold and Luma Floor belong to the
legacy matrix-less camera-RGB fallback, so the Renderer does not display them.

Film Sim section headers provide switches for DIR, print, filters, diffusion,
grain, halation, and scanner. Print switches the existing print/scan workflow.
DIR restores its previous amount and multi-effect switches restore their previous
flag selection while the editor state is retained; effect tuning is unchanged.
These use the job-local settings dispatcher, with no global preference writes.

## Natural binning

`NaturalBayerBin.h` adapts the supplied RAW Editor 0.1.20 Natural equations to
floating-point, black-subtracted/linearized samples. It retains the chroma,
brightness, directional alias-guide, reflection, and same-phase envelope rules.
It omits integer packing and dithering. Tile scratch and normalization bands are
bounded; a reusable disk-backed floating-point CFA cache survives demosaic changes.
Unsupported Natural geometry uses the existing same-color filter, still before
demosaic. An odd active Bayer crop on a 200MP source is rejected explicitly.

The golden fixture `tests/fixtures/renderer_natural_reference.u16` is a synthetic
96×80 input reduced to 48×40 by the supplied Rust reconstruction with no dither,
round-to-nearest packing, CFA RGGB, black levels [64,128,128,96], white 16383,
and WB [2,1,1,1.5]. Input codes are
`1024 + (x*173 + y*317 + (x*y % 23)*71) % 12000`.
The floating-point port agrees within 0.501 sensor DN, including borders.

## Validation

Renderer Kotlin tests cover source defaults, restored recipes, local edit
ownership, inactive-mode persistence, resolution policy, scheduling, export-only
invalidation, and EXIF/letterbox crop mapping. The native Bayer test covers the
Rust golden output, all CFA phases, tiled/full equivalence, and cancellation;
it also passes AddressSanitizer and UndefinedBehaviorSanitizer.

For device validation, compare warm slider input-to-presentation latency using
`RawrRenderer` logcat entries and the native stage timing logs. Target p95 ≤50ms
for the interactive overview. Check all three tone modes, continuous slider
movement during refinement, portrait/landscape pane resizing, zoom/pan, and
preview/export differences on 200MP and native 25MP/12.5MP inputs. The current
refactor has not been interaction-tested on the connected phone; the user is
testing the previous build. Do not install during an active render.


## Editor inspection and controls

The editor preview uses Compose Images for both overview and refinement;
detail tiles retain their source coordinates and belong to the displayed recipe.
WAVEFORM toggles the same `image_scopes::ImageScopes` GPU display-waveform
backend used by the viewfinder, with RGB overlay mode, FullReference sampling,
default density/grid presentation, and a 512×384 output. It measures the native
rendered image before viewport cropping, with EXIF-aware column orientation.
The GPU scope image is read back for Compose presentation; there is no screenshot
sampling or separate CPU histogram. Scopes are computed only while enabled, on
interactive overview and full-image refinement renders (not zoomed detail crops).
It represents rendered display values, not RAW sensor levels.
METADATA shows source dimensions, EXIF orientation, ISO, shutter, aperture and
black/white levels; absent capture tags are labeled as not recorded.

Quad Filter pads edge tiles to its required 16-pixel alignment using samples of
the same Bayer parity. Its scratch GPU buffers remain alive until the submission
fence completes. The cache version changes so previously unfiltered caches are
not reused. The initial fast proxy remains approximate until refinement completes.
Defringe sliders now match the reducer ranges: edge threshold 0.005–0.2 and luma
floor 0–0.5.
