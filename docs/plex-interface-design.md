# Plex Client Interface for HDZero Goggle — Design

Turn the goggles into a lightweight Plex front-end for a **local** Plex Media
Server: browse every movie in the library as a poster grid, pick one, and watch
it through the goggle's hardware video pipeline.

This is a design document; no implementation is included.

---

## 1. What the codebase already gives us

| Need | Existing building block |
|---|---|
| Join the home LAN | `src/core/wifi.c` — wpa_supplicant station mode, scan, remembered networks (`page_wifi.c`), text entry via `src/ui/ui_keyboard.c` |
| Play H.264 video | `src/player/` — `awdmx` (Allwinner demuxer, MP4 + MPEG-TS with `ts_probe`) → `vdec2vo` (hardware decode to video-out layer) + `adec2ao` (audio) |
| Player UI/controls | `src/ui/ui_player.c` (`mplayer_file`, pause/seek/on-key), `media_instantiate()` API in `src/player/media.h` |
| Grid-of-thumbnails UI | `src/ui/page_playback.c` — LVGL page with per-item `_img` + `_label` cells, dial navigation |
| HTTP/demux library | `lib/ffmpeg` — ffmpeg 5.0.1 built for the V536 (`libavformat`, `libavcodec`, `libavutil`), already linked by the record app |
| JPEG handling | Hardware JPEG encode exists (`jpegenc`); LVGL image widgets already display DVR thumbnails from SD |
| Persistent settings | `src/core/settings.c` (minIni `.ini` store) |
| Menu integration | `src/ui/ui_main_menu.c` + the `page_*.c` pattern |

Conclusion: this is an integration project, not a platform project. The two
genuinely new pieces are a **Plex HTTP API client** and a **network input path
into the existing decoder pipeline**.

---

## 2. Architecture overview

```
                ┌────────────────────────────────────────────┐
                │                Plex Media Server (LAN)     │
                │  :32400  REST API (JSON) · poster transcode│
                │          media parts · universal transcode │
                └───────▲──────────────▲─────────────────────┘
                        │ metadata     │ media (HTTP)
                        │ + posters    │
        ┌───────────────┴──────┐   ┌───┴────────────────────────┐
        │ plexapi.c            │   │ netdmx.c                   │
        │ - GDM discovery      │   │ - libavformat HTTP input   │
        │ - auth (token / PIN) │   │ - demux → ES packets       │
        │ - sections / movies  │   │ - feeds same queues as     │
        │ - poster cache to SD │   │   awdmx does today         │
        └───────▲──────────────┘   └───┬────────────────────────┘
                │                      │
        ┌───────┴──────────────┐   ┌───▼──────────┐  ┌──────────┐
        │ page_plex.c (LVGL)   │   │ vdec2vo      │  │ adec2ao  │
        │ server setup ·       │──▶│ HW H.264     │  │ AAC →    │
        │ poster grid · detail │   │ → video out  │  │ audio out│
        └──────────────────────┘   └──────────────┘  └──────────┘
```

New modules:

- `src/core/plexapi.{c,h}` — server discovery, auth, library queries, poster
  fetch/cache. Pure C, uses libavformat's `avio` HTTP (or a ~200-line raw
  socket HTTP/1.1 client as fallback, see §8 risk 1) plus a minimal JSON
  parser (vendored single-header, e.g. jsmn).
- `src/player/netdmx.{c,h}` — network counterpart to `awdmx`: opens a URL with
  `avformat_open_input()`, pulls packets, and pushes them into the existing
  `vdec2vo`/`adec2ao` queues. `media.c` picks `netdmx` when the "filename"
  starts with `http://`.
- `src/ui/page_plex.{c,h}` — the interface itself (§5).

---

## 3. Plex protocol — what we actually need

Everything is plain HTTP against `http://<server>:32400`, authenticated with an
`X-Plex-Token` query parameter. Send `Accept: application/json` to get JSON
instead of XML. Required client headers: `X-Plex-Client-Identifier` (stable
UUID, generated once and stored), `X-Plex-Product=HDZero Goggle`,
`X-Plex-Version`, `X-Plex-Device`.

