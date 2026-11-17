# Web radio

Internet radio on a touch screen, made with LVGL MPY Studio and its MCP tools: a list of stations,
play and stop, previous and next station, the time on air and the volume. It uses the same
`player.py` as the MP3 player: GStreamer's `gst-play-1.0` plays the `http://` or `https://` MP3
stream in the background, driven through `linuxproc.Process`.

In the Studio simulator, where there is no Linux, a stand-in pretends to play, so the screen can be
designed and tried without a board.

## On the board

1. Sound: a USB audio adapter on one of the USB-A ports, or any sound card ALSA sees.
2. Network: the board must reach the internet, for example through its Ethernet port.
3. Stations: three are built in (Radio Paradise, Radio Swiss Jazz, FIP). For your own list, create
   `stations.txt` in the board's folder (`/usr/local/mpy/fs` on the STM32MP135F-DK), one station per
   line:

   ```text
   My station|http://example.com/stream.mp3
   ```

   and copy it with `mpremote fs cp stations.txt :`.
4. Open `project.json` in LVGL MPY Studio, connect to the board and run it.

When a stream cannot be played (no network, no sound card, a wrong address) the status line shows
GStreamer's message.

## Files

| Module | What it does |
|---|---|
| `player` | the same `Player` as in `../mp3-player`, created with `live=True` |
| `radio_app` | stations, the screen, update from the page's timer |
