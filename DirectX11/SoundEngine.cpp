#include "SoundEngine.h"

#include "globals.h"
#include "Overlay.h"

#include <mutex>
#include <unordered_map>

namespace SoundEngine {

	// The engine, the sounds it loaded, and the lock over both.
	// A command list usually runs on the game's render thread, with the update,
	// but a deferred context can run one from elsewhere,
	// and neither DirectXTK's voice pool nor the cache below is safe against that.
	static std::mutex &lock()
	{
		static std::mutex mutex;
		return mutex;
	}

	static std::unique_ptr<DirectX::AudioEngine> engine;
	static std::unordered_map<std::wstring, std::unique_ptr<Sound>> sounds;
	// Set once the engine has been asked for, whether or not it came up:
	// a machine with no audio device must not be asked again every frame.
	static bool attempted = false;

	static DirectX::AudioEngine *get_engine()
	{
		if (attempted)
			return engine.get();

		attempted = true;

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
		LogInfo("Sound: audio engine started%s\n", engine->IsAudioDevicePresent() ? "" : " (no audio device, silent)");
		return engine.get();
	}

	bool Started()
	{
		std::lock_guard<std::mutex> held(lock());

		return !!engine;
	}

	Sound *Load(const std::wstring &path)
	{
		std::lock_guard<std::mutex> held(lock());

		auto cached = sounds.find(path);
		if (cached != sounds.end())
			return cached->second.get();

		DirectX::AudioEngine *audio = get_engine();
		if (!audio)
			return NULL;

		std::unique_ptr<DirectX::SoundEffect> effect;
		try {
			effect = std::make_unique<DirectX::SoundEffect>(audio, path.c_str());
		} catch (const std::exception &e) {
			LogOverlayW(LOG_NOTICE, L"Sound: cannot load %ls\n", path.c_str());
			LogInfo("Sound: %s\n", e.what());
			// Remembered as unloadable:
			// a per draw call sound must not open a missing file every frame.
			sounds.emplace(path, nullptr);
			return NULL;
		}

		LogInfo("Sound: loaded %S (%zu ms)\n", path.c_str(), effect->GetSampleDurationMS());

		auto added = sounds.emplace(path, std::make_unique<Sound>(std::move(effect)));
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

		// Stops every voice before the sounds they are playing go away:
		engine->Suspend();
		sounds.clear();
		engine.reset();
	}

	void Sound::PlayOneShot(float volume, float pitch, float pan)
	{
		effect->Play(volume, pitch, pan);
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

		std::lock_guard<std::mutex> held(lock());

		if (voice && engine)
			engine->DestroyVoice(voice);
	}

	bool Track::Start(Sound *sound, bool loop, float volume)
	{
		XAUDIO2_BUFFER buffer = {};
		XAUDIO2_BUFFER_WMA wma = {};
		XAUDIO2_VOICE_STATE voice_state;

		if (!sound)
			return false;

		Stop();

		std::lock_guard<std::mutex> held(lock());

		if (!engine)
			return false;

		if (!voice) {
			engine->AllocateVoice(sound->Format(), DirectX::SoundEffectInstance_Default, false, &voice);
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
		std::lock_guard<std::mutex> held(lock());

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
		return (double)(voice_state.SamplesPlayed - samples_at_start) * 1000.0 / (double)rate;
	}
}