| Purpose | Endpoint |
|---|---|
| Server discovery on LAN | GDM: UDP multicast `M-SEARCH` to `239.0.0.250:32414`; server replies with name, port, machine id. Manual IP entry as fallback. |
| Reachability / server info | `GET /identity` (no auth needed) |
| List libraries | `GET /library/sections` → keep `type == "movie"` sections |
| All movies in a section | `GET /library/sections/{key}/all?type=1&X-Plex-Container-Start=N&X-Plex-Container-Size=50` — paged; fields used: `ratingKey`, `title`, `year`, `duration`, `thumb`, `Media[].Part[].key`, codec info |
| Poster, server-scaled | `GET /photo/:/transcode?width=160&height=240&minSize=1&url=<thumb>` — the server does the resize, goggle just decodes a small JPEG |
| Direct play | `GET <Part.key>` (e.g. `/library/parts/12345/file.mp4`) — plain HTTP, supports range requests and therefore seek |
| Transcoded play | `GET /video/:/transcode/universal/start.m3u8?path=/library/metadata/{ratingKey}&protocol=hls&videoCodec=h264&audioCodec=aac&maxVideoBitrate=12000` — HLS with MPEG-TS segments |
| Resume position | `GET /:/timeline?ratingKey=..&key=..&state=playing&time=<ms>&duration=<ms>` every ~10 s; movie's `viewOffset` field gives the resume point on browse |

### Authentication for a local server

Two supported paths, in this order of preference in the UI:

1. **plex.tv PIN link** (needs internet once): `POST https://plex.tv/api/v2/pins`
   → show the 4-character code on the goggle screen, user enters it at
   plex.tv/link on their phone, goggle polls the pin endpoint until it returns
   a token. Best UX, no typing on the goggle.
2. **Manual token entry** via `ui_keyboard` — works fully offline, good escape
   hatch (and for servers with "Advertise as player"/managed setups).

The token is stored in settings and reused; Plex tokens don't expire in
practice.

---

## 4. Playback strategy

The hardware decoder is H.264 (the DVR records and plays back H.264 up to
1080p90, so 1080p24 Blu-ray-class movie streams are easy). Container support in
the existing pipeline is MP4 and MPEG-TS.

**Decision ladder per movie, evaluated from the codec metadata Plex already
returns in the library listing:**

1. **Direct Play** — container `mp4`, video `h264` (≤ High@L4.2), audio `aac`:
   open `Part.key` URL straight into `netdmx`. Zero server CPU, full quality.
   Seeking uses HTTP range + the MP4 index, which libavformat handles.
2. **Server transcode / remux** — anything else (HEVC, MKV, AC3/DTS/TrueHD
   audio, 4K): request the universal transcode as **HLS with TS segments**,
   `videoCodec=h264&audioCodec=aac`. libavformat's built-in HLS demuxer makes
   this look identical to case 1 from `netdmx`'s point of view — it outputs the
   same H.264/AAC packet stream. Seeking is a transcode-session restart with
   an `offset` parameter (Plex supports this natively).

Either way, `media_instantiate("http://...")` routes to `netdmx`, and
everything downstream (`vdec2vo`, `adec2ao`, OSD player controls, the
pause/seek key handling in `ui_player.c`) is reused unchanged. A ~4 MB packet
pre-buffer in `netdmx` absorbs Wi-Fi jitter; buffer state is surfaced to the
player OSD ("buffering…" spinner).

Audio note: movies make audio matter more than DVR clips ever did — AAC stereo
via the existing `adec2ao` path to the goggle's headphone jack/speaker is the
target; multichannel formats are downmixed by the server transcode.

---

## 5. The interface

### 5.1 Entry point

New top-level menu item **"Plex"** in `ui_main_menu.c`, following the existing
page registration pattern. Greyed out with a hint label when Wi-Fi is not
connected (state already exposed by `wifi_connected_ssid()`).

### 5.2 Screen flow

