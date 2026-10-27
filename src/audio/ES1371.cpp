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
// ES1371 adaptation of the ES1370 port to ES40 by Cacodemon345 from qemu.
#if defined(HAVE_SDL) || defined(_WIN32)
#include "ES1371.h"

#include <algorithm>
#include <mutex>
#include <cmath>

// Register offsets and bit assignments from the ES1371 datasheet.

#define ES1371_REG_CONTROL        0x00
#define ES1371_REG_STATUS         0x04
#define ES1371_REG_UART_DATA      0x08
#define ES1371_REG_UART_STATUS    0x09
#define ES1371_REG_UART_CONTROL   0x09
#define ES1371_REG_UART_TEST      0x0a
#define ES1371_REG_MEMPAGE        0x0c
#define ES1371_REG_SRC            0x10
#define ES1371_REG_CODEC          0x14
#define ES1371_REG_LEGACY         0x18
#define ES1371_REG_SERIAL_CONTROL 0x20
#define ES1371_REG_DAC1_SCOUNT    0x24
#define ES1371_REG_DAC2_SCOUNT    0x28
#define ES1371_REG_ADC_SCOUNT     0x2c

#define ES1371_REG_DAC1_FRAMEADR    0xc30
#define ES1371_REG_DAC1_FRAMECNT    0xc34
#define ES1371_REG_DAC2_FRAMEADR    0xc38
#define ES1371_REG_DAC2_FRAMECNT    0xc3c
#define ES1371_REG_ADC_FRAMEADR     0xd30
#define ES1371_REG_ADC_FRAMECNT     0xd34
#define ES1371_REG_PHANTOM_FRAMEADR 0xd38
#define ES1371_REG_PHANTOM_FRAMECNT 0xd3c

#define CTRL_ADC_STOP   0x00002000
#define CTRL_SYNC_RES   0x00004000
#define CTRL_DAC1_EN    0x00000040
#define CTRL_DAC2_EN    0x00000020
#define CTRL_ADC_EN     0x00000010
#define CTRL_UART_EN    0x00000008
#define CTRL_PWR_INTRM  0x00001000
#define SRC_DISABLE    0x00400000
#define SRC_DIS_DAC1   0x00200000
#define SRC_DIS_DAC2   0x00100000
#define SRC_DIS_ADC    0x00080000
#define SRC_WRITE      0x01000000
#define SRC_STATE      0x00010000
#define CODEC_READ     0x00800000
#define CODEC_READY    0x80000000
#define STAT_INTR       0x80000000  /* wired or of all interrupt bits */
#define STAT_MPWR       0x00000020  /* requested/current power levels differ */
#define STAT_UART       0x00000008  /* UART int pending */
#define STAT_DAC1       0x00000004  /* DAC1 int pending */
#define STAT_DAC2       0x00000002  /* DAC2 int pending */
#define STAT_ADC        0x00000001  /* ADC int pending */

#define USTAT_RXINT     0x80        /* UART rx int pending */
#define USTAT_TXINT     0x04        /* UART tx int pending */
#define USTAT_TXRDY     0x02        /* UART tx ready */
#define USTAT_RXRDY     0x01        /* UART rx ready */

#define UCTRL_RXINTEN   0x80        /* 1 = enable RX ints */
#define UCTRL_TXINTEN   0x60        /* TX int enable field mask */
#define UCTRL_ENA_TXINT 0x20        /* enable TX int */
#define UCTRL_CNTRL     0x03        /* control field */
#define UCTRL_CNTRL_SWR 0x03        /* software reset command */

#define SCTRL_P2ENDINC    0x00380000  /*  */
#define SCTRL_SH_P2ENDINC 19
#define SCTRL_P2STINC     0x00070000  /*  */
#define SCTRL_SH_P2STINC  16
#define SCTRL_R1LOOPSEL   0x00008000  /* 0 = loop mode */
#define SCTRL_P2LOOPSEL   0x00004000  /* 0 = loop mode */
#define SCTRL_P1LOOPSEL   0x00002000  /* 0 = loop mode */
#define SCTRL_P2PAUSE     0x00001000  /* 1 = pause mode */
#define SCTRL_P1PAUSE     0x00000800  /* 1 = pause mode */
#define SCTRL_R1INTEN     0x00000400  /* enable interrupt */
#define SCTRL_P2INTEN     0x00000200  /* enable interrupt */
#define SCTRL_P1INTEN     0x00000100  /* enable interrupt */
#define SCTRL_P1SCTRLD    0x00000080  /* reload sample count register for DAC1 */
#define SCTRL_P2DACSEN    0x00000040  /* 1 = DAC2 play back last sample when disabled */
#define SCTRL_R1SEB       0x00000020  /* 1 = 16bit */
#define SCTRL_R1SMB       0x00000010  /* 1 = stereo */
#define SCTRL_R1FMT       0x00000030  /* format mask */
#define SCTRL_SH_R1FMT    4
#define SCTRL_P2SEB       0x00000008  /* 1 = 16bit */
#define SCTRL_P2SMB       0x00000004  /* 1 = stereo */
#define SCTRL_P2FMT       0x0000000c  /* format mask */
#define SCTRL_SH_P2FMT    2
#define SCTRL_P1SEB       0x00000002  /* 1 = 16bit */
#define SCTRL_P1SMB       0x00000001  /* 1 = stereo */
#define SCTRL_P1FMT       0x00000003  /* format mask */
#define SCTRL_SH_P1FMT    0


#define NB_CHANNELS 3
#define DAC1_CHANNEL 0
#define DAC2_CHANNEL 1
#define ADC_CHANNEL 2




