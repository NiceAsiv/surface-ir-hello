# surface-ir-hello

Native infrared face unlock for Surface Go on Linux. `surface-ir-hellod` drives the built-in OV7251 infrared camera and TPS68470 emitter, then uses OpenCV YuNet and SFace to match a local template. KDE connects through a PAM module and offers an optional C++/QML enrollment page.

The first-generation Surface Go has been tested on Arch Linux with the `linux-surface` kernel and KDE Plasma 6.8.3 (Wayland). The hardware code also recognizes Surface Go 2 and 3. Those models still need on-device testing.

## Build and install

The core uses C++20, native OpenCV, OpenSSL, libcurl, JsonCpp, toml++, and Linux-PAM. On Arch:

```sh
sudo pacman -S --needed gcc cmake ninja pkgconf opencv openssl curl jsoncpp tomlplusplus pam
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
sudo surface-ir hwcheck
sudo systemctl daemon-reload
sudo systemctl enable --now surface-ir-hellod.service
```

`hwcheck` exercises the camera and emitter and fetches the OpenCV Zoo models on first use. Each model is checked against its upstream SHA-256 digest before OpenCV loads it. The binaries use Linux media-controller, V4L2 and I²C interfaces directly.

Arch users can also run `makepkg` in `packaging/` to build `surface-ir-hello` and the optional `surface-ir-hello-kde` package. The CMake option `SURFACE_IR_KDE=ON` builds both components from source.

## Enroll and verify

In a local terminal, run:

```sh
sudo surface-ir enroll --preview --check
surface-ir verify
```

Look into the top-bezel camera from your usual unlock position. Slowly vary your angle and distance after each capture. The terminal shows a grayscale preview, a face box, capture progress, and the detector confidence. The separate live check compares a fresh image against the new samples using the same recognition threshold as the daemon.

`--check` prompts for Enter before the fresh capture. The CLI saves after the requested number of samples and the optional check succeed. Use `--append` to add to existing samples.

## KDE System Settings enrollment page

The optional native module adds **Infrared Face Unlock** under **Security & Privacy**. Its Polkit helper asks for administrator authorization from the active desktop session and enrolls the calling user's account.

For a source build, install extra-cmake-modules, Qt 6 Quick, KDE Frameworks 6 KCMUtils, CoreAddons and I18n, and a Polkit authentication agent. Add `-DSURFACE_IR_KDE=ON` to the build command above. With a package build, install `surface-ir-hello-kde` alongside the core package.

Reopen System Settings or run `kcmshell6 kcm_surfaceir`. Select **Enroll face**, follow the live preview for five captures, then select **Verify and save**. A passing check appends the samples.

## Enable the KDE Face button

For KDE Plasma 6.8 with the `kde-face` service compiled in, install the example PAM file on a clean setup and enable the user setting. Run `kwriteconfig6` as your desktop user. If `/etc/pam.d/kde-face` already exists, review it before applying the example.

```sh
sudo install -m 0644 /usr/share/surface-ir-hello/kde-face.pam /etc/pam.d/kde-face
kwriteconfig6 --file kscreenlockerrc --group Authenticators --key Face --type bool true
```

Press `Meta+L`, show the lock screen controls, choose **Face**, and look at the camera. Choose **Password** to return to password authentication. If you previously used `surface-ir pam-enable`, review the legacy hook in `/etc/pam.d/kde` before enabling the separate Face entry.

To remove the dedicated entry, first unlock the desktop using **Password**. Disable the setting and remove the PAM file if you installed it from this repository:

```sh
kwriteconfig6 --file kscreenlockerrc --group Authenticators --key Face --type bool false
sudo rm /etc/pam.d/kde-face
```

## Upgrading from the Python prototype

Install both native components if you already use the graphical enrollment page. Reload systemd and restart `surface-ir-hellod.service`. The native implementation reads the existing version-1 JSON templates in `/etc/surface-ir-hello/faces/` and the existing TOML settings. `surface-ir --version` identifies the native CLI.

## Privacy and authentication

Face templates are local, root-readable files under `/etc/surface-ir-hello/faces/`. The settings page and terminal render previews in memory. The program writes a camera image when you explicitly pass `hwcheck --save-debug`. Leave templates and debug images out of issue reports and patches.

The active IR check measures how scene lighting changes when the emitter is turned on. Its presentation-attack resistance has not been validated. Keep a working password and test it before relying on face unlock.

## Licenses

The project source is [MIT](LICENSE). Models are fetched separately from OpenCV Zoo. [YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) uses MIT and [SFace](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface) uses Apache-2.0.

Hardware research and references include [linux-surface](https://github.com/linux-surface/linux-surface) and [surface-go-ipu3-camera](https://github.com/pkarnakov/surface-go-ipu3-camera).
