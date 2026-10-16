#pragma once
// The frame schedule of the three modes, shared by both renderers so that the thread orchestration
// (what is recorded where, when lists are executed) is identical and only the graphics API differs.
//
// A renderer R provides:
//   void OpenList(int pass, int chunk);                    // begin recording list (pass, chunk)
//   void RecordPass(int pass, int chunk, int chunkCount, int listPass);
//                                   // record this chunk's share of `pass` into list (listPass, chunk);
//                                   // chunk 0 also records the pass prologue, chunk chunkCount-1 the epilogue
//   void CloseList(int pass, int chunk);
//   void ExecuteLists(int pass, int chunkCount);           // execute lists (pass, 0..chunkCount-1) in order
//   void ExecuteAllPasses();                               // execute lists (0,0) .. (kNumPasses-1,0) in order
//
// st:       one list records every pass -> one execute
// mt_scene: passes 0..kNumPasses-2 are jobs on the worker pool, the direct pass is recorded on the
//           main thread; one execute of all lists in pass order
// mt_chunk: every pass is split into `chunkCount` chunks recorded concurrently (the main thread takes
//           jobs too); each pass is executed as soon as its chunks are recorded, as the sample did
//
// Columns: render = everything until the last list of the frame is closed; submit = the execute calls.

#include "benchmark.h"
#include "scene.h"
#include "workers.h"

namespace MtScene
{

template <class R>
void RunFrame(R& r, Mode mode, uint32_t chunkCount, WorkerPool& pool, Benchmark::FrameTimes& times)
{
    Benchmark::Lap lap;
    switch (mode)
    {
        case Mode::SingleThreaded:
        {
            r.OpenList(0, 0);
            for (int pass = 0; pass < kNumPasses; ++pass)
                r.RecordPass(pass, 0, 1, 0); // every pass into list (0, 0)
            r.CloseList(0, 0);
            lap.To(times.render);
            r.ExecuteLists(0, 1);
            lap.To(times.submit);
            break;
        }
        case Mode::PerScene:
        {
            pool.Dispatch(kNumPasses - 1, [&r](uint32_t pass) {
                r.OpenList(int(pass), 0);
                r.RecordPass(int(pass), 0, 1, int(pass));
                r.CloseList(int(pass), 0);
            });
            r.OpenList(kPassDirect, 0);
            r.RecordPass(kPassDirect, 0, 1, kPassDirect);
            r.CloseList(kPassDirect, 0);
            pool.Wait();
            lap.To(times.render);
            r.ExecuteAllPasses();
            lap.To(times.submit);
            break;
        }
        case Mode::PerChunk:
        {
            for (int pass = 0; pass < kNumPasses; ++pass)
            {
                pool.Dispatch(chunkCount, [&r, pass, chunkCount](uint32_t chunk) {
                    r.OpenList(pass, int(chunk));
                    r.RecordPass(pass, int(chunk), int(chunkCount), pass);
                    r.CloseList(pass, int(chunk));
                });
                pool.Help();
                pool.Wait();
                lap.To(times.render);
                r.ExecuteLists(pass, int(chunkCount));
                lap.To(times.submit);
            }
            break;
        }
    }
}

} // namespace MtScene
