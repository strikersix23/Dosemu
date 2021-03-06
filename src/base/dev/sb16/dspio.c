/*
 *  Copyright (C) 2006 Stas Sergeev <stsp@users.sourceforge.net>
 *
 * The below copyright strings have to be distributed unchanged together
 * with this file. This prefix can not be modified or separated.
 */

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Purpose:
 *   DSP I/O layer, DMA and DAC handling. Also MIDI sometimes.
 *   Currently used by SB16, but may be used with anything, e.g. GUS.
 *
 * Author: Stas Sergeev.
 *
 */

#include "emu.h"
#include "timers.h"
#include "sound/sound.h"
#include "sound/sndpcm.h"
#include "sound/midi.h"
#include "adlib.h"
#include "dmanew.h"
#include "sb16.h"
#include "dspio.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <semaphore.h>
#include <pthread.h>

static sem_t syn_sem;
static pthread_t syn_thr;

#define DAC_BASE_FREQ 5625
#define PCM_MAX_BUF 512

struct dspio_dma {
    int running;
    int num;
    int broken_hdma;
    int rate;
    int is16bit;
    int stereo;
    int samp_signed;
    int input;
    int silence;
    int dsp_fifo_enabled;
    hitimer_t time_cur;
};

struct dspio_state {
    double input_time_cur, output_time_cur, midi_time_cur;
    int dma_strm, dac_strm;
    int input_running, output_running, dac_running, speaker;
#define DSP_FIFO_SIZE 64
    struct rng_s fifo_in;
    struct rng_s fifo_out;
#define DSP_OUT_FIFO_TRIGGER 32
#define DSP_IN_FIFO_TRIGGER 32
#define MIDI_FIFO_SIZE 32
    struct rng_s midi_fifo_in;
    struct rng_s midi_fifo_out;
    struct dspio_dma dma;
};

#define DSPIO ((struct dspio_state *)dspio)

static void *synth_thread(void *arg);

static void dma_get_silence(int is_signed, int is16bit, void *ptr)
{
    if (is16bit) {
	Bit16u *tmp16 = ptr;
	*tmp16 = is_signed ? 0 : 0x8000;
    } else {
	Bit8u *tmp8 = ptr;
	*tmp8 = is_signed ? 0 : 0x80;
    }
}

void dspio_toggle_speaker(void *dspio, int on)
{
    if (!on && DSPIO->speaker) {
	if (DSPIO->output_running) {
	    pcm_flush(DSPIO->dma_strm);
	}
	if (DSPIO->dac_running) {
	    pcm_flush(DSPIO->dac_strm);
	    DSPIO->dac_running = 0;
	}
    }
    DSPIO->speaker = on;
}

int dspio_get_speaker_state(void *dspio)
{
    return DSPIO->speaker;
}

void dspio_write_midi(void *dspio, Bit8u value)
{
    rng_put(&DSPIO->midi_fifo_out, &value);

    run_new_sb();
}

static int dspio_out_fifo_len(struct dspio_dma *dma)
{
    return dma->dsp_fifo_enabled ? DSP_OUT_FIFO_TRIGGER : 2;
}

static int dspio_in_fifo_len(struct dspio_dma *dma)
{
    return dma->dsp_fifo_enabled ? DSP_IN_FIFO_TRIGGER : 2;
}

static int dspio_output_fifo_filled(struct dspio_state *state)
{
    return rng_count(&state->fifo_out) >= dspio_out_fifo_len(&state->dma);
}

static int dspio_input_fifo_filled(struct dspio_state *state)
{
    return rng_count(&state->fifo_in) >= dspio_in_fifo_len(&state->dma);
}

static int dspio_input_fifo_empty(struct dspio_state *state)
{
    return !rng_count(&state->fifo_in);
}

static int dspio_midi_output_empty(struct dspio_state *state)
{
    return !rng_count(&state->midi_fifo_out);
}

static Bit8u dspio_get_midi_data(struct dspio_state *state)
{
    Bit8u val;
    int ret = rng_get(&state->midi_fifo_out, &val);
    assert(ret == 1);
    return val;
}