#ifndef MAX
#define MAX(a,b)            (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a,b)            (((a) < (b)) ? (a) : (b))
#endif

// ES1371 register behavior follows the datasheet, Linux and 86Box.
/**
 * PCI Configuration Data Block
 **/
static u32 es1371_cfg_data[64] =
{
    /*00*/  0x13711274,         // CFID: vendor + device
    /*04*/  0x04100000,         // CFCS: slow DEVSEL, capabilities, command disabled
    /*08*/  0x04010002,         // CFRV: class + revision
    /*0c*/  0x00000000,         // CFLT: latency timer + cache line size
    /*10*/  0x00000001,         // BAR0: IO Space
    /*14*/  0x00000000,         // BAR1:
    /*18*/  0x00000000,         // BAR2:
    /*1c*/  0x00000000,         // BAR3:
    /*20*/  0x00000000,         // BAR4:
    /*24*/  0x00000000,         // BAR5:
    /*28*/  0x00000000,         // CCIC: CardBus
    /*2c*/  0x13711274,         // CSID: subsystem + vendor
    /*30*/  0x00000000,         // BAR6: expansion rom base
    /*34*/  0x000000dc,         // CCAP: power management capability
    /*38*/  0x00000000,
    /*3c*/  0x800c01ff,         // CFIT: interrupt configuration
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/**
 * PCI Configuration Mask Block
 **/
static u32 es1371_cfg_mask[64] = {
    /*00*/ 0x00000000,  // CFID: vendor + device
    /*04*/ 0x00000105,  // CFCS: command + status
    /*08*/ 0x00000000,  // CFRV: class + revision
    /*0c*/ 0x0000ffff,  // CFLT: latency timer + cache line size
    /*10*/ 0xffffffc0,  // BAR0: IO space (64 bytes)
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

CES1371::CES1371(CConfigurator* cfg, class CSystem* c, int pcibus, int pcidev) : CPCIDevice(cfg, c, pcibus, pcidev)
{
    host_audio::Format as;

    as.freq = 44100;
    as.channels = 2;
    as.format = host_audio::SampleFormat::s16_le;
	memset((void*)&state, 0, sizeof(state));
    const u64 latency = cfg->get_num_value("latency_ms", true, 40);
    m_latency_ms = (unsigned)(std::min)((std::max)(latency, (u64)10), (u64)500);
    m_audio_backend = host_audio::backend_for_gui(cfg->get_gui_name());
    host_audio::SetupGuard setup(m_audio_backend, {&state.audio_be_in, &state.audio_be_out},
        {&state.adc_voice, &state.dac_voice[0], &state.dac_voice[1]});
    if (!host_audio::initialize(m_audio_backend)) {
		HOST_AUDIO_FAILURE(m_audio_backend, "Failed to initialize %s audio");
    }
    // Request ~10ms host periods; SDL environment overrides still apply.
    host_audio::set_sample_frames_hint("512", m_audio_backend);
	state.audio_be_in = host_audio::open_input_device(m_audio_backend);
    if (!state.audio_be_in) {
        HOST_AUDIO_FAILURE(m_audio_backend, "Failed to initialize %s audio input");
    }
    state.audio_be_out = host_audio::open_output_device(m_audio_backend);
    if (!state.audio_be_out) {
        HOST_AUDIO_FAILURE(m_audio_backend, "Failed to initialize %s audio output");
    }
    {
        int out_freq = 0;
        int out_frames = 0;
        if (!host_audio::output_device_format(state.audio_be_out, &out_freq, &out_frames))
            HOST_AUDIO_FAILURE(m_audio_backend, "Failed to query %s audio output");
        m_out_freq = out_freq > 0 ? out_freq : 48000;
        m_out_period_frames = out_frames > 0 ? out_frames : 512;
    }
    host_audio::create_stream(state.adc_voice, nullptr, &as, m_audio_backend);
	host_audio::create_stream(state.dac_voice[0], &as, nullptr, m_audio_backend);
    host_audio::create_stream(state.dac_voice[1], &as, nullptr, m_audio_backend);

    if (!state.adc_voice || !state.dac_voice[0] || !state.dac_voice[1])
        HOST_AUDIO_FAILURE(m_audio_backend, "Failed to initialize ES1371 audio streams");

	host_audio::set_input_callback(state.adc_voice, es1371_dac_callback_adc, this);
    host_audio::set_output_callback(state.dac_voice[0], es1371_dac_callback_dac1, this);
    host_audio::set_output_callback(state.dac_voice[1], es1371_dac_callback_dac2, this);
    setup.complete();
}

CES1371::~CES1371()
{
    stop_threads();
    host_audio::destroy_stream(state.adc_voice);
    host_audio::destroy_stream(state.dac_voice[0]);
    host_audio::destroy_stream(state.dac_voice[1]);
    host_audio::close_device(state.audio_be_in);
    host_audio::close_device(state.audio_be_out);
}

void CES1371::init()
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    es1371_cfg_data[0xdc / 4] = 0x6c310001;
    es1371_cfg_mask[0xe0 / 4] = 0x00000103;
    add_function(0, es1371_cfg_data, es1371_cfg_mask);
    ResetPCI();
}

void CES1371::ResetPCI()
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    CPCIDevice::ResetPCI();
    for (size_t i = 0; i < NB_CHANNELS; ++i)
        reset_audio_request(i, false, true);
    es1371_reset(&state);
}

