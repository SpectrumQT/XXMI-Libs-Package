#include "Dx12Common.h"

#include <smmintrin.h>

#include "util.h"

#include "../globals.h"
#include "../IniHandler.h"

#include "HashDump.h"

Dx12Stats dx12_stats;
volatile UINT dx12_frame;

uint32_t Dx12Crc32cWriteCombined(uint32_t hash, const BYTE *src, size_t length)
{
	__declspec(align(16)) BYTE chunk[16 * 1024];
	size_t count, head, i;

	while (length) {
		count = length < sizeof(chunk) ? length : sizeof(chunk);
		head = (16 - ((uintptr_t)src & 15)) & 15;
		if (head > count)
			head = count;

		memcpy(chunk, src, head);
		for (i = head; i + 16 <= count; i += 16)
			_mm_storeu_si128((__m128i*)(chunk + i), _mm_stream_load_si128((__m128i*)(src + i)));
		memcpy(chunk + i, src + i, count - i);

		hash = crc32c_hw(hash, chunk, count);
		src += count;
		length -= count;
	}

	return hash;
}

void Dx12LogForwardedQuery(const char *proxy, REFIID riid, HRESULT hr)
{
	static SRWLOCK lock = SRWLOCK_INIT;
	static GUID seen[64];
	static unsigned count;
	bool is_new = true;
	unsigned i;

	AcquireSRWLockExclusive(&lock);
	for (i = 0; i < count && is_new; i++)
		is_new = !IsEqualGUID(seen[i], riid);
	if (is_new && count < ARRAYSIZE(seen))
		seen[count++] = riid;
	else
		is_new = false;
	ReleaseSRWLockExclusive(&lock);

	if (is_new)
		Dx12LogInfo("%s forwarded QueryInterface(%s) to the original = 0x%x\n", proxy, NameFromIID(riid).c_str(), hr);
}

void Dx12FramePresented()
{
	static ULONGLONG last_report;
	static unsigned frames, reports;
	ULONGLONG now = GetTickCount64();
	double seconds, per_frame;
	LONG64 draws, dispatches, closed, executed, calls;

	dx12_frame++;
	Dx12DumpFramePresented();

	if (!G->gConfigInitialized
	    && (GetSystemTicks() - G->ticks_at_launch) / 1'000'000.0f > G->gConfigInitializationDelay)
		LoadSkippedIncludes();

	if (!last_report)
		last_report = now;
	frames++;
	if (now - last_report < 5000)
		return;

	draws = InterlockedExchange64(&dx12_stats.draws, 0);
	dispatches = InterlockedExchange64(&dx12_stats.dispatches, 0);
	closed = InterlockedExchange64(&dx12_stats.lists_closed, 0);
	executed = InterlockedExchange64(&dx12_stats.lists_executed, 0);
	calls = InterlockedExchange64(&dx12_stats.execute_calls, 0);
	seconds = (now - last_report) / 1000.0;
	per_frame = 1.0 / frames;

	// Keeps a long session from filling the log at the default log level
	if (reports++ < 12) {
		Dx12LogInfo("%u frames in %.1fs (%.1f fps), per frame: %.0f draws, %.0f dispatches, %.1f command lists closed, %.1f executed in %.1f calls\n",
			frames, seconds, frames / seconds, draws * per_frame, dispatches * per_frame,
			closed * per_frame, executed * per_frame, calls * per_frame);
	} else {
		Dx12LogDebug("%u frames in %.1fs (%.1f fps), per frame: %.0f draws, %.0f dispatches, %.1f command lists closed, %.1f executed in %.1f calls\n",
			frames, seconds, frames / seconds, draws * per_frame, dispatches * per_frame,
			closed * per_frame, executed * per_frame, calls * per_frame);
	}
	Dx12HashReport(reports <= 12);
	if (LogFile)
		fflush(LogFile);

	frames = 0;
	last_report = now;
}