Bit8u dspio_get_midi_in_byte(void *dspio)
{
    Bit8u val;
    int ret = rng_get(&DSPIO->midi_fifo_in, &val);
    assert(ret == 1);
    return val;
}

void dspio_put_midi_in_byte(void *dspio, Bit8u val)
{
    rng_put_const(&DSPIO->midi_fifo_in, val);
}

int dspio_get_midi_in_fillup(void *dspio)
{
    return rng_count(&DSPIO->midi_fifo_in);
}

void dspio_clear_midi_in_fifo(void *dspio)
{
    rng_clear(&DSPIO->midi_fifo_in);
}

static int dspio_get_dma_data(struct dspio_state *state, void *ptr, int is16bit)
{
    if (sb_get_dma_data(ptr, is16bit))
	return 1;
    if (rng_count(&state->fifo_in)) {
	if (is16bit) {
	    rng_get(&state->fifo_in, ptr);
	} else {
	    Bit16u tmp;
	    rng_get(&state->fifo_in, &tmp);
	    *(Bit8u *) ptr = tmp;
	}
	return 1;
    }
    error("SB: input fifo empty\n");
    return 0;
}

static void dspio_put_dma_data(struct dspio_state *state, void *ptr, int is16bit)
{
    if (dspio_output_fifo_filled(state)) {
	error("SB: output fifo overflow\n");
	return;
    }
    if (is16bit) {
	rng_put(&state->fifo_out, ptr);
    } else {
	Bit16u tmp = *(Bit8u *) ptr;
	rng_put(&state->fifo_out, &tmp);
    }
}

static int dspio_get_output_sample(struct dspio_state *state, void *ptr,
	int is16bit)
{
    if (rng_count(&state->fifo_out)) {
	if (is16bit) {
	    rng_get(&state->fifo_out, ptr);
	} else {
	    Bit16u tmp;
	    rng_get(&state->fifo_out, &tmp);
	    *(Bit8u *) ptr = tmp;
	}
	return 1;
    }
    return 0;
}

static int dspio_put_input_sample(struct dspio_state *state, void *ptr,
	int is16bit)
{
    int ret;
    if (!sb_input_enabled())
	return 0;
    if (dspio_input_fifo_filled(state)) {
	S_printf("SB: ERROR: input fifo overflow\n");
	return 0;
    }
    if (is16bit) {
	ret = rng_put(&state->fifo_in, ptr);
    } else {
	Bit16u tmp = *(Bit8u *) ptr;
	ret = rng_put(&state->fifo_in, &tmp);
    }
    return ret;
}

void dspio_clear_fifos(void *dspio)
{
    rng_clear(&DSPIO->fifo_in);
    rng_clear(&DSPIO->fifo_out);
    DSPIO->dma.dsp_fifo_enabled = 1;
}

void *dspio_init(void)
{
    struct dspio_state *state;
    state = malloc(sizeof(struct dspio_state));
    if (!state)
	return NULL;
    memset(&state->dma, 0, sizeof(struct dspio_dma));
    state->input_running =
	state->output_running = state->dac_running = state->speaker = 0;
    state->dma.dsp_fifo_enabled = 1;
    pcm_init();
    state->dac_strm = pcm_allocate_stream(1, "SB DAC");
    pcm_set_flag(state->dac_strm, PCM_FLAG_RAW);
    state->dma_strm = pcm_allocate_stream(2, "SB DMA");

    rng_init(&state->fifo_in, DSP_FIFO_SIZE, 2);
    rng_init(&state->fifo_out, DSP_FIFO_SIZE, 2);
    rng_init(&state->midi_fifo_in, MIDI_FIFO_SIZE, 1);
    rng_init(&state->midi_fifo_out, MIDI_FIFO_SIZE, 1);

    adlib_init();
    midi_init();

    sem_init(&syn_sem, 0, 0);
    pthread_create(&syn_thr, NULL, synth_thread, NULL);

    return state;
}

void dspio_reset(void *dspio)
{
    pcm_reset();
    midi_reset();
}

