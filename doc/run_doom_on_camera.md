# How to run DOOM on a real camera

> ⚠️ **WARNING**: EXECUTING THE REMAINDER OF THIS GUIDE
> MAY IRREVERSIBLY DAMAGE YOUR CAMERA AND/OR MAY VOID ITS WARRANTY.
> **EXCLUSIVELY CONTINUE AT YOUR OWN RISK.**

Steps:
1. If you in any way feel unconfortable while following this guide,
   immediately abort following this guide.
2. This guide assumes your camera is a Nikon Z 6II
   running unmodified Nikon firmware version 1.70.
   Other Nikon cameras neither other firmware versions are currently supported.
   **Do not try it on other models or versions.**
3. Make sure you're inside the container (see [README.md](README.md)).
   Then, cross-compile DOOM by running
   ```sh
   ./programs/doom/build.sh
   ```
4. If the build is successful, you should see something like
   ```
   built out/doom_port/doom.bin (381552 bytes, image + .bss to 0x909d3328), sha256 a0f125288da3305f
   ```
   If you're not seeing something like this, the build failed,
   and you must not continue.
5. [Download the shareware DOOM 1.9 WAD from here](https://doomwiki.org/wiki/DOOM1.WAD),
   save it on an XQD card in the root directory and put the XQD card in your camera.
   (Don't try with an SD card, it won't work.)
6. Turn on your camera and connect it to your computer over USB.
7. On your camera, press MENU and leave your camera in the menu screen.
8. At this point, some existing software (for example, your file manager)
   is probably trying to connect to the camera.
   **You must stop that other software,**
   **as otherwise it will probably interfere with this guide.**
9. Exit the container
   and run the script to load the DOOM binary onto the camera:
   ```sh
   ./scripts/usb/load_doom.py --load-sgi out/doom_port/doom.bin
   ```
10. Your camera should be running DOOM within two seconds of exiting the script.
    Adjust the game sound volume using the Fn1 and Fn2 buttons.
11. To exit DOOM, press DISP.
    It is recommended to thereafter disconnect the camera from USB
    and shut it down by its power switch.

In any further use of your camera,
your camera is expected to function exactly as before,
without any trace of DOOM,
as the script nor the DOOM executable
ought to have written anything to persistent memory.

> In case, somehow, your camera does not function as before,
> or you encounter any other unexpected events while following this guide,
> [please report this immediately to Arne](mailto:qemu-expeed@arnebaeyens.com).
