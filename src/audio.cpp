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
}

bool WaveWriter::open(const std::filesystem::path& path, unsigned rate) {
    close(); frames_ = 0;
    stream_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream_) return false;
    stream_.write("RIFF", 4); writeLE<uint32_t>(stream_, 36);
    stream_.write("WAVEfmt ", 8); writeLE<uint32_t>(stream_, 16);
    writeLE<uint16_t>(stream_, 1); writeLE<uint16_t>(stream_, 1);
    writeLE<uint32_t>(stream_, rate); writeLE<uint32_t>(stream_, rate * 2);
    writeLE<uint16_t>(stream_, 2); writeLE<uint16_t>(stream_, 16);
    stream_.write("data", 4); writeLE<uint32_t>(stream_, 0);
    return bool(stream_);
}
bool WaveWriter::append(const int16_t* samples, size_t count) {
    if (!stream_ || (frames_ + count) * 2 > 0xFFFFFFFFull - 36) return false;
    stream_.write(reinterpret_cast<const char*>(samples), std::streamsize(count * 2));
    if (!stream_) return false;
    frames_ += count;
    return true;
}
bool WaveWriter::checkpoint() {
    if (!stream_.is_open() || !stream_) return false;
    stream_.seekp(4); writeLE<uint32_t>(stream_, uint32_t(36 + frames_ * 2));
    stream_.seekp(40); writeLE<uint32_t>(stream_, uint32_t(frames_ * 2));
    stream_.seekp(0, std::ios::end); stream_.flush();
    return bool(stream_);
}
bool WaveWriter::close() {
    if (!stream_.is_open()) return true;
    const bool ok = checkpoint(); stream_.close(); return ok;
}

std::vector<int16_t> toMono(const BYTE* data, size_t frames,
    unsigned channels, unsigned bits, bool floatingPoint, bool silent) {
    std::vector<int16_t> result(frames, 0);
    if (silent || !channels) return result;
    for (size_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (unsigned channel = 0; channel < channels; ++channel) {
            const BYTE* p = data + (i * channels + channel) * (bits / 8);
            double sample = 0;
            if (floatingPoint && bits == 32) {
                float f; std::memcpy(&f, p, 4); sample = std::isfinite(f) ? f : 0;
            } else if (bits == 16) {
                int16_t n; std::memcpy(&n, p, 2); sample = n / 32768.;
            } else if (bits == 24) {
                int32_t n = p[0] | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
                if (n & 0x800000) n -= 0x1000000;
                sample = n / 8388608.;
            } else if (bits == 32) {
                int32_t n; std::memcpy(&n, p, 4); sample = n / 2147483648.;
            } else if (bits == 8) sample = (int(p[0]) - 128) / 128.;
            sum += sample;
        }
        result[i] = int16_t(std::clamp(sum / channels, -1., 1.) * 32767.);
    }
    return result;
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
            if (!check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&client.p)), L"Не удалось открыть микрофон.")) break;
            if (!check(client->GetMixFormat(&format), L"Не удалось определить формат микрофона.")) break;
            bool floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            bool pcm = format->wFormatTag == WAVE_FORMAT_PCM;
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
                auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
                floating = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                pcm = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
            }
            if ((!floating && !pcm) || (floating && format->wBitsPerSample != 32) ||
                (pcm && format->wBitsPerSample != 8 && format->wBitsPerSample != 16 &&
                format->wBitsPerSample != 24 && format->wBitsPerSample != 32)) {
                failure = L"Микрофон использует неподдерживаемый формат звука."; break;
            }
            if (!check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr),
                L"Не удалось начать запись. Проверьте разрешение на микрофон в Windows.")) break;
            if (!check(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture.p)),
                L"Не удалось получить поток микрофона.")) break;
            rate_ = format->nSamplesPerSec;
            if (!writer.open(path, rate_)) { failure = L"Не удалось создать временный WAV. Проверьте свободное место."; break; }
            if (!check(client->Start(), L"Микрофон не запустился.")) break;
            startup->ready.set_value(L""); announced = true;
            ULONGLONG checkpoint = GetTickCount64();
            while (!stop_) {
                UINT32 packet = 0;
                if (!check(capture->GetNextPacketSize(&packet), L"Микрофон отключён или недоступен.")) break;
                while (packet && !stop_) {
                    BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                    if (!check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), L"Ошибка чтения микрофона.")) break;
                    if (recording_) {
                        auto mono = toMono(data, frames, format->nChannels, format->wBitsPerSample,
                            floating, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                        float peak = 0;
                        for (auto sample : mono) peak = std::max(peak, std::abs(int(sample)) / 32768.f);
                        peak_ = peak;
                        if (!writer.append(mono.data(), mono.size())) failure = L"Не удалось записать WAV: диск заполнен или достигнут предел 4 ГБ. Запись остановлена.";
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
    const std::wstring& extension, const std::filesystem::path& ffmpeg, std::wstring& error) {
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
        if (extension == L"wav") {
            if (!CopyFileW(source.c_str(), partial.c_str(), TRUE)) {
                error = L"Не удалось скопировать WAV. Проверьте папку и свободное место."; return false;
            }
        } else {
            if (ffmpeg.empty()) { error = L"Для этого формата нужен ffmpeg.exe рядом с Dictophone.exe. Запустите scripts/setup-ffmpeg.ps1. Запись сохранена во временной папке; можно выбрать WAV и повторить сохранение."; return false; }
            std::wstring codec;
            if (extension == L"mp3") codec = L"-c:a libmp3lame -b:a 192k";
            else if (extension == L"ogg") codec = L"-c:a libvorbis -q:a 5";
            else if (extension == L"flac") codec = L"-c:a flac";
            else if (extension == L"m4a") codec = L"-c:a aac -b:a 192k";
            else { error = L"Неизвестный формат."; return false; }
            std::wstring command = L"\"" + ffmpeg.wstring() + L"\" -hide_banner -loglevel error -nostdin -n -i \"" +
                source.wstring() + L"\" -vn " + codec + L" \"" + partial.wstring() + L"\"";
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