void dspio_done(void *dspio)
{
    pthread_cancel(syn_thr);
    pthread_join(syn_thr, NULL);
    sem_destroy(&syn_sem);

    pcm_done();
    midi_done();

    rng_destroy(&DSPIO->fifo_in);
    rng_destroy(&DSPIO->fifo_out);
    rng_destroy(&DSPIO->midi_fifo_in);
    rng_destroy(&DSPIO->midi_fifo_out);

    free(dspio);
}

void dspio_stop_midi(void *dspio)
{
    DSPIO->midi_time_cur = GETusTIME(0);
    midi_stop();
}

Bit32u dspio_get_midi_in_time(void *dspio)
{
    Bit32u delta = GETusTIME(0) - DSPIO->midi_time_cur;
    S_printf("SB: midi clock, delta=%i\n", delta);
    return delta;
}

static void dspio_start_output(struct dspio_state *state)
{
    if (state->output_running)
	return;
    S_printf("SB: starting output\n");
    /* We would need real time here, but the HACK is to use stream
     * timestamp instead.
     * That compensates the hack of dspio_process_dma() */
    state->output_time_cur = pcm_calc_tstamp(state->dma.rate, state->dma_strm);
    state->output_running = 1;
}

static void dspio_stop_output(struct dspio_state *state)
{
    if (!state->output_running)
	return;
    S_printf("SB: stopping output\n");
    if (state->speaker)
	pcm_flush(state->dma_strm);
    state->output_running = 0;
}

static void dspio_start_input(struct dspio_state *state)
{
    if (state->input_running)
	return;
    S_printf("SB: starting input\n");
    /* TODO */
    state->input_time_cur = GETusTIME(0);
    state->input_running = 1;
}

static void dspio_stop_input(struct dspio_state *state)
{
    if (!state->input_running)
	return;
    S_printf("SB: stopping input\n");
    /* TODO */
    state->input_running = 0;
}

static int do_run_dma(struct dspio_state *state)
{
    Bit8u dma_buf[2];
    struct dspio_dma *dma = &state->dma;

    dma_get_silence(dma->samp_signed, dma->is16bit, dma_buf);
    if (!dma->silence) {
	if (dma->input)
	    dspio_get_dma_data(state, dma_buf, dma->is16bit);
	if (dma_pulse_DRQ(dma->num, dma_buf) != DMA_DACK) {
	    S_printf("SB: DMA %i doesn't DACK!\n", dma->num);
	    return 0;
	}
	if (dma->broken_hdma) {
	    if (dma_pulse_DRQ(dma->num, dma_buf + 1) != DMA_DACK) {
		S_printf("SB: DMA (broken) %i doesn't DACK!\n", dma->num);
		return 0;
	    }
	}
    }
    if (!dma->input)
	dspio_put_dma_data(state, dma_buf, dma->is16bit);
    return 1;
}

static int dspio_run_dma(struct dspio_state *state)
{
#define DMA_TIMEOUT_US 100000
    int ret;
    struct dspio_dma *dma = &state->dma;
    hitimer_t now = GETusTIME(0);
    sb_dma_processing();	// notify that DMA busy
    ret = do_run_dma(state);
    if (ret) {
	sb_handle_dma();
	dma->time_cur = now;
    } else if (now - dma->time_cur > DMA_TIMEOUT_US) {
	S_printf("SB: Warning: DMA busy for too long, releasing\n");
//	error("SB: DMA timeout\n");
	sb_handle_dma_timeout();
    }
    return ret;
}

static void get_dma_params(struct dspio_dma *dma)
{
    int dma_16bit = sb_dma_16bit();
    int dma_num = dma_16bit ? sb_get_hdma_num() : sb_get_dma_num();
    int broken_hdma = (dma_16bit && dma_num == -1);
    if (broken_hdma) {
	dma_num = sb_get_dma_num();
	S_printf("SB: Warning: HDMA is broken, using 8-bit DMA channel %i\n",
	     dma_num);
    }

    dma->num = dma_num;
    dma->is16bit = dma_16bit;
    dma->broken_hdma = broken_hdma;
    dma->rate = sb_get_dma_sampling_rate();
    dma->stereo = sb_dma_samp_stereo();
    dma->samp_signed = sb_dma_samp_signed();
    dma->input = sb_dma_input();
    dma->silence = sb_dma_silence();
    dma->dsp_fifo_enabled = sb_fifo_enabled();
}

