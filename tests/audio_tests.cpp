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
void checkWave(const fs::path& path, uint32_t frames, unsigned rate, unsigned channels, uint32_t mask = 0) {
    const uint32_t dataOffset = mask || channels > 2 ? 68 : 44;
    const uint32_t dataBytes = frames * channels * 3;
    const uint32_t fileSize = dataOffset + dataBytes + (dataBytes & 1);
    std::ifstream in(path, std::ios::binary); char header[12]{}; in.read(header, 12);
    expect(std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header+8, "WAVE", 4) == 0, "WAV signature");
    in.seekg(4); expect(read32(in) == fileSize - 8, "RIFF size includes padding");
    in.seekg(22); expect((read32(in) & 0xffff) == channels, "preserves channel count");
    in.seekg(24); expect(read32(in) == rate, "sample rate");
    in.seekg(34); expect((read32(in) & 0xffff) == 24, "24-bit samples");
    if (dataOffset == 68) { in.seekg(40); expect(read32(in) == mask, "preserves channel mask"); }
    in.seekg(dataOffset - 4); expect(read32(in) == dataBytes, "data size excludes padding");
    expect(fs::file_size(path) == fileSize, "actual WAV length");
}
void conversions() {
    const float stereo[]{.5f, -.5f, 2.f, -2.f, std::numeric_limits<float>::quiet_NaN(), 1.f / 8388608.f};
    std::vector<BYTE> pcm;
    const auto peak = packPcm24(pcm, reinterpret_cast<const BYTE*>(stereo), 6, 32, true, false);
    expect(pcm == std::vector<BYTE>{0,0,0x40, 0,0,0xc0, 0xff,0xff,0x7f, 0,0,0x80, 0,0,0, 1,0,0},
        "preserves opposite-phase channels and quiet samples, clips out-of-range floats and handles NaN");
    expect(peak == 1.f, "peak uses the loudest channel");
    expect(packPcm24(pcm, nullptr, 8, 32, true, true) == 0.f && pcm == std::vector<BYTE>(24, 0),
        "silent packets never dereference null");

    const BYTE pcm8[]{0,128,255};
    packPcm24(pcm, pcm8, 3, 8, false, false);
    expect(pcm == std::vector<BYTE>{0,0,0x80, 0,0,0, 0,0,0x7f}, "PCM8 unsigned conversion");
    const BYTE pcm16[]{0,0x80, 0xff,0x7f};
    packPcm24(pcm, pcm16, 2, 16, false, false);
    expect(pcm == std::vector<BYTE>{0,0,0x80, 0,0xff,0x7f}, "PCM16 signed conversion");
    const BYTE pcm24[]{1,0,0x80, 0xff,0xff,0x7f};
    packPcm24(pcm, pcm24, 2, 24, false, false);
    expect(pcm == std::vector<BYTE>(pcm24, pcm24 + 6), "PCM24 bytes are preserved");
    const BYTE pcm32[]{0x20,1,0,0x80, 0x80,0xfe,0xff,0x7f};
    packPcm24(pcm, pcm32, 2, 32, false, false);
    expect(pcm == std::vector<BYTE>{1,0,0x80, 0xfe,0xff,0x7f}, "PCM32 keeps its high 24 bits");
}
void verifyEncoding(const fs::path& probe, const fs::path& file, const AudioEncoding& encoding) {
    auto log = file; log += L".probe.txt";
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS, 0, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
    expect(output != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE, "create probe handles");
    STARTUPINFOW info{}; info.cb = sizeof(info); info.dwFlags = STARTF_USESTDHANDLES;
    info.hStdOutput = output; info.hStdError = output; info.hStdInput = input;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + probe.wstring() + L"\" -v error -select_streams a:0 "
        L"-show_entries stream=codec_name,sample_rate,channels -of default=noprint_wrappers=1 \"" + file.wstring() + L"\"";
    const BOOL started = CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &info, &process);
    CloseHandle(output); CloseHandle(input);
    expect(started, "start bundled ffprobe");
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
    DWORD exitCode = 1; GetExitCodeProcess(process.hProcess, &exitCode); CloseHandle(process.hProcess);
    expect(wait == WAIT_OBJECT_0 && exitCode == 0, "probe exported audio");
    std::ifstream in(log);
    const std::string metadata((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::wstring wideId(encoding.id);
    const std::string id(wideId.begin(), wideId.end());
    expect(metadata.find("codec_name=" + id + "\n") != std::string::npos, "export uses the chosen codec");
    expect(metadata.find("sample_rate=48000\n") != std::string::npos && metadata.find("channels=2\n") != std::string::npos,
        "chosen codec preserves test sample rate and channels");
}
void writersAndExports(const fs::path& root) {
    const auto mono = root / L"odd.wav";
    WaveWriter odd;
    expect(odd.open(mono, 48000, 1, 0) && odd.append({1,2,3}) && odd.checkpoint(), "odd-sized WAV checkpoint");
    checkWave(mono, 1, 48000, 1);
    expect(odd.append({4,5,6}) && odd.close(), "append replaces the checkpoint padding");
    checkWave(mono, 2, 48000, 1);
    std::ifstream monoData(mono, std::ios::binary);
    monoData.seekg(44);
    for (int value = 1; value <= 6; ++value) expect(monoData.get() == value, "padding does not interrupt audio");

    const auto source = root / L"Исходник тест.wav";
    WaveWriter writer; expect(writer.open(source, 48000, 2, 3), "open stereo WAV with Unicode path");
    std::vector<float> samples(48000 * 2);
    for (size_t frame = 0; frame < 48000; ++frame) {
        const auto tone = float(std::sin(frame * 440. * 6.283185307179586 / 48000.) * .5);
        samples[frame * 2] = tone;
        samples[frame * 2 + 1] = -tone;
    }
    std::vector<BYTE> pcm;
    packPcm24(pcm, reinterpret_cast<const BYTE*>(samples.data()), 12000 * 2, 32, true, false);
    expect(writer.append(pcm), "first segment");
    expect(writer.checkpoint(), "checkpoint"); checkWave(source, 12000, 48000, 2, 3);
    packPcm24(pcm, reinterpret_cast<const BYTE*>(samples.data() + 12000 * 2), 36000 * 2, 32, true, false);
    expect(writer.append(pcm), "second segment");
    expect(writer.close(), "finalize"); checkWave(source, 48000, 48000, 2, 3);
    std::wstring error;
    auto wav = root / L"Экспорт с пробелами.wav";
    const auto existingPartial = root / L"Экспорт с пробелами.part.wav";
    std::ofstream(existingPartial, std::ios::binary).put('x');
    expect(exportAudio(source, wav, L"wav", {}, error), "WAV export"); checkWave(wav, 48000, 48000, 2, 3);
    expect(std::ifstream(existingPartial, std::ios::binary).get() == 'x', "named export preserves unrelated partial files");
    expect(!exportAudio(source, wav, L"wav", {}, error), "never overwrite an existing recording");
    expect(!exportAudio(source, root / L"no-encoder.mp3", L"mp3", {}, error), "report missing encoder");
    expect(!exportAudio(source, root / L"mismatch.wav", L"wav", {}, error, L"opus"), "reject incompatible codec and container");
    expect(!fs::exists(root / L"mismatch.wav"), "codec mismatch creates no output");
    expect(!exportAudio(source, root / L"unknown.wav", L"wav", {}, error, L"unknown"), "reject invalid codec id");
    expect(findAudioEncoding(L"unknown") == nullptr && defaultAudioEncoding(L"unknown") == nullptr, "invalid settings have no codec");
    expect(std::wstring(defaultAudioEncoding(L"wav")->id) == L"pcm_s24le" &&
        std::wstring(defaultAudioEncoding(L"ogg")->id) == L"vorbis" &&
        std::wstring(defaultAudioEncoding(L"m4a")->id) == L"aac", "old settings retain their codecs");
    expect(fs::exists(source), "keep source on encoder failure");
    const fs::path encoder = fs::absolute(L"app/ffmpeg.exe");
    if (fs::exists(encoder)) {
        for (const std::wstring ext : {L"mp3", L"ogg", L"flac", L"m4a"}) {
            auto destination = root / (L"Формат с пробелами." + ext);
            expect(exportAudio(source, destination, ext, encoder, error), "compressed export");
            expect(fs::file_size(destination) > 100, "encoded audio is nonempty");
            std::wcout << L"PASS: export " << ext << L"\n";
        }
        const auto probe = encoder.parent_path() / L"ffprobe.exe";
        expect(fs::exists(probe), "bundled ffprobe is available for codec verification");
        for (const auto& encoding : AudioEncodings) {
            const auto destination = root / (std::wstring(L"Кодировка ") + encoding.id + L"." + encoding.extension);
            expect(exportAudio(source, destination, encoding.extension, encoder, error, encoding.id), "export selected codec");
            verifyEncoding(probe, destination, encoding);
            std::wcout << L"PASS: selected codec " << encoding.id << L"\n";
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