void CES1371::config_write_custom(int func, u32 address, int dsize,
    u32 old_data, u32 new_data, u32 data)
{
    // Changes to PCI bus mastering and the power state also gate DMA.
    if (func == 0 && ((address & ~3U) == 0x04 || (address & ~3U) == 0xe0)) {
        std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
        es1371_update_voices(&state, state.ctl, state.sctl);
        es1371_update_status(&state, state.status);
    }
}

void CES1371::unbind_voices()
{
    host_audio::unbind_stream(state.adc_voice);
    host_audio::unbind_stream(state.dac_voice[0]);
    host_audio::unbind_stream(state.dac_voice[1]);
}

// Lock the bus before streams; callbacks only wake the worker.
class CES1371StreamLock
{
public:
    explicit CES1371StreamLock(host_audio::Stream& stream) : m_stream(&stream)
    {
        if (!host_audio::lock_stream(*m_stream))
            HOST_AUDIO_FAILURE(m_stream->backend, "Unable to lock audio stream");
    }
    ~CES1371StreamLock() { unlock(); }
    void unlock()
    {
        if (m_stream) host_audio::unlock_stream(*m_stream);
        m_stream = nullptr;
    }
private:
    host_audio::Stream* m_stream;
};

void CES1371::reset_audio_request(size_t channel, bool enabled, bool clear)
{
    host_audio::Stream& voice = channel == ADC_CHANNEL
        ? state.adc_voice : state.dac_voice[channel];
    CES1371StreamLock stream_lock(voice);
    m_audio_enabled[channel].store(false);
    state.chan[channel].credit_q16 = 0;
    state.chan[channel].pace_last = std::chrono::steady_clock::now();
    if (clear && !host_audio::clear_stream(voice))
        HOST_AUDIO_FAILURE(m_audio_backend, "Unable to clear audio stream");
    m_audio_enabled[channel].store(enabled);
}

void CES1371::stop_threads()
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

void CES1371::start_threads()
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
        es1371_update_voices(&state, state.ctl, state.sctl, true);
        m_audio_stop.store(false);
        audio_running = true;
        m_audio_thread = new CThread("es1371");
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

void CES1371::check_state()
{
    if (m_audio_failed.load())
        FAILURE(Thread, "ES1371 audio refill thread failed");
}

// Guest-format bytes per second at the channel's programmed rate.
uint32_t CES1371::chan_rate_bps(size_t channel) const
{
    return channel_rate(channel) << state.chan[channel].shift;
}
// Guest-format bytes one host device pull removes from the stream.
int CES1371::dac_pull_bytes(uint32_t rate_bps) const
{
    return (int)(std::min)((u64)rate_bps * m_out_period_frames / m_out_freq,
        (u64)(1 << 20));
}

// Lead to keep queued: configured latency, never under three host pulls.
int CES1371::dac_lead_bytes(size_t channel, uint32_t rate_bps) const
{
    const u64 by_latency = (u64)rate_bps * m_latency_ms / 1000;
    const u64 by_period = (u64)dac_pull_bytes(rate_bps) * 3;
    return (int)(std::min)((std::max)(by_latency, by_period), (u64)(1 << 20));
}

// One worker tick: playback is paced at the sample rate against a fixed SDL lead.
CES1371::ServiceResult CES1371::service_audio_channel(size_t channel,
    std::chrono::steady_clock::time_point now)
{
    std::unique_lock<std::recursive_mutex> bus_lock(
        cSystem->get_device_bus_mutex(), std::try_to_lock);
    if (!bus_lock.owns_lock())
        return SERVICE_BUSY;
    if (!audio_running || m_audio_stop.load())
        return SERVICE_IDLE;
    if (!channel_enabled(channel, state.ctl, state.sctl))
        return SERVICE_IDLE;

    struct chan* d = &state.chan[channel];
    host_audio::Stream& voice = channel == ADC_CHANNEL
        ? state.adc_voice : state.dac_voice[channel];
    CES1371StreamLock stream_lock(voice);

    if (channel == ADC_CHANNEL)
    {
        const int avail = host_audio::available(voice);
        if (avail < 0)
            HOST_AUDIO_FAILURE(m_audio_backend, "Unable to query audio stream");
        if (avail <= 0)
            return SERVICE_IDLE;
        const int moved = es1371_run_channel(&state, channel, (std::min)(avail, 4096));
        // Release the stream lock before unbinding to preserve SDL's lock order.
        stream_lock.unlock();
        if (d->halted) host_audio::unbind_stream(voice);
        return moved > 0 && moved < avail ? SERVICE_MORE : SERVICE_IDLE;
    }

    const uint32_t rate = chan_rate_bps(channel);
    if (!rate)
        return SERVICE_IDLE;
    const int queued_output = host_audio::queued(voice);
    if (queued_output < 0)
        HOST_AUDIO_FAILURE(m_audio_backend, "Unable to query audio stream");
    // Convert queued stereo-S16 bytes back to guest bytes for pacing.
    const int queued = (queued_output / 4) << d->shift;
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
    const int moved = es1371_run_channel(&state, channel, budget);
    d->credit_q16 -= (int64_t)moved << 16;
    if (d->credit_q16 < 0)
        d->credit_q16 = 0;
    return SERVICE_IDLE;
}

void CES1371::run()
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

static const u32 es1371_magic1 = 0x45533171;
static const u32 es1371_magic2 = 0x71315345;

