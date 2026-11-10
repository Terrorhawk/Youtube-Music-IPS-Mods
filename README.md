# YouTube Music IPS Mods

Source code for the iOS player used with [Interactive Playlist System](https://github.com/Terrorhawk/InteractivePlaylistSystem). IPS lets an event server control playback on a paired iPhone or iPad.

- Pair with an IPS server using a QR code or manual settings.
- Handle play, pause and next, with crossfade and fallback playback.
- Keep track information and controls in sync with the active player, including on the lock screen.
- Suppress selected promotional dialogs in IPS mode.

This snapshot corresponds to **IPS V0.5.2**, targeting YouTube Music **8.21.3** and **9.43.2**. The hooks use version-specific app internals; other versions are untested.

## Source layout

- `native/RemoteV6.x`: Logos entry point for 8.21.3.
- `native/Remote943.x`: Logos entry point for 9.43.2.
- `native/*.h`: shared playback, settings and presentation logic.
- `upstream/`: playback tweaks from [YTMusicUltimate](https://github.com/dayanch96/YTMusicUltimate/tree/e5530b1a1323d6ae48dd4fe0f1f2b932d59962ba), including the adapted ad hooks for 9.43.2.

Build the matching entry point and playback tweaks with Logos and an iOS Objective-C toolchain. The original builds used the Logos internal backend, ARC and an arm64 iOS target. Use `RemoveAds.x` for 8.21.3 or `RemoveAds943.x` for 9.43.2, plus `BackgroundPlayback.x` and `DisableAutoPlay.x`. The target links UIKit, Foundation, Security, AVFoundation, AudioToolbox and MediaPlayer. In the running mod, open **IPS Player** to enter your own HTTPS server address and pair the device.

This repository contains source only. It does not include IPAs, the YouTube Music app, signing material, or the IPS server. Derived YTMusicUltimate code retains its GPL-3.0 license; see [LICENSE](LICENSE).
