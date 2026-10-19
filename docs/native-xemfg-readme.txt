MFGAdaUnlock - Native ReShade XeMFG R1

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
- Select DLSS-G or XeMFG. Restart the game after changing the backend.
- XeMFG multiplier is 2x-6x. Enable frame generation in the game's menu.
- The panel reports the bridge, model, XeLL and Tensor status.
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
Native R1 gameplay validation remains pending.

ROLLBACK (game closed):
  powershell -NoProfile -ExecutionPolicy Bypass -File Restore-MFGBackup.ps1 -BackupDirectory "<backup printed by installer>"

This restores the exact preceding setup, including working X1.3 + OptiScaler
when that was the installed version. rollback/ also contains official 1.4.5.
Do not hot reload: model, provider, hooks and callbacks have process lifetime.

No GPL OptiScaler implementation is included. Preserve notices/ when sharing.