int CES1371::SaveState(FILE* f)
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (!f || audio_running)
        return -1;
    const int res = CPCIDevice::SaveState(f);
    if (res)
        return res;

    ES1371SavedState saved{};
    saved.ctl = state.ctl;
    saved.status = state.status;
    saved.mempage = state.mempage;
    saved.codec = state.codec;
    saved.sctl = state.sctl;
    saved.src = state.src;
    saved.src_phase = state.src_phase;
    saved.legacy = state.legacy;
    saved.uart_control = state.uart_control;
    saved.uart_test = state.uart_test;
    saved.phantom_addr = state.phantom_addr;
    saved.phantom_count = state.phantom_count;
    memcpy(saved.src_ram, state.src_ram, sizeof(saved.src_ram));
    memcpy(saved.ac97, state.ac97, sizeof(saved.ac97));
    for (size_t i = 0; i < NB_CHANNELS; ++i)
    {
        saved.channel[i].leftover = state.chan[i].leftover;
        saved.channel[i].scount = state.chan[i].scount;
        saved.channel[i].frame_addr = state.chan[i].frame_addr;
        saved.channel[i].frame_cnt = state.chan[i].frame_cnt;
        saved.channel[i].halted = state.chan[i].halted;
    }
    static_assert(sizeof(ES1371SavedState) == 123 * sizeof(u32),
        "Audio snapshot contains only fixed-width guest fields");
    const u32 size = sizeof(saved);
    if (fwrite(&es1371_magic1, sizeof(es1371_magic1), 1, f) != 1 ||
        fwrite(&size, sizeof(size), 1, f) != 1 ||
        fwrite(&saved, sizeof(saved), 1, f) != 1 ||
        fwrite(&es1371_magic2, sizeof(es1371_magic2), 1, f) != 1)
        return -1;
    return 0;
}

int CES1371::RestoreState(FILE* f)
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (!f || audio_running)
        return -1;
    const int res = CPCIDevice::RestoreState(f);
    if (res)
        return res;

    ES1371SavedState saved{};
    u32 magic = 0, size = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != es1371_magic1 ||
        fread(&size, sizeof(size), 1, f) != 1 || size != sizeof(saved) ||
        fread(&saved, sizeof(saved), 1, f) != 1 ||
        fread(&magic, sizeof(magic), 1, f) != 1 || magic != es1371_magic2 ||
        saved.mempage > 0xf || saved.src_phase > 1)
        return -1;
    for (size_t i = 0; i < NB_CHANNELS; ++i)
        if (saved.channel[i].leftover > 3 || saved.channel[i].halted > 1)
            return -1;

    state.ctl = saved.ctl;
    state.status = saved.status;
    state.mempage = saved.mempage;
    state.codec = saved.codec;
    state.sctl = saved.sctl;
    state.src = saved.src;
    state.src_phase = saved.src_phase;
    state.legacy = saved.legacy;
    state.uart_control = saved.uart_control;
    state.uart_test = saved.uart_test;
    state.phantom_addr = saved.phantom_addr;
    state.phantom_count = saved.phantom_count;
    memcpy(state.src_ram, saved.src_ram, sizeof(state.src_ram));
    memcpy(state.ac97, saved.ac97, sizeof(state.ac97));
    for (size_t i = 0; i < NB_CHANNELS; ++i)
    {
        const auto& b = es1371_chan_bits[i];
        const u32 fmt = (state.sctl & b.sctl_fmt) >> b.sctl_sh_fmt;
        state.chan[i].shift = (fmt & 1) + (fmt >> 1);
        state.chan[i].leftover = saved.channel[i].leftover;
        state.chan[i].scount = saved.channel[i].scount;
        state.chan[i].frame_addr = saved.channel[i].frame_addr;
        state.chan[i].frame_cnt = saved.channel[i].frame_cnt;
        state.chan[i].halted = saved.channel[i].halted;
        state.chan[i].freq = channel_rate(i);
    }
    es1371_update_status(&state, state.status);
    return 0;
}

void CES1371::es1371_update_status(ES1371State* s, uint32_t new_status)
{
    new_status &= ~STAT_UART;
    if ((s->ctl & CTRL_UART_EN) &&
        (s->uart_control & UCTRL_TXINTEN) == UCTRL_ENA_TXINT)
        new_status |= STAT_UART;
    new_status &= ~STAT_MPWR;
    if ((endian_32(pci_state.config_data[0][0xe0 / 4]) & 3) != ((s->ctl >> 8) & 3))
        new_status |= STAT_MPWR;
	uint32_t level = (new_status & (STAT_DAC1 | STAT_DAC2 | STAT_ADC | STAT_UART)) |
        (s->legacy & 0x01000000); // FIRQ is independent of the x86 trap engine.
    if (s->ctl & CTRL_PWR_INTRM) level |= new_status & STAT_MPWR;

	if (level) {
		s->status = new_status | STAT_INTR;
	}
	else {
		s->status = new_status & ~STAT_INTR;
	}
	do_pci_interrupt(0, !!level);
}

void CES1371::es1371_reset(ES1371State* s)
{
	size_t i;

	s->ctl = 0xfcff0000;
	s->status = 0x7ffffec0;
	s->mempage = 0;
	s->codec = 0;
	s->sctl = 0xff800000;
    s->src = s->src_phase = 0;
    s->legacy = 0x0000f801;
    s->uart_control = s->uart_test = 0;
    s->phantom_addr = s->phantom_count = 0;
    memset(s->src_ram, 0, sizeof(s->src_ram));
    reset_codec();

	for (i = 0; i < NB_CHANNELS; ++i) {
		struct chan* d = &s->chan[i];
		d->scount = 0;
		d->leftover = 0;
        d->frame_addr = d->frame_cnt = d->halted = d->freq = 0;
		if (i == ADC_CHANNEL) {
			host_audio::unbind_stream(s->adc_voice);
		}
		else {
			host_audio::unbind_stream(s->dac_voice[i]);
		}
	}
	do_pci_interrupt(0, 0);
    es1371_update_voices(s, s->ctl, s->sctl);
}

