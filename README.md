# surface-ir-hello

Infrared face unlock for Surface Go on Linux. A small PAM module connects KDE's lock screen to a local daemon that drives the built-in OV7251 infrared camera and loads OpenCV YuNet and SFace.

The first-generation Surface Go has been tested on Arch Linux with the `linux-surface` kernel, Python 3.14, OpenCV 5.1, and KDE Plasma 6.8.3 (Wayland). Surface Go 2 and 3 are recognized by the hardware code and still need on-device testing.

## Install

Install Python, NumPy, OpenCV with `FaceDetectorYN` and `FaceRecognizerSF`, `v4l-utils`, Linux-PAM headers, `curl`, a C compiler, and `make`. On Arch:

```sh
sudo pacman -S --needed python python-numpy python-opencv v4l-utils pam curl gcc make
make
sudo make install
sudo surface-ir hwcheck
sudo systemctl enable --now surface-ir-hellod.service
```

The installed command uses `/usr/bin/python3`, the same interpreter used to install the package. `hwcheck` tests the camera and emitter and fetches checksum-verified OpenCV Zoo models on first use.

## Enroll and verify

In a local terminal, run:

```sh
sudo surface-ir enroll --preview --check
surface-ir verify
```

Look into the top-bezel camera from your usual unlock position. Slowly vary your angle and distance after each capture. The terminal shows a grayscale preview, a bounding box when a face is detected, capture progress, and the detector confidence. Detector confidence measures face detection. The separate live check compares a fresh frame against the new samples using the same recognition threshold as the daemon.

`--check` prompts for Enter before the fresh capture. The CLI saves only after the requested sample count and optional check succeed. Use `--append` to add to existing samples.

## KDE System Settings enrollment page

The optional native module adds **Infrared Face Unlock** under **Security & Privacy**. It uses C++ and QML, and calls the existing Python capture and verification code. The Polkit helper asks for administrator authorization from the active desktop session and enrolls the calling user's account.

Install the core package first. Install CMake, extra-cmake-modules, Qt 6 Quick, KDE Frameworks 6 KCMUtils, CoreAddons and I18n, and a Polkit authentication agent. Then build the module:

```sh
cmake -S kcm -B build-kcm -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-kcm
sudo cmake --install build-kcm
```

Reopen System Settings or run `kcmshell6 kcm_surfaceir` to open the page directly. Select **Enroll face**, follow the live preview for five captures, then select **Verify and save**. A passing check appends the new samples. Configure the KDE Face button using the instructions below to use them on the lock screen.

## Enable the KDE Face button

For KDE Plasma 6.8 with the `kde-face` PAM service compiled in, install the example PAM file on a clean setup and enable the user setting. Run `kwriteconfig6` as your desktop user. If `/etc/pam.d/kde-face` already exists, review it before applying the example.

```sh
sudo install -m 0644 packaging/kde-face.pam /etc/pam.d/kde-face
kwriteconfig6 --file kscreenlockerrc --group Authenticators --key Face --type bool true
```

Press `Meta+L`, show the lock screen controls, choose **Face**, and look at the camera. Choose **Password** to return to password authentication. If you previously ran the project's `pam-enable` command, review the legacy hook in `/etc/pam.d/kde` before enabling the separate Face entry.

To remove the dedicated entry, first unlock the desktop and save **Password** as the lock screen method. Then disable the setting and remove the PAM file if you installed it from this repository:

```sh
kwriteconfig6 --file kscreenlockerrc --group Authenticators --key Face --type bool false
sudo rm /etc/pam.d/kde-face
```

## Privacy and authentication

Face templates are local, root-readable files under `/etc/surface-ir-hello/faces/`. Enrollment previews are rendered in the terminal. The program writes a camera image only when you explicitly pass `hwcheck --save-debug`. Leave templates and debug images out of issue reports and patches.

The active IR check measures how scene lighting changes when the emitter is turned on. It has no validated presentation-attack resistance rating. Keep a working password and test it before relying on face unlock.

## Licenses

The project source is [MIT](LICENSE). Models are fetched separately from OpenCV Zoo. [YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) uses MIT and [SFace](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface) uses Apache-2.0.

Hardware research and references include the [linux-surface project](https://github.com/linux-surface/linux-surface) and [surface-go-ipu3-camera](https://github.com/pkarnakov/surface-go-ipu3-camera).
