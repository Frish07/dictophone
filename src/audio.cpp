#include "audio.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <iomanip>
#include <sstream>

namespace dictophone {
namespace {
template<class T> void writeLE(std::ostream& out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) out.put(char((value >> (i * 8)) & 255));
}
std::wstring hresultText(HRESULT hr) {
    std::wostringstream s;
    s << L"Код Windows: 0x" << std::hex << std::uppercase << unsigned(hr);
    return s.str();
}
struct Startup { std::promise<std::wstring> ready; };
template<class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T* operator->() const { return p; }
};

bool captureLayout(const WAVEFORMATEX& format, bool& floating, DWORD& channelMask) {
    floating = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = format.wFormatTag == WAVE_FORMAT_PCM;
    channelMask = 0;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        floating = IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        pcm = IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
        channelMask = extended.dwChannelMask;
    }
    const unsigned bits = format.wBitsPerSample;
    const bool supported = floating ? bits == 32 : pcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    return supported && format.nSamplesPerSec && format.nChannels &&
        format.nBlockAlign == format.nChannels * (bits / 8u);
}

HRESULT openCapture(IMMDevice* device, bool raw, ComPtr<IAudioClient>& client, WAVEFORMATEX*& format) {
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client.p));
    if (FAILED(hr)) return hr;
    if (raw) {
        ComPtr<IAudioClient2> client2;
        hr = client->QueryInterface(IID_PPV_ARGS(&client2.p));
        if (FAILED(hr)) return hr;
        AudioClientProperties properties{sizeof(properties), FALSE, AudioCategory_Other, AUDCLNT_STREAMOPTIONS_RAW};
        hr = client2->SetClientProperties(&properties);
        if (FAILED(hr)) return hr;
    }
    hr = client->GetMixFormat(&format);
    if (FAILED(hr)) return hr;
    bool floating = false;
    DWORD mask = 0;
    if (!captureLayout(*format, floating, mask)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    return client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr);
}
}

bool WaveWriter::open(const std::filesystem::path& path, unsigned rate, unsigned channels, uint32_t channelMask) {
    close(); frames_ = 0;
    if (!channels || channels > UINT16_MAX / 3 || !rate || rate > UINT32_MAX / (channels * 3u)) return false;
    frameBytes_ = channels * 3;
    const bool extensible = channels > 2 || channelMask != 0;
    headerBytes_ = extensible ? 68 : 44;
    stream_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream_) return false;
    stream_.write("RIFF", 4); writeLE<uint32_t>(stream_, headerBytes_ - 8);
    stream_.write("WAVEfmt ", 8); writeLE<uint32_t>(stream_, extensible ? 40 : 16);
    writeLE<uint16_t>(stream_, extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM);
    writeLE<uint16_t>(stream_, uint16_t(channels));
    writeLE<uint32_t>(stream_, rate); writeLE<uint32_t>(stream_, rate * frameBytes_);
    writeLE<uint16_t>(stream_, uint16_t(frameBytes_)); writeLE<uint16_t>(stream_, 24);
    if (extensible) {
        writeLE<uint16_t>(stream_, 22);
        writeLE<uint16_t>(stream_, 24);
        writeLE<uint32_t>(stream_, channelMask);
        stream_.write(reinterpret_cast<const char*>(&KSDATAFORMAT_SUBTYPE_PCM), sizeof(GUID));
    }
    stream_.write("data", 4); writeLE<uint32_t>(stream_, 0);
    return bool(stream_);
}
bool WaveWriter::append(const std::vector<BYTE>& pcm24) {
    if (!stream_.is_open() || !stream_ || pcm24.size() % frameBytes_ != 0) return false;
    const uint64_t bytes = frames_ * frameBytes_ + pcm24.size();
    if (bytes + (bytes & 1) + headerBytes_ - 8 > UINT32_MAX) return false;
    if (pcm24.empty()) return true;
    stream_.write(reinterpret_cast<const char*>(pcm24.data()), std::streamsize(pcm24.size()));
    if (!stream_) return false;
    frames_ += pcm24.size() / frameBytes_;
    return true;
}
bool WaveWriter::checkpoint() {
    if (!stream_.is_open() || !stream_) return false;
    const uint32_t bytes = uint32_t(frames_ * frameBytes_);
    const auto dataEnd = std::streamoff(headerBytes_) + bytes;
    stream_.seekp(4); writeLE<uint32_t>(stream_, uint32_t(dataEnd - 8 + (bytes & 1)));
    stream_.seekp(headerBytes_ - 4); writeLE<uint32_t>(stream_, bytes);
    stream_.seekp(dataEnd);
    if (bytes & 1) { stream_.put('\0'); stream_.seekp(dataEnd); }
    stream_.flush();
    return bool(stream_);
}
bool WaveWriter::close() {
    if (!stream_.is_open()) return true;
    const bool ok = checkpoint(); stream_.close(); return ok;
}

