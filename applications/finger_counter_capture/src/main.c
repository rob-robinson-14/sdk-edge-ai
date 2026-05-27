/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Continuously capture Arducam frames and stream them out of uart20
 * (J-Link VCOM) as framed binary payloads. A host-side Python tool decodes
 * the frames into PNG files for upload to Edge Impulse Studio.
 *
 * Wire format (little-endian fields):
 *   offset  size  field
 *   0       4     magic    = 0xAA 0x55 0xAA 0x55
 *   4       2     width    (pixels)
 *   6       2     height   (pixels)
 *   8       1     pixfmt   (0x01 = RGB565, camera byte order)
 *   9       3     reserved (0)
 *   12      4     length   (bytes in payload)
 *   16      N     payload  (width * height * 2 bytes RGB565)
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(capture, LOG_LEVEL_INF);

#define CAM_WIDTH  128
#define CAM_HEIGHT 128
#define FRAME_RGB565_BYTES ((CAM_WIDTH) * (CAM_HEIGHT) * 2)

/* Continuous capture mode: the camera stream stays running from boot, so
 * AE/AGC converges once and stays converged. We discard the first few
 * frames at boot to let exposure settle, then emit every frame thereafter.
 * Frame rate is capped by 1 Mbaud UART throughput (~33 KB/frame => ~3 fps).
 * Set INTER_FRAME_DELAY_MS to slow it down further if 3 fps is too fast.
 */
#define AE_WARMUP_FRAMES     4
#define INTER_FRAME_DELAY_MS 0

#define FRAME_MAGIC_0 0xAA
#define FRAME_MAGIC_1 0x55
#define FRAME_MAGIC_2 0xAA
#define FRAME_MAGIC_3 0x55

#define PIXFMT_RGB565 0x01

static const struct gpio_dt_spec led_capture =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart20));

/* Static frame buffer: 32 KiB at 128x128 RGB565. */
static uint8_t frame_buf[FRAME_RGB565_BYTES];

static int led_init(const struct gpio_dt_spec *spec)
{
	int err;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("LED %s not ready", spec->port->name);
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("LED configure failed (err %d)", err);
		return err;
	}

	return 0;
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

		memcpy(&frame_buf[total], vbuf->buffer, chunk);
		total += chunk;

		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
		(void)video_enqueue(video, vbuf);
	}

	return 0;
}

static void uart_send_buf(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, buf[i]);
	}
}

static void send_frame(void)
{
	uint8_t header[16];

	header[0] = FRAME_MAGIC_0;
	header[1] = FRAME_MAGIC_1;
	header[2] = FRAME_MAGIC_2;
	header[3] = FRAME_MAGIC_3;
	sys_put_le16(CAM_WIDTH,  &header[4]);
	sys_put_le16(CAM_HEIGHT, &header[6]);
	header[8] = PIXFMT_RGB565;
	header[9] = 0;
	header[10] = 0;
	header[11] = 0;
	sys_put_le32(FRAME_RGB565_BYTES, &header[12]);

	uart_send_buf(header, sizeof(header));
	uart_send_buf(frame_buf, FRAME_RGB565_BYTES);
}

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

	if (led_init(&led_capture)) {
		return -1;
	}

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("Data UART not ready");
		return -1;
	}

	if (!device_is_ready(video)) {
		LOG_ERR("Arducam not ready");
		return -1;
	}

	err = video_set_format(video, &fmt);
	if (err) {
		LOG_ERR("video_set_format failed (err %d)", err);
		return -1;
	}

	for (size_t i = 0; i < ARRAY_SIZE(vbufs); i++) {
		vbufs[i] = video_buffer_alloc(1024, K_NO_WAIT);
		if (!vbufs[i]) {
			LOG_ERR("video_buffer_alloc failed for buf %u", i);
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

	LOG_INF("Discarding %u AE warm-up frames", AE_WARMUP_FRAMES);
	for (uint8_t i = 0; i < AE_WARMUP_FRAMES; i++) {
		err = capture_one_frame_into_buf(video);
		if (err) {
			LOG_ERR("warm-up frame %u failed (err %d)", i, err);
			return -1;
		}
	}

	LOG_INF("Continuous capture started.");

	uint32_t frame_count = 0;
	while (true) {
		err = capture_one_frame_into_buf(video);
		if (err) {
			LOG_ERR("capture failed (err %d)", err);
			k_sleep(K_MSEC(100));
			continue;
		}

		(void)gpio_pin_toggle_dt(&led_capture);
		send_frame();
		frame_count++;

		if ((frame_count % 10) == 0) {
			LOG_INF("Streamed %u frames", frame_count);
		}

		if (INTER_FRAME_DELAY_MS > 0) {
			k_sleep(K_MSEC(INTER_FRAME_DELAY_MS));
		}
	}

	return 0;
}
