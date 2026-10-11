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
//
// A command list usually runs on the game's render thread, with the update,
// but a deferred context can run one from elsewhere,
// so one lock covers the engine and everything it holds.
// The free functions and Section's methods take it;
// Sound and Track never take it themselves.
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

		// start_ms is where in the sound to begin,
		// which is what `play = SoundFoo, 500` asks for.
		bool Start(Sound *sound, bool loop, float volume, double start_ms);
		void Stop();
		bool Playing() const;

		// Where the track is in the sound,
		// counted from the samples XAudio2 has actually played:
		// the clock to place notes against, not a frame counter.
		// Includes whatever offset it was started at.
		double PositionMs() const;

	private:
		IXAudio2SourceVoice *voice = nullptr;
		uint32_t rate = 0;
		// SamplesPlayed counts from when the voice was created, across stops,
		// so a position is measured from where the last Start() began:
		uint64_t samples_at_start = 0;
		double start_ms = 0.0;
		bool started = false;
	};

	// A [Sound*] section: the wav it names, and how the ini wants it played.
	// Declared while the ini is read, loaded on the first play,
	// so a section nothing ever plays costs nothing but its name.
	class Section {
	public:
		Section(const std::wstring &name, const std::wstring &path,
				float volume, float pitch, bool loop, bool overlap);

		// play = SoundFoo, or play = SoundFoo, <ms>
		void Play(double start_ms);
		void Stop();

		bool Playing();
		double PositionMs();
		double LengthMs();

	private:
		std::wstring name;
		std::wstring path;
		float volume;
		float pitch;
		bool loop;
		// true: every play starts a voice of its own,
		// so hits can overlap, but none of them can be stopped or placed;
		// false: one voice, restarted by each play, which a music track needs.
		bool overlap;

		Sound *sound = nullptr;
		bool load_failed = false;
		Track track; // Unused while overlap is set

		Sound *Loaded();
	};

	// Called as the ini is read.
	// The handle is what a play or stop command holds,
	// so that running one is not a name lookup.
	Section *Declare(const std::wstring &name, const std::wstring &path,
			float volume, float pitch, bool loop, bool overlap);
	Section *Find(const std::wstring &name);
	// Drops every declaration on an ini reload. Sounds already playing stop,
	// since the sections they belong to are about to be replaced.
	void forget_sounds();

	void sound_engine_update();
	void sound_engine_shutdown();

	// Whether anything has asked for sound yet, for the log and the overlay:
	bool Started();
}
