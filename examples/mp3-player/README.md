# MP3 player

A touch MP3 player made with Copper and Code Studio and its MCP tools: a track list read from the
`music/` folder (titles and artists from the ID3 tags), play, pause, previous, next, a progress bar
and the volume. It shows how a GUI drives a Linux program: `player.py` runs GStreamer's
`gst-play-1.0` in the background with `linuxproc.Process` and controls it with its keyboard keys.

In the Studio simulator, where there is no Linux, a stand-in plays four silent demo tracks, so the
screen can be designed and tried without a board.

## On the board

1. Sound: a USB audio adapter on one of the USB-A ports, or any sound card ALSA sees. The
   STM32MP135F-DK has no audio output of its own.
2. Music: copy MP3 files to the board's music folder:

   ```sh
   mpremote connect /dev/ttyACM2 fs mkdir :music
   mpremote connect /dev/ttyACM2 fs cp song.mp3 :music/
   ```

3. Open `project.json` in Copper and Code Studio, connect to the board and run it.

Without a sound card, set `MPY_AUDIO_SINK=fakesink sync=true` in the environment of
`mpy-repl.service` (a systemd drop-in) to try everything but the sound.

## Files

| Module | What it does |
|---|---|
| `player` | `Player`: play, pause, next, previous, volume, position; one `gst-play-1.0` per track |
| `library` | the `.mp3` files of a folder, with their ID3v2 title and artist |
| `music_app` | the screen: list, labels, buttons, update from the page's timer |

The modules are inside `project.json`; the Studio shows them in its Modules panel.
