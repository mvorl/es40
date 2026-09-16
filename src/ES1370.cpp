/*
 * QEMU ES1370 emulation
 *
 * Copyright (c) 2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
// Straight port to es40 by Cacodemon345.
#ifdef HAVE_SDL
#include "ES1370.h"

#include <algorithm>
#include <mutex>

#ifndef MAX
#define MAX(a,b)            (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a,b)            (((a) < (b)) ? (a) : (b))
#endif

/* Missing stuff:
   SCTRL_P[12](END|ST)INC
   SCTRL_P1SCTRLD
   SCTRL_P2DACSEN
   CTRL_DAC_SYNC
   MIDI
   non looped mode
   surely more
*/

/**
 * PCI Configuration Data Block
 **/
u32 es_cfg_data[64] =
{
    /*00*/  0x50001274,         // CFID: vendor + device
    /*04*/  0x02000001,         // CFCS: command + status
    /*08*/  0x04010000,         // CFRV: class + revision
    /*0c*/  0x00000000,         // CFLT: latency timer + cache line size
    /*10*/  0x00000001,         // BAR0: IO Space
    /*14*/  0x00000000,         // BAR1:
    /*18*/  0x00000000,         // BAR2:
    /*1c*/  0x00000000,         // BAR3:
    /*20*/  0x00000000,         // BAR4:
    /*24*/  0x00000000,         // BAR5:
    /*28*/  0x00000000,         // CCIC: CardBus
    /*2c*/  0x4c4c4942,         // CSID: subsystem + vendor
    /*30*/  0x00000000,         // BAR6: expansion rom base
    /*34*/  0x00000000,         // CCAP: capabilities pointer
    /*38*/  0x00000000,
    /*3c*/  0x401101ff,         // CFIT: interrupt configuration
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/**
 * PCI Configuration Mask Block
 **/
u32 es_cfg_mask[64] = {
    /*00*/ 0x00000000,  // CFID: vendor + device
    /*04*/ 0x00000157,  // CFCS: command + status
    /*08*/ 0x00000000,  // CFRV: class + revision
    /*0c*/ 0x0000ffff,  // CFLT: latency timer + cache line size
    /*10*/ 0xffffff00,  // BAR0: IO space (256 bytes)
    /*14*/ 0x00000000,  // BAR1:
    /*18*/ 0x00000000,  // BAR2:
    /*1c*/ 0x00000000,  // BAR3:
    /*20*/ 0x00000000,  // BAR4:
    /*24*/ 0x00000000,  // BAR5:
    /*28*/ 0x00000000,  // CCIC: CardBus
    /*2c*/ 0x00000000,  // CSID: subsystem + vendor
    /*30*/ 0x00000000,  // BAR6: expansion rom base
    /*34*/ 0x00000000,  // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x000000ff,  // CFIT: interrupt configuration
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

CES1370::CES1370(CConfigurator* cfg, class CSystem* c, int pcibus, int pcidev) : CPCIDevice(cfg, c, pcibus, pcidev)
{
    SDL_AudioSpec as;

    as.freq = 44100;
    as.channels = 2;
    as.format = SDL_AUDIO_S16LE;
	memset((void*)&state, 0, sizeof(state));
    if (!SDL_Init(SDL_INIT_AUDIO)) {
		FAILURE_1(SDL, "Failed to initialize SDL audio: %s", SDL_GetError());
    }
	state.audio_be_in = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, nullptr);
    if (!state.audio_be_in) {
        FAILURE_1(SDL, "Failed to initialize SDL audio input: %s", SDL_GetError());
    }
    state.audio_be_out = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!state.audio_be_out) {
        FAILURE_1(SDL, "Failed to initialize SDL audio output: %s", SDL_GetError());
    }
    state.adc_voice = SDL_CreateAudioStream(NULL, &as);
	state.dac_voice[0] = SDL_CreateAudioStream(&as, NULL);
    state.dac_voice[1] = SDL_CreateAudioStream(&as, NULL);

	SDL_SetAudioStreamPutCallback(state.adc_voice, es1370_dac_callback_adc, this);
    SDL_SetAudioStreamGetCallback(state.dac_voice[0], es1370_dac_callback_dac1, this);
    SDL_SetAudioStreamGetCallback(state.dac_voice[1], es1370_dac_callback_dac2, this);
}

