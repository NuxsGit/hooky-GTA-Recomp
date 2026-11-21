// pipeline_completion.h: the shared wait/completion primitive used by the shader worker.
// Wait returns true only when the job is ready; a zero or expired budget must return false
// without blocking the caller for longer than the budget.
#include "pipeline_completion.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
  {
    gtav::native::PipelineCompletion job;
    double waited = -1.0;
    if (job.Wait(2.0, waited)) { std::cerr << "fresh job must not be ready\n"; return 1; }
    // Budget was spent: the wait must cost ~2 ms, not 0 and not much more.
    if (waited < 1.0 || waited > 500.0) { std::cerr << "budget wait not honored (" << waited << " ms)\n"; return 1; }
  }
  {
    gtav::native::PipelineCompletion job;
    double waited = 0.0;
    if (job.Wait(0.0, waited)) { std::cerr << "zero budget must not block\n"; return 1; }
    if (waited != 0.0) { std::cerr << "zero budget must not accumulate time\n"; return 1; }
  }
  {
    gtav::native::PipelineCompletion job;
    job.Signal();
    double waited = 0.0;
    if (!job.Wait(1000.0, waited)) { std::cerr << "signaled job must be ready\n"; return 1; }
    if (waited > 500.0) { std::cerr << "ready check was slow (" << waited << " ms)\n"; return 1; }
  }
  {
    gtav::native::PipelineCompletion job;
    std::thread signaler([&job] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      job.Signal();
    });
    double waited = 0.0;
    if (!job.Wait(1000.0, waited)) { std::cerr << "signaler after the wait must wake it\n"; return 1; }
    if (waited < 3.0) { std::cerr << "Wait did not observe the signaler (" << waited << " ms)\n"; return 1; }
    signaler.join();
  }
  return 0;
}
