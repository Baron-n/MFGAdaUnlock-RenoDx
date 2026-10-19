MFGAdaUnlock - Native ReShade XeMFG R1.3

One ReShade addon: DLSS-G from official 1.4.5, or Intel XeMFG with XeLL,
the validated MFG model port, NVIDIA Tensor route and output pacing.
The Streamline -> XeFG bridge is new MIT code. OptiScaler and an ASI loader
are not required. Intel runtime DLLs remain external runtime requirements.

This is a local candidate, not a new published release. Initial target:
NVIDIA + DirectX 12 + Streamline, starting with Cyberpunk 2077.
It does not implement every OptiScaler feature, input backend or game workaround.

INSTALL (game closed):
  powershell -NoProfile -ExecutionPolicy Bypass -File Install-NativeXeMFG.ps1 -GameDirectory "<game exe directory>" -PackageDirectory "<this directory>"

The installer preserves the game RenoDX addon and presets, saves the current
DXGI loader, MFG addon, companion and affected INIs, installs ReShade as the
DXGI loader, and removes only the backed-up competing MFG files.
The installer expects an existing addon-enabled ReShade installation and the
validated Intel pair. It does not download or replace vendor runtime DLLs.

Intel XeFG: libxess_fg.dll 1.3.1.78, SHA-256
ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27
XeLL: libxell.dll 1.3.0.5 or validated 1.3.2.10 (manifest lists identities).

In ReShade's MFG Unlock panel:
- General, Quality, Latency and Diagnostics are shared across the two backends.
  Technical codes/counters are on Diagnostics; common controls have short
  explanations. Narrow windows use a page selector and compact summary.
- Select DLSS-G or XeMFG. Restart the game after changing the backend.
- XeMFG multiplier is 2x-6x. Enable frame generation in the game's menu.
- The panel reports the effective FG state, model port, CUDA compute capability,
  recognized layers, Tensor launches and dispatch fallbacks.
- Driver flip metering can be toggled live. A request is committed by the
  presenter at the next generated batch, not in the middle of one. Status
  distinguishes NVAPI availability, queue readiness, driver acceptance,
  independent-flip feedback, refusal and SDK fallback. This is driver feedback,
  not a measurement of monitor scanout or proof of Blackwell display hardware.
- Force Borderless is optional (off by default). Window changes run on the
  HWND owner's message thread. The panel shows the actual style/monitor coverage,
  pending requests and Win32 errors. Unchecking restores the preceding style and
  placement only while they are still owned by this controller. Exclusive
  fullscreen requests are redirected to windowed mode while force is selected.
- Latency input: Auto or Reflex (Streamline). Output: Auto or XeLL. Auto resolves
  to this same implemented route. Both preferences persist in MFGAdaUnlock.ini.
  These selectors do not add native XeLL/NVAPI input adapters. Reflex output with
  XeMFG is not offered: XeFG requires XeLL. Use the DLSS-G backend for native Reflex.
- Active latency labels are based on recent received/successful sleep calls,
  with marker counters and the native Reflex-disable acknowledgement.
- Source FPS limits from Reflex are passed to XeLL; no second Reflex sleep.

Current output formats: SDR RGBA8/BGRA8 and HDR10 RGB10A2.
FP16/scRGB output, Vulkan, DX11, non-Streamline inputs, arbitrary multiadapter/
multiviewport setups and fullscreen-exclusive FG are not implemented.
InvertedDepth and JitteredMotion initialization flags default to 1 and 0;
incompatible actual constants disable FG instead of silently changing inputs.

VALIDATION:
Real ReShade + Intel model inference and Tensor launches passed isolated tests
at 2x, 3x, 4x and 6x, with both tagging APIs, volatile inputs, incomplete-input
bypass, FG disable, resize and context destruction. Full suite: 50 passed,
3 optional checks skipped. Synthetic/occluded windows do not validate display
cadence, real-game input conventions, HDR appearance, FPS or physical latency.
The X1.3 gameplay approval belongs to the previous OptiScaler transport.
R1 opened Cyberpunk but did not activate generation: admission incorrectly
required RenderSubmitEnd before Present. R1.1 allows these phases to overlap,
as XeLL permits, and keeps forwarding the game's actual END timestamp.
All four native inference tests passed again with END after Present, while
still checking incomplete-input bypass. A subsequent Cyberpunk session also
confirmed the late END markers, 2x/3x/4x/6x SDK output, Tensor execution and
accepted driver metering. The user also reports image and responsiveness
remain good versus X1.3. This is initial functional and subjective approval;
controlled performance, blind image equivalence, monitor cadence and physical
latency have not been measured.

R1.2 adds the controls above and preserves the R1.1 inference/input path. Its
isolated real-SDK tests cover the explicit latency preferences, unsupported
output rejection, live flip requests and window apply/restore. A separate
Win32 test covers cross-thread requests, reentrant resize/retirement, hidden
window visibility and foreign ownership. R1.1 gameplay approval does not by
itself validate the new window controls in every game.

R1.3 redesigns the player-facing panels and corrects marker locking in the
native latency bridge. Late markers from another frame/thread can arrive
while XeLL sleeps, and identical sleep settings are no longer sent every
frame. Context ownership and reset epochs protect outstanding sleep calls.
These are tested code changes, not a measured input-to-photon improvement.
The source comparison and remaining differences versus dashdogy/OptiScaler
are in interface-and-latency.md. The package includes synthetic-status PNG
renders of the actual XeMFG widgets; they are layout evidence only.

ROLLBACK (game closed):
  powershell -NoProfile -ExecutionPolicy Bypass -File Restore-MFGBackup.ps1 -BackupDirectory "<backup printed by installer>"

This restores the exact preceding setup, including working X1.3 + OptiScaler
when that was the installed version. rollback/ also contains official 1.4.5.
Do not hot reload: model, provider, hooks and callbacks have process lifetime.

No GPL OptiScaler implementation is included. Preserve notices/ when sharing.