void CES1371::es1371_maybe_lower_irq(ES1371State* s, uint32_t sctl)
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
		es1371_update_status(s, new_status);
	}
}

const CES1371::chan_bits CES1371::es1371_chan_bits[3] = {
    {CTRL_DAC1_EN, STAT_DAC1, SCTRL_P1PAUSE, SCTRL_P1INTEN,
     SCTRL_P1FMT, SCTRL_SH_P1FMT, SCTRL_P1LOOPSEL},
    {CTRL_DAC2_EN, STAT_DAC2, SCTRL_P2PAUSE, SCTRL_P2INTEN,
     SCTRL_P2FMT, SCTRL_SH_P2FMT, SCTRL_P2LOOPSEL},
    {CTRL_ADC_EN, STAT_ADC, 0, SCTRL_R1INTEN,
     SCTRL_R1FMT, SCTRL_SH_R1FMT, SCTRL_R1LOOPSEL}
};

uint32_t CES1371::channel_rate(size_t channel) const
{
    const unsigned base = 0x70 + (unsigned)channel * 4;
    const uint32_t frequency = ((state.src_ram[base + 1] & 0xfc00U) << 5) |
        (state.src_ram[base + 3] & 0x7fffU);
    if (!frequency)
        return 0;
    uint64_t rate;
    if (channel == ADC_CHANNEL) {
        const uint32_t n = (state.src_ram[0x78] >> 4) & 0x1f;
        rate = ((uint64_t)48000 * 32768 * n + frequency / 2) / frequency;
    } else {
        rate = ((uint64_t)frequency * 3000 + 16384) / 32768;
    }
    // Clamp invalid rates to the 48 kHz AC-link clock.
    return (uint32_t)(std::min)(rate, (uint64_t)48000);
}

bool CES1371::channel_enabled(size_t channel, uint32_t ctl, uint32_t sctl) const
{
    const auto& b = es1371_chan_bits[channel];
    const uint32_t disable = SRC_DIS_DAC1 >> channel;
    return (ctl & b.ctl_en) && !(sctl & b.sctl_pause) &&
        !(state.src & (SRC_DISABLE | disable)) &&
        !(channel == ADC_CHANNEL && (ctl & CTRL_ADC_STOP)) &&
        !state.chan[channel].halted && channel_rate(channel) != 0 &&
        (endian_32(pci_state.config_data[0][0x04 / 4]) & 4) &&
        !(endian_32(pci_state.config_data[0][0xe0 / 4]) & 3);
}

void CES1371::es1371_update_voices(ES1371State* s, uint32_t ctl, uint32_t sctl,
    bool force)
{
    for (size_t i = 0; i < NB_CHANNELS; ++i) {
        auto& d = s->chan[i];
        const auto& b = es1371_chan_bits[i];
        if ((ctl & b.ctl_en) && !(s->ctl & b.ctl_en))
            d.halted = 0;
        const uint32_t fmt = (sctl & b.sctl_fmt) >> b.sctl_sh_fmt;
        const uint32_t old_fmt = (s->sctl & b.sctl_fmt) >> b.sctl_sh_fmt;
        const uint32_t freq = channel_rate(i);
        const bool format_changed = force || fmt != old_fmt || freq != d.freq;
        const bool enabled = channel_enabled(i, ctl, sctl);
        const bool changed = format_changed || enabled != m_audio_enabled[i].load() ||
            ((ctl ^ s->ctl) & b.ctl_en) || ((sctl ^ s->sctl) & b.sctl_pause);
        d.shift = (fmt & 1) + (fmt >> 1);
        d.freq = freq;
        if (!(audio_running || force) || !changed)
            continue;
        reset_audio_request(i, false, format_changed);
        host_audio::Stream& voice = i == ADC_CHANNEL ? s->adc_voice : s->dac_voice[i];
        // SDL rejects rebinding; unbind first without holding the stream lock.
        host_audio::unbind_stream(voice);
        if (format_changed && freq) {
            host_audio::Format spec;
            spec.freq = freq;
            spec.channels = i == ADC_CHANNEL ? 1 << (fmt & 1) : 2;
            spec.format = i == ADC_CHANNEL && !(fmt & 2) ? host_audio::SampleFormat::u8 : host_audio::SampleFormat::s16_le;
            // Preserve SDL's host end of the stream when changing guest format.
            if (!host_audio::set_format(voice,
                    i == ADC_CHANNEL ? nullptr : &spec,
                    i == ADC_CHANNEL ? &spec : nullptr))
                HOST_AUDIO_FAILURE(m_audio_backend, "Unable to set ES1371 audio format");
        }
        if (enabled) {
            if (!host_audio::bind_stream(i == ADC_CHANNEL ? s->audio_be_in : s->audio_be_out, voice))
                HOST_AUDIO_FAILURE(m_audio_backend, "Unable to bind ES1371 audio stream");
        } else {
            host_audio::unbind_stream(voice);
        }
        reset_audio_request(i, enabled, false);
    }
    s->ctl = ctl;
    s->sctl = sctl;
}

