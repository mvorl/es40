// ES40 emulator.
#ifndef ES40_X11_DISPATCH_H
#define ES40_X11_DISPATCH_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

// Callers retain captured buffers until invoke() returns.
class x11_dispatcher
{
public:
  bool start()
  {
    if(std::this_thread::get_id() != owner) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if(state != initial) return false;
    state = running;
    return true;
  }

  bool invoke(std::function<void()> fn)
  {
    // Permit nested calls and owner cleanup after stop.
    if(std::this_thread::get_id() == owner)
    {
      fn();
      return true;
    }

    call request{std::move(fn)};
    std::unique_lock<std::mutex> lock(mutex);
    if(state != running) return false;
    calls.push_back(&request);
    available.notify_one();
    request.finished.wait(lock, [&request] { return request.complete; });
    if(request.error) std::rethrow_exception(request.error);
    return true;
  }

  // idle must contain exceptions so shutdown calls can still run.
  void pump(const std::function<void()>& idle = {})
  {
    if(std::this_thread::get_id() != owner) return;
    std::unique_lock<std::mutex> lock(mutex);
    for(;;)
    {
      const auto ready = [this] { return state != running || !calls.empty(); };
      if(idle) available.wait_for(lock, std::chrono::milliseconds(10), ready);
      else available.wait(lock, ready);
      // Drain accepted calls before stopping.
      if(state != running && calls.empty()) return;
      for(unsigned count = 0; count < 32 && !calls.empty(); ++count)
      {
        call* request = calls.front();
        calls.pop_front();
        lock.unlock();
        std::exception_ptr error;
        try { request->fn(); }
        catch(...) { error = std::current_exception(); }
        lock.lock();
        request->error = error;
        request->complete = true;
        request->finished.notify_one();
      }
      if(state == running && idle)
      {
        lock.unlock();
        idle();
        lock.lock();
      }
    }
  }

  void stop()
  {
    std::lock_guard<std::mutex> lock(mutex);
    state = stopped;
    available.notify_one();
  }

private:
  struct call
  {
    std::function<void()> fn;
    std::exception_ptr error;
    std::condition_variable finished;
    bool complete = false;
  };

  const std::thread::id owner = std::this_thread::get_id();
  enum { initial, running, stopped } state = initial;
  std::mutex mutex;
  std::condition_variable available;
  std::deque<call*> calls;
};

#endif
