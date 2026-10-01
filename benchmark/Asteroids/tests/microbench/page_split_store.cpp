// Microbenchmark behind docs/LIMITATIONS.md 5.1: what a store that straddles a 4 KB page boundary costs when
// write-combined memory is written between two such stores.
//
// Per iteration: copy 160 bytes into a 256-byte slot of a 16 MB buffer (nothing / ordinary memory /
// write-combined memory) - what nvrhi's D3D12 writeBuffer does for a volatile constant buffer - then one
// 32-byte or 16-byte store to ordinary memory that either straddles a page boundary or not.
//
// Build and run (x64 developer prompt):  cl /nologo /O2 /arch:AVX2 /EHsc page_split_store.cpp && page_split_store
// Not part of the CMake build. Result on the benchmark machine (Ryzen 7 8845HS, Windows 10 19045):
//   no buffer write                  aligned  0.62 ns   page-split 32-byte  7.27 ns   page-split 16-byte  7.24 ns
//   160 B into normal memory         aligned  6.12 ns   page-split 32-byte  8.44 ns   page-split 16-byte  8.52 ns
//   160 B into write-combined memory aligned 19.24 ns   page-split 32-byte 71.98 ns   page-split 16-byte 72.92 ns
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
static double tscPerNs;
int main()
{
    SetThreadAffinityMask(GetCurrentThread(), 1 << 2);
    { LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a); uint64_t t0 = __rdtsc(); Sleep(200);
      QueryPerformanceCounter(&b); uint64_t t1 = __rdtsc(); tscPerNs = double(t1 - t0) / (double(b.QuadPart - a.QuadPart) / f.QuadPart * 1e9); }
    const size_t chunk = 16u << 20;
    char* wc  = (char*)VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE);
    char* wb  = (char*)VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    char* mem = (char*)VirtualAlloc(nullptr, 4 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!wc || !wb || !mem) { printf("alloc failed\n"); return 1; }
    memset(wc, 0, chunk); memset(wb, 0, chunk); memset(mem, 1, 4 * 4096);
    alignas(64) char src[160]; memset(src, 5, sizeof src);
    __m256i v = _mm256_set1_epi32(7);
    const int slots = int(chunk / 256);
    for (int kind = 0; kind < 3; ++kind)             // 0: no upload write, 1: normal memory, 2: write-combined memory
        for (int split = 0; split < 3; ++split)      // 0: aligned, 1: page-split 32-byte store, 2: page-split 16-byte store
        {
            char* dst = kind == 2 ? wc : wb;
            volatile char* p = mem + 4096 + (split ? -8 : -64);
            double best = 1e30;
            for (int rep = 0; rep < 7; ++rep)
            {
                const uint64_t t0 = __rdtsc();
                for (int i = 0; i < slots; ++i)
                {
                    if (kind) memcpy(dst + size_t(i) * 256, src, 160);
                    src[i & 127] = char(i);
                    if (split == 2) _mm_storeu_si128((__m128i*)p, _mm256_castsi256_si128(v));
                    else _mm256_storeu_si256((__m256i*)p, v);
                }
                const uint64_t t1 = __rdtsc();
                const double ns = double(t1 - t0) / tscPerNs / slots;
                if (ns < best) best = ns;
            }
            printf("%-34s + %-26s : %7.2f ns per iteration\n",
                   kind == 0 ? "no buffer write" : kind == 1 ? "160 B into normal memory" : "160 B into write-combined memory",
                   split == 0 ? "aligned 32-byte store" : split == 1 ? "page-split 32-byte store" : "page-split 16-byte store", best);
        }
    return 0;
}