void CES1371::reset_codec()
{
    // TriTech TR28023 / Creative CT1297, a fixed-rate AC'97 1.0 codec.
    memset(state.ac97, 0, sizeof(state.ac97));
    state.ac97[0x02 / 2] = 0x8000;
    state.ac97[0x06 / 2] = 0x8000;
    state.ac97[0x0a / 2] = 0x0000;
    state.ac97[0x0c / 2] = 0x8008;
    state.ac97[0x0e / 2] = 0x8008;
    for (unsigned reg = 0x10; reg <= 0x18; reg += 2)
        state.ac97[reg / 2] = 0x8808;
    state.ac97[0x1c / 2] = 0x8000;
    state.ac97[0x26 / 2] = 0x000f;
    state.ac97[0x7c / 2] = 0x5452;
    state.ac97[0x7e / 2] = 0x4103;
}

void CES1371::write_codec(uint32_t command)
{
    const unsigned reg = (command >> 16) & 0x7f;
    if (command & CODEC_READ) {
        state.codec = (command & 0x00ff0000) | CODEC_READY |
            ((reg & 1) ? 0xffffU : state.ac97[reg / 2]);
        return;
    }
    state.codec = command & 0x007fffff;
    if (reg & 1)
        return;
    uint16_t mask = 0;
    switch (reg) {
    case 0x00: reset_codec(); return;
    case 0x02: mask = 0xbf3f; break;
    case 0x06: mask = 0x803f; break;
    case 0x0a: mask = 0x801e; break;
    case 0x0c: mask = 0x801f; break;
    case 0x0e: mask = 0x805f; break;
    case 0x10: case 0x12: case 0x14: case 0x16: case 0x18:
        mask = 0x9f1f; break;
    case 0x1a: mask = 0x0707; break;
    case 0x1c: mask = 0x8f0f; break;
    case 0x20: mask = 0x0200; break;
    case 0x26: {
        // Derive ready flags from power state; PR4/PR5 are write-only.
        const uint16_t power = command & 0x3f00;
        uint16_t ready = (~(power >> 8)) & 0x0f;
        if (power & 0x0800) ready &= ~0x0c;
        state.ac97[reg / 2] = (power & 0x0f00) | ready;
        return;
    }
    default: return; // Unsupported features and identity registers are read-only.
    }
    state.ac97[reg / 2] = command & mask;
}

int CES1371::apply_volume(size_t channel, uint8_t* data, int bytes)
{
    const auto& b = es1371_chan_bits[channel];
    const uint32_t fmt = (state.sctl & b.sctl_fmt) >> b.sctl_sh_fmt;
    const bool stereo = (fmt & 1) != 0;
    const int sample_bytes = (fmt & 2) ? 2 : 1;
    double gain[2];
    for (unsigned side = 0; side < 2; ++side) {
        const unsigned shift = side ? 0 : 8;
        if (channel == ADC_CHANNEL) {
            const uint16_t record = state.ac97[0x1c / 2];
            const unsigned n = (state.src_ram[0x78] >> 4) & 0x1f;
            gain[side] = (record & 0x8000) || (state.ac97[0x26 / 2] & 0x0100) || !n
                ? 0.0 : std::pow(10.0, ((record >> shift) & 15) * 1.5 / 20.0) *
                    (int16_t)state.src_ram[0x6c + side] / (n * 256.0);
        } else {
            const uint16_t master = state.ac97[0x02 / 2];
            const uint16_t pcm = state.ac97[0x18 / 2];
            const double db = 12.0 - (((master >> shift) & 63) +
                ((pcm >> shift) & 31)) * 1.5;
            gain[side] = ((master | pcm) & 0x8000) || (state.ac97[0x26 / 2] & 0x0e00)
                ? 0.0 : std::pow(10.0, db / 20.0) *
                    (int16_t)state.src_ram[0x7c + channel * 2 + side] / 4096.0;
        }
    }
    if (channel != ADC_CHANNEL) {
        const int frame_bytes = sample_bytes * (stereo ? 2 : 1);
        const int frames = bytes / frame_bytes;
        // Expand backwards to stereo S16 without overwriting unread samples.
        for (int frame = frames - 1; frame >= 0; --frame) {
            int samples[2];
            for (unsigned side = 0; side < 2; ++side) {
                const int pos = frame * frame_bytes + (stereo ? side * sample_bytes : 0);
                samples[side] = sample_bytes == 2
                    ? (int16_t)((uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8))
                    : ((int)data[pos] - 128) * 256;
            }
            for (unsigned side = 0; side < 2; ++side) {
                const double scaled = samples[side] * gain[side];
                const int sample = (int)(std::max)(-32768.0, (std::min)(32767.0, scaled));
                const int pos = frame * 4 + side * 2;
                data[pos] = (uint8_t)sample;
                data[pos + 1] = (uint8_t)((uint16_t)sample >> 8);
            }
        }
        return frames * 4;
    }
    for (int pos = 0; pos + sample_bytes <= bytes; pos += sample_bytes) {
        const unsigned side = stereo ? (pos / sample_bytes) & 1 : 0;
        int sample = sample_bytes == 2
            ? (int16_t)((uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8))
            : ((int)data[pos] - 128) * 256;
        const double scaled = sample * gain[side];
        sample = (int)(std::max)(-32768.0, (std::min)(32767.0, scaled));
        if (sample_bytes == 2) {
            data[pos] = (uint8_t)sample;
            data[pos + 1] = (uint8_t)((uint16_t)sample >> 8);
        } else {
            data[pos] = (uint8_t)((sample + 32768) >> 8);
        }
    }
    return bytes;
}
uint32_t CES1371::es1371_fixup(ES1371State* s, uint32_t addr)
{
	addr &= 0xff;
	if (addr >= 0x30 && addr <= 0x3f) {
		addr |= s->mempage << 8;
	}
	return addr;
}

