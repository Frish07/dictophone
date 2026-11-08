#include "../src/audio.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using namespace dictophone;
namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
uint32_t read32(std::ifstream& in) {
    unsigned char bytes[4]{}; in.read(reinterpret_cast<char*>(bytes), 4);
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
void checkWave(const fs::path& path, uint32_t frames, unsigned rate) {
    std::ifstream in(path, std::ios::binary); char header[12]{}; in.read(header, 12);
    expect(std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header+8, "WAVE", 4) == 0, "WAV signature");
    in.seekg(4); expect(read32(in) == frames * 2 + 36, "RIFF size");
    in.seekg(24); expect(read32(in) == rate, "sample rate");
    in.seekg(40); expect(read32(in) == frames * 2, "data size");
    expect(fs::file_size(path) == 44ull + frames * 2, "actual WAV length");
}
void conversions() {
    float stereo[]{1, -1, .5f, .5f, 2, 2, -2, -2, std::numeric_limits<float>::quiet_NaN(), 0};
    auto pcm = toMono(reinterpret_cast<const BYTE*>(stereo), 5, 2, 32, true, false);
    expect(pcm[0] == 0 && pcm[1] == 16383 && pcm[2] == 32767 && pcm[3] == -32767 && pcm[4] == 0, "float, clipping, downmix, NaN");
    auto silence = toMono(nullptr, 8, 2, 32, true, true);
    expect(silence == std::vector<int16_t>(8, 0), "silent packets never dereference null");
    int16_t signed16[]{-32768, 32767};
    pcm = toMono(reinterpret_cast<const BYTE*>(signed16), 2, 1, 16, false, false);
    expect(pcm[0] == -32767 && pcm[1] >= 32765, "PCM16 sign");
    BYTE signed24[]{0,0,0x80, 0xff,0xff,0x7f};
    pcm = toMono(signed24, 2, 1, 24, false, false);
    expect(pcm[0] == -32767 && pcm[1] >= 32766, "PCM24 sign extension");
    int32_t signed32[]{INT32_MIN, INT32_MAX};
    pcm = toMono(reinterpret_cast<const BYTE*>(signed32), 2, 1, 32, false, false);
    expect(pcm[0] == -32767 && pcm[1] >= 32766, "PCM32 sign");
    BYTE unsigned8[]{0,128,255};
    pcm = toMono(unsigned8, 3, 1, 8, false, false);
    expect(pcm[0] == -32767 && pcm[1] == 0 && pcm[2] > 32000, "PCM8 unsigned");
}
void writersAndExports(const fs::path& root) {
    const auto source = root / L"Исходник тест.wav";
    WaveWriter writer; expect(writer.open(source, 48000), "open Unicode WAV path");
    std::vector<int16_t> samples(48000);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = int16_t(std::sin(i * 440. * 6.283185307179586 / 48000.) * 16000.);
    expect(writer.append(samples.data(), 12000), "first segment");
    expect(writer.checkpoint(), "checkpoint"); checkWave(source, 12000, 48000);
    expect(writer.append(samples.data()+12000, 36000), "second segment");
    expect(writer.close(), "finalize"); checkWave(source, 48000, 48000);
    std::wstring error;
    auto wav = root / L"Экспорт с пробелами.wav";
    expect(exportAudio(source, wav, L"wav", {}, error), "WAV export"); checkWave(wav, 48000, 48000);
    expect(!exportAudio(source, wav, L"wav", {}, error), "never overwrite an existing recording");
    expect(!exportAudio(source, root / L"no-encoder.mp3", L"mp3", {}, error), "report missing encoder");
    expect(fs::exists(source), "keep source on encoder failure");
    const fs::path encoder = fs::absolute(L"app/ffmpeg.exe");
    if (fs::exists(encoder)) {
        for (const std::wstring ext : {L"mp3", L"ogg", L"flac", L"m4a"}) {
            auto destination = root / (L"Формат с пробелами." + ext);
            expect(exportAudio(source, destination, ext, encoder, error), "compressed export");
            expect(fs::file_size(destination) > 100, "encoded audio is nonempty");
            std::wcout << L"PASS: export " << ext << L"\n";
        }
    } else std::cout << "SKIP: compressed exports (run scripts/setup-ffmpeg.ps1)\n";
    Recorder recorder;
    const auto disposable = root / L"Удалить черновик.wav";
    fs::copy_file(source, disposable);
    expect(!discardAudio(recorder, root, error), "never delete a directory");
    expect(fs::exists(disposable), "failed deletion preserves draft");
    expect(discardAudio(recorder, disposable, error), "discard a temporary WAV");
    expect(!fs::exists(disposable), "discard removes the draft file");
    expect(recorder.seconds() == 0 && !recorder.isRecording() && recorder.peak() == 0, "discard resets timer and audio state");
    expect(fs::exists(source) && fs::exists(wav), "discard preserves other recordings");
    std::cout << "PASS: discard / reset / deletion failure preserves recordings\n";
}
void microphone(const fs::path& root) {
    Recorder recorder; std::wstring error; auto path = root / L"microphone.wav";
    if (!recorder.start(path, nullptr, error)) { std::wcerr << error << L"\n"; throw std::runtime_error("microphone startup"); }
    // Some USB/Bluetooth endpoints need extra time for their first packet.
    for (int attempt = 0; attempt < 100 && recorder.seconds() < .2 && recorder.error().empty(); ++attempt) Sleep(20);
    const double first = recorder.seconds();
    if (first < .15) std::wcerr << L"Capture error: " << recorder.error() << L"; duration: " << first << L"\n";
    expect(first >= .15, "microphone delivers packets");
    recorder.setRecording(false); Sleep(50); const double paused = recorder.seconds();
    Sleep(350); expect(recorder.seconds() == paused, "no samples or silence during pause");
    recorder.setRecording(true); Sleep(350); recorder.setRecording(false);
    Sleep(30); const double resumed = recorder.seconds();
    expect(resumed > paused + .15, "resume appends samples");
    // Repeated radio-style holds are one file; releases never append silence.
    for (int i = 0; i < 3; ++i) {
        recorder.setRecording(true); Sleep(100); recorder.setRecording(false); Sleep(30);
        const double released = recorder.seconds(); Sleep(100);
        expect(recorder.seconds() == released, "radio release pauses");
    }
    recorder.finish();
    expect(recorder.error().empty(), "no background capture error");
    expect(recorder.seconds() < 1.5 && recorder.seconds() > .7, "duration excludes pauses");
    std::cout << "PASS: real microphone / pause / resume / repeated radio holds (" << recorder.seconds() << "s)\n";
    expect(discardAudio(recorder, path, error), "discard recorded microphone audio");
    expect(!fs::exists(path) && recorder.seconds() == 0 && !recorder.isRecording(), "discard clears real capture and timer");
    // Test reopening after finish, including a session which is too short to save.
    expect(recorder.start(root / L"restart.wav", nullptr, error), "restart microphone");
    recorder.finish();
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        const auto root = fs::absolute(L"build/test-output") / (L"run-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);
        conversions(); std::cout << "PASS: audio conversion\n";
        writersAndExports(root); std::cout << "PASS: WAV checkpoints / Unicode paths / exports / recovery\n";
        if (argc > 1 && std::wstring(argv[1]) == L"--microphone") microphone(root);
        std::cout << "All checks passed.\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\n"; return 1; }
}
