package com.sthrecompiled.shadow;

// The port's side of the Java calls in native/main.cpp and native/input.cpp. Android hands the app controllers SDL
// does not take (a phone's game service may present one as a virtual device); their keys and sticks come through here
// as port 0's, and the activity calls back into the C++ side from its own methods, which host::CallActivity finds.
final class NativeBridge {
    private NativeBridge() {}

    static native void looseKey(int keyCode, boolean down);

    static native void looseAxes(float lx, float ly, float rx, float ry, float lt, float rt, float hx, float hy);

    static native void looseRelease();
}