void CES1371::es1371_write(void* opaque, u64 addr, uint32_t val, uint32_t mask)
{
    ES1371State* s = (ES1371State*)opaque;
    addr = es1371_fixup(s, (uint32_t)addr);
    const auto merge = [val, mask](uint32_t old) { return (old & ~mask) | (val & mask); };
    switch (addr) {
    case ES1371_REG_CONTROL: {
        // GPIO inputs and reserved high bits cannot be changed by the guest.
        const uint32_t ctl = (merge(s->ctl) & 0x030fffff) | 0xfcf00000;
        es1371_update_voices(s, ctl, s->sctl);
        es1371_update_status(s, s->status);
        break;
    }
    case ES1371_REG_MEMPAGE: s->mempage = merge(s->mempage) & 0xf; break;
    case ES1371_REG_UART_DATA:
        if (mask & 0xff00) {
            s->uart_control = (val >> 8) & 0xe3;
            if ((s->uart_control & UCTRL_CNTRL) == UCTRL_CNTRL_SWR)
                s->uart_control = 0;
        }
        if (mask & 0xff0000) s->uart_test = (val >> 16) & 1;
        // The transmitter consumes bytes immediately; there is no host MIDI sink.
        es1371_update_status(s, s->status);
        break;
    case ES1371_REG_SRC: {
        s->src = merge(s->src) & 0xff790000;
        s->src_phase = 0;
        if (s->src & SRC_WRITE) {
            const unsigned reg = (s->src >> 25) & 0x7f;
            const uint32_t data_mask = mask & 0xffff;
            s->src_ram[reg] = (uint16_t)((s->src_ram[reg] & ~data_mask) | (val & data_mask));
        }
        es1371_update_voices(s, s->ctl, s->sctl);
        break;
    }
    case ES1371_REG_CODEC:
        // Command writes normally use a dword, but retain unwritten byte lanes.
        write_codec(merge(s->codec));
        break;
    case ES1371_REG_LEGACY:
        // No x86 legacy event trap is connected on the Alpha PCI bus.
        s->legacy = (merge(s->legacy) & 0xffff0000) | 0x0000f801;
        es1371_update_status(s, s->status);
        break;
    case ES1371_REG_SERIAL_CONTROL: {
        const uint32_t serial = (merge(s->sctl) & 0x007fffff) | 0xff800000;
        if ((serial & SCTRL_P1SCTRLD) && !(s->sctl & SCTRL_P1SCTRLD)) {
            auto& d = s->chan[DAC1_CHANNEL];
            d.scount = (d.scount & 0xffff) * 0x10001U;
        }
        es1371_maybe_lower_irq(s, serial);
        es1371_update_voices(s, s->ctl, serial);
        break;
    }
    case ES1371_REG_DAC1_SCOUNT:
    case ES1371_REG_DAC2_SCOUNT:
    case ES1371_REG_ADC_SCOUNT: {
        if (!(mask & 0xffff)) break;
        auto& d = s->chan[(addr - ES1371_REG_DAC1_SCOUNT) >> 2];
        const uint32_t reload = merge(d.scount) & 0xffff;
        d.scount = reload * 0x10001U;
        break;
    }
    case ES1371_REG_DAC1_FRAMEADR:
    case ES1371_REG_DAC2_FRAMEADR:
    case ES1371_REG_ADC_FRAMEADR: {
        const unsigned channel = addr == ES1371_REG_ADC_FRAMEADR ? ADC_CHANNEL :
            (unsigned)(addr - ES1371_REG_DAC1_FRAMEADR) >> 3;
        s->chan[channel].frame_addr = merge(s->chan[channel].frame_addr) & ~3U;
        break;
    }
    case ES1371_REG_DAC1_FRAMECNT:
    case ES1371_REG_DAC2_FRAMECNT:
    case ES1371_REG_ADC_FRAMECNT: {
        const unsigned channel = addr == ES1371_REG_ADC_FRAMECNT ? ADC_CHANNEL :
            (unsigned)(addr - ES1371_REG_DAC1_FRAMECNT) >> 3;
        s->chan[channel].frame_cnt = merge(s->chan[channel].frame_cnt);
        s->chan[channel].leftover = 0;
        break;
    }
    case ES1371_REG_PHANTOM_FRAMEADR:
        s->phantom_addr = merge(s->phantom_addr) & ~3U; break;
    case ES1371_REG_PHANTOM_FRAMECNT:
        s->phantom_count = merge(s->phantom_count); break;
    default: break;
    }
}

uint64_t CES1371::es1371_read(void* opaque, u64 addr, unsigned size)
{
    ES1371State* s = (ES1371State*)opaque;
    addr = es1371_fixup(s, (uint32_t)addr);
    switch (addr) {
    case ES1371_REG_CONTROL: return s->ctl;
    case ES1371_REG_STATUS: return s->status;
    case ES1371_REG_UART_DATA:
        return (s->uart_test << 16) | (USTAT_TXRDY << 8) |
            ((s->status & STAT_UART) ? USTAT_TXINT << 8 : 0);
    case ES1371_REG_MEMPAGE: return s->mempage;
    case ES1371_REG_SRC: {
        uint32_t value = (s->src & ~SRC_STATE) | s->src_ram[(s->src >> 25) & 0x7f];
        // Report idle, then safe, to satisfy both driver polling sequences.
        if ((s->src & SRC_STATE) && s->src_phase++) value |= SRC_STATE;
        if (s->src_phase > 1) s->src_phase = 1;
        return value;
    }
    case ES1371_REG_CODEC: return s->codec;
    case ES1371_REG_LEGACY: return s->legacy;
    case ES1371_REG_SERIAL_CONTROL: return s->sctl;
    case ES1371_REG_DAC1_SCOUNT:
    case ES1371_REG_DAC2_SCOUNT:
    case ES1371_REG_ADC_SCOUNT:
        return s->chan[(addr - ES1371_REG_DAC1_SCOUNT) >> 2].scount;
    case ES1371_REG_DAC1_FRAMEADR: return s->chan[0].frame_addr;
    case ES1371_REG_DAC2_FRAMEADR: return s->chan[1].frame_addr;
    case ES1371_REG_ADC_FRAMEADR: return s->chan[2].frame_addr;
    case ES1371_REG_DAC1_FRAMECNT: return s->chan[0].frame_cnt;
    case ES1371_REG_DAC2_FRAMECNT: return s->chan[1].frame_cnt;
    case ES1371_REG_ADC_FRAMECNT: return s->chan[2].frame_cnt;
    case ES1371_REG_PHANTOM_FRAMEADR: return s->phantom_addr;
    case ES1371_REG_PHANTOM_FRAMECNT: return s->phantom_count;
    case 0xe30: return 0; // Empty UART receive FIFO (VALID clear).
    default: return ~0U;
    }
}

