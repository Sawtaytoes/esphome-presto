# Touch scrolling moves the scanout over a picture drawn ahead

- **Status:** Accepted
- **Date:** 2026-10-26
- **Type:** Performance and architecture
- **Supersedes:** None
- **Superseded by:** None

## Decision

The firmware keeps one tall 480-pixel-wide RGB565 **scroll picture** in PSRAM, which CastKit
draws ahead of time and uploads like a frame slot (`scroll_chunk`, zlib or RLE, decoded in
bounded idle steps). `scroll_show(frame_id, top, bottom, scroll_y)` makes panel rows
`[top, bottom)`, both multiples of 8, scan out of the picture from its row `scroll_y`
instead of from the framebuffer. The scanout's row prefetch reads row `r` of the window from
`picture + 480 * (r - top + scroll_y)`, so moving the window is a change of source address
and costs no PSRAM writes.

1. **Latching.** Core 0 requests a window as one packed 32-bit word. The display core
   latches it at the first 8-row prefetch batch of a frame (row 0), so every row of one frame
   uses one window; a framebuffer swap carries the window its first 128 rows were copied
   with. The batch planner is `plan_batch` in `presto_scroll.h`, inlined into the interrupt
   handler and driven by the host tests.
2. **Nothing writes into what the scanout may read.** An upload into the picture first hides
   the window and waits for two latches (the first may have read the old request). Live
   patches never touch the picture.
3. **The window goes away** with `scroll_hide`, a slot show, a full live frame (base 0), the
   offline blank, `drop_slots` (which also empties the picture), and any new upload. Each is
   reported as `scroll_window,0,0,0,0,0` on Display Slot Events.
4. **The board drags on its own.** A contact that starts inside a shown window is reported
   as usual, so a tap still reaches the page. Once it has moved 12 rows vertically, and more
   vertically than sideways, the board takes it over: it reports that move as a release with
   flag `1` (CastKit cancels the contact), reports nothing more of it, and moves the window
   with the finger from the press, clamped to the picture. A quick release glides on with
   exponential friction (time constant 325 ms); a press during a glide stops it and is never
   a tap. At rest the board reports `scroll_at,<frame_id>,<scroll_y>` so CastKit can scroll
   its page to match. Every forwarded touch also carries flag `2` and the window's offset in
   its last field, so CastKit can match the page before it hit-tests a press.
5. **PSRAM budget.** The picture is allocated once at boot after the six slots: as many rows
   as fit while 1 MiB stays free, at most 4,096 rows, and none below 480 rows. With today's
   allocations that is about 3,600 rows (about 3.5 MB). `Display Codecs` advertises it as
   `scroll=<rows>`.
6. Reports use Display Slot Events (`scroll,`, `scroll_window,`, `scroll_at,`), one per loop
   step, so they never replace a frame acknowledgement on Display Events.

## Context

Touch scrolling on a CastKit list went through the whole live path for every drag frame:
Chromium capture, an RGB565 diff, compression, the encrypted native API over Wi-Fi, and an
on-board decode whose PSRAM writes are paced to 960 bytes per 50 µs to protect the scanout
prefetch (about 4 MB/s). A 480x360 JPEG patch took about 210 ms to decode on the board, and
touch to acknowledgement during a drag measured about 1,050 ms. Any on-board rendering of a
scrolled list is capped by the same paced writes. Showing a stored slot is fast because it
is a pointer swap at vertical blank; the scroll window extends that pointer move to a row
offset.

The prefetch is a cyclic 8-row XIP stream into a 128-row SRAM ring that runs up to 64 rows
ahead, so it fetches the first rows of a frame while the previous frame is still being
scanned. Latching at the row-0 batch, not at the frame-start interrupt, keeps a frame from
mixing two windows.

## Why

A pointer move is the only drag update that does not compete with the scanout for PSRAM, and
it moves on the board within one touch poll (20 ms) and one frame. Keeping the upload,
hide, and show on the slot path reuses the bounded decoder and the paced writer that are
already measured as steady. Taking a contact over only after a vertical threshold keeps taps
and sideways view swipes exactly as they were. 8-row alignment matches the prefetch batch,
so the interrupt handler does one comparison per batch and never splits a fetch. Window
columns cannot differ from the framebuffer: a batch is whole rows, so CastKit uses a window
only where everything else on those rows is the same in every row of a column.

Known limits: rows of a scroll container that are not on the 8-row grid keep the live
picture (CastKit snaps the window inward); the picture is not decoded from JPEG; the drag
and glide have not been measured on a panel yet.

## Evidence

Owner: touch scrolling on the Presto is "super laggy"; the drag should move the display
scanout's source address over a tall picture that CastKit rendered ahead, with taps still
going to CastKit.

Source: T3 Code Presto scroll-window conversation, 2026-10-26.
