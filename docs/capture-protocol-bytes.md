# How to capture the exact Anti-collision / Select Card byte sequences

## Why this is needed

The test app proves the fix: `Wake up` -> `Anti-collision` -> `Select Card`
reads both the metro card and the debit card correctly, while the merged
`Inventory` command only works for the metro card. The Arduino sketch
currently only knows the byte codes for `Wake up` (`CMD_WUPA`) and
`Inventory` (`CMD_INVENTORY`) — it has no `Anti-collision` or
`Select Card` command defined, because those bytes were never captured
from a working reference.

Guessing those byte values would repeat the same mistake that caused the
last regression. The reliable way to get them is to capture the actual
bytes the test app sends, since that app is a known-good reference
implementation for this exact reader.

## Option A: Wireshark (recommended, most reliable)

The test app talks to the reader over plain TCP (you can see this in the
app: `TCP/IP`, `Reader IP 192.168.1.200`, `Server Port 9090`). That means
the traffic can be captured with Wireshark on the PC running the app.

1. Install Wireshark if not already available: https://www.wireshark.org/download.html
2. Make sure the Arduino is **not** connected to the reader at the same
   time (only one TCP client should be attached, otherwise the reader
   may reject the second connection or interleave traffic).
3. Start Wireshark, capture on the network interface connected to the
   reader (or `Npcap Loopback Adapter` if the app and reader are on the
   same machine — unlikely here since the reader has its own IP).
4. Apply a capture/display filter: `ip.addr == 192.168.1.200 && tcp.port == 9090`
5. In the test app, click buttons in this exact order, pausing a second
   between each so they're easy to tell apart in the capture:
   - `Wake up`
   - `Anti-collision`
   - `Select Card`
   - (also capture `Inventory` again for comparison, and `Halt` if you
     want the full picture)
6. Stop the capture. For each of those packets, right-click -> "Follow"
   -> "TCP Stream", or just look at the raw hex bytes in the packet
   detail pane for the TCP payload (not the whole packet — you want just
   the payload bytes, which is what the app is sending after the TCP/IP
   headers).
7. Note down the raw hex payload bytes for the request the app sent for
   each button, and the raw hex payload bytes of the reader's response.

## Option B: If Wireshark isn't available — app-side logging

Some versions of this kind of vendor test app log raw TX/RX bytes
somewhere (a log file, or a verbose/debug mode toggle). Check:
- Any menu bar option (View / Options / Debug) in the app for a "show
  raw bytes" or "log" toggle.
- A log file dropped next to the app's `.exe`, often named something
  like `log.txt` or in an `AppData` folder.

If the app only shows human-readable text (like the "Sending Wake Up
command" messages seen in the screenshots) and not raw bytes, this
option won't give byte-level detail and Option A (Wireshark) is
necessary.

## What to send back

Once captured, share (as hex, e.g. `20 02 52` style):
1. Request bytes sent for `Anti-collision`.
2. Response bytes received for `Anti-collision` (for both card types if
   possible — the debit card's response is likely longer since it needs
   a second cascade level).
3. Request bytes sent for `Select Card`.
4. Response bytes received for `Select Card`.

With those exact bytes, I can add real `CMD_ANTICOLLISION` and
`CMD_SELECT_CARD` commands to the sketch and wire `processCardRead()` to
use the same working sequence the test app uses, instead of relying on
the merged `Inventory` command that doesn't handle the debit card's UID
length/cascade level.
