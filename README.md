# qemu-expeed

> **Disclaimer**:
> This project is not, in any way whatsoever,
> affiliated to, endorsed by or supported by any of the following entities:
> Nikon Corporation, Socionext Inc., Sony Group Corporation,
> or the QEMU project.
> In case of legal questions about this project,
> [please contact the lead author here](mailto:legal@arnebaeyens.com).

> **Note**:
> NIKON, EXPEED, Picture Controls, VR, NEF and Z 6II
> are trademarks or registered trademarks of Nikon Corporation.

A QEMU emulator of the Nikon Z 6II digital camera
and its dual Expeed 6 SoC processors,
capable of running firmware version 1.70 with basic functionality operational
(live view, menu navigation, image playback, stills capture etc.).
You're welcome to use this emulator to study the camera's workings.

Some emulator screenshots, to provide an idea of the current state:

![Row of three emulator screenshot images, showing content similar to that a Nikon Z 6II camera shows on its rear display](
   https://files.arnebaeyens.com:2202/qemu-expeed/screenshot_row.webp)

To run the emulator, you'll need a Z 6II firmware update file version 1.70,
which you should bring yourself.
(For legal reasons, this repo does not include the Nikon firmware or parts thereof.)

You also find a DOOM port,
including the required cross-compilation set up.

## Functionality status
Last updated 2026-11-08

| Subject                 | Works ? |
| ----------------------- | ------- |
| photo live view | yes, including i-menu |
| buttons and dials | yes (through keyboard) |
| menus | yes |
| photo playback | yes |
| video live view | partially, has flickering |
| second Expeed develop | yes |
| card slots | XQD and SD |
| stills capture | silent shutter only |
| saving JPEGs and NEFs | yes |
| video recording | partially, file has empty frames |
| live view zoom | partially, zooming out may crash |
| Picture Controls | partially |
| white balance | partially (not accurate) |
| auto exposure | partially (not accurate) |
| audio | internal speaker |
| EVF | no |
| HDMI output | no |
| autofocus | no |
| lens | no |
| VR | no |


## Quick start
The following instructions assume you have a **Linux machine with Docker**.
You'll need about 6 GB of free disk space.
We currently do not support other OSes.

Steps:
1. In a directory of your choice, clone the repository
   and build and start the Docker container:
   ```sh
   git clone https://github.com/abaeyens/qemu-expeed.git
   cd qemu-expeed
   git submodule update --init --recursive

   ./create_dot_env.sh
   docker compose build
   docker compose run --rm app bash
   ```

2. In the container, build the QEMU emulator (this tends to take several minutes):
   ```sh
   ./qemu/apply_and_build.sh
   ```

3. Next, you'll need `Z6_2_0170.bin`, Nikon's firmware update 1.70 for the Z 6II.
   Download it from the Nikon site
   and save it in the root of the `qemu-expeed` directory.

4. In order to be runnable, we must extract the two Expeed executables
   from the firmware update package.
   Run
   ```sh
   ./scripts/unpack_fw_update.py Z6_2_0170.bin
   ```

5. To simulate the sensor, we need a raw picture (NEF).
   Arne provides one of his pictures (`canoe.NEF`), download it by running
   ```sh
   ./sample_pictures/fetch_pictures.sh
   ```

6. The camera's user settings start from the firmware's own defaults.
   Boot the emulator once to generate and save them (takes about one minute):
   ```sh
   ./scripts/emu/default_settings.sh
   ```

7. Finally, launch the emulator:
   ```sh
   ./scripts/emu/run_emulator.sh
   ```
   This command assembles bootable flash images from the unpacked firmware
   and synthetically generated configuration and settings regions.

8. A window opens; after about one minute it shows live view.
   **Wait about one minute more until the terminal prints in red**
   **the message "boot completed".**
   If the display remains black for more than five minutes,
   press `Ctrl + C` to exit and go back to step 7 to retry.

9. The emulator is now ready for interaction.
   The keyboard acts as the camera's buttons.
   - Press F1 in the window to print the button and dial to keyboard key mapping.
     Some important ones:
     MENU: `M`, shutter: `Shift` and `S`, i-menu: `I`, multi selector: arrow keys.
   - The emulator runs slower than real time,
     and as a result actions that are instant on the real camera
     tend to take several seconds on the emulated one.


## Quick start: DOOM in the emulator
The following steps assume that you're **inside the container**
and have a runnable emulator (see steps 1 to 5 above).

Steps:
1. The DOOM game consists of two parts: the game engine,
   already included in this repository as a submodule,
   and `DOOM1.WAD`, which contains the story and art.
   [Download the shareware DOOM 1.9 WAD from here](https://doomwiki.org/wiki/DOOM1.WAD)
   and save it in the root of the `qemu-expeed` directory.
2. Cross-compile DOOM with
   ```sh
   ./programs/doom/build.sh
   ```
   This builds `out/doom_port/doom.bin`.
3. And finally launch the emulator with DOOM:
   ```sh
   EXPEED_AUDIO_WAV=out/doom.wav ./scripts/emu/doom.sh
   ```
   This first boots the machine fully, then enters the camera's menu,
   and subsequently loads the DOOM executable into the emulator's RAM
   and starts it as its own T-Kernel task,
   which then loads `DOOM1.WAD` from the emulated XQD card.
   The virtual speaker output gets saved in `out/doom.wav`.


## Documentation
I write documentation mostly by hand, which means it takes time.
You can expect it to grow slowly over the coming months.
In the meantime, if you've a particular question, please just ask.


## Contributing
For large changes, please first open an issue
or [send an email](mailto:mail@arnebaeyens.com)
such that we can coordinate our efforts and ease the PR review.

Most of the current C code has been generated with LLMs and is slop.
I'll be cleaning this out part by part, which will take time.
The documentation, like this README, is mostly handwritten.
In your commit messages, please indicate which parts of the diff
where mostly or fully written with LLMs.
(See this repo's commit messages for examples.)
Please no "Co-authored-by \<LLM name\>"
stochastic parrot anthropomorphisation/advertisement.


## Licence
GPL-2.0-or-later: see [COPYING](COPYING) and the header of each file.

Concerning the Wonder Lantern project,
which also does reverse engineering on Nikon cameras:
this project is independent from Wonder Lantern
and did not copy any material or findings from them.
