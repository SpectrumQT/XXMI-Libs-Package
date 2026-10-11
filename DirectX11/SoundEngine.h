#pragma once

#include <memory>
#include <string>

#include <Audio.h>

// Sound playback for the command list, over DirectXTK's XAudio2 wrapper.
//
// Nothing here is created until an ini asks for a sound:
// a mod that makes no sound pays for no audio device, no mixer and no thread.
// Once created the engine is held for the life of the process,
// since XAudio2 outlives the D3D11 device and has nothing to do with it.
//
// sound_engine_update() belongs once a frame, beside the input dispatch:
// it retires finished one-shots and notices the audio device being pulled out.
namespace SoundEngine {

	// A decoded wav, held once per path however many ini sections name it.
	class Sound {
	public:
		explicit Sound(std::unique_ptr<DirectX::SoundEffect> effect) : effect(std::move(effect)) {}

		// Fire and forget, for a hit or a click: the engine owns the voice,
		// recycles it when the sound ends, and lets several overlap.
		void PlayOneShot(float volume, float pitch, float pan);

		size_t Samples() const;      // Length in samples
		size_t Milliseconds() const; // Length in ms

		const WAVEFORMATEX *Format() const;
		// What SubmitSourceBuffer() needs, to play this sound on another voice:
		bool FillSubmitBuffer(XAUDIO2_BUFFER *buffer, XAUDIO2_BUFFER_WMA *wma) const;

	private:
		std::unique_ptr<DirectX::SoundEffect> effect;
	};

	// A sound playing on a voice of its own, which is what a music track needs:
	// it can be stopped and, the point of it, asked where it is.
	// DirectXTK's SoundEffectInstance reports no position,
	// so the voice is allocated from the engine and driven here,
	// where XAudio2's own sample counter is reachable.
	class Track {
	public:
		~Track();

		bool Start(Sound *sound, bool loop, float volume);
		void Stop();
		bool Playing() const;

		// Where the track is, counted from the samples XAudio2 has played:
		// the clock to place notes against, not a frame counter.
		double PositionMs() const;

	private:
		IXAudio2SourceVoice *voice = nullptr;
		uint32_t rate = 0;
		// SamplesPlayed counts from when the voice was created, across stops,
		// so a position is measured from where the last Start() began:
		uint64_t samples_at_start = 0;
		bool started = false;
	};

	// The wav at this path, loaded on the first ask and cached.
	// NULL with a logged reason if it will not load,
	// so that a missing file is a silent sound rather than a dead mod.
	Sound *Load(const std::wstring &path);

	void sound_engine_update();
	void sound_engine_shutdown();

	// Whether anything has asked for sound yet, for the log and the overlay:
	bool Started();
}
