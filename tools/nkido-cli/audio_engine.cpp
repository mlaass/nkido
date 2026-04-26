#include "audio_engine.hpp"
#include <SDL2/SDL.h>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>

namespace nkido {

// Global signal flag
std::atomic<bool> g_signal_received{false};

static void signal_handler(int signal) {
    (void)signal;
    g_signal_received.store(true, std::memory_order_release);
}

void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    stop();
    if (initialized_.load()) {
        SDL_Quit();
    }
}

void AudioEngine::list_capture_devices(std::ostream& out) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        out << "error: SDL_Init failed: " << SDL_GetError() << "\n";
        return;
    }

    int count = SDL_GetNumAudioDevices(/*iscapture=*/1);
    out << "Capture devices (" << count << "):\n";
    out << "  default: <system default>\n";
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(i, /*iscapture=*/1);
        out << "  " << i << ": " << (name ? name : "<unknown>") << "\n";
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioEngine::init_capture(const char* device_name) {
    if (!initialized_.load()) {
        std::cerr << "error: init_capture called before init()\n";
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = static_cast<int>(config_.sample_rate);
    want.format = AUDIO_F32SYS;
    want.channels = 2;  // request stereo; SDL may downmix from mono devices
    want.samples = static_cast<std::uint16_t>(config_.buffer_size);
    want.callback = capture_callback;
    want.userdata = this;

    SDL_AudioSpec have{};
    capture_device_id_ = SDL_OpenAudioDevice(
        device_name, /*iscapture=*/1, &want, &have,
        SDL_AUDIO_ALLOW_CHANNELS_CHANGE);

    if (capture_device_id_ == 0) {
        std::cerr << "warning: capture device unavailable";
        if (device_name) std::cerr << " (\"" << device_name << "\")";
        std::cerr << ": " << SDL_GetError() << "\n"
                  << "  in() will return silence.\n";
        return false;
    }

    if (have.freq != want.freq) {
        std::cerr << "warning: capture sample rate is " << have.freq
                  << " Hz, requested " << want.freq << " Hz\n";
    }

    capture_channels_ = have.channels;
    if (capture_channels_ != 1 && capture_channels_ != 2) {
        std::cerr << "warning: capture device reports "
                  << static_cast<int>(capture_channels_)
                  << " channels; only 1 or 2 supported. Closing capture.\n";
        SDL_CloseAudioDevice(capture_device_id_);
        capture_device_id_ = 0;
        return false;
    }

    return true;
}

bool AudioEngine::init(const Config& config) {
    config_ = config;

    // Initialize SDL2 audio subsystem
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "error: SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }
    initialized_.store(true);

    // Configure audio spec
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(config_.sample_rate);
    want.format = AUDIO_F32SYS;  // 32-bit float, system byte order
    want.channels = static_cast<std::uint8_t>(config_.channels);
    want.samples = static_cast<std::uint16_t>(config_.buffer_size);
    want.callback = audio_callback;
    want.userdata = this;

    SDL_AudioSpec have{};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);

    if (device_id_ == 0) {
        std::cerr << "error: SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
        return false;
    }

    // Verify we got what we asked for
    if (have.freq != want.freq || have.format != want.format ||
        have.channels != want.channels) {
        std::cerr << "warning: audio format differs from requested\n";
        std::cerr << "  requested: " << want.freq << " Hz, "
                  << static_cast<int>(want.channels) << " channels\n";
        std::cerr << "  got: " << have.freq << " Hz, "
                  << static_cast<int>(have.channels) << " channels\n";
    }

    // Configure VM
    vm_.set_sample_rate(static_cast<float>(have.freq));

    return true;
}

bool AudioEngine::start() {
    if (device_id_ == 0) {
        std::cerr << "error: audio device not initialized\n";
        return false;
    }

    running_.store(true, std::memory_order_release);
    shutdown_requested_.store(false, std::memory_order_release);

    // Unpause audio device to start playback
    SDL_PauseAudioDevice(device_id_, 0);
    if (capture_device_id_ != 0) {
        SDL_PauseAudioDevice(capture_device_id_, 0);
    }

    return true;
}

void AudioEngine::pause() {
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, 1);
    }
    if (capture_device_id_ != 0) {
        SDL_PauseAudioDevice(capture_device_id_, 1);
    }
    running_.store(false, std::memory_order_release);
}

void AudioEngine::stop() {
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, 1);
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    if (capture_device_id_ != 0) {
        SDL_PauseAudioDevice(capture_device_id_, 1);
        SDL_CloseAudioDevice(capture_device_id_);
        capture_device_id_ = 0;
    }
    running_.store(false, std::memory_order_release);
}

void AudioEngine::request_shutdown() {
    shutdown_requested_.store(true, std::memory_order_release);
}

bool AudioEngine::should_shutdown() const {
    return shutdown_requested_.load(std::memory_order_acquire);
}

