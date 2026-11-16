#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace dictophone {
class WaveWriter {
public:
    bool open(const std::filesystem::path& path, unsigned rate, unsigned channels, uint32_t channelMask);
    bool append(const std::vector<BYTE>& pcm24);
    bool checkpoint();
    bool close();
    uint64_t frames() const { return frames_; }
    ~WaveWriter() { close(); }
private:
    std::fstream stream_;
    uint64_t frames_ = 0;
    unsigned frameBytes_ = 0;
    uint32_t headerBytes_ = 0;
};

// Pack each channel independently and return the peak for the meter.
float packPcm24(std::vector<BYTE>& output, const BYTE* data, size_t samples,
    unsigned bits, bool floatingPoint, bool silent);

class Recorder {
public:
    ~Recorder();
    bool start(const std::filesystem::path& path, HWND notify, std::wstring& error);
    void setRecording(bool recording) { recording_ = recording; }
    bool isRecording() const { return recording_; }
    void finish();
    void reset();
    double seconds() const { return double(frames_.load()) / rate_.load(); }
    unsigned sampleRate() const { return rate_; }
    float peak() const { return recording_ ? peak_.load() : 0.f; }
    std::wstring error() const;
    static constexpr UINT ErrorMessage = WM_APP + 1;
private:
    void run(std::filesystem::path path, HWND notify, void* startup);
    std::atomic_bool stop_{false}, recording_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<unsigned> rate_{48000};
    std::atomic<float> peak_{0};
    std::thread thread_;
    mutable CRITICAL_SECTION errorLock_{};
    std::wstring error_;
public:
    Recorder() { InitializeCriticalSection(&errorLock_); }
};

std::filesystem::path findFfmpeg();
// Close capture before deleting its temporary WAV; preserve counters on failure.
bool discardAudio(Recorder& recorder, const std::filesystem::path& draft, std::wstring& error);
// Never overwrite a destination; keep the source WAV on all errors.
bool exportAudio(const std::filesystem::path& source,
    const std::filesystem::path& destination, const std::wstring& extension,
    const std::filesystem::path& ffmpeg, std::wstring& error);
}