float packPcm24(std::vector<BYTE>& output, const BYTE* data, size_t samples,
    unsigned bits, bool floatingPoint, bool silent) {
    output.resize(samples * 3);
    if (silent) {
        std::fill(output.begin(), output.end(), 0);
        return 0.f;
    }
    int32_t peak = 0;
    for (size_t i = 0; i < samples; ++i) {
        const BYTE* input = data + i * (bits / 8);
        int32_t sample = 0;
        if (floatingPoint) {
            float value; std::memcpy(&value, input, sizeof(value));
            if (std::isfinite(value)) {
                sample = int32_t(std::clamp(std::lround(std::clamp(value, -1.f, 1.f) * 8388608.), -8388608L, 8388607L));
            }
        } else if (bits == 8) {
            sample = (int32_t(input[0]) - 128) * 65536;
        } else {
            // PCM is little-endian. Keep its top three bytes, or pad a 16-bit sample on the right.
            const auto* pcm = input + (bits == 32 ? 1 : 0);
            const int32_t packed = bits == 16 ? (int32_t(pcm[0]) << 8) | (int32_t(pcm[1]) << 16) :
                pcm[0] | (int32_t(pcm[1]) << 8) | (int32_t(pcm[2]) << 16);
            sample = packed >= 0x800000 ? packed - 0x1000000 : packed;
        }
        peak = std::max(peak, std::abs(sample));
        const auto packed = uint32_t(sample);
        output[i * 3] = BYTE(packed);
        output[i * 3 + 1] = BYTE(packed >> 8);
        output[i * 3 + 2] = BYTE(packed >> 16);
    }
    return peak / 8388608.f;
}

Recorder::~Recorder() { finish(); DeleteCriticalSection(&errorLock_); }
std::wstring Recorder::error() const {
    EnterCriticalSection(&errorLock_); auto copy = error_; LeaveCriticalSection(&errorLock_); return copy;
}
bool Recorder::start(const std::filesystem::path& path, HWND notify, std::wstring& error) {
    finish(); stop_ = false; recording_ = true; frames_ = 0; peak_ = 0;
    EnterCriticalSection(&errorLock_); error_.clear(); LeaveCriticalSection(&errorLock_);
    Startup startup; auto future = startup.ready.get_future();
    thread_ = std::thread(&Recorder::run, this, path, notify, &startup);
    error = future.get();
    if (!error.empty()) { finish(); return false; }
    return true;
}
void Recorder::finish() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    recording_ = false; peak_ = 0;
}
void Recorder::reset() {
    finish();
    frames_ = 0;
    EnterCriticalSection(&errorLock_); error_.clear(); LeaveCriticalSection(&errorLock_);
}
void Recorder::run(std::filesystem::path path, HWND notify, void* startupPtr) {
    auto* startup = static_cast<Startup*>(startupPtr);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring failure;
    bool announced = false;
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* format = nullptr;
        WaveWriter writer;
        HRESULT hr = com;
        auto check = [&](HRESULT result, const wchar_t* what) {
            if (FAILED(result)) { failure = std::wstring(what) + L"\n" + hresultText(result); return false; }
            return true;
        };
        do {
            if (!check(hr, L"Не удалось запустить аудиосистему.")) break;
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator.p));
            if (!check(hr, L"Аудиоустройства недоступны.")) break;
            if (!check(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device.p),
                L"Микрофон не найден. Подключите микрофон и проверьте настройки Windows.")) break;
            hr = openCapture(device.p, true, client, format);
            if (FAILED(hr)) {
                if (client.p) client.p->Release();
                client.p = nullptr;
                CoTaskMemFree(format);
                format = nullptr;
                hr = openCapture(device.p, false, client, format);
            }
            if (!check(hr,
                L"Не удалось начать запись. Проверьте разрешение на микрофон в Windows.")) break;
            bool floating = false;
            DWORD channelMask = 0;
            captureLayout(*format, floating, channelMask);
            if (!check(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture.p)),
                L"Не удалось получить поток микрофона.")) break;
            rate_ = format->nSamplesPerSec;
            if (!writer.open(path, rate_, format->nChannels, channelMask)) {
                failure = L"Не удалось создать временный WAV. Проверьте свободное место."; break;
            }
            if (!check(client->Start(), L"Микрофон не запустился.")) break;
            startup->ready.set_value(L""); announced = true;
            ULONGLONG checkpoint = GetTickCount64();
            std::vector<BYTE> pcm24;
            while (!stop_) {
                UINT32 packet = 0;
                if (!check(capture->GetNextPacketSize(&packet), L"Микрофон отключён или недоступен.")) break;
                while (packet && !stop_) {
                    BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                    if (!check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), L"Ошибка чтения микрофона.")) break;
                    if (recording_) {
                        peak_ = packPcm24(pcm24, data, size_t(frames) * format->nChannels, format->wBitsPerSample,
                            floating, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                        if (!writer.append(pcm24)) failure = L"Не удалось записать WAV: диск заполнен или достигнут предел 4 ГБ. Запись остановлена.";
                        frames_ = writer.frames();
                    }
                    const HRESULT released = capture->ReleaseBuffer(frames);
                    if (!failure.empty() || !check(released, L"Ошибка аудиобуфера.")) break;
                    if (!check(capture->GetNextPacketSize(&packet), L"Микрофон отключён.")) break;
                }
                if (!failure.empty()) break;
                if (GetTickCount64() - checkpoint >= 1000) {
                    if (!writer.checkpoint()) { failure = L"Ошибка записи WAV на диск."; break; }
                    checkpoint = GetTickCount64();
                }
                Sleep(5);
            }
            client->Stop();
        } while (false);
        if (!writer.close() && failure.empty()) failure = L"Не удалось завершить WAV. Проверьте свободное место.";
        CoTaskMemFree(format);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (!announced) startup->ready.set_value(failure);
    else if (!failure.empty()) {
        EnterCriticalSection(&errorLock_); error_ = failure; LeaveCriticalSection(&errorLock_);
        recording_ = false;
        if (notify) PostMessageW(notify, ErrorMessage, 0, 0);
    }
}

