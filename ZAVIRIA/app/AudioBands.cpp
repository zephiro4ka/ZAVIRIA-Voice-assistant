#include "AudioBands.h"

#include "Log.h"

#include "../miniaudio.h" // реализация miniaudio собрана в Ears.cpp

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <mutex>
#include <thread>
#include <vector>

namespace {
    constexpr int kFft = 1024;            // ~21 мс звука при 48 кГц — успевает за каждой нотой
    std::atomic<bool> g_started{ false }, g_live{ false };
    std::atomic<long long> g_wantUntil{ 0 }; // мс: до какого времени ещё нужно слушать
    std::mutex g_mutex;
    std::vector<float> g_ring(kFft * 2, 0.0f);
    size_t g_pos = 0;
    std::atomic<unsigned> g_rate{ 48000 };

    long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    void OnAudio(ma_device*, void*, const void* input, ma_uint32 frames) {
        if (!input) return;
        const float* s = static_cast<const float*>(input);
        std::lock_guard lock(g_mutex);
        for (ma_uint32 i = 0; i < frames; ++i) { g_ring[g_pos] = s[i]; g_pos = (g_pos + 1) % g_ring.size(); }
    }

    // Слушаем выход звука только пока нужно: без просьб пару секунд — отпускаем устройство
    void Worker() {
        ma_context context{};
        ma_backend backends[] = { ma_backend_wasapi }; // «что играет в колонках» умеет только WASAPI
        if (ma_context_init(backends, 1, nullptr, &context) != MA_SUCCESS) { Log::Write("волна: нет доступа к звуку"); return; }
        ma_device device{};
        bool open = false;
        for (;;) {
            const bool want = NowMs() < g_wantUntil.load();
            if (want && !open) {
                ma_device_config config = ma_device_config_init(ma_device_type_loopback);
                config.capture.format = ma_format_f32;
                config.capture.channels = 1;
                config.sampleRate = 0; // как у колонок — без пересчёта
                config.dataCallback = OnAudio;
                if (ma_device_init(&context, &config, &device) == MA_SUCCESS && ma_device_start(&device) == MA_SUCCESS) {
                    g_rate = device.sampleRate;
                    open = true;
                } else std::this_thread::sleep_for(std::chrono::seconds(3)); // не вышло — попробуем позже
            } else if (!want && open) {
                ma_device_uninit(&device);
                open = false;
            }
            g_live = open;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void Fft(std::vector<std::complex<float>>& a) {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const float ang = -2.0f * 3.14159265f / len;
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<float> w(1);
                for (size_t k = 0; k < len / 2; ++k) {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    float g_level[4] = {}, g_peak[4] = { -40, -40, -40, -40 };
}

void AudioBands::Want(bool on) {
    if (!on) return;
    g_wantUntil = NowMs() + 2000;
    if (!g_started.exchange(true)) std::thread(Worker).detach();
}

bool AudioBands::Get(float out[4], float dt) {
    if (!g_live.load()) { for (int b = 0; b < 4; ++b) out[b] = g_level[b] = 0; return false; }
    std::vector<std::complex<float>> a(kFft);
    {
        std::lock_guard lock(g_mutex);
        const size_t start = (g_pos + g_ring.size() - kFft) % g_ring.size();
        for (int i = 0; i < kFft; ++i) {
            const float hann = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / (kFft - 1));
            a[i] = g_ring[(start + i) % g_ring.size()] * hann;
        }
    }
    Fft(a);
    // 4 полосы, как слышит ухо: бас, низкая середина, середина (голос), высокие
    static const float kEdges[5] = { 40, 180, 700, 2800, 11000 };
    const float binHz = static_cast<float>(g_rate.load()) / kFft;
    bool any = false;
    for (int b = 0; b < 4; ++b) {
        const int lo = std::max(1, static_cast<int>(kEdges[b] / binHz)), hi = std::min(kFft / 2 - 1, std::max(lo + 1, static_cast<int>(kEdges[b + 1] / binHz)));
        float sum = 0;
        for (int k = lo; k < hi; ++k) sum += std::norm(a[k]);
        const float db = 10.0f * std::log10(sum / (hi - lo) + 1e-12f);
        // своя «громкость» у каждой полосы: подстраиваемся под трек, чтобы и тихая, и громкая песня двигали столбики
        g_peak[b] = std::max(db, g_peak[b] - dt * 6.0f);
        const float target = std::clamp((db - (g_peak[b] - 28.0f)) / 28.0f, 0.0f, 1.0f);
        if (db > -75.0f) any = true;
        const float k = target > g_level[b] ? 1.0f - std::exp(-dt * 38.0f) : 1.0f - std::exp(-dt * 9.0f); // вверх — сразу, вниз — плавно
        g_level[b] += (target - g_level[b]) * k;
        out[b] = g_level[b];
    }
    return any;
}