CES1370::~CES1370()
{
    stop_threads();
    SDL_DestroyAudioStream(state.adc_voice);
    SDL_DestroyAudioStream(state.dac_voice[0]);
    SDL_DestroyAudioStream(state.dac_voice[1]);
    SDL_CloseAudioDevice(state.audio_be_in);
    SDL_CloseAudioDevice(state.audio_be_out);
}

void CES1370::init()
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    add_function(0, es_cfg_data, es_cfg_mask);
    ResetPCI();
    es1370_reset(&state);
    es1370_update_voices(&state, state.ctl, state.sctl);
}

void CES1370::unbind_voices()
{
    SDL_UnbindAudioStream(state.adc_voice);
    SDL_UnbindAudioStream(state.dac_voice[0]);
    SDL_UnbindAudioStream(state.dac_voice[1]);
}

void CES1370::stop_threads()
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    audio_running = false;
    unbind_voices();
}

void CES1370::start_threads()
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (audio_running)
        return;

    if (!SDL_ClearAudioStream(state.adc_voice) ||
        !SDL_ClearAudioStream(state.dac_voice[0]) ||
        !SDL_ClearAudioStream(state.dac_voice[1]))
        FAILURE_1(SDL, "Unable to clear resumed audio streams: %s", SDL_GetError());
    try
    {
        es1370_update_voices(&state, state.ctl, state.sctl, true);
    }
    catch (...)
    {
        unbind_voices();
        throw;
    }
    audio_running = true;
}

static const u32 es1370_magic1 = 0x45533137;
static const u32 es1370_magic2 = 0x37315345;

int CES1370::SaveState(FILE* f)
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (!f || audio_running)
        return -1;
    const int res = CPCIDevice::SaveState(f);
    if (res)
        return res;

    ES1370SavedState saved{};
    saved.ctl = state.ctl;
    saved.status = state.status;
    saved.mempage = state.mempage;
    saved.codec = state.codec;
    saved.sctl = state.sctl;
    for (size_t i = 0; i < NB_CHANNELS; ++i)
    {
        saved.channel[i].leftover = state.chan[i].leftover;
        saved.channel[i].scount = state.chan[i].scount;
        saved.channel[i].frame_addr = state.chan[i].frame_addr;
        saved.channel[i].frame_cnt = state.chan[i].frame_cnt;
    }
    static_assert(sizeof(ES1370SavedState) == 17 * sizeof(u32),
        "Audio snapshot contains only fixed-width guest fields");
    const u32 size = sizeof(saved);
    if (fwrite(&es1370_magic1, sizeof(es1370_magic1), 1, f) != 1 ||
        fwrite(&size, sizeof(size), 1, f) != 1 ||
        fwrite(&saved, sizeof(saved), 1, f) != 1 ||
        fwrite(&es1370_magic2, sizeof(es1370_magic2), 1, f) != 1)
        return -1;
    return 0;
}

int CES1370::RestoreState(FILE* f)
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (!f || audio_running)
        return -1;
    const int res = CPCIDevice::RestoreState(f);
    if (res)
        return res;

    ES1370SavedState saved{};
    u32 magic = 0, size = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != es1370_magic1 ||
        fread(&size, sizeof(size), 1, f) != 1 || size != sizeof(saved) ||
        fread(&saved, sizeof(saved), 1, f) != 1 ||
        fread(&magic, sizeof(magic), 1, f) != 1 || magic != es1370_magic2 ||
        saved.mempage > 0xf)
        return -1;
    for (size_t i = 0; i < NB_CHANNELS; ++i)
        if (saved.channel[i].leftover > 3)
            return -1;

    state.ctl = saved.ctl;
    state.status = saved.status;
    state.mempage = saved.mempage;
    state.codec = saved.codec;
    state.sctl = saved.sctl;
    for (size_t i = 0; i < NB_CHANNELS; ++i)
    {
        const auto& b = es1370_chan_bits[i];
        const u32 fmt = (state.sctl & b.sctl_fmt) >> b.sctl_sh_fmt;
        state.chan[i].shift = (fmt & 1) + (fmt >> 1);
        state.chan[i].leftover = saved.channel[i].leftover;
        state.chan[i].scount = saved.channel[i].scount;
        state.chan[i].frame_addr = saved.channel[i].frame_addr;
        state.chan[i].frame_cnt = saved.channel[i].frame_cnt;
    }
    return 0;
}

