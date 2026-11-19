/* host check for hifi_eq.c: cc -I. hifi_eq.c test.c -lm && ./a.out (README.md) */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "esp_equalizer.h"
static double run(float *g, double f, int rate) {
	void *h = esp_equalizer_init(2, rate, 10, 0);
	for (int i = 0; i < 10; i++) { esp_equalizer_set_band_value(h, g[i], i, 0); esp_equalizer_set_band_value(h, g[i], i, 1); }
	static int16_t buf[2 * 48000];
	int N = rate; double amp = 8000, sin_=0, sout=0;
	for (int n = 0; n < N; n++) buf[2*n] = buf[2*n+1] = (int16_t)lrint(amp * sin(2*M_PI*f*n/rate));
	int16_t in[2*48000]; memcpy(in, buf, sizeof(int16_t)*2*N);
	for (int o = 0; o < N; o += 1024) { int len = (N - o < 1024 ? N - o : 1024); esp_equalizer_process(h, (unsigned char*)(buf + 2*o), len*4, rate, 2); }
	for (int n = N/2; n < N; n++) { sin_ += (double)in[2*n]*in[2*n]; sout += (double)buf[2*n]*buf[2*n]; }
	int same = 1; for (int n = 0; n < N; n++) if (buf[2*n] != buf[2*n+1]) same = 0;
	esp_equalizer_uninit(h);
	if (!same) printf("  channels differ!\n");
	return 10*log10(sout/sin_);
}
int main(void) {
	float flat[10] = {0}, one[10] = {0}, cut[10] = {0}, all[10];
	printf("flat 1k: %.3f dB\n", run(flat, 1000, 44100));
	one[5] = 6; printf("+6@1k at 1k: %.2f dB, at 125: %.2f, at 8k: %.2f\n", run(one,1000,44100), run(one,125,44100), run(one,8000,44100));
	cut[2] = -10; printf("-10@125 at 125: %.2f dB (48k: %.2f)\n", run(cut,125,44100), run(cut,125,48000));
	for (int i=0;i<10;i++) all[i]=3;
	printf("+3 all at 1k: %.2f dB\n", run(all,1000,44100));
	float hi[10]={0}; hi[9]=6; printf("+6@16k at 11025 (skipped) 1k: %.2f dB\n", run(hi,1000,11025));
	/* identity bit-exact */
	void *h = esp_equalizer_init(2, 44100, 10, 0); int16_t b[8]={1,-2,32767,-32768,5,6,7,8}, c[8]; memcpy(c,b,16);
	esp_equalizer_process(h,(unsigned char*)b,16,44100,2); printf("flat bit-exact: %s\n", memcmp(b,c,16)?"NO":"yes");
	printf("bad args: init %p, process %d\n", esp_equalizer_init(3,44100,10,0), esp_equalizer_process(h,(unsigned char*)b,16,44100,1));
	return 0;
}
