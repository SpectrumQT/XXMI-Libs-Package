#include "SoundEngine.h"

#include "globals.h"
#include "Overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SoundEngine {

	// The lock over the engine, the sounds it loaded and the sections below.
	// Which functions take it is laid out in the header;
	// everything static here assumes it is already held.
	static std::mutex &lock()
	{
		static std::mutex mutex;
		return mutex;
	}

	static std::unique_ptr<DirectX::AudioEngine> engine;
	static std::unordered_map<std::wstring, std::unique_ptr<Sound>> sounds;
	static std::unordered_map<std::wstring, std::unique_ptr<Section>> sections;
	// Set once the engine has been asked for, whether or not it came up:
	// a machine with no audio device must not be asked again every frame.
	static bool attempted = false;

	// Both maps below are keyed in lower case:
	// an ini may name a sound in whatever case it likes, as it may a resource,
	// and a path on Windows is case insensitive as well.
	static std::wstring key_of(const std::wstring &name)
	{
		std::wstring key(name);

		std::transform(key.begin(), key.end(), key.begin(), ::towlower);
		return key;
	}

	// Milliseconds the performance counter has advanced since `from`.
	static double elapsed_ms(const LARGE_INTEGER &from)
	{
		LARGE_INTEGER now, freq;

		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);

		return (double)(now.QuadPart - from.QuadPart) * 1000.0 / (double)freq.QuadPart;
	}

	static DirectX::AudioEngine *get_engine()
	{
		LARGE_INTEGER before;

		if (attempted)
			return engine.get();

		attempted = true;

		QueryPerformanceCounter(&before);

		DirectX::AUDIO_ENGINE_FLAGS flags = DirectX::AudioEngine_Default;
#if _DEBUG
		flags = flags | DirectX::AudioEngine_Debug;
#endif

		try {
			engine = std::make_unique<DirectX::AudioEngine>(flags);
		} catch (const std::exception &e) {
			LogInfo("Sound: audio engine unavailable: %s\n", e.what());
			return NULL;
		}

		// No audio device is not an error:
		// DirectXTK runs silent and picks one up later if the user plugs one in,
		// which Update() notices.
		LogInfo("Sound: audio engine started in %.1fms%s\n", elapsed_ms(before),
				engine->IsAudioDevicePresent() ? "" : " (no audio device, silent)");
		return engine.get();
	}

	bool Started()
	{
		std::lock_guard<std::mutex> held(lock());

		return !!engine;
	}

	// Everything below is reached only once DirectXTK has refused a file.
	// It reports its own reason through OutputDebugString,
	// which a release build drops,
	// so the file is read again here, in words a mod author can act on.

	static uint16_t read16(const std::vector<uint8_t> &buf, size_t pos)
	{
		return (uint16_t)(buf[pos] | (buf[pos + 1] << 8));
	}

	static uint32_t read32(const std::vector<uint8_t> &buf, size_t pos)
	{
		return (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
			((uint32_t)buf[pos + 2] << 16) | ((uint32_t)buf[pos + 3] << 24);
	}

	static constexpr uint32_t fourcc(char a, char b, char c, char d)
	{
		return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
			((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
	}

	// The front of the file, which is as much as its headers need.
	static bool read_head(const std::wstring &path, std::vector<uint8_t> *head, DWORD *error)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD read = 0;

		if (file == INVALID_HANDLE_VALUE) {
			*error = GetLastError();
			return false;
		}

		head->resize(64 * 1024);

		if (!ReadFile(file, head->data(), (DWORD)head->size(), &read, NULL))
			*error = GetLastError();

		CloseHandle(file);
		head->resize(read);

		return !*error;
	}

	// As much of a wav header as judging the file needs.
	struct WavHead {
		uint16_t format = 0;
		uint16_t channels = 0;
		uint16_t bits = 0;
		uint32_t rate = 0;
		bool fmt_chunk = false;
		bool data_chunk = false;
	};

	static WavHead scan_wav_chunks(const std::vector<uint8_t> &head)
	{
		WavHead wav;
		size_t pos = 12; // Past "RIFF" <size> "WAVE"

		while (pos + 8 <= head.size()) {
			uint32_t id = read32(head, pos);
			uint32_t size = read32(head, pos + 4);

			pos += 8;

			if (id == fourcc('d', 'a', 't', 'a'))
				wav.data_chunk = true;

			if (id == fourcc('f', 'm', 't', ' ') && pos + 16 <= head.size()) {
				wav.fmt_chunk = true;
				wav.format = read16(head, pos);
				wav.channels = read16(head, pos + 2);
				wav.rate = read32(head, pos + 4);
				wav.bits = read16(head, pos + 14);

				// An extensible header names its real format elsewhere:
				// the first field of the subformat GUID.
				if (wav.format == 0xfffe && pos + 28 <= head.size())
					wav.format = (uint16_t)read32(head, pos + 24);
			}

			// Widened first: a size of all ones would wrap the step to zero.
			pos += (size_t)size + (size & 1);
		}

		return wav;
	}

	// The formats likely to turn up in a file called .wav,
	// of which DirectXTK takes PCM, IEEE float and MS ADPCM.
	// NULL for one not worth naming.
	static const wchar_t *format_name(uint16_t format)
	{
		switch (format) {
			case 0x0001: return L"PCM";
			case 0x0002: return L"MS ADPCM";
			case 0x0003: return L"IEEE float";
			case 0x0006: return L"A-law";
			case 0x0007: return L"mu-law";
			case 0x0011: return L"IMA ADPCM";
			case 0x0055: return L"MP3";
			case 0x0161: return L"WMA";
			case 0x0162: return L"WMA Pro";
			case 0x2000: return L"AC-3";
			case 0x674f:
			case 0x6771: return L"Vorbis";
			case 0xf1ac: return L"FLAC";
		}

		return NULL;
	}

	// A file named .wav that is really something else is the usual reason,
	// so say what it looks like rather than leaving the author to guess.
	static const wchar_t *disguise(const std::vector<uint8_t> &head)
	{
		if (head.size() >= 3 && !memcmp(head.data(), "ID3", 3))
			return L", it looks like an MP3";

		if (head.size() >= 2 && head[0] == 0xff && (head[1] & 0xe0) == 0xe0)
			return L", it looks like an MP3";

		if (head.size() >= 4 && !memcmp(head.data(), "OggS", 4))
			return L", it looks like an Ogg";

		if (head.size() >= 4 && !memcmp(head.data(), "fLaC", 4))
			return L", it looks like a FLAC";

		if (head.size() >= 12 && !memcmp(head.data() + 4, "ftyp", 4))
			return L", it looks like an MP4 or M4A";

		return L"";
	}

	static std::wstring wav_problem(const std::wstring &path)
	{
		std::vector<uint8_t> head;
		DWORD error = 0;
		wchar_t detail[192];

		if (!read_head(path, &head, &error)) {
			swprintf_s(detail, L"it cannot be read (error %u)", error);
			return detail;
		}

		if (head.size() < 44)
			return L"it is too short to hold a wav header";

		if (read32(head, 0) != fourcc('R', 'I', 'F', 'F') || read32(head, 8) != fourcc('W', 'A', 'V', 'E'))
			return std::wstring(L"it is not a RIFF WAVE file") + disguise(head);

		WavHead wav = scan_wav_chunks(head);

		if (!wav.fmt_chunk)
			return L"it has no fmt chunk";

		if (!wav.data_chunk)
			return L"it has no data chunk";

		const wchar_t *name = format_name(wav.format);

		// A supported format and a rejected file leaves the data itself:
		// truncated, or longer than the header says.
		if (wav.format == 0x0001 || wav.format == 0x0002 || wav.format == 0x0003) {
			swprintf_s(detail, L"its header reads as %ls, %u channels, %u-bit, %u Hz, so the file itself is damaged",
					name, wav.channels, wav.bits, wav.rate);
			return detail;
		}

		if (name)
			swprintf_s(detail, L"it holds %ls data, which XAudio2 does not take - convert it to 16-bit PCM", name);
		else
			swprintf_s(detail, L"its format tag %u is not one XAudio2 takes - convert it to 16-bit PCM", wav.format);

		return detail;
	}

	// The wav at this path, loaded on the first ask and cached.
	// NULL with a logged reason if it will not load,
	// so that a missing file is a silent sound rather than a dead mod.
	static Sound *load_locked(const std::wstring &path)
	{
		LARGE_INTEGER before;
		std::wstring key = key_of(path);

		auto cached = sounds.find(key);
		if (cached != sounds.end())
			return cached->second.get();

		DirectX::AudioEngine *audio = get_engine();
		if (!audio)
			return NULL;

		QueryPerformanceCounter(&before);

		std::unique_ptr<DirectX::SoundEffect> effect;
		try {
			effect = std::make_unique<DirectX::SoundEffect>(audio, path.c_str());
		} catch (const std::exception &e) {
			LogOverlayW(LOG_NOTICE, L"Sound: cannot load %ls: %ls\n", path.c_str(), wav_problem(path).c_str());
			LogInfo("Sound: %s\n", e.what());
			// Remembered as unloadable:
			// a per draw call sound must not open a missing file every frame.
			sounds.emplace(key, nullptr);
			return NULL;
		}

		LogInfo("Sound: loaded %S: %zu ms of audio, read in %.1fms\n",
				path.c_str(), effect->GetSampleDurationMS(), elapsed_ms(before));

		auto added = sounds.emplace(key, std::make_unique<Sound>(std::move(effect)));
		return added.first->second.get();
	}

	void sound_engine_update()
	{
		std::lock_guard<std::mutex> held(lock());

		if (!engine)
			return;

		if (engine->Update())
			return;

		// Update() only fails for a lost audio device.
		// Reset() takes whatever device is there now,
		// and gives up quietly if there still is none:
		// the sounds themselves stay loaded either way.
		if (engine->IsCriticalError()) {
			LogInfo("Sound: audio device lost, resetting\n");
			engine->Reset();
		}
	}

	void sound_engine_shutdown()
	{
		std::lock_guard<std::mutex> held(lock());

		if (!engine)
			return;

		// Stops every voice before the sounds they are playing go away,
		// and the sections own those voices:
		engine->Suspend();
		sections.clear();
		sounds.clear();
		engine.reset();
	}

	Section *Declare(const std::wstring &name, const std::wstring &path,
			float volume, float speed, bool loop, bool overlap)
	{
		std::lock_guard<std::mutex> held(lock());

		auto added = sections.emplace(key_of(name), std::make_unique<Section>(name, path, volume, speed, loop, overlap));
		Section *section = added.first->second.get();

		// The engine and the file are wanted now, while the ini is being read:
		// a device opening and a file being read are a few ms either way,
		// and the first play is inside a frame, where that is a hitch.
		section->Preload();

		return section;
	}

	Section *Find(const std::wstring &name)
	{
		std::lock_guard<std::mutex> held(lock());

		auto found = sections.find(key_of(name));
		return found == sections.end() ? NULL : found->second.get();
	}

	void forget_sounds()
	{
		std::lock_guard<std::mutex> held(lock());

		// The sections own the voices,
		// so they go before the sounds those voices are playing:
		sections.clear();
		sounds.clear();
	}

	Section::Section(const std::wstring &name, const std::wstring &path,
			float volume, float speed, bool loop, bool overlap) :
		name(name),
		path(path),
		volume(volume),
		speed(speed),
		loop(loop),
		overlap(overlap)
	{
	}

	bool Section::Preload()
	{
		return !!Loaded();
	}

	Sound *Section::Loaded()
	{
		if (sound || load_failed)
			return sound;

		sound = load_locked(path);
		load_failed = !sound;
		return sound;
	}

	void Section::Play(double start_ms)
	{
		std::lock_guard<std::mutex> held(lock());

		Sound *loaded = Loaded();
		if (!loaded)
			return;

		if (overlap) {
			// Nothing to seek or restart:
			// each play is its own voice, which the engine takes back at the end.
			loaded->PlayOneShot(volume, speed, 0.0f);
			return;
		}

		track.Start(loaded, loop, volume, speed, start_ms);
	}

	void Section::Stop()
	{
		std::lock_guard<std::mutex> held(lock());

		if (!overlap)
			track.Stop();
	}

	bool Section::Playing()
	{
		std::lock_guard<std::mutex> held(lock());

		return overlap ? false : track.Playing();
	}

	double Section::PositionMs()
	{
		std::lock_guard<std::mutex> held(lock());

		return overlap ? 0.0 : track.PositionMs();
	}

	double Section::LengthMs()
	{
		std::lock_guard<std::mutex> held(lock());

		Sound *loaded = Loaded();
		return loaded ? (double)loaded->Milliseconds() : 0.0;
	}

	void Sound::PlayOneShot(float volume, float speed, float pan)
	{
		// DirectXTK asks for the speed as a shift in octaves,
		// which is the same resampling the other way around:
		float pitch = speed == 1.0f ? 0.0f : log2f(speed);

		// DirectXTK throws when it cannot get a voice:
		// a sound too many, not a reason to take the game down with us.
		try {
			effect->Play(volume, pitch, pan);
		} catch (const std::exception &e) {
			LogInfo("Sound: cannot play one shot: %s\n", e.what());
		}
	}

	size_t Sound::Samples() const
	{
		return effect->GetSampleDuration();
	}

	size_t Sound::Milliseconds() const
	{
		return effect->GetSampleDurationMS();
	}

	const WAVEFORMATEX *Sound::Format() const
	{
		return effect->GetFormat();
	}

	bool Sound::FillSubmitBuffer(XAUDIO2_BUFFER *buffer, XAUDIO2_BUFFER_WMA *wma) const
	{
		return effect->FillSubmitBuffer(*buffer, *wma);
	}

	Track::~Track()
	{
		Stop();

		if (voice && engine)
			engine->DestroyVoice(voice);
	}

	bool Track::Start(Sound *sound, bool loop, float volume, float speed, double start_ms)
	{
		XAUDIO2_BUFFER buffer = {};
		XAUDIO2_BUFFER_WMA wma = {};
		XAUDIO2_VOICE_STATE voice_state;

		if (!sound)
			return false;

		Stop();

		if (!engine)
			return false;

		if (!voice) {
			// Throws once the engine is out of instance voices:
			// a mod with too many sounds, not a reason to take the game down.
			try {
				engine->AllocateVoice(sound->Format(), DirectX::SoundEffectInstance_Default, false, &voice);
			} catch (const std::exception &e) {
				LogInfo("Sound: cannot allocate a voice: %s\n", e.what());
				return false;
			}

			if (!voice)
				return false;

			rate = sound->Format()->nSamplesPerSec;
		}

		// A voice counts the samples it has played since it was created,
		// across stops and starts, so the position is measured from here:
		voice->GetState(&voice_state);
		samples_at_start = voice_state.SamplesPlayed;

		bool have_wma = sound->FillSubmitBuffer(&buffer, &wma);
		buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

		// Where in the sound to begin, in samples.
		// Past the end would be rejected by XAudio2,
		// so it reads as "from the start" instead:
		if (start_ms > 0.0) {
			UINT32 begin = (UINT32)(start_ms * rate / 1000.0);
			if (begin < sound->Samples()) {
				buffer.PlayBegin = begin;
				buffer.PlayLength = 0; // To the end
			} else {
				start_ms = 0.0;
			}
		}
		this->start_ms = start_ms;

		if (FAILED(voice->SubmitSourceBuffer(&buffer, have_wma ? &wma : NULL)))
			return false;

		voice->SetVolume(volume);
		// SamplesPlayed counts the buffer's own samples, not the mixer's,
		// so the position stays in the sound's own time whatever the speed:
		voice->SetFrequencyRatio(speed);

		if (FAILED(voice->Start(0)))
			return false;

		started = true;
		return true;
	}

	void Track::Stop()
	{
		if (!voice || !started)
			return;

		voice->Stop(0);
		voice->FlushSourceBuffers();
		started = false;
	}

	bool Track::Playing() const
	{
		XAUDIO2_VOICE_STATE voice_state;

		if (!voice || !started)
			return false;

		voice->GetState(&voice_state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		return voice_state.BuffersQueued > 0;
	}

	double Track::PositionMs() const
	{
		XAUDIO2_VOICE_STATE voice_state;

		if (!voice || !started || !rate)
			return 0.0;

		voice->GetState(&voice_state);
		return start_ms + (double)(voice_state.SamplesPlayed - samples_at_start) * 1000.0 / (double)rate;
	}
}
