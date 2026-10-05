/* ES40 emulator.
 * Copyright (C) 2026 by Cacodemon345
 * Copyright (C) 2026 by gdwnldsKSC
 * Copyright (c) 2005 Vassili Karpov (malc) from QEMU
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: MIT
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
    const u64 latency = cfg->get_num_value("latency_ms", true, 40);
    m_latency_ms = (unsigned)(std::min)((std::max)(latency, (u64)10), (u64)500);
    if (!SDL_Init(SDL_INIT_AUDIO)) {
		FAILURE_1(SDL, "Failed to initialize SDL audio: %s", SDL_GetError());
    }
    // ~10ms host pulls; the SDL_AUDIO_DEVICE_SAMPLE_FRAMES env var still wins.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
	state.audio_be_in = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, nullptr);
    if (!state.audio_be_in) {
        FAILURE_1(SDL, "Failed to initialize SDL audio input: %s", SDL_GetError());
    }
    state.audio_be_out = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!state.audio_be_out) {
        FAILURE_1(SDL, "Failed to initialize SDL audio output: %s", SDL_GetError());
    }
    {
        SDL_AudioSpec out_spec;
        int out_frames = 0;
        if (!SDL_GetAudioDeviceFormat(state.audio_be_out, &out_spec, &out_frames))
            FAILURE_1(SDL, "Failed to query SDL audio output: %s", SDL_GetError());
        m_out_freq = out_spec.freq > 0 ? out_spec.freq : 48000;
        m_out_period_frames = out_frames > 0 ? out_frames : 512;
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

// All callers acquire the bus before a stream lock. SDL callbacks acquire
// neither the bus nor the worker's wait mutex.
class CES1370StreamLock
{
public:
    explicit CES1370StreamLock(SDL_AudioStream* stream) : m_stream(stream)
    {
        if (!SDL_LockAudioStream(m_stream))
            FAILURE_1(SDL, "Unable to lock audio stream: %s", SDL_GetError());
    }
    ~CES1370StreamLock() { SDL_UnlockAudioStream(m_stream); }
private:
    SDL_AudioStream* m_stream;
};

void CES1370::reset_audio_request(size_t channel, bool enabled, bool clear)
{
    SDL_AudioStream* voice = channel == ADC_CHANNEL
        ? state.adc_voice : state.dac_voice[channel];
    CES1370StreamLock stream_lock(voice);
    m_audio_enabled[channel].store(false);
    state.chan[channel].credit_q16 = 0;
    state.chan[channel].pace_last = std::chrono::steady_clock::now();
    if (clear && !SDL_ClearAudioStream(voice))
        FAILURE_1(SDL, "Unable to clear audio stream: %s", SDL_GetError());
    m_audio_enabled[channel].store(enabled);
}

void CES1370::stop_threads()
{
    m_audio_stop.store(true);
    m_audio_wait.notify_one();
    {
        std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
        audio_running = false;
        for (size_t i = 0; i < NB_CHANNELS; ++i)
            reset_audio_request(i, false, false);
    }
    // The worker never waits for the bus, including when our caller already
    // owns it. Joining here therefore cannot leave it blocked behind us.
    if (m_audio_thread)
    {
        m_audio_thread->join();
        delete m_audio_thread;
        m_audio_thread = nullptr;
    }
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    unbind_voices();
}

void CES1370::start_threads()
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (audio_running)
        return;

    for (size_t i = 0; i < NB_CHANNELS; ++i)
        reset_audio_request(i, false, true);
    m_audio_failed.store(false);
    m_audio_wake.store(false);
    try
    {
        es1370_update_voices(&state, state.ctl, state.sctl, true);
        m_audio_stop.store(false);
        audio_running = true;
        m_audio_thread = new CThread("es1370");
        m_audio_thread->start(*this);
    }
    catch (...)
    {
        audio_running = false;
        m_audio_stop.store(true);
        delete m_audio_thread;
        m_audio_thread = nullptr;
        for (size_t i = 0; i < NB_CHANNELS; ++i)
            reset_audio_request(i, false, false);
        unbind_voices();
        throw;
    }
}

void CES1370::check_state()
{
    if (m_audio_failed.load())
        FAILURE(Thread, "ES1370 audio refill thread failed");
}

// Guest-format bytes per second at the channel's programmed rate.
uint32_t CES1370::chan_rate_bps(size_t channel) const
{
    uint32_t freq = 0, unused = 0;
    es1370_chan_bits[channel].calc_freq(const_cast<ES1370State*>(&state),
        state.ctl, &unused, &freq);
    return freq << state.chan[channel].shift;
}

// Guest-format bytes one host device pull removes from the stream.
int CES1370::dac_pull_bytes(uint32_t rate_bps) const
{
    return (int)(std::min)((u64)rate_bps * m_out_period_frames / m_out_freq,
        (u64)(1 << 20));
}

// Lead to keep queued: configured latency, never under three host pulls.
int CES1370::dac_lead_bytes(size_t channel, uint32_t rate_bps) const
{
    const u64 by_latency = (u64)rate_bps * m_latency_ms / 1000;
    const u64 by_period = (u64)dac_pull_bytes(rate_bps) * 3;
    return (int)(std::min)((std::max)(by_latency, by_period), (u64)(1 << 20));
}

// One worker tick: playback is paced at the sample rate against a fixed SDL lead.
CES1370::ServiceResult CES1370::service_audio_channel(size_t channel,
    std::chrono::steady_clock::time_point now)
{
    std::unique_lock<std::recursive_mutex> bus_lock(
        cSystem->get_device_bus_mutex(), std::try_to_lock);
    if (!bus_lock.owns_lock())
        return SERVICE_BUSY;
    if (!audio_running || m_audio_stop.load())
        return SERVICE_IDLE;
    const chan_bits& bits = es1370_chan_bits[channel];
    if (!(state.ctl & bits.ctl_en) || (state.sctl & bits.sctl_pause))
        return SERVICE_IDLE;

    struct chan* d = &state.chan[channel];
    SDL_AudioStream* voice = channel == ADC_CHANNEL
        ? state.adc_voice : state.dac_voice[channel];
    CES1370StreamLock stream_lock(voice);

    if (channel == ADC_CHANNEL)
    {
        const int avail = SDL_GetAudioStreamAvailable(voice);
        if (avail < 0)
            FAILURE_1(SDL, "Unable to query audio stream: %s", SDL_GetError());
        if (avail <= 0)
            return SERVICE_IDLE;
        const int moved = es1370_run_channel(&state, channel, (std::min)(avail, 4096));
        return moved > 0 && moved < avail ? SERVICE_MORE : SERVICE_IDLE;
    }

    const uint32_t rate = chan_rate_bps(channel);
    if (!rate)
        return SERVICE_IDLE;
    const int queued = SDL_GetAudioStreamQueued(voice);
    if (queued < 0)
        FAILURE_1(SDL, "Unable to query audio stream: %s", SDL_GetError());
    const int lead = dac_lead_bytes(channel, rate);
    const int deficit = lead - queued;

    auto elapsed = now - d->pace_last;
    d->pace_last = now;
    if (deficit <= 0)
    {
        d->credit_q16 = 0;
        return SERVICE_IDLE;
    }
    const int64_t dt_ns = (std::min)((int64_t)
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(),
        (int64_t)50000000);
    // 2x when more than a pull behind (start-up/stall), else 2% over-rate.
    const int pull = dac_pull_bytes(rate);
    const int rate_q8 = deficit > pull + pull / 4 ? 512 : 261;
    if (dt_ns > 0)
        d->credit_q16 += ((int64_t)rate * dt_ns * rate_q8 << 8) / 1000000000;

    // Step cap of half a period (2..8ms) so a tick crosses at most one IRQ boundary.
    const int sc_bytes = ((d->scount & 0xffff) + 1) << d->shift;
    const int ms2 = (int)((u64)rate * 2 / 1000);
    const int ms8 = (int)((u64)rate * 8 / 1000);
    const int step_cap = (std::min)((std::max)(sc_bytes / 2, ms2), ms8);
    if (d->credit_q16 > ((int64_t)step_cap << 16))
        d->credit_q16 = (int64_t)step_cap << 16;

    const int budget = (std::min)((int)(d->credit_q16 >> 16), deficit);
    if (budget <= 0)
        return SERVICE_IDLE;
    const int moved = es1370_run_channel(&state, channel, budget);
    d->credit_q16 -= (int64_t)moved << 16;
    if (d->credit_q16 < 0)
        d->credit_q16 = 0;
    return SERVICE_IDLE;
}

void CES1370::run()
{
    try
    {
        int busy_spins = 0;
        while (!m_audio_stop.load())
        {
            const auto now = std::chrono::steady_clock::now();
            bool more = false, busy = false;
            for (size_t i = 0; i < NB_CHANNELS; ++i)
            {
                const ServiceResult r = service_audio_channel(i, now);
                more = more || r == SERVICE_MORE;
                busy = busy || r == SERVICE_BUSY;
            }
            // Bus collisions are short; retry a few times before sleeping.
            if (more || (busy && ++busy_spins < 16))
            {
                std::this_thread::yield();
                continue;
            }
            busy_spins = 0;
            std::unique_lock<std::mutex> wait_lock(m_audio_wait_mutex);
            m_audio_wait.wait_for(wait_lock, std::chrono::milliseconds(1), [this] {
                return m_audio_stop.load() || m_audio_wake.exchange(false);
            });
        }
    }
    catch (const CException& e)
    {
        printf("%s: Exception in audio refill thread: %s.\n",
            devid_string, e.displayText().c_str());
        m_audio_failed.store(true);
    }
    catch (const std::exception& e)
    {
        printf("%s: Exception in audio refill thread: %s.\n",
            devid_string, e.what());
        m_audio_failed.store(true);
    }
    catch (...)
    {
        printf("%s: Unknown exception in audio refill thread.\n", devid_string);
        m_audio_failed.store(true);
    }
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
        const bool format_changed = force || old_fmt != new_fmt || old_freq != new_freq;
        const bool changed = format_changed || ((ctl ^ s->ctl) & b->ctl_en) ||
            ((sctl ^ s->sctl) & b->sctl_pause);
        if (changed && (audio_running || force))
            reset_audio_request(i, false, format_changed);
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
        if (changed && (audio_running || force))
            reset_audio_request(i,
                (ctl & b->ctl_en) && !(sctl & b->sctl_pause), format_changed);
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

int CES1370::es1370_transfer_audio(ES1370State* s, struct chan* d, int loop_sel,
    int maxb, bool* irq)
{
    uint8_t tmpbuf[4096];
    const int index = d - s->chan;
    const int sc = d->scount & 0xffff;
    int csc_bytes = ((d->scount >> 16) + 1) << d->shift;
    const bool nonloop = (s->sctl & loop_sel) != 0;
    int remaining = maxb;
    bool period_done = false;

    // Loop only to wrap the frame buffer; one sample-count period (IRQ) per call.
    while (remaining > 0 && !period_done) {
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
            *irq = true;
            period_done = true;
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
    return maxb - remaining;
}

int CES1370::es1370_run_channel(ES1370State* s, size_t chan, int free_or_avail)
{
    uint32_t new_status = s->status;
    int max_bytes;
    bool irq;
    struct chan* d = &s->chan[chan];
    const struct chan_bits* b = &es1370_chan_bits[chan];

    if (!(s->ctl & b->ctl_en) || (s->sctl & b->sctl_pause)) {
        return 0;
    }

    max_bytes = free_or_avail;
    max_bytes &= ~((1 << d->shift) - 1);
    if (max_bytes <= 0) {
        return 0;
    }

    irq = s->sctl & b->sctl_inten && s->status & b->stat_int;

    const int moved = es1370_transfer_audio(s, d, b->sctl_loopsel, max_bytes, &irq);

    if (irq) {
        if (s->sctl & b->sctl_inten) {
            new_status |= b->stat_int;
        }
    }

    if (new_status != s->status) {
        es1370_update_status(s, new_status);
    }
    return moved;
}

// SDL holds its stream lock here; only wake the worker, which owns DMA and IRQs.
void CES1370::request_audio(size_t channel)
{
    if (!m_audio_enabled[channel].load())
        return;
    m_audio_wake.store(true);
    m_audio_wait.notify_one();
}

void CES1370::es1370_dac_callback_dac1(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    CES1370* dev = (CES1370*)userdata;
    dev->request_audio(DAC1_CHANNEL);
}

void CES1370::es1370_dac_callback_dac2(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    CES1370* dev = (CES1370*)userdata;
    dev->request_audio(DAC2_CHANNEL);
}

void CES1370::es1370_dac_callback_adc(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    CES1370* dev = (CES1370*)userdata;
    dev->request_audio(ADC_CHANNEL);
}
#endif /* HAVE_SDL */
