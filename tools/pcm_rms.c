/* SPDX-License-Identifier: MIT
 * Stream 48 kHz S16_LE stereo PCM into relative unweighted level measurements.
 * No recordings are retained and no sound-pressure calibration is implied.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
	unsigned char frame[4];
	uint64_t count = 0, clipped[2] = {0, 0};
	double mean[2] = {0, 0}, squares[2] = {0, 0}, peak[2] = {0, 0};
	size_t length;

	while ((length = fread(frame, 1, sizeof(frame), stdin)) == sizeof(frame)) {
		count++;
		for (unsigned int channel = 0; channel < 2; channel++) {
			unsigned int raw = frame[channel * 2] | (frame[channel * 2 + 1] << 8);
			int sample = raw >= 32768 ? (int)raw - 65536 : (int)raw;
			double delta = sample - mean[channel];
			mean[channel] += delta / count;
			squares[channel] += delta * (sample - mean[channel]);
			peak[channel] = fmax(peak[channel], fabs((double)sample));
			clipped[channel] += sample == -32768 || sample == 32767;
		}
	}
	if (length != 0 || ferror(stdin) || count == 0) {
		fprintf(stderr, "Expected nonempty S16_LE stereo PCM with complete frames\n");
		return 1;
	}
	printf("{\"frames\": %llu, \"seconds\": %.4f, \"channels\": [",
	       (unsigned long long)count, count / 48000.0);
	for (unsigned int channel = 0; channel < 2; channel++) {
		double rms = sqrt(fmax(squares[channel] / count, 0));
		printf("%s{\"rms_dbfs\": %.4f, \"peak_dbfs\": %.4f, "
		       "\"dc_mean\": %.4f, \"clipped_samples\": %llu}",
		       channel ? "," : "", 20 * log10(fmax(rms, 1e-12) / 32768),
		       20 * log10(fmax(peak[channel], 1e-12) / 32768),
		       mean[channel], (unsigned long long)clipped[channel]);
	}
	puts("]}");
	return 0;
}
