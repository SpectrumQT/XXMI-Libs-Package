#include "Dx12Common.h"

#include "util.h"

Dx12Stats dx12_stats;

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
	if (LogFile)
		fflush(LogFile);

	frames = 0;
	last_report = now;
}