std::filesystem::path findFfmpeg() {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    auto nextToApp = std::filesystem::path(executable).parent_path() / L"ffmpeg.exe";
    if (std::filesystem::exists(nextToApp)) return nextToApp;
    // Use an explicit colocated encoder; an unrelated PATH executable may be incompatible.
    return {};
}
const AudioEncoding* findAudioEncoding(const std::wstring& id) {
    for (const auto& encoding : AudioEncodings) {
        if (id == encoding.id) return &encoding;
    }
    return nullptr;
}
const AudioEncoding* defaultAudioEncoding(const std::wstring& extension) {
    for (const auto& encoding : AudioEncodings) {
        if (extension == encoding.extension) return &encoding;
    }
    return nullptr;
}
bool discardAudio(Recorder& recorder, const std::filesystem::path& draft, std::wstring& error) {
    recorder.finish();
    std::error_code ec;
    const bool exists = std::filesystem::exists(draft, ec);
    if (ec || (exists && !std::filesystem::is_regular_file(draft, ec))) {
        error = L"Не удалось удалить временную запись. Можно повторить удаление или сохранить аудио.";
        return false;
    }
    if (exists) std::filesystem::remove(draft, ec);
    if (ec) {
        error = L"Не удалось удалить временную запись. Можно повторить удаление или сохранить аудио.";
        return false;
    }
    recorder.reset(); error.clear(); return true;
}
bool exportAudio(const std::filesystem::path& source, const std::filesystem::path& destination,
    const std::wstring& extension, const std::filesystem::path& ffmpeg, std::wstring& error,
    const std::wstring& encodingId) {
    const auto* encoding = encodingId.empty() ? defaultAudioEncoding(extension) : findAudioEncoding(encodingId);
    if (!encoding || extension != encoding->extension) {
        error = L"Выбранная кодировка не подходит к формату файла.";
        return false;
    }
    try {
        std::filesystem::create_directories(destination.parent_path());
        if (std::filesystem::exists(destination)) { error = L"Файл уже существует. Выберите другое имя."; return false; }
        GUID exportId{};
        wchar_t temporaryName[39]{};
        if (FAILED(CoCreateGuid(&exportId)) || !StringFromGUID2(exportId, temporaryName, 39)) {
            error = L"Не удалось создать временное имя для экспорта.";
            return false;
        }
        const auto partial = destination.parent_path() / (std::wstring(temporaryName) + L".part." + extension);
        if (!encoding->ffmpegOptions[0]) {
            if (!CopyFileW(source.c_str(), partial.c_str(), TRUE)) {
                error = L"Не удалось скопировать WAV. Проверьте папку и свободное место."; return false;
            }
        } else {
            if (ffmpeg.empty()) {
                error = L"Для этой кодировки нужен ffmpeg.exe рядом с Dictophone.exe. Запустите scripts/setup-ffmpeg.ps1 или выберите PCM 24 бит (WAV). Временная запись сохранена.";
                return false;
            }
            std::wstring command = L"\"" + ffmpeg.wstring() + L"\" -hide_banner -loglevel error -nostdin -n -i \"" +
                source.wstring() + L"\" -vn " + encoding->ffmpegOptions + L" \"" + partial.wstring() + L"\"";
            STARTUPINFOW info{}; info.cb = sizeof(info); PROCESS_INFORMATION process{};
            if (!CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &info, &process)) {
                error = L"Не удалось запустить FFmpeg. Временный WAV сохранён."; return false;
            }
            CloseHandle(process.hThread);
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD exitCode = 1; GetExitCodeProcess(process.hProcess, &exitCode); CloseHandle(process.hProcess);
            if (exitCode != 0) {
                std::error_code ignored; std::filesystem::remove(partial, ignored);
                error = L"FFmpeg не смог сохранить аудио. Проверьте свободное место и сборку FFmpeg. Временный WAV сохранён."; return false;
            }
        }
        if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = L"Не удалось завершить сохранение. Готовый файл остался здесь:\n" + partial.wstring(); return false;
        }
        return true;
    } catch (const std::exception&) { error = L"Ошибка доступа к папке сохранения. Временный WAV сохранён."; return false; }
}
}