```
Main menu ─▶ Plex
              │ no server configured
              ▼
        ┌─ Server setup ────────────────┐
        │ ▸ Found: "Living Room NAS"    │   GDM results + [Enter IP manually]
        │ ▸ Link with plex.tv  (CODE)   │   PIN flow, or
        │ ▸ Enter token manually        │   ui_keyboard
        └──────────────┬────────────────┘
                       ▼ paired (stored in settings)
        ┌─ Movie library (poster grid) ─┐
        │  [All Movies ▾]      142 films │  section picker if >1 movie library
        │ ┌────┐┌────┐┌────┐┌────┐┌────┐│
        │ │ ▓▓ ││ ▓▓ ││ ▓▓ ││ ▓▓ ││ ▓▓ ││  5 × 2 posters per page
        │ │ ▓▓ ││ ▓▓ ││ ▓▓ ││ ▓▓ ││ ▓▓ ││  (160×240 px art)
        │ └────┘└────┘└────┘└────┘└────┘│
        │  Dune   Heat  Alien  Tron  Up  │  title + year under focused cell
        │ ┌────┐┌────┐┌────┐┌────┐┌────┐│
        │ │ ▓▓ ││ ▓▓ ││ ▓▓ ││ ▓▓ ││ ▓▓ ││
        │ └────┘└────┘└────┘└────┘└────┘│
        │        ◂ page 3 / 15 ▸        │
        └──────────────┬────────────────┘
                       ▼ click
        ┌─ Detail / confirm ────────────┐
        │  DUNE (2021)  2h35m  4K HEVC  │
        │  ▸ Play  (will transcode)     │  "Resume from 1:12:04" when
        │  ▸ Play from beginning        │  viewOffset > 0
        │  ▸ Back                       │
        └──────────────┬────────────────┘
                       ▼
             Fullscreen playback (existing ui_player OSD:
             pause / seek ±10 s / seek bar / exit)
```

### 5.3 Poster grid details

- Modeled directly on `page_playback.c`'s item cells (`_img` + `_label` +
  focus arrow), but laid out as a 5×2 grid inside the standard
  `UI_PAGE_VIEW_SIZE` menu page with the existing `style_submenu` styling so it
  looks native to the goggle UI.
- **Navigation** maps to the goggle's dial-and-button input model exactly like
  existing pages: dial = move focus left/right (wrapping to next row), click =
  select, function button = back. Row wrap at the page edge flips the page, so
  the whole library is reachable with one dial.
- **Sort order**: title (default), recently added, year — cycled by a header
  control. Implemented server-side via the `sort=` query parameter, so paging
  stays simple.
- **Posters** are fetched at exactly the cell size via Plex's photo transcoder
  and cached on the SD card at `/mnt/extsd/.plexcache/<ratingKey>.jpg`
  (bounded LRU, ~20 MB ≈ 1000+ posters). Cells render the title immediately
  and the poster pops in when its fetch lands — fetches run on a worker thread,
  never on the LVGL thread; completion is posted back the same way DVR
  thumbnail loads are. Cache-hit pages render instantly with zero network.
- **Paging, not virtual scrolling**: 10 posters per page keeps LVGL memory flat
  and matches the `X-Plex-Container-Size` paging of the API. Page N+1's
  metadata is prefetched when page N is shown, so paging feels instant.
- The full library list (`ratingKey`, title, year, duration, codec flags —
  ~100 bytes/movie) is held in RAM; even a 5 000-movie library is ~500 KB.

### 5.4 Settings page additions

Under the Plex page's long-press menu (pattern used elsewhere in the UI):
server address (read-only, with "re-pair" action), preferred quality cap for
transcodes (Original / 12 Mbps / 8 Mbps / 4 Mbps), clear poster cache, forget
server.

Stored via `settings.c`: `[plex] server_ip`, `server_port`, `token`,
`client_uuid`, `machine_id`, `quality_cap`.

---

## 6. Threading model

- **LVGL thread**: untouched — only widget updates, driven by posted events.
- **plexapi worker** (one thread): serializes API calls, poster downloads,
  timeline progress pings. Results posted to the UI via the existing
  app-message mechanism.
