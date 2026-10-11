#include "SoundEngine.h"

#include "globals.h"
#include "Overlay.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

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
			LogOverlayW(LOG_NOTICE, L"Sound: cannot load %ls\n", path.c_str());
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
			float volume, float pitch, bool loop, bool overlap)
	{
		std::lock_guard<std::mutex> held(lock());

		auto added = sections.emplace(key_of(name), std::make_unique<Section>(name, path, volume, pitch, loop, overlap));
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
			float volume, float pitch, bool loop, bool overlap) :
		name(name),
		path(path),
		volume(volume),
		pitch(pitch),
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
			loaded->PlayOneShot(volume, pitch, 0.0f);
			return;
		}

		track.Start(loaded, loop, volume, start_ms);
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

	void Sound::PlayOneShot(float volume, float pitch, float pan)
	{
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

	bool Track::Start(Sound *sound, bool loop, float volume, double start_ms)
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