static int dspio_fill_output(struct dspio_state *state)
{
    int dma_cnt = 0;
    while (state->dma.running && !dspio_output_fifo_filled(state)) {
	if (!dspio_run_dma(state))
	    break;
	dma_cnt++;
    }
#if 0
    if (!state->output_running && !sb_output_fifo_empty())
#else
    /* incomplete fifo needs a timeout, so lets not deal with it at all.
     * Instead, deal with the filled fifo only. */
    if (dspio_output_fifo_filled(state))
#endif
	dspio_start_output(state);
    return dma_cnt;
}

static int dspio_drain_input(struct dspio_state *state)
{
    int dma_cnt = 0;
    while (state->dma.running && !dspio_input_fifo_empty(state)) {
	if (!dspio_run_dma(state))
	    break;
	dma_cnt++;
    }
    return dma_cnt;
}

void dspio_start_dma(void *dspio)
{
    int dma_cnt = 0;
    DSPIO->dma.running = 1;
    DSPIO->dma.time_cur = GETusTIME(0);
    get_dma_params(&DSPIO->dma);

    if (DSPIO->dma.input) {
	dspio_start_input(DSPIO);
    } else {
	dma_cnt = dspio_fill_output(DSPIO);
	if (DSPIO->dma.running && dspio_output_fifo_filled(DSPIO))
	    S_printf("SB: Output filled, processed %i DMA cycles\n",
		     dma_cnt);
	else
	    S_printf("SB: Output fillup incomplete (%i %i %i)\n",
		     DSPIO->dma.running, DSPIO->output_running, dma_cnt);
    }
}

void dspio_stop_dma(void *dspio)
{
    dspio_stop_input(DSPIO);
    DSPIO->dma.running = 0;
}

static int calc_nframes(struct dspio_state *state,
	hitimer_t time_beg, hitimer_t time_dst)
{
    int nfr;
    if (state->dma.rate) {
	nfr = (time_dst - time_beg) / pcm_frame_period_us(state->dma.rate);
	if (nfr < 0)	// happens because of get_stream_time() hack
	    nfr = 0;
	if (nfr > PCM_MAX_BUF)
	    nfr = PCM_MAX_BUF;
    } else {
	nfr = 1;
    }
    return nfr;
}

