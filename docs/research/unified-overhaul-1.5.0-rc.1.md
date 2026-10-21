# Unified engineering candidate 1.5.0-rc.1

Baseline: `a065fffcdcee694f13c88d9449038d0e595fb91f`, branch
`feature/1.4.5-xemfg-reframework-r1.12`. The unified DLSS-G/XeMFG worktree was
clean before source changes, so no empty checkpoint was created. The dirty RD9
worktree is excluded. R1.12 and official 1.4.5 remain rollback references.

This is a local candidate only. No push, tag, GitHub release, distribution or
automatic game installation is authorized. Existing game files remain intact.

## Architecture and ownership

- The single addon selects DLSS-G or XeMFG once at startup. Changing backend is
  a saved preference requiring restart, never a live ownership transfer.
- DLSS-G retains NVIDIA's proxy/pacer, Streamline options, native Reflex,
  validated 1.4.5 payloads and engineering lifetime policy.
- XeMFG intercepts D3D12 factory creation and Streamline inputs, owns an outer
  DXGI identity and an Intel public proxy/context. ReShade renders on the final
  output. Source Present serialization and per-output input serialization are
  distinct. SDK-owned queues/fences are not replaced by addon timers.
- FrameStore entries belong to a source token/viewport and output epoch.
  Volatile inputs use Intel ONLY_NOW copies recorded on the producer list.
  AddRef alone does not preserve texture contents. Optional HUD inputs may aid
  interpolation with composition off; composition remains an explicit opt-in.
- Retirement drains source calls, invalidates the epoch, performs optional
  REFramework pre-retire cooperation without bridge locks, then destroys Intel
  ownership. Refused handoffs quarantine both proxy and SDK context.
- Window mutations run on the HWND owner thread. The new display controller
  must distinguish DXGI exclusive state from window geometry and preserve
  native borderless and foreign changes.
- Dependency loading admits exact Intel file identities. Dependency inspection
  and repair must run off the render thread. An external helper may apply only
  after review, game exit, identity recheck, backup and journal creation.

## Validation boundaries

Automated fixtures and real SDK isolated rendering are distinct from gameplay.
No game compatibility, image quality, displayed pacing or physical latency
benefit is established by compilation. Gameplay and paired performance results
will be listed explicitly as unverified unless actually collected.

Intel public headers are MIT; Intel runtime/program data has separate terms.
No additional authorization was supplied. This candidate does not resolve the
license review required for public distribution of derived runtime programs.
