/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Live finger-count classification:
 *   - Continuously captures RGB565 frames from an Arducam Mega
 *   - Converts each frame into the packed-0xRRGGBB float layout expected by
 *     the Edge Impulse image processing block
 *   - Runs run_classifier() on the Axon NPU (or CPU, depending on the EI
 *     deployment archive supplied via CONFIG_EDGE_IMPULSE_PATH)
 *   - Prints the top class and confidence to the console / RTT log
 *
 * The Edge Impulse zip must be a "Nordic Axon NPU Library" (or "Zephyr
 * library" for CPU fallback) export from Edge Impulse Studio. The training
 * impulse must use Image input at the resolution defined by EI_CLASSIFIER_INPUT_*
 * macros; this file uses those macros directly so it adapts automatically.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <edge-impulse-sdk/dsp/numpy_types.h>
#include <edge-impulse-sdk/classifier/ei_classifier_types.h>
/* model_metadata.h (which defines EI_CLASSIFIER_*) is pulled in transitively
 * via ei_classifier_types.h. */

LOG_MODULE_REGISTER(finger_counter, LOG_LEVEL_INF);

#define AE_WARMUP_FRAMES 4

#if EI_CLASSIFIER_INPUT_WIDTH != EI_CLASSIFIER_INPUT_HEIGHT
#warning "Non-square model input; the camera will be configured to width x height directly."
#endif

#define CAM_WIDTH  EI_CLASSIFIER_INPUT_WIDTH
#define CAM_HEIGHT EI_CLASSIFIER_INPUT_HEIGHT
#define FRAME_RGB565_BYTES ((CAM_WIDTH) * (CAM_HEIGHT) * 2)

/* The Image processing block packs one pixel into one float, so the DSP
 * input frame contains width*height floats. EI_CLASSIFIER_RAW_SAMPLE_COUNT
 * holds that value; EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME is unrelated (=1
 * for image impulses). */
BUILD_ASSERT(EI_CLASSIFIER_RAW_SAMPLE_COUNT == (CAM_WIDTH * CAM_HEIGHT),
	"Edge Impulse model expects one float per pixel - verify the impulse "
	"uses the Image input block (RGB).");

/* Captured RGB565 frame buffer (sensor-side). The packed-RGB888 floats EI
 * expects are computed on demand in get_features() so we don't need to keep
 * a second 64 KiB scratch buffer in RAM. */
static uint8_t  rgb565_frame[FRAME_RGB565_BYTES];

/* Snapshot of the first four floats EI actually pulled via get_features().
 * Logged once per inference so we can confirm the model is receiving varying
 * data, not a constant. */
static uint32_t ei_first_pixels[4];
static bool     ei_first_pixels_valid;

static const struct gpio_dt_spec led_active =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* See edge-impulse-sdk/classifier/ei_run_classifier.h. */
extern EI_IMPULSE_ERROR run_classifier(signal_t *signal,
				       ei_impulse_result_t *result, bool debug);

static int led_init(const struct gpio_dt_spec *spec)
{
	if (!gpio_is_ready_dt(spec)) {
		return -ENODEV;
	}
	return gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
}

static int capture_one_frame_into_buf(const struct device *video)
{
	size_t total = 0;
	int err;

	while (total < FRAME_RGB565_BYTES) {
		struct video_buffer *vbuf;

		err = video_dequeue(video, &vbuf, K_FOREVER);
		if (err == -EAGAIN) {
			continue;
		}
		if (err) {
			LOG_ERR("video_dequeue failed (err %d)", err);
			return err;
		}

		const size_t room = FRAME_RGB565_BYTES - total;
		const size_t chunk = MIN(vbuf->bytesused, room);

		memcpy(&rgb565_frame[total], vbuf->buffer, chunk);
		total += chunk;

		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
		(void)video_enqueue(video, vbuf);
	}

	return 0;
}

/* Decode one RGB565 pixel at index `p` into 0x00RRGGBB. */
static inline uint32_t rgb565_pixel(size_t p)
{
	const uint8_t hi = rgb565_frame[p * 2 + 0];
	const uint8_t lo = rgb565_frame[p * 2 + 1];
	const uint16_t pix = ((uint16_t)hi << 8) | lo;

	const uint8_t r5 = (pix >> 11) & 0x1F;
	const uint8_t g6 = (pix >> 5)  & 0x3F;
	const uint8_t b5 =  pix        & 0x1F;

	const uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
	const uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
	const uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

	return ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

/* EI signal_t.get_data callback: compute packed-RGB888 floats on the fly from
 * the captured RGB565 buffer. EI's Image processing block treats each float
 * as a uint32 with layout 0x00RRGGBB (see edge-impulse-sdk/classifier/
 * ei_run_dsp.h:extract_image_features). */
static int get_features(size_t offset, size_t length, float *out_ptr)
{
	for (size_t i = 0; i < length; i++) {
		const uint32_t packed = rgb565_pixel(offset + i);
		out_ptr[i] = (float)packed;
	}

	/* PROBE #2: remember the first four pixels EI actually fetched. The
	 * DSP block may request data in any order/chunk, so snapshot the
	 * lowest-offset fetch we see this inference. */
	if (offset == 0 && length >= 4 && !ei_first_pixels_valid) {
		ei_first_pixels[0] = (uint32_t)out_ptr[0];
		ei_first_pixels[1] = (uint32_t)out_ptr[1];
		ei_first_pixels[2] = (uint32_t)out_ptr[2];
		ei_first_pixels[3] = (uint32_t)out_ptr[3];
		ei_first_pixels_valid = true;
	}
	return 0;
}

/* PROBE #1: aggregate camera-side statistics over the freshly captured
 * RGB565 frame. If `meanRGB` and the corner samples don't change when the
 * lens is covered, the camera/SPI side is frozen. */
static void log_camera_stats(void)
{
	uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
	uint8_t  min_lum = 255, max_lum = 0;
	const size_t n = (size_t)CAM_WIDTH * (size_t)CAM_HEIGHT;

	for (size_t p = 0; p < n; p++) {
		const uint32_t px = rgb565_pixel(p);
		const uint8_t r = (px >> 16) & 0xFF;
		const uint8_t g = (px >> 8)  & 0xFF;
		const uint8_t b = (px)       & 0xFF;
		sum_r += r;
		sum_g += g;
		sum_b += b;
		const uint8_t lum = (uint8_t)((r + g + b) / 3);
		if (lum < min_lum) min_lum = lum;
		if (lum > max_lum) max_lum = lum;
	}

	const uint32_t TL = rgb565_pixel(0);
	const uint32_t TR = rgb565_pixel((size_t)(CAM_WIDTH - 1));
	const uint32_t BL = rgb565_pixel((size_t)(CAM_HEIGHT - 1) * CAM_WIDTH);
	const uint32_t BR = rgb565_pixel((size_t)CAM_WIDTH * CAM_HEIGHT - 1);
	const uint32_t C  = rgb565_pixel((size_t)(CAM_HEIGHT / 2) * CAM_WIDTH +
					 (size_t)(CAM_WIDTH / 2));

	LOG_INF("cam: meanRGB=(%u,%u,%u) lum=%u..%u",
		(unsigned)(sum_r / n), (unsigned)(sum_g / n),
		(unsigned)(sum_b / n), min_lum, max_lum);
	LOG_INF("     TL=%06x TR=%06x BL=%06x BR=%06x C=%06x",
		(unsigned)TL, (unsigned)TR, (unsigned)BL, (unsigned)BR,
		(unsigned)C);
}

#if EI_CLASSIFIER_OBJECT_DETECTION == 1

/* Object-detection (FOMO / YOLO / etc.) path: print every box above the
 * model's training-time confidence threshold. */
static void log_result(const ei_impulse_result_t *res)
{
	LOG_INF("=> %u detection(s)  (dsp %lld us, cls %lld us)",
		(unsigned)res->bounding_boxes_count,
		res->timing.dsp_us,
		res->timing.classification_us);

	for (uint32_t i = 0; i < res->bounding_boxes_count; i++) {
		const ei_impulse_result_bounding_box_t *b = &res->bounding_boxes[i];

		LOG_INF("   [%u] %-10s  %.3f  @ x=%u y=%u w=%u h=%u",
			(unsigned)i, b->label, (double)b->value,
			(unsigned)b->x, (unsigned)b->y,
			(unsigned)b->width, (unsigned)b->height);
	}
}

#else /* image classification */

static void log_result(const ei_impulse_result_t *res)
{
	size_t top_i = 0;
	float top_v = res->classification[0].value;

	for (size_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
		if (res->classification[i].value > top_v) {
			top_v = res->classification[i].value;
			top_i = i;
		}
	}

	LOG_INF("=> %-10s  %.3f  (dsp %lld us, cls %lld us)",
		res->classification[top_i].label,
		(double)top_v,
		res->timing.dsp_us,
		res->timing.classification_us);

	char line[160];
	int n = 0;

	for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
		int w = snprintk(line + n, sizeof(line) - n, "%s%s=%.2f",
				 (i == 0 ? "" : " "),
				 res->classification[i].label,
				 (double)res->classification[i].value);
		if (w <= 0 || (size_t)(n + w) >= sizeof(line)) {
			break;
		}
		n += w;
	}
	LOG_INF("   %s", line);
}

#endif /* EI_CLASSIFIER_OBJECT_DETECTION */

int main(void)
{
	int err;
	const struct device *video = DEVICE_DT_GET(DT_NODELABEL(arducam_mega));
	struct video_buffer *vbufs[2];
	struct video_format fmt = {
		.type = VIDEO_BUF_TYPE_INPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = CAM_WIDTH,
		.height = CAM_HEIGHT,
		.pitch = CAM_WIDTH * 2,
	};

	if (led_init(&led_active)) {
		LOG_ERR("LED init failed");
		return -1;
	}

	if (!device_is_ready(video)) {
		LOG_ERR("Arducam not ready");
		return -1;
	}

	err = video_set_format(video, &fmt);
	if (err) {
		LOG_ERR("video_set_format(%ux%u) failed (err %d) — does the "
			"camera support this resolution?",
			(unsigned)CAM_WIDTH, (unsigned)CAM_HEIGHT, err);
		return -1;
	}

	for (size_t i = 0; i < ARRAY_SIZE(vbufs); i++) {
		vbufs[i] = video_buffer_alloc(1024, K_NO_WAIT);
		if (!vbufs[i]) {
			LOG_ERR("video_buffer_alloc(%u) failed", (unsigned)i);
			return -1;
		}
		vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		(void)video_enqueue(video, vbufs[i]);
	}

	err = video_stream_start(video, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("video_stream_start failed (err %d)", err);
		return -1;
	}

	LOG_INF("Finger counter - model '%s', input %ux%u, %d label(s), %s",
		EI_CLASSIFIER_PROJECT_NAME,
		(unsigned)CAM_WIDTH, (unsigned)CAM_HEIGHT,
		EI_CLASSIFIER_LABEL_COUNT,
#if EI_CLASSIFIER_OBJECT_DETECTION == 1
		"object detection (FOMO/YOLO)"
#else
		"image classification"
#endif
		);

	LOG_INF("Discarding %u AE warm-up frames", AE_WARMUP_FRAMES);
	for (uint8_t i = 0; i < AE_WARMUP_FRAMES; i++) {
		err = capture_one_frame_into_buf(video);
		if (err) {
			return -1;
		}
	}
	LOG_INF("Starting live classification");

	while (true) {
		err = capture_one_frame_into_buf(video);
		if (err) {
			k_sleep(K_MSEC(100));
			continue;
		}

		(void)gpio_pin_toggle_dt(&led_active);

		log_camera_stats();
		ei_first_pixels_valid = false;

		signal_t signal = {
			.get_data = get_features,
			.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
		};
		ei_impulse_result_t result;

		EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
		if (r != EI_IMPULSE_OK) {
			LOG_ERR("run_classifier failed (err %d)", r);
			continue;
		}

		if (ei_first_pixels_valid) {
			LOG_INF("ei : first4=%06x %06x %06x %06x",
				(unsigned)ei_first_pixels[0],
				(unsigned)ei_first_pixels[1],
				(unsigned)ei_first_pixels[2],
				(unsigned)ei_first_pixels[3]);
		} else {
			LOG_WRN("ei : DSP did not fetch offset=0 — signal path broken");
		}

		log_result(&result);

		if (CONFIG_FINGER_COUNTER_INFERENCE_INTERVAL_MS > 0) {
			k_sleep(K_MSEC(CONFIG_FINGER_COUNTER_INFERENCE_INTERVAL_MS));
		}
	}

	return 0;
}