void CES1370::es1370_update_status(ES1370State* s, uint32_t new_status)
{
	uint32_t level = new_status & (STAT_DAC1 | STAT_DAC2 | STAT_ADC);

	if (level) {
		s->status = new_status | STAT_INTR;
	}
	else {
		s->status = new_status & ~STAT_INTR;
	}
	do_pci_interrupt(0, !!level);
}

void CES1370::es1370_reset(ES1370State* s)
{
	size_t i;

	s->ctl = 1;
	s->status = 0x60;
	s->mempage = 0;
	s->codec = 0;
	s->sctl = 0;

	for (i = 0; i < NB_CHANNELS; ++i) {
		struct chan* d = &s->chan[i];
		d->scount = 0;
		d->leftover = 0;
		if (i == ADC_CHANNEL) {
			SDL_UnbindAudioStream(s->adc_voice);
		}
		else {
			SDL_UnbindAudioStream(s->dac_voice[i]);
		}
	}
	do_pci_interrupt(0, 0);
    es1370_update_voices(s, s->ctl, s->sctl);
}

void CES1370::es1370_maybe_lower_irq(ES1370State* s, uint32_t sctl)
{
	uint32_t new_status = s->status;

	if (!(sctl & SCTRL_P1INTEN) && (s->sctl & SCTRL_P1INTEN)) {
		new_status &= ~STAT_DAC1;
	}

	if (!(sctl & SCTRL_P2INTEN) && (s->sctl & SCTRL_P2INTEN)) {
		new_status &= ~STAT_DAC2;
	}

	if (!(sctl & SCTRL_R1INTEN) && (s->sctl & SCTRL_R1INTEN)) {
		new_status &= ~STAT_ADC;
	}

	if (new_status != s->status) {
		es1370_update_status(s, new_status);
	}
}

void CES1370::es1370_dac1_calc_freq(ES1370State* s, uint32_t ctl,
	uint32_t* old_freq, uint32_t* new_freq)
{
	*old_freq = dac1_samplerate[(s->ctl & CTRL_WTSRSEL) >> CTRL_SH_WTSRSEL];
	*new_freq = dac1_samplerate[(ctl & CTRL_WTSRSEL) >> CTRL_SH_WTSRSEL];
}

void CES1370::es1370_dac2_and_adc_calc_freq(ES1370State* s, uint32_t ctl,
	uint32_t* old_freq,
	uint32_t* new_freq)
{
	uint32_t old_pclkdiv, new_pclkdiv;

	new_pclkdiv = (ctl & CTRL_PCLKDIV) >> CTRL_SH_PCLKDIV;
	old_pclkdiv = (s->ctl & CTRL_PCLKDIV) >> CTRL_SH_PCLKDIV;
	*new_freq = DAC2_DIVTOSR(new_pclkdiv);
	*old_freq = DAC2_DIVTOSR(old_pclkdiv);
}

