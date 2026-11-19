#pragma once
#include <windows.h>
#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace dictophone {
struct AudioEncoding {
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* extension;
    const wchar_t* ffmpegOptions;
};

inline constexpr std::array<AudioEncoding, 10> AudioEncodings{{
    {L"pcm_s24le", L"PCM 24 бит (WAV)", L"wav", L""},
    {L"pcm_s16le", L"PCM 16 бит (WAV)", L"wav", L"-c:a pcm_s16le"},
    {L"pcm_s32le", L"PCM 32 бит (WAV)", L"wav", L"-c:a pcm_s32le"},
    {L"pcm_f32le", L"Float32 (WAV)", L"wav", L"-c:a pcm_f32le"},
    {L"mp3", L"MP3 — 192 кбит/с", L"mp3", L"-c:a libmp3lame -b:a 192k"},
    {L"vorbis", L"Vorbis (OGG)", L"ogg", L"-c:a libvorbis -q:a 5"},
    {L"opus", L"Opus (OGG) — 160 кбит/с", L"ogg", L"-c:a libopus -b:a 160k -vbr on"},
    {L"flac", L"FLAC", L"flac", L"-c:a flac"},
    {L"aac", L"AAC (M4A) — 192 кбит/с", L"m4a", L"-c:a aac -b:a 192k"},
    {L"alac", L"ALAC (M4A)", L"m4a", L"-c:a alac"},
}};

const AudioEncoding* findAudioEncoding(const std::wstring& id);
const AudioEncoding* defaultAudioEncoding(const std::wstring& extension);

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
    const std::filesystem::path& ffmpeg, std::wstring& error,
    const std::wstring& encodingId = L"");
}