- **netdmx thread**: replaces the `awdmx` read loop during network playback;
  same lifecycle as today's media thread (`media_instantiate` /
  `media_exit`), so `PLAYER_STOP`/`SEEK`/`PAUSE` commands keep their existing
  semantics.

---

## 7. Phased implementation plan

| Phase | Deliverable | Touches |
|---|---|---|
| 1 | Settings + server setup page: GDM discovery, PIN link, manual token; "server reachable" check against `/identity` | `plexapi.c`, `page_plex.c`, `settings.c`, `ui_main_menu.c` |
| 2 | Library browse: sections, paged movie grid, SD poster cache, sort | `plexapi.c`, `page_plex.c` |
| 3 | Direct Play: `netdmx` via libavformat HTTP, route `media_instantiate` by URL scheme, buffering OSD | `netdmx.c`, `media.c`, `ui_player.c` |
| 4 | Transcode fallback (HLS/TS) for non-H.264/MP4 titles; quality cap setting | `netdmx.c`, `plexapi.c` |
| 5 | Polish: resume/viewOffset, timeline progress reporting, watched badges on posters, error toasts (server gone, Wi-Fi drop → clean return to grid) | `page_plex.c`, `plexapi.c` |

Phases 1–2 are shippable on their own (a working library browser); playback
lands in phase 3.

---

## 8. Risks and open questions

1. **Bundled libavformat capabilities — now verified** (from the build flags
   embedded in `lib/ffmpeg/lib/libavformat.so.59.16.100`): it is a minimal
   build (`--disable-everything --disable-network`) with exactly
   `mov`/`mpegts`/`h264`/`hevc` demuxers and only the `file` protocol enabled.
   Consequences: the **demuxers we need are already there**; there is **no
   HTTP protocol and no HLS demuxer**. Plan: `netdmx` supplies a custom
   `AVIOContext` whose read/seek callbacks are a hand-rolled HTTP/1.1
   range-request client (~200 lines, LAN-only so no TLS) — this sidesteps
   `--disable-network` entirely and covers phases 1–3. For phase 4 (HLS),
   either rebuild ffmpeg in the docker build image with
   `--enable-protocol=http,tcp --enable-demuxer=hls`, or skip the HLS demuxer:
   fetch the `.m3u8` and feed the downloaded TS segments back-to-back through
   the already-enabled `mpegts` demuxer via the same AVIO callback (TS is
   designed to concatenate cleanly).
2. **HEVC direct play may be possible** — the SoC middleware exposes
   `PT_H265` alongside `PT_H264` (`lib/softwinner/.../mm_common.h`), and
   `vdec2vo` merely hardcodes `PT_H264` today. If a target-side test confirms
   hardware HEVC decode at movie bitrates, most modern libraries direct-play
   without any server transcode. Treat as an optimization to evaluate in
   phase 4, not a dependency.
3. **Decoder container/codec edge cases** — MP4s with unusual muxing
   (fragmented MP4, edit lists) may confuse downstream timing. `netdmx` uses
   libavformat rather than `awdmx`, which normalizes most of this; the
   transcode path (phase 4) is the universal fallback for anything that
   misbehaves.
4. **Memory** — the record/player processes already link the ffmpeg `.so`s, so
   code size is paid; new steady-state cost is the ~4 MB stream buffer + JSON
   page buffers (~100 KB). Needs a check against the app's headroom on target.
5. **Wi-Fi throughput** — the goggle's Wi-Fi must sustain the movie bitrate.
   High-bitrate remuxes (40 Mbps+) may stutter; the quality-cap setting forcing
   a 12 Mbps transcode is the practical ceiling and the recommended default.
6. **UI blocking** — all network I/O is off-thread by design (§6); the failure
   mode to guard in review is any accidental synchronous fetch on the LVGL
   thread.
7. **Multiple movie libraries / non-movie sections** — handled (section picker,
   `type=1` filter); TV shows are out of scope for this design but the
   endpoint structure extends naturally (seasons/episodes are one more grid
   level) — worth keeping `page_plex.c`'s grid generic over "item lists".
