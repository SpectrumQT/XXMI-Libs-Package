#pragma once

#include <windows.h>
#include <vector>

// This version of EnterCriticalSection will use the function and line number
// in any lock stacks dumped when potential deadlock hazards are detected:
#define EnterCriticalSectionPretty(lock) \
	_EnterCriticalSectionPretty(lock, __FUNCTION__, __LINE__)
void _EnterCriticalSectionPretty(CRITICAL_SECTION *lock, char *function, int line);

// Use this when initialising a critical section in 3DMigoto to give it a nice
// name in lock stack dumps rather than using its address.
//
// **AVOID CALLING THIS FROM GLOBAL CONSTRUCTORS**
// https://yosefk.com/c++fqa/ctors.html#fqa-10.12
#define InitializeCriticalSectionPretty(lock) \
	_InitializeCriticalSectionPretty(lock, #lock)
void _InitializeCriticalSectionPretty(CRITICAL_SECTION *lock, char *lock_name);

void enable_lock_dependency_checks();
struct held_lock_info {
	CRITICAL_SECTION *lock;
	uintptr_t ret;
	size_t stack_hash;
	char *function;
	int line;
};
typedef std::vector<held_lock_info> LockStack;

// RAII guard for a Windows CRITICAL_SECTION.
//
// The critical section is acquired when the guard is constructed and
// automatically released when the guard goes out of scope. This ensures
// the lock is released even when returning early or unwinding due to an
// exception.
class CriticalSectionLockGuard
{
public:
    // Acquires the critical section and records the source location for
    // diagnostic/logging purposes.
    CriticalSectionLockGuard(CRITICAL_SECTION* lock, char* function, int line)
        : m_lock(lock)
    {
        _EnterCriticalSectionPretty(lock, function, line);
    }

    // Releases the critical section when the guard leaves its scope.
    ~CriticalSectionLockGuard()
    {
        LeaveCriticalSection(m_lock);
    }

private:
    CRITICAL_SECTION* m_lock;

    // A lock guard represents exclusive ownership of a lock scope, so it
    // must not be copied or moved.
    CriticalSectionLockGuard(const CriticalSectionLockGuard&) = delete;
    CriticalSectionLockGuard& operator=(const CriticalSectionLockGuard&) = delete;
    CriticalSectionLockGuard(CriticalSectionLockGuard&&) = delete;
    CriticalSectionLockGuard& operator=(CriticalSectionLockGuard&&) = delete;
};

// Creates a uniquely named lock guard using the current source line.
// The guard acquires the critical section immediately and releases it
// automatically at the end of the current scope.
#define CriticalSectionGuard(lock) \
    CriticalSectionLockGuard lock_critical_section_##__LINE__(lock, __FUNCTION__, __LINE__)