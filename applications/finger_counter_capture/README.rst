.. _app_finger_counter_capture:

Finger counter — data capture
#############################

.. contents::
   :local:
   :depth: 2

This application turns the nRF54LM20DK + Arducam Mega 5MP into a labelled
image collector for training a 6-class "how many fingers are up" model on
`Edge Impulse Studio`_. The trained model is then deployed back to the same
board on the Axon NPU.

It is intentionally a stripped-down sibling of
:ref:`applications/person_detection <app_person_detection>` — same camera, same
GPIO pins, no on-device inference.

Hardware
********

Same as ``person_detection``:

.. list-table::
   :header-rows: 1

   * - Description
     - Arducam Mega Pin
     - nRF54LM20DK Pin
   * - Power supply (3.3 V)
     - ``VCC``
     - ``VDD:IO`` (configure via Board Configurator)
   * - Ground
     - ``GND``
     - ``GND``
   * - Chip select
     - ``CS``
     - ``P1.7``
   * - SPI MOSI
     - ``MOSI``
     - ``P1.6``
   * - SPI MISO
     - ``MISO``
     - ``P1.5``
   * - SPI Clock
     - ``SCK``
     - ``P1.4``

How it works
************

* Logging goes to **RTT** so the J-Link VCOM (``uart20``) carries pure binary
  frames — view logs with ``JLinkRTTLogger`` or ``JLinkRTTViewer``.
* The camera streams **continuously from boot** at the UART-limited rate of
  approximately **2 fps** (1 Mbaud / 33 KB per frame). No button presses
  required — just point and let it run.
* On boot, the app discards ``AE_WARMUP_FRAMES`` (default 4) frames to let
  the Arducam's auto-exposure / auto-gain converge, then streams every
  subsequent frame.
* LED0 toggles on each frame so you can confirm the stream is alive from
  across the room.
* The host-side Python tool (``tools/capture_host.py``) demuxes frames from
  the serial stream and writes one PNG per frame into ``out_dir/<label>/``,
  exiting after ``--count`` frames are saved.

Wire format
===========

Every frame is a 16-byte header followed by the RGB565 payload. All multi-byte
fields are little-endian.

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Field
   * - 0
     - 4
     - ``magic`` = ``AA 55 AA 55``
   * - 4
     - 2
     - ``width`` (pixels)
   * - 6
     - 2
     - ``height`` (pixels)
   * - 8
     - 1
     - ``pixfmt`` = ``0x01`` (RGB565, camera-native big-endian per pixel)
   * - 9
     - 3
     - reserved (0)
   * - 12
     - 4
     - ``length`` (bytes in payload)
   * - 16
     - N
     - payload (``width * height * 2`` bytes RGB565)

Building and running
********************

#. Build and flash:

   .. code-block:: console

      west build -b nrf54lm20dk/nrf54lm20b/cpuapp applications/finger_counter_capture -p
      west flash

#. (Optional) attach RTT to watch the firmware log:

   .. code-block:: console

      nrfutil device rtt --snr <jlink-serial> --channel 0

#. Install the host tool requirements once:

   .. code-block:: console

      python -m pip install -r applications/finger_counter_capture/tools/requirements.txt

#. Identify the J-Link VCOM serial device. On Linux it usually appears as
   ``/dev/ttyACM1`` (``ttyACM0`` is the J-Link control endpoint). On macOS it
   is ``/dev/tty.usbmodem*``; on Windows it is a ``COMx`` port.

#. Capture data per class. Repeat for each label ``zero``, ``one``, …, ``five``:

   .. code-block:: console

      python applications/finger_counter_capture/tools/capture_host.py \
          --port /dev/ttyACM1 --label two --out images/ --count 100

   The firmware streams continuously at ~2 fps, so 100 images takes about
   one minute. **While the tool is running, keep showing the camera the
   target gesture from many angles, distances, and orientations** — every
   captured frame goes into your dataset. Move slowly so you don't blur frames.

   To collect all six classes in one go::

      for L in zero one two three four five; do
          read -p "Class '$L': position your hand and press Enter to start (~1 min)..."
          python applications/finger_counter_capture/tools/capture_host.py \
              --port /dev/ttyACM1 --label "$L" --out images/ --count 100
      done

#. Upload to Edge Impulse Studio per class:

   .. code-block:: console

      edge-impulse-uploader --category split --label zero  images/zero/*.png
      edge-impulse-uploader --category split --label one   images/one/*.png
      edge-impulse-uploader --category split --label two   images/two/*.png
      edge-impulse-uploader --category split --label three images/three/*.png
      edge-impulse-uploader --category split --label four  images/four/*.png
      edge-impulse-uploader --category split --label five  images/five/*.png

   ``--category split`` lets Edge Impulse pick the train/test split automatically.

Troubleshooting
***************

If the captured PNGs look like "lines of colour" or are very dark / mostly
uniform:

* Check that the firmware is the latest version with ``AE_WARMUP_FRAMES``
  set to >= 2. Earlier builds that sent the very first frame after
  ``video_stream_start`` produced underexposed garbage because the Arducam's
  AE/AGC had not converged.
* Use the included diagnostic helpers in ``tools/`` to verify the decode is
  the right pixel format:

  * ``diagnose_offline.py`` takes a raw header+payload dump produced by a
    small UART read and emits ~15 PNGs decoded under different
    interpretations (RGB565 BE/LE, BGR565 BE/LE, YUV422, rotations, etc.).
    Open the variants and identify which one matches the scene.
  * Capture a raw stream with::

      python3 -u -c '
      import serial, time
      s = serial.Serial("/dev/ttyACM1", 1_000_000, timeout=0.2)
      s.reset_input_buffer()
      buf = bytearray()
      end = time.monotonic() + 30
      while time.monotonic() < end:
          buf.extend(s.read(16384))
          if len(buf) >= 16 + 128*128*2 + 4: break
      open("/tmp/diag_raw.bin","wb").write(bytes(buf))'

    Press Button 1 once during the 30s window, then::

      python3 tools/diagnose_offline.py /tmp/diag_raw.bin /tmp/finger_diag

Next step
*********

Once you have a trained Axon-NPU model from Edge Impulse, build the inference
firmware by branching :ref:`hello_ei_sample` and porting in the camera capture
loop from this app (the ``capture_one_frame()`` + ``video_set_format()`` plumbing).
See the Edge Impulse integration guide :ref:`edge_impulse_integration` for the
``CONFIG_EDGE_IMPULSE_PATH`` and Axon Kconfig wiring.
