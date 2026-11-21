# Web radio

Internet radio on a touch screen, made with Copper and Code Studio and its MCP tools: a list of stations,
play and stop, previous and next station, the time on air and the volume, and a page that fills
the list from a catalogue of tens of thousands of stations by country, region, language and genre. It uses the same
`player.py` as the MP3 player: GStreamer's `gst-play-1.0` plays the `http://` or `https://` MP3
stream in the background, driven through `linuxproc.Process`.

In the Studio simulator, where there is no Linux, a stand-in pretends to play, so the screen can be
designed and tried without a board.

## On the board

1. Sound: a USB audio adapter on one of the USB-A ports, or any sound card ALSA sees.
2. Network: the board must reach the internet, for example through its Ethernet port.
3. Stations: the **+** button opens *Find stations*. Choose a country, one of its regions, a
   language and a genre (each can stay "Any"), then **Search**: the most listened stations that
   match, at most 300, from [Radio Browser](https://www.radio-browser.info), a free and open
   catalogue (no account, no key). **Add to my list** puts them after the stations you have,
   **Replace my list** keeps only them. Only streams the board plays are kept (MP3, AAC, Ogg); the
   board's fonts have no accents, so "Citta" stands for "Città", and names in other scripts are
   left out. The last search is remembered in `settings.json`, section `radio`.

   Three stations are built in (Radio Paradise, Radio Swiss Jazz, FIP). For your own list, create
   `stations.txt` in the board's folder (`/usr/local/mpy/fs` on the STM32MP135F-DK), one station per
   line:

   ```text
   My station|http://example.com/stream.mp3
   ```

   and copy it with `mpremote fs cp stations.txt :` (or with the FTP Server example). The Find page
   writes the same file, at most 500 stations.
4. Open `project.json` in Copper and Code Studio, connect to the board and run it.

When a stream cannot be played (no network, no sound card, a wrong address) the status line shows
GStreamer's message.

## Files

| Module | What it does |
|---|---|
| `player` | the same `Player` as in `../mp3-player`, created with `live=True` |
| `radio_app` | stations, both pages, update from the page's timer |
| `radio_find` | the Radio Browser catalogue: countries, regions, languages, search, in the background |
