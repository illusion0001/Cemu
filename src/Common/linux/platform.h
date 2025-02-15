#include <shared_mutex>

class SlimRWLock
{
public:
	void LockRead()
	{
		m_sm.lock_shared();
	}

	void UnlockRead()
	{
		m_sm.unlock_shared();
	}

	void LockWrite()
	{
		m_sm.lock();
	}

	void UnlockWrite()
	{
		m_sm.unlock();
	}

private:
	std::shared_mutex m_sm;
};

inline uint32_t GetExceptionError() 
{
    return errno;
}

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xutil.h>
#undef False
#undef True
#undef None
#undef Bool
#undef Status
#undef Success
#undef ClientMessage

// cpu id (somewhat hacky, reorganize later)
void (__cpuid)(int __cpuVal[4], unsigned int __leaf);
void __cpuidex (int __cpuid_info[4], int __leaf, int __subleaf);

// placeholder
uint32_t GetTickCount();

// strcpy_s and strcat_s implementations
template<size_t N> 
void strcpy_s(char (&dst)[N], const char* src)  
{
	if(N == 0)
		return;
	char* dstP = dst;
	const char* end = src + N - 1;
	while(src < end)
	{
		char c = *src;
		*dstP = c;
		if(c == '\0')
			return;
		dstP++;
		src++;
		c++;
	}
	*dstP = '\0';
	return;
}

template<size_t N> 
void strcat_s(char (&dst)[N], const char* src)  
{
	if(N == 0)
		return;
	char* dstP = dst;
	const char* end = dstP + N - 1;
	while(dstP < end && *dstP != '\0')
		dstP++;
	while(dstP < end)
	{
		char c = *src;
		*dstP = c;
		if(c == '\0')
			return;
		dstP++;
		src++;
		c++;
	}
	*dstP = '\0';
	return;
}