static void dspio_process_dma(struct dspio_state *state)
{
    int dma_cnt, nfr, in_fifo_cnt, out_fifo_cnt, i, j;
    unsigned long long time_dst;
    sndbuf_t buf[PCM_MAX_BUF][SNDBUF_CHANS];

    dma_cnt = in_fifo_cnt = out_fifo_cnt = 0;

    time_dst = GETusTIME(0);
    if (state->dma.running) {
	state->dma.stereo = sb_dma_samp_stereo();
	state->dma.rate = sb_get_dma_sampling_rate();
	state->dma.samp_signed = sb_dma_samp_signed();
	state->dma.dsp_fifo_enabled = sb_fifo_enabled();
    }

    if (state->output_running)
	nfr = calc_nframes(state, state->output_time_cur, time_dst);
    else
	nfr = 0;
    for (i = 0; i < nfr; i++) {
	for (j = 0; j < state->dma.stereo + 1; j++) {
	    if (state->dma.running && !dspio_output_fifo_filled(state)) {
		if (!dspio_run_dma(state))
		    break;
		dma_cnt++;
	    }
	    if (!dspio_get_output_sample(state, &buf[i][j],
		    state->dma.is16bit))
		break;
	}
	if (j != state->dma.stereo + 1)
	    break;
	out_fifo_cnt++;
    }
    if (out_fifo_cnt) {
	if (state->dma.rate) {
	    if (state->speaker && !state->dma.silence) {
		pcm_write_interleaved(buf, out_fifo_cnt, state->dma.rate,
			  pcm_get_format(state->dma.is16bit,
					 state->dma.samp_signed),
			  state->dma.stereo + 1, state->dma_strm);
	    }
	    state->output_time_cur += out_fifo_cnt *
		    pcm_frame_period_us(state->dma.rate);
	} else {
	    S_printf("SB: Warning: rate not set?\n");
	    state->output_time_cur = time_dst;
	}
    }
    if (state->dma.running && state->output_time_cur > time_dst - 1)
	pcm_set_mode(state->dma_strm, PCM_MODE_NORMAL);
    if (out_fifo_cnt < nfr) {
	/* not enough samples, see why */
	if (!sb_dma_active()) {
	    dspio_stop_output(state);
	} else {
	    if (debug_level('S') > 7)
		S_printf("SB: Output FIFO exhausted while DMA is still active (ol=%f)\n",
			 time_dst - state->output_time_cur);
	    if (state->dma.running)
		S_printf("SB: Output FIFO exhausted while DMA is running\n");
	    /* DMA is active but currently not running and the FIFO is
	     * already exhausted. Normally we should flush the channel
	     * and stop the output timing.
	     * HACK: try to not flush the channel for as long as possible
	     * in a hope the PCM buffers are large enough to hold till
	     * the DMA is restarted. */
	    pcm_set_mode(state->dma_strm, PCM_MODE_POST);
	    /* awake dosemu */
	    reset_idle(0);
	}
    }

    if (state->input_running)
	nfr = calc_nframes(state, state->input_time_cur, time_dst);
    else
	nfr = 0;
    for (i = 0; i < nfr; i++) {
	for (j = 0; j < state->dma.stereo + 1; j++) {
	    if (sb_input_enabled()) {
		//if (!state->speaker)  /* TODO: input */
		dma_get_silence(state->dma.samp_signed,
			state->dma.is16bit, &buf[i][j]);
		if (!dspio_put_input_sample(state, &buf[i][j],
			state->dma.is16bit))
		    break;
	    }
	}
	if (j == state->dma.stereo + 1)
	    in_fifo_cnt++;
	for (j = 0; j < state->dma.stereo + 1; j++) {
	    if (state->dma.running) {
		if (!dspio_run_dma(state))
		    break;
		dma_cnt++;
	    }
	}
	if (j != state->dma.stereo + 1)
	    break;
    }
    if (in_fifo_cnt) {
	if (state->dma.rate) {
	    state->input_time_cur += in_fifo_cnt *
		    pcm_frame_period_us(state->dma.rate);
	} else {
	    state->input_time_cur = time_dst;
	}
    }

    if (state->dma.running)
	dma_cnt += state->dma.input ? dspio_drain_input(state) :
	    dspio_fill_output(state);

    if (in_fifo_cnt || out_fifo_cnt || dma_cnt)
	S_printf("SB: Processed %i %i FIFO, %i DMA, or=%i dr=%i time=%lli\n",
	     in_fifo_cnt, out_fifo_cnt, dma_cnt, state->output_running, state->dma.running,
	     time_dst);
}

static void dspio_process_midi(struct dspio_state *state)
{
    Bit8u data;
    /* no timing for now */
    while (!dspio_midi_output_empty(state)) {
	data = dspio_get_midi_data(state);
	midi_write(data);
    }

    while (midi_get_data_byte(&data)) {
	dspio_put_midi_in_byte(state, data);
	sb_handle_midi_data();
    }
}

static void *synth_thread(void *arg)
{
    while (1) {
	sem_wait(&syn_sem);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	adlib_timer();
	midi_timer();
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }
    return NULL;
}

void dspio_timer(void *dspio)
{
    sem_post(&syn_sem);
    dspio_process_dma(DSPIO);
    dspio_process_midi(DSPIO);
    pcm_timer();
}

void dspio_write_dac(void *dspio, Bit8u samp)
{
    if (DSPIO->speaker) {
	sndbuf_t buf[1][SNDBUF_CHANS];
	buf[0][0] = samp;
	DSPIO->dac_running = 1;
	pcm_write_interleaved(buf, 1, DAC_BASE_FREQ, PCM_FORMAT_U8,
			  1, DSPIO->dac_strm);
    }
}
