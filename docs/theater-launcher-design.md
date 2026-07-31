# Theater Launcher — a consumer interface for the HDZero Goggle

Product goal: make the goggle sellable to the general public as a **personal
theater and private big-screen**, with an Android-TV-*style* launcher — while
keeping the FPV firmware intact behind an "FPV Mode" tile.

Companion docs: `plex-interface-design.md` (media client foundation, largely
implemented on branch `plex-client`).

---

## 1. Positioning: what this product is and is not

**Is:**
- A private cinema: dual 1080p micro-OLEDs + hardware H.264/H.265 decode.
  Sources: Plex/Jellyfin on the home LAN, files on SD/USB.
- A private big screen: **HDMI-in already exists in the hardware** — Steam
  Deck, Switch, laptop, phone-with-adapter. For a consumer this is a headline
  feature, not a hidden input.
- A DVR/media player for its own recordings.

**Is not (and can never be):**
- A Netflix/Disney+/Prime device *natively*. Those require Widevine/PlayReady
  DRM and vendor-certified Android/tvOS apps; no custom firmware on this SoC
  (buildroot Linux, no GPU, no Android BSP) can offer them. Marketing must
  not promise native "streaming apps".
- A YouTube device natively (no browser engine, no DRM, API terms hostile to
  unofficial clients).

**How "all the Android TV apps" IS delivered — the HDMI bundle:**
The goggle's existing HDMI input displays a certified Android TV device
(Google TV Streamer / Chromecast, Fire TV stick). That stick provides every
real app — Netflix, YouTube, Disney+, with full DRM — maintained by Google,
with zero certification burden on this firmware. Product implication:

- Sell a **"personal cinema kit"**: goggle + Android TV stick + powered
  USB/HDMI harness. The stick is "all the apps"; the goggle is the private
  big screen; the native launcher (below) covers offline/local use where a
  stick has no network (planes) or isn't attached.
- Firmware's job is to make HDMI-in feel first-class: auto-enter HDMI on
  signal detect, clean battery/volume overlay while in HDMI, instant
  launcher <-> HDMI switching. (HDMI-in and signal detection already exist
  in the codebase: `g_source_info.hdmi_in_status`.)

## 2. The launcher

Boot target for the consumer SKU (or a "Simple Mode" toggle on the shared
firmware). LVGL, same rendering stack as today.

Layout — 10-foot design language, one focus row at a time, dial-native:

- **Top bar**: clock, battery, WiFi state.
- **"Continue watching" row**: resume items merged from all media backends
  (Plex viewOffset already provides this; Jellyfin equivalent exists).
- **Sources row (big tiles)**: Movies (Plex/Jellyfin poster wall), HDMI
  Input, SD Files, FPV Mode, Settings.
- Selecting Movies opens the existing poster-wall page; HDMI Input jumps
  straight to the existing HDMI-in path; FPV Mode enters today's main menu.

Input mapping: dial scroll = move focus, click = select, long-press = back,
Func = context (refresh/options). Identical to the grammar the Plex page
already established.

## 3. Media backends behind one UI

The poster wall, detail page, and (phase 3) network playback pipeline are
backend-agnostic by design. Add adapters, not new UIs:

| Backend | Effort | Notes |
|---|---|---|
| Plex | done (browse) / phase 3 (play) | `src/core/plexapi.c` |
| **Jellyfin** | small | Same architecture as plexapi (HTTP + JSON/XML, `/Items` listing, image transcoder, HLS transcode). Free + no account — removes the plex.tv dependency entirely for privacy-minded buyers. |
| DLNA/UPnP browse | medium | SSDP discovery is GDM-shaped; ContentDirectory browse is XML we already parse. |
| SD/USB files | exists | Today's DVR playback page, reskinned into the launcher. |

## 4. Phone as the remote (companion web page)

The goggle serves a static page + small JSON API over its existing WiFi
(pattern proven by the rtspLive server). The phone handles everything a dial
is bad at:

- Search and fast library browsing ("play on goggle" button).
- All text entry (server addresses, tokens) — complements the implemented
  plex.tv/link sign-in.
- Settings, firmware update trigger.

This is the Chromecast interaction model: phone chooses, goggle plays.
Discoverability: show `http://<ip>` (and a QR code rendered by LVGL) on the
launcher's Settings tile.

## 5. Playback pipeline (shared)

One pipeline serves every backend — the phase-3 `netdmx` design from
`plex-interface-design.md` §4: libavformat demux (mov/mpegts enabled in the
bundled build) over a hand-rolled HTTP AVIO layer, hardware decode via
`vdec2vo`, AAC audio via `adec2ao`, server transcode fallback (HLS/TS) for
exotic codecs. HEVC direct play pending a `PT_H265` decoder test.

## 6. Roadmap

| Phase | Deliverable |
|---|---|
| A | Plex playback (netdmx) — completes the movie experience end-to-end |
| B | Launcher shell (rows, tiles, boot-into-launcher toggle, HDMI/SD/FPV tiles wiring) |
| C | Companion web remote (HTTP server, search/play/settings, QR pairing) |
| D | Jellyfin adapter; Continue-watching row merging backends |
| E | DLNA browse; polish (sleep timer, brightness/comfort presets, guided first-run: WiFi → sign-in → play) |

## 7. Risks / open questions

1. **Comfort & optics for non-pilots** — fixed IPD/focus tuned for FPV;
   a consumer SKU likely needs hardware answers (diopters, face padding).
   Out of firmware scope but central to the product.
2. **WiFi throughput** (~20–40 Mbps realistic) — 1080p direct play fine;
   heavy remuxes/4K require server transcode (quality cap default 12 Mbps).
3. **Licensing hygiene** — ship nothing that implies Netflix/YouTube/Android
   TV compatibility; "works with Plex and Jellyfin" is the honest claim.
4. **Two-audience firmware** — keep FPV boot path selectable and fast;
   racers must lose nothing (launcher must be skippable via setting).
