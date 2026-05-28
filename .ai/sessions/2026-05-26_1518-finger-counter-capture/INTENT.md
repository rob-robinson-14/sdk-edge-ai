# INTENT

Scaffold `applications/finger_counter_capture` as a stripped-down derivative
of `applications/person_detection`:

1. Re-use the arducam_mega Zephyr driver and its DT binding from
   `applications/person_detection` (via relative paths in CMake + DTS_ROOT)
   so we don't duplicate driver code.
2. Replace the NN inference path with a UART binary framing path on
   `uart20` (J-Link VCOM).
3. Move logging to RTT so the data UART carries only framed binary payloads.
4. Trigger one capture per Button 1 (sw0) press via a gpio interrupt that
   gives a semaphore the main loop waits on (same idiom as person_detection's
   k_timer).
5. Ship a Python host tool that consumes the framed stream and writes per-label
   PNGs, ready for `edge-impulse-uploader --label <label> --category split`.
