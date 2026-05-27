.. _app_finger_counter:

Finger counter — live inference
###############################

.. contents::
   :local:
   :depth: 2

This application takes a trained Edge Impulse image classification model and
runs it live on the nRF54LM20DK + Arducam Mega, printing the predicted finger
count to the console at ~5–20 Hz depending on the model and Kconfig delay.

It is the deployment counterpart of :ref:`app_finger_counter_capture`. After
collecting training data with that app, training a model in Edge Impulse
Studio, and exporting it as a **Nordic Axon NPU Library**, drop the resulting
zip into this directory and rebuild.

Hardware
********

Same as ``finger_counter_capture`` / ``person_detection``: Arducam Mega 5MP on
SPIM22 (P1.4/5/6/7), 3V3 provided via Board Configurator.

Prerequisites
*************

1. Train a 6-class image classification model in
   `Edge Impulse Studio <https://studio.edgeimpulse.com>`_ using data captured
   with ``finger_counter_capture``.
2. Configure your impulse:

   * Input block: **Image**, 96x96 or 128x128, **RGB** (must match the
     resolution this app will request from the camera).
   * Processing block: **Image** (Color depth = RGB).
   * Learning block: **Classification**.

3. Train and verify the model on the Live Classification tab.
4. On the Deployment tab, choose **Nordic Axon NPU Library**, click **Build**,
   and download the resulting zip.

Drop-in steps
*************

#. Place the Edge Impulse zip in this application directory and rename it::

      mv ~/Downloads/your-project-id-nordic-axon-npu-library-v1.zip \
         applications/finger_counter/finger_counter.zip

   (or set ``CONFIG_EDGE_IMPULSE_PATH`` in ``prj.conf`` to a different name).

#. Pull the model-specific Kconfig values from the zip's ``conf_overlay.conf``.
   The simplest path is to feed it directly to the build::

      unzip -p applications/finger_counter/finger_counter.zip conf_overlay.conf \
          > /tmp/finger_counter_overlay.conf

      west build -b nrf54lm20dk/nrf54lm20b/cpuapp applications/finger_counter \
          -p -- -DEXTRA_CONF_FILE=/tmp/finger_counter_overlay.conf

   The overlay sets at least:

   * ``CONFIG_NRF_AXON_MODEL_NAME`` — the model identifier used inside the
     deployment, baked into the Axon header file names.
   * ``CONFIG_NRF_AXON_INTERLAYER_BUFFER_SIZE``
   * ``CONFIG_NRF_AXON_PSUM_BUFFER_SIZE``

   If the build fails with ``Allocated 0, need <N>`` after starting inference,
   bump the corresponding buffer Kconfig to ``<N>`` or higher.

#. Flash and watch the live output over RTT or the J-Link VCOM
   (``CONFIG_UART_CONSOLE`` is enabled in this app — logs go out ``uart20``)::

      west flash

      # console
      tio /dev/ttyACM1            # or screen / minicom / picocom

   You should see a stream like::

      [00:00:00.412] <inf> finger_counter: Finger counter — model finger_counter, input 96x96, 6 classes
      [00:00:00.413] <inf> finger_counter: Discarding 4 AE warm-up frames
      [00:00:00.530] <inf> finger_counter: Starting live classification
      [00:00:00.621] <inf> finger_counter: => two     0.962  (dsp 8 us, cls 6231 us)
      [00:00:00.622] <inf> finger_counter:    zero=0.01 one=0.02 two=0.96 three=0.00 four=0.00 five=0.01
      [00:00:00.843] <inf> finger_counter: => two     0.951  (dsp 8 us, cls 6228 us)
      ...

   LED0 toggles on every inference so you can confirm the loop is live from
   across the room.

Tuning
******

* ``CONFIG_FINGER_COUNTER_INFERENCE_INTERVAL_MS`` (Kconfig) — delay between
  inferences. Default ``200``. Set to ``0`` to run at full speed.
* For better accuracy/robustness: re-train with more data captured from the
  Arducam directly (using ``finger_counter_capture``); models trained on
  phone-camera images often regress on the embedded camera's colour balance
  and field of view.
* If a class is consistently misclassified, look at the per-class
  probabilities printed under the top-class line — that quickly tells you
  whether it's a confusion between two specific classes or a generalised
  collapse.

Application file overview
*************************

* ``src/main.c``           — capture + colour-convert + ``run_classifier()`` loop.
* ``CMakeLists.txt``       — pulls in the EI zip via ``FetchContent`` and the
  arducam driver from ``applications/person_detection``.
* ``Kconfig``              — knobs (model path, inference interval).
* ``prj.conf``             — EI SDK + Axon NPU + Video stack + LOG configuration.
* ``boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay`` — SPI22 + Arducam node + ``&axon`` enable.

Choosing the right impulse type
*******************************

The Edge Impulse Studio "Image" example will gladly let you build an
**Object detection (FOMO)** impulse from frames labelled "zero", "one",
... "five". Don't. For *counting fingers in a frame* there are two
sensible options:

* **Image classification with N classes** ("zero" … "five") — pick this
  if every captured frame is *one* whole hand pose. This is what the
  ``finger_counter_capture`` workflow naturally produces. Output is a
  probability vector across the classes; ``log_result()`` prints the top
  one.
