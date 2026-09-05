# Pre-SR placement and persistent multipass

This experimental branch adds two opt-in controls to the `[DlssNr]` section:

- `RunBeforeSR=true` runs Neural Rendering on the colour input immediately before Super Resolution.
  The default is `false`, preserving the v0.2.0 post-upscale seam. Ray Reconstruction/DLSSD is
  deliberately forced to remain post-upscale because its input contract is not compatible with the
  PR #6 pre-SR path.
- `Passes=N` selects one to three sequential model layers. The default is `1`.

## Multipass lifetime and data flow

Every layer owns a persistent NGX feature and temporal history. A missing feature is created in one
submission epoch and remains pending until that epoch changes; no Neural Rendering feature is
evaluated on the command-list recording that created it. Native DX12 uses the wrapped Present count,
while the DX11/Vulkan bridges supply their post-submit frame counter. At most one extra feature is
created per submitted frame. A failed extra creation is latched and the ready contiguous prefix
remains active, rather than reusing the main feature or retrying every frame.

The frame is encoded once. Its base proxy remains immutable while model answers ping-pong through two
same-format, same-size resources:

`base -> A -> B -> A` (as needed for one, two, or three layers)

The final answer is composed once against the immutable base. This keeps matched-residual transfer
cumulative (`final - base`) without compounding colour/transfer settings. Local tone is applied by the
first model layer only. A camera cut resets every active layer; a newly created extra layer is also
reset on its first evaluation.

Features and scratch resources are parked for deferred release on tuning, raster, format, placement,
or pass-count changes so in-flight frame-generation work cannot retain freed objects.

## Resource-state rules

Post-SR output uses the existing output-arrival state. Pre-SR colour arrives and is returned as a
non-pixel shader resource. If Color has no UAV flag, composition writes to an owned scratch texture and
copies back instead of binding an illegal UAV.

## Guardrails

- Pass count is clamped to `1..3`; historical testing found later layers converged while cost and
  artifacts continued to grow.
- The driver-proxy backend remains single-pass and logs the effective fallback.
- A padded, offset, or max-sized dynamic-resolution Color allocation falls back to post-SR until the
  colour codec supports subrect origins; this avoids processing stale pixels or reporting a false model
  resolution.
- Placement is part of the rebuild key even when pre/post surfaces happen to share dimensions and
  format (for example DLAA).
- Working scales from 25% through 200% remain supported; the ping-pong resources use model-work size.
