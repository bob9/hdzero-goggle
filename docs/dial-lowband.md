# Dial Lowband — the all-band channel dial

## How it works

HDZero goggles group their channels into two bands: **Raceband mode**
(R1–R8, plus E1, F1, F2, F4) and **Lowband** (L1–L8, lower frequencies).
On stock firmware those are walled off from each other — the "HDZero Band"
setting on the Source page decides which set exists, and the channel dial
only scrolls within it. Sitting on R4 and needing L2 means: land the
video, open the menu, Source page, flip the band, back out, dial to the
channel.

With **Menu > Source > Dial Lowband = On**, the wall comes down at the
controls level:

- The **channel dial** scrolls one continuous loop of all 20 channels:
  R1 → … → R8 → E1 → F1 → F2 → F4 → L1 → … → L8 → back to R1. The
  "To L2?" preview shows exactly what you're about to select.
- Confirming a channel from the other band does the band housekeeping for
  you — switches the band setting, saves it, retunes. One dial action, no
  menu trip.
- **Scan Now** sweeps all 20 channels in one pass (about two seconds
  longer) and shows Lowband as two extra rows of signal bars, so a single
  scan finds every pilot in the air, whichever band they're on. Picking a
  Lowband result also band-switches automatically.

Underneath, nothing about the radio changes — same tuner, same
frequencies, same video. The band setting still exists; the feature just
drives it for you.

## The advantages

1. **Speed at the race line.** Moved to a Lowband slot for the next heat?
   One dial scroll instead of a six-step menu dance — with gloves on,
   between packs.
2. **The whole field on one scan.** With pilots split across Raceband and
   Lowband, a stock scan only shows half the picture. All-band scan shows
   everyone with signal on one screen.
3. **Fewer wrong-band mistakes.** No more "channel's right, band's wrong,
   screen's static" — every channel name on the dial is the channel you
   get.
4. **Plays with the VTX changer.** With Auto Send VTX on: dial to any of
   the 20 channels, long-press, and your quad's VTX follows — including
   across the band boundary.

The option ships **Off**, and Off is bit-for-bit stock behavior.