void CES1370::es1370_update_voices(ES1370State* s, uint32_t ctl, uint32_t sctl,
    bool force)
{
	size_t i;
	uint32_t old_freq, new_freq, old_fmt, new_fmt;

	for (i = 0; i < NB_CHANNELS; ++i) {
		struct chan* d = &s->chan[i];
		const struct chan_bits* b = &es1370_chan_bits[i];

		new_fmt = (sctl & b->sctl_fmt) >> b->sctl_sh_fmt;
		old_fmt = (s->sctl & b->sctl_fmt) >> b->sctl_sh_fmt;

		b->calc_freq(s, ctl, &old_freq, &new_freq);
		if (force || (old_fmt != new_fmt) || (old_freq != new_freq)) {
			d->shift = (new_fmt & 1) + (new_fmt >> 1);
			if (new_freq && (audio_running || force)) {
				//struct audsettings as;
				SDL_AudioSpec as;

				as.freq = new_freq;
				as.channels = 1 << (new_fmt & 1);
				as.format = (new_fmt & 2) ? SDL_AUDIO_S16LE : SDL_AUDIO_U8;

                SDL_AudioStream* voice = i == ADC_CHANNEL ? s->adc_voice : s->dac_voice[i];
                if (!SDL_SetAudioStreamFormat(voice, &as, &as) && force)
                    FAILURE_1(SDL, "Unable to restore audio format: %s", SDL_GetError());
			}
		}

		if ((audio_running || force) &&
            (force || ((ctl ^ s->ctl) & b->ctl_en) || ((sctl ^ s->sctl) & b->sctl_pause))) {
			int on = (ctl & b->ctl_en) && !(sctl & b->sctl_pause);

			if (i == ADC_CHANNEL) {
				if (on) {
					if (!SDL_BindAudioStream(s->audio_be_in, s->adc_voice) && force)
                        FAILURE_1(SDL, "Unable to resume audio capture: %s", SDL_GetError());
				}
				else {
					SDL_UnbindAudioStream(s->adc_voice);
				}
			} else {
				if (on) {
					if (!SDL_BindAudioStream(s->audio_be_out, s->dac_voice[i]) && force)
                        FAILURE_1(SDL, "Unable to resume audio playback: %s", SDL_GetError());
				}
				else {
					SDL_UnbindAudioStream(s->dac_voice[i]);
				}
			}
		}
	}

    s->ctl = ctl;
    s->sctl = sctl;
}

uint32_t CES1370::es1370_fixup(ES1370State* s, uint32_t addr)
{
	addr &= 0xff;
	if (addr >= 0x30 && addr <= 0x3f) {
		addr |= s->mempage << 8;
	}
	return addr;
}

void CES1370::es1370_write(void* opaque, u64 addr, uint64_t val, unsigned size)
{
    ES1370State* s = (ES1370State * )opaque;
    struct chan* d = &s->chan[0];

    addr = es1370_fixup(s, addr);

    switch (addr) {
    case ES1370_REG_CONTROL:
        es1370_update_voices(s, val, s->sctl);
        // print_ctl(val);
        break;

    case ES1370_REG_MEMPAGE:
        s->mempage = val & 0xf;
        break;

    case ES1370_REG_SERIAL_CONTROL:
        es1370_maybe_lower_irq(s, val);
        es1370_update_voices(s, s->ctl, val);
        // print_sctl(val);
        break;

    case ES1370_REG_DAC1_SCOUNT:
    case ES1370_REG_DAC2_SCOUNT:
    case ES1370_REG_ADC_SCOUNT:
        d += (addr - ES1370_REG_DAC1_SCOUNT) >> 2;
        d->scount = (val & 0xffff) << 16 | (val & 0xffff);
        // trace_es1370_sample_count_wr(d - &s->chan[0],
        //    d->scount >> 16, d->scount & 0xffff);
        break;

    case ES1370_REG_ADC_FRAMEADR:
        d += 2;
        goto frameadr;
    case ES1370_REG_DAC1_FRAMEADR:
    case ES1370_REG_DAC2_FRAMEADR:
        d += (addr - ES1370_REG_DAC1_FRAMEADR) >> 3;
    frameadr:
        d->frame_addr = val;
        //trace_es1370_frame_address_wr(d - &s->chan[0], d->frame_addr);
        break;

    case ES1370_REG_PHANTOM_FRAMECNT:
        //lwarn("writing to phantom frame count 0x%" PRIx64, val);
        break;
    case ES1370_REG_PHANTOM_FRAMEADR:
        //lwarn("writing to phantom frame address 0x%" PRIx64, val);
        break;

    case ES1370_REG_ADC_FRAMECNT:
        d += 2;
        goto framecnt;
    case ES1370_REG_DAC1_FRAMECNT:
    case ES1370_REG_DAC2_FRAMECNT:
        d += (addr - ES1370_REG_DAC1_FRAMECNT) >> 3;
    framecnt:
        d->frame_cnt = val;
        d->leftover = 0;
        //trace_es1370_frame_count_wr(d - &s->chan[0],
        //    d->frame_cnt >> 16, d->frame_cnt & 0xffff);
        break;

    default:
        //lwarn("writel 0x%" PRIx64 " <- 0x%" PRIx64, addr, val);
        break;
    }
}

