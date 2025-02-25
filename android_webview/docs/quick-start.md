# WebView quick start

[TOC]

*** promo
Googlers may wish to consult http://go/clank-webview for Google-specific
developer guides.
***

## Overview

This is not a thorough guide for how to build WebView, but is the **fastest**
way to get a local build of WebView up and running.

### Building for preview Android releases

Googlers should see internal instructions. External contributors should switch
to a public (and finalized) Android release (there's no workaround).

## System requirements, tools, etc.

See general Android instructions for:

* [System
  requirements](/docs/android_build_instructions.md#System-requirements)
* [Installing `depot_tools`](/docs/android_build_instructions.md#install)
* [Getting the code](/docs/android_build_instructions.md#get-the-code) **or**
  [converting a Linux
  checkout](/docs/android_build_instructions.md#converting-an-existing-linux-checkout)
* [Installing build
  dependencies](/docs/android_build_instructions.md#install-additional-build-dependencies)
  **and** [running hooks](/docs/android_build_instructions.md#run-the-hooks)

## Device setup

Easiest configuration is to choose a **Google APIs** emulator running **Android
N or higher**. See [Device Setup](./device-setup.md) for instructions.

*** promo
**Android O or higher** comes with troubleshooting tools, so that's highly
recommended.
***

## Setting up the build

Assuming you took the advice from above:

```gn
# Minimal
target_os = "android"
target_cpu = "x86"  # Assuming you chose an x86 emulator

# This package name is whitelisted for debuggable (userdebug) devices, and lets
# devs install a WebView provider without the usual signature checks. This only
# works on N+.
system_webview_package_name = "com.google.android.apps.chrome"
```

## Build WebView

```shell
$ autoninja -C out/Default system_webview_apk
```

## Install WebView and switch provider

```shell
# Install the APK
$ out/Default/bin/system_webview_apk install

# If you don't have `adb` in your path, you can source this file to use
# the copy from chromium's Android SDK.
$ source build/android/envsetup.sh

# Tell Android platform to load a WebView implementation from this APK.
# Use the package name from GN args.
$ adb shell cmd webviewupdate set-webview-implementation com.google.android.apps.chrome
```

## Start running an app

**That it!** Your APK should be installed and should be providing the WebView
implementation for all apps on the system. If you want to verify this, look at
the steps for [building the System WebView Shell](./webview-shell.md). The
version number in the shell's top bar should match the version number in your
local copy of `//chrome/VERSION`.

## Troubleshooting

If the install command succeeded but something else is wrong, the best way to
troubleshoot the problem is to query the state of the on-device
WebViewUpdateService:

```shell
# Only available on O+
$ adb shell dumpsys webviewupdate

Current WebView Update Service state
  Fallback logic enabled: true
  Current WebView package (name, version): (com.google.android.apps.chrome, 75.0.3741.0)
  Minimum WebView version code: 303012512
  Number of relros started: 1
  Number of relros finished: 1
  WebView package dirty: false
  Any WebView package installed: true
  Preferred WebView package (name, version): (com.google.android.apps.chrome, 75.0.3741.0)
  WebView packages:
    Valid package com.android.chrome (versionName: 58.0.3029.125, versionCode: 303012512, targetSdkVersion: 26) is  installed/enabled for all users
    Valid package com.google.android.webview (versionName: 58.0.3029.125, versionCode: 303012500, targetSdkVersion: 26) is NOT installed/enabled for all users
    Invalid package com.chrome.beta (versionName: 74.0.3729.23, versionCode: 372902311, targetSdkVersion: 28), reason: No WebView-library manifest flag
    Invalid package com.chrome.dev (versionName: 54.0.2840.98, versionCode: 284009811, targetSdkVersion: 24), reason: SDK version too low
    Invalid package com.chrome.canary (versionName: 75.0.3741.0, versionCode: 374100010, targetSdkVersion: 25), reason: SDK version too low
    Valid package com.google.android.apps.chrome (versionName: 75.0.3741.0, versionCode: 2, targetSdkVersion: 28) is  installed/enabled for all users
```

### Invalid package ... No WebView-library manifest flag

This APK does not contain a WebView implementation. Make sure you're building
`system_webview_apk`.

### Invalid package ... Version code too low

This shouldn't happen for userdebug builds. If it does, add this GN arg:

```gn
# Any number >= "Minimum WebView version code":
android_override_version_code = "987654321"
```

### Invalid package ... SDK version too low

The targetSdkVersion of your APK is too low (it must be >= the platform's API
level). This shouldn't happen for local builds using tip-of-tree chromium on
public OS versions (see [note](#Building-for-preview-Android-releases)).

*** note
**Note:** we only support local development using the latest revision of the
master branch. Checkout out release branches introduces a lot of complexity, and
it might not even be possible to build WebView for your device.
***

### Valid package ... is  installed/enabled for all users

This is the correct state. If this is not the "preferred WebView package" or the
"current WebView package", call `set-webview-implementation` again.

### My package isn't in the list

Either your package didn't install (see below) or you chose a package which is
not in the WebView provider whitelist. Double-check the package name in your GN
args. If you're on AOSP (any OS level), choose
`"com.android.webview"`. If you're on L-M, choose
`"com.android.google.webview"`. In either case, you'll likely need to [remove
the preinstalled WebView
APK](/android_webview/tools/remove_preinstalled_webview.py).

### WebView shell doesn't show the correct version

Check the "Current WebView package" in the dumpsys output. You're probably
hitting one of the cases above.

### I couldn't install the APK

This could fail for an even wider variety of reasons than already listed. Please
[reach out to the
team](https://groups.google.com/a/chromium.org/forum/#!forum/android-webview-dev).

### I couldn't **build** the APK

Try building Chromium. If that doesn't work, please reach out to [the chromium
team](https://groups.google.com/a/chromium.org/forum/#!forum/chromium-dev) for
general guidance. If `system_webview_apk` is the only troublesome target, please
reach out to the WebView team (see previous section).

## What if I didn't follow these instructions exactly?

**Proceed at your own risk.** Building and installing WebView is, for a variety
of reasons, complex. If you've deviated from **any** of these instructions (and
don't know what you're doing) there's a good chance of making mistakes (some of
which don't have any error messages).

If you can't follow the quick start guide for some reason, please consult our
[public](https://www.chromium.org/developers/how-tos/build-instructions-android-webview)
or [internal](http://go/clank-webview) instructions.