* **FOMO with one "finger" class** — pick this if you want to draw
  boxes around individual fingers and *count* them post-detection. You'd
  then label each visible finger tip in your training images. The output
  is a list of bounding boxes; the count is ``bounding_boxes_count``.

What you *don't* want is **FOMO with class labels "zero"…"five"**: FOMO
labels each detection independently, so a single-hand frame can produce
multiple boxes labelled with *different* counts simultaneously, and the
confidence threshold has to be tuned globally. The author of this app
found out the hard way; the firmware now logs the model type at boot so
you notice immediately.

Tuned-buffer overlay
********************

The ``conf_overlay.conf`` shipped inside the EI zip sets
``CONFIG_NRF_AXON_INTERLAYER_BUFFER_SIZE`` to a generous round number
that may overshoot what the model actually needs. The model header
declares the minimum:

.. code-block:: console

   grep -E "MAX_IL_BUFFER_USED|MAX_PSUM_BUFFER_USED" \
       build/finger_counter/_deps/edge_impulse-src/nordic-axon-model/*.h

Setting the Kconfig values to those minimums frees BSS for the heap. On
a 128x128 RGB FOMO model this app drops the buffer from 262144 to
196608 (saves 64 KiB of static RAM) which makes the difference between
fitting and not fitting.

SDK patch required for RGB models (channel_cnt > 1)
****************************************************

**The upstream Nordic Axon inference wrapper in
``modules/edge-impulse-sdk-zephyr`` has a bug for multi-channel image
models.** ``inferencing_engines/nordic_axon.h`` sizes its int8 input
buffer as ``height * width`` only, but the conversion loop iterates
``matrix->rows * matrix->cols`` = ``height * width * channel_cnt``.
For an RGB (channels=3) model the loop walks 3x past the end of the
stack VLA, corrupts the stack, and faults with either "Unaligned memory
access" or "Stack overflow".

We've patched the local checkout to include ``channel_cnt`` in
``vector_size``. The patched file lives at::

    modules/edge-impulse-sdk-zephyr/edge-impulse-sdk/classifier/inferencing_engines/nordic_axon.h

If you ever ``west update`` you'll lose this patch and have to re-apply
it. A grayscale impulse avoids the bug entirely (``channel_cnt = 1``)
and uses ~3x less RAM at every stage — strongly preferred.

Memory budget cheat-sheet (128x128 model)
*****************************************

A working build for this hardware needs roughly:

============================== ====== =================================
Region                         Bytes  Notes
============================== ====== =================================
Axon interlayer buffer         ~190K  Set via ``CONFIG_NRF_AXON_INTERLAYER_BUFFER_SIZE``; check model header for actual.
Camera frame buffer (RGB565)    32K   ``rgb565_frame`` in this app.
EI DSP-expanded matrix         ~192K  RGB: ``W*H*3*sizeof(float)``. **Grayscale**: ``W*H*1*sizeof(float)`` = 64K.
Axon int8 input VLA            ~48K   On main stack. Grayscale: 16K.
Zephyr OS + drivers + EI BSS   ~40K
============================== ====== =================================

On a 511 KiB DK with an RGB 128x128 FOMO model the total is right on
the edge: this app reports ~52% RAM use after linking, and inference
peak heap touches ~96% during ``run_classifier()``. **Train as
grayscale** if you have any margin to lose.

Troubleshooting
***************

* **Build fails with ``ei_classifier_DSP_INPUT_FRAME_SIZE`` mismatch**:
  your impulse uses a different input layout than expected. The
  ``BUILD_ASSERT`` in ``src/main.c`` checks ``EI_CLASSIFIER_RAW_SAMPLE_COUNT
  == CAM_WIDTH * CAM_HEIGHT``. Use a square Image input block.
* **``video_set_format failed (err -22)``**: the Arducam Mega only
  supports specific resolutions (96x96, 128x128, 320x240, 320x320, ...).
  Retrain at one of those.
* **``ERR: Out of memory, can't allocate matrix_ptrs[0]``** followed by
  a heap double-free assert: picolibc's malloc arena ran out. **Don't**
  set ``CONFIG_HEAP_MEM_POOL_SIZE`` — that's the kernel heap, which EI
  doesn't use. Picolibc malloc auto-sizes to "remaining RAM" via
  ``CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1`` (Zephyr default). Free
  RAM by trimming static buffers or training a smaller model.
* **``USAGE FAULT — Unaligned memory access``** in ``run_nn_inference``:
  the upstream EI Nordic Axon wrapper bug for RGB models. See the SDK
  patch section above.
* **``USAGE FAULT — Stack overflow``**: the Axon engine's VLA outgrew
  the main stack. Bump ``CONFIG_MAIN_STACK_SIZE`` to at least
  ``W * H * channel_cnt + 8K``.
* **``Allocated X, need Y``** runtime: bump
  ``CONFIG_NRF_AXON_INTERLAYER_BUFFER_SIZE`` / ``_PSUM_BUFFER_SIZE`` to
  ``Y`` or higher.
* **Model returns every class at every frame with identical
  confidence**: the model is undertrained or the post-processing
  threshold is too low. Retrain with more data per class (>=100), more
  epochs, and verify with the **Live classification** tab in EI Studio
  before deploying.
* **CPU fallback**: if your model didn't compile for the Axon NPU,
  deploy as **Zephyr library** instead and set ``CONFIG_NRF_AXON=n`` in
  ``prj.conf``. Slower (50–500 ms / inference) but otherwise functional.