uint64_t CES1370::es1370_read(void* opaque, u64 addr, unsigned size)
{
    ES1370State* s = (ES1370State*)opaque;
    uint32_t val;
    struct chan* d = &s->chan[0];

    addr = es1370_fixup(s, addr);

    switch (addr) {
    case ES1370_REG_CONTROL:
        val = s->ctl;
        break;
    case ES1370_REG_STATUS:
        val = s->status;
        break;
    case ES1370_REG_MEMPAGE:
        val = s->mempage;
        break;
    case ES1370_REG_CODEC:
        val = s->codec;
        break;
    case ES1370_REG_SERIAL_CONTROL:
        val = s->sctl;
        break;

    case ES1370_REG_DAC1_SCOUNT:
    case ES1370_REG_DAC2_SCOUNT:
    case ES1370_REG_ADC_SCOUNT:
        d += (addr - ES1370_REG_DAC1_SCOUNT) >> 2;
        val = d->scount;
        break;

    case ES1370_REG_ADC_FRAMECNT:
        d += 2;
        goto framecnt;
    case ES1370_REG_DAC1_FRAMECNT:
    case ES1370_REG_DAC2_FRAMECNT:
        d += (addr - ES1370_REG_DAC1_FRAMECNT) >> 3;
    framecnt:
        val = d->frame_cnt;
        break;

    case ES1370_REG_ADC_FRAMEADR:
        d += 2;
        goto frameadr;
    case ES1370_REG_DAC1_FRAMEADR:
    case ES1370_REG_DAC2_FRAMEADR:
        d += (addr - ES1370_REG_DAC1_FRAMEADR) >> 3;
    frameadr:
        val = d->frame_addr;
        break;

    case ES1370_REG_PHANTOM_FRAMECNT:
        val = ~0U;
        //lwarn("reading from phantom frame count");
        break;
    case ES1370_REG_PHANTOM_FRAMEADR:
        val = ~0U;
        //lwarn("reading from phantom frame address");
        break;

    default:
        val = ~0U;
        //lwarn("readl 0x%" PRIx64 " -> 0x%x", addr, val);
        break;
    }
    return val;
}

u32 CES1370::ReadMem_Bar(int func, int bar, u32 address, int dsize)
{
    if (bar != 0) return ~0U;
    if (dsize < 32) {
        auto val = CES1370::ReadMem_Bar(func, bar, address & ~3, 32);
        switch (dsize)
        {
        case 8:
            return (val >> ((address & 3) * 8)) & 0xff;
        case 16:
            if (address & 2) {
                return (val >> 16) & 0xffff;
            }
            else {
                return val & 0xffff;
            }
        }
    }
    if (dsize == 32) {
        return es1370_read(&state, address, dsize);
    }
    if (dsize == 64) {
        uint64_t low = es1370_read(&state, address, 32);
        uint64_t high = es1370_read(&state, address + 4, 32);
        return low | (high << 32);
	}
    return ~0U;
}

void CES1370::WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data)
{
    if (bar != 0) return;

    if (dsize < 32) {
        auto val = CES1370::ReadMem_Bar(func, bar, address & ~3, 32);
        switch (dsize)
        {
        case 8:
        {
			val = (val & ~(0xff << ((address & 3) * 8))) | ((data & 0xff) << ((address & 3) * 8));
			CES1370::WriteMem_Bar(func, bar, address & ~3, 32, val);
            return;
        }
        case 16:
        {
			if (address & 2) {
                val = (val & 0xffff) | (data << 16);
            }
            else {
                val = (val & 0xffff0000) | data;
            }
			CES1370::WriteMem_Bar(func, bar, address & ~3, 32, val);
            return;
        }
        }
    }

	if (dsize == 32) {
        es1370_write(&state, address, data, dsize);
        return;
    }
}