u32 CES1371::ReadMem_Bar(int func, int bar, u32 address, int dsize)
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (func != 0 || bar != 0 || address >= 0x40 ||
        (dsize != 8 && dsize != 16 && dsize != 32) ||
        (address & (dsize / 8 - 1)))
        return ~0U;
    const uint32_t value = (uint32_t)es1371_read(&state, address & ~3U, 32);
    const uint32_t mask = dsize == 32 ? ~0U : (1U << dsize) - 1;
    return (value >> ((address & 3) * 8)) & mask;
}

void CES1371::WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data)
{
    std::lock_guard<std::recursive_mutex> bus_lock(cSystem->get_device_bus_mutex());
    if (func != 0 || bar != 0 || address >= 0x40 ||
        (dsize != 8 && dsize != 16 && dsize != 32) ||
        (address & (dsize / 8 - 1)))
        return;
    const unsigned shift = (address & 3) * 8;
    const uint32_t mask = dsize == 32 ? ~0U : ((1U << dsize) - 1) << shift;
    es1371_write(&state, address & ~3U, data << shift, mask);
}
int CES1371::es1371_transfer_audio(ES1371State* s, struct chan* d, int loop_sel,
    int maxb, bool* irq)
{
    uint8_t tmpbuf[4096 * 4]; // Maximum mono-U8 to stereo-S16 expansion.
    const int index = (int)(d - s->chan);
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
            const int to_copy = MIN(target - transferred, 4096);
            int copied;
            if (index == ADC_CHANNEL) {
                copied = host_audio::read(s->adc_voice, tmpbuf, to_copy);
                if (copied <= 0) break;
                apply_volume(index, tmpbuf, copied);
                do_pci_write(addr, tmpbuf, 1, copied);
            } else {
                do_pci_read(addr, tmpbuf, 1, to_copy);
                const int output_bytes = apply_volume(index, tmpbuf, to_copy);
                copied = host_audio::write(s->dac_voice[index], tmpbuf, output_bytes) ? to_copy : 0;
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
            if (nonloop) d->halted = 1;
        }
        d->scount = sc | (((csc_bytes - 1) >> d->shift) << 16);

        cnt += (transferred + d->leftover) >> 2;
        d->frame_cnt = size;
        if (cnt <= size) d->frame_cnt |= cnt << 16;
        d->leftover = (transferred + d->leftover) & 3;

        // Stop on an SDL failure/empty capture queue.
        if (transferred < target) break;
    }
    return maxb - remaining;
}

int CES1371::es1371_run_channel(ES1371State* s, size_t chan, int free_or_avail)
{
    uint32_t new_status = s->status;
    int max_bytes;
    bool irq;
    struct chan* d = &s->chan[chan];
    const struct chan_bits* b = &es1371_chan_bits[chan];

    if (!channel_enabled(chan, s->ctl, s->sctl)) {
        return 0;
    }

    max_bytes = free_or_avail;
    max_bytes &= ~((1 << d->shift) - 1);
    if (max_bytes <= 0) {
        return 0;
    }

    irq = s->sctl & b->sctl_inten && s->status & b->stat_int;

    const int moved = es1371_transfer_audio(s, d, b->sctl_loopsel, max_bytes, &irq);

    if (d->halted) {
        m_audio_enabled[chan].store(false);
        // Capture unbinds after unlock; playback stays bound to drain.
    }

    if (irq) {
        if (s->sctl & b->sctl_inten) {
            new_status |= b->stat_int;
        }
    }

    if (new_status != s->status) {
        es1371_update_status(s, new_status);
    }
    return moved;
}

// SDL holds its stream lock here; only wake the worker, which owns DMA and IRQs.
void CES1371::request_audio(size_t channel)
{
    if (!m_audio_enabled[channel].load())
        return;
    m_audio_wake.store(true);
    m_audio_wait.notify_one();
}

void CES1371::es1371_dac_callback_dac1(void* userdata)
{
    CES1371* dev = (CES1371*)userdata;
    dev->request_audio(DAC1_CHANNEL);
}

void CES1371::es1371_dac_callback_dac2(void* userdata)
{
    CES1371* dev = (CES1371*)userdata;
    dev->request_audio(DAC2_CHANNEL);
}

void CES1371::es1371_dac_callback_adc(void* userdata)
{
    CES1371* dev = (CES1371*)userdata;
    dev->request_audio(ADC_CHANNEL);
}
#endif // HAVE_SDL || _WIN32