void AudioEngine::wait_for_shutdown() {
    while (running_.load(std::memory_order_acquire)) {
        // Check for shutdown request or signal
        if (shutdown_requested_.load(std::memory_order_acquire) ||
            g_signal_received.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void AudioEngine::capture_callback(void* userdata, std::uint8_t* stream, int len) {
    auto* engine = static_cast<AudioEngine*>(userdata);
    const auto* input = reinterpret_cast<const float*>(stream);

    const std::size_t bytes = static_cast<std::size_t>(len);
    const std::size_t total_floats = bytes / sizeof(float);
    const std::uint8_t channels = engine->capture_channels_;
    if (channels == 0) return;
    const std::size_t num_frames = total_floats / channels;

    std::uint64_t write_pos = engine->capture_write_pos_.load(std::memory_order_relaxed);
    const std::uint64_t read_pos = engine->capture_read_pos_.load(std::memory_order_acquire);
    constexpr std::size_t cap = CAPTURE_RING_FRAMES;

    for (std::size_t i = 0; i < num_frames; ++i) {
        // Drop frames if the consumer is too far behind to avoid overruns
        // (silently — the consumer's ring offset will catch up next block).
        if (write_pos - read_pos >= cap) break;

        std::size_t slot = (write_pos % cap) * 2u;
        if (channels == 1) {
            float s = input[i];
            engine->capture_ring_[slot]     = s;
            engine->capture_ring_[slot + 1] = s;
        } else {
            engine->capture_ring_[slot]     = input[i * 2];
            engine->capture_ring_[slot + 1] = input[i * 2 + 1];
        }
        ++write_pos;
    }
    engine->capture_write_pos_.store(write_pos, std::memory_order_release);
}

void AudioEngine::audio_callback(void* userdata, std::uint8_t* stream, int len) {
    auto* engine = static_cast<AudioEngine*>(userdata);
    auto* output = reinterpret_cast<float*>(stream);

    // Check for shutdown request
    if (engine->shutdown_requested_.load(std::memory_order_relaxed) ||
        g_signal_received.load(std::memory_order_relaxed)) {
        // Fill with silence
        std::memset(stream, 0, static_cast<std::size_t>(len));
        engine->running_.store(false, std::memory_order_release);
        return;
    }

    // Calculate number of samples (stereo interleaved)
    std::size_t total_samples = static_cast<std::size_t>(len) / sizeof(float);
    std::size_t num_frames = total_samples / 2;  // Stereo

    // Process in BLOCK_SIZE chunks
    std::size_t offset = 0;
    while (offset < num_frames) {
        std::size_t chunk = std::min(num_frames - offset, cedar::BLOCK_SIZE);

        // Drain captured input into per-block L/R buffers (when capture is active).
        // If the producer hasn't filled BLOCK_SIZE frames yet, the unfilled tail
        // is silence — preferred over blocking the audio thread.
        alignas(32) float input_l[cedar::BLOCK_SIZE]{};
        alignas(32) float input_r[cedar::BLOCK_SIZE]{};
        if (engine->capture_device_id_ != 0) {
            std::uint64_t read_pos = engine->capture_read_pos_.load(std::memory_order_relaxed);
            const std::uint64_t write_pos = engine->capture_write_pos_.load(std::memory_order_acquire);
            constexpr std::size_t cap = CAPTURE_RING_FRAMES;
            for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
                if (read_pos < write_pos) {
                    std::size_t slot = (read_pos % cap) * 2u;
                    input_l[i] = engine->capture_ring_[slot];
                    input_r[i] = engine->capture_ring_[slot + 1];
                    ++read_pos;
                } else {
                    // Underrun — leave remaining samples zero.
                    break;
                }
            }
            engine->capture_read_pos_.store(read_pos, std::memory_order_release);
            engine->vm_.set_input_buffers(input_l, input_r);
        } else {
            engine->vm_.set_input_buffers(nullptr, nullptr);
        }

        // VM outputs to separate L/R buffers
        alignas(32) float left[cedar::BLOCK_SIZE]{};
        alignas(32) float right[cedar::BLOCK_SIZE]{};

        engine->vm_.process_block(left, right);

        // Interleave to output buffer
        for (std::size_t i = 0; i < chunk; ++i) {
            output[(offset + i) * 2]     = left[i];
            output[(offset + i) * 2 + 1] = right[i];
        }

        // Capture waveform for visualization (mix to mono)
        std::size_t write_pos = engine->waveform_write_pos_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < chunk; ++i) {
            float mono = (left[i] + right[i]) * 0.5f;
            engine->waveform_buffer_[write_pos % WAVEFORM_SIZE] = mono;
            write_pos++;
        }
        engine->waveform_write_pos_.store(write_pos % WAVEFORM_SIZE, std::memory_order_release);

        offset += chunk;
    }
}

void AudioEngine::get_waveform(float* out, std::size_t count) const {
    if (count > WAVEFORM_SIZE) count = WAVEFORM_SIZE;
    std::size_t pos = waveform_write_pos_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t idx = (pos + WAVEFORM_SIZE - count + i) % WAVEFORM_SIZE;
        out[i] = waveform_buffer_[idx];
    }
}

}  // namespace nkido