void CES1370::es1370_transfer_audio(ES1370State* s, struct chan* d, int loop_sel,
    int maxb, bool* irq)
{
    uint8_t tmpbuf[4096];
    const int index = d - s->chan;
    const int sc = d->scount & 0xffff;
    int csc_bytes = ((d->scount >> 16) + 1) << d->shift;
    const bool nonloop = (s->sctl & loop_sel) != 0;
    int remaining = maxb;

    // SDL asks once for this refill. 
    // A sample period or DMA updates the device counters, but doesn't finish..... 
    while (remaining > 0) {
        int cnt = d->frame_cnt >> 16;
        const int size = d->frame_cnt & 0xffff;
        if (size < cnt) break;

        // leftover bytes in the current dword have already been consumed.
        const int left = ((size - cnt + 1) << 2) - (int)d->leftover;
        if (left <= 0) break;
        const int target = MIN(remaining, MIN(left, csc_bytes));
        uint32_t addr = d->frame_addr + (cnt << 2) + d->leftover;
        int transferred = 0;
        while (transferred < target) {
            const int to_copy = MIN(target - transferred, (int)sizeof(tmpbuf));
            int copied;
            if (index == ADC_CHANNEL) {
                copied = SDL_GetAudioStreamData(s->adc_voice, tmpbuf, to_copy);
                if (copied <= 0) break;
                do_pci_write(addr, tmpbuf, 1, copied);
            } else {
                do_pci_read(addr, tmpbuf, 1, to_copy);
                copied = SDL_PutAudioStreamData(s->dac_voice[index], tmpbuf, to_copy) ? to_copy : 0;
                if (!copied) break;
            }
            addr += copied;
            transferred += copied;
        }
        remaining -= transferred;
        csc_bytes -= transferred;
        if (!csc_bytes) {
            // Keep a completed period latched even if the next segment ends or fails to reach SDL
            *irq = true;
            csc_bytes = (sc + 1) << d->shift;
        }
        d->scount = sc | (((csc_bytes - 1) >> d->shift) << 16);

        cnt += (transferred + d->leftover) >> 2;
        if (!nonloop) {
            d->frame_cnt = size;
            if (cnt <= size) d->frame_cnt |= cnt << 16;
        }
        d->leftover = (transferred + d->leftover) & 3;

        // Stop on an SDL failure/empty capture queue.
        if (transferred < target || nonloop) break;
    }
}

void CES1370::es1370_run_channel(ES1370State* s, size_t chan, int free_or_avail)
{
    uint32_t new_status = s->status;
    int max_bytes;
    bool irq;
    struct chan* d = &s->chan[chan];
    const struct chan_bits* b = &es1370_chan_bits[chan];

    if (!(s->ctl & b->ctl_en) || (s->sctl & b->sctl_pause)) {
        return;
    }

    max_bytes = free_or_avail;
    max_bytes &= ~((1 << d->shift) - 1);
    if (max_bytes <= 0) {
        return;
    }

    irq = s->sctl & b->sctl_inten && s->status & b->stat_int;

    es1370_transfer_audio(s, d, b->sctl_loopsel, max_bytes, &irq);

    if (irq) {
        if (s->sctl & b->sctl_inten) {
            new_status |= b->stat_int;
        }
    }

    if (new_status != s->status) {
        es1370_update_status(s, new_status);
    }
}

// SDL holds the stream lock during these callbacks, so we should never wait for the 
// bus gate here. 
// Deferring a callback leaves DMA state and queued capture data intact.
void CES1370::es1370_dac_callback_dac1(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    CES1370* dev = (CES1370*)userdata;
    std::unique_lock<std::recursive_mutex> bus_lock(
        dev->cSystem->get_device_bus_mutex(), std::try_to_lock);
    if (!bus_lock.owns_lock() || !dev->audio_running)
        return;
    ES1370State* s = &dev->state;
    dev->es1370_run_channel(s, 0, additional_amount);
}

void CES1370::es1370_dac_callback_dac2(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    CES1370* dev = (CES1370*)userdata;
    std::unique_lock<std::recursive_mutex> bus_lock(
        dev->cSystem->get_device_bus_mutex(), std::try_to_lock);
    if (!bus_lock.owns_lock() || !dev->audio_running)
        return;
    ES1370State* s = &dev->state;
    dev->es1370_run_channel(s, 1, additional_amount);
}

void CES1370::es1370_dac_callback_adc(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    CES1370* dev = (CES1370*)userdata;
    std::unique_lock<std::recursive_mutex> bus_lock(
        dev->cSystem->get_device_bus_mutex(), std::try_to_lock);
    if (!bus_lock.owns_lock() || !dev->audio_running)
        return;
    ES1370State* s = &dev->state;
    dev->es1370_run_channel(s, 2, additional_amount);
}
#endif /* HAVE_SDL */
