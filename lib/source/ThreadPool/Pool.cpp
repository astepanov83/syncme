#include <cassert>

#include <Syncme/ProcessThreadId.h>
#include <Syncme/Sleep.h>
#include <Syncme/ThreadPool/Pool.h>
#include <Syncme/TimePoint.h>

#define LOCK_GUARD() \
  std::lock_guard<std::mutex> guard(Lock); \
  Owner = GetCurrentThreadId()

#define SET_TIMER() \
  SetWaitableTimer(Timer, 4 * MaxIdleTime / 3, 0, nullptr)

using namespace Syncme::ThreadPool;

static const size_t MAX_UNUSED_THREADS = 12;
static const size_t MAX_THREADS = 100;
static const long MAX_IDLE_TIME = 3000; // 3 sec

namespace Syncme::ThreadPool
{
  std::atomic<uint64_t> ThreadsTotal;
  std::atomic<uint64_t> ThreadsUnused;
  std::atomic<uint64_t> ThreadsStopped;
  std::atomic<uint64_t> LockedInRun;
  std::atomic<uint64_t> OnTimerCalls;
  std::atomic<uint64_t> Errors;
}

Pool::Pool()
  : MaxUnusedThreads(MAX_UNUSED_THREADS)
  , MaxThreads(MAX_THREADS)
  , MaxIdleTime(MAX_IDLE_TIME)
  , Mode(OVERFLOW_MODE::WAIT)
  , Timer(CreateAutoResetTimer())
  , Owner(0)
  , Stopping(false)
{
  FreeEvent = CreateSynchronizationEvent();
  StopEvent = CreateNotificationEvent();
}

Pool::~Pool()
{
  CloseHandle(StopEvent);
  CloseHandle(FreeEvent);
}

size_t Pool::GetMaxThread() const
{
  return MaxThreads;
}

void Pool::SetMaxThreads(size_t n)
{
  assert(n > 0);
  MaxThreads = n;
}

size_t Pool::GetMaxUnusedThreads() const
{
  return MaxUnusedThreads;
}

void Pool::SetMaxUnusedThreads(size_t n)
{
  MaxUnusedThreads = n;
}

long Pool::GetMaxIdleTime() const
{
  return MaxIdleTime;
}

void Pool::SetMaxIdleTime(long t)
{
  MaxIdleTime = t;
}

OVERFLOW_MODE Pool::GetOverflowMode() const
{
  return Mode;
}

void Pool::SetOverflowMode(OVERFLOW_MODE mode)
{
  Mode = mode;
}

void Pool::SetStopping()
{
  LOCK_GUARD();
  Stopping = true;
  SetEvent(StopEvent);
}

void Pool::Stop()
{
  SetStopping();

  for (auto& e : All)
  {
    e->Stop();
    ThreadsStopped++;
  }

  LOCK_GUARD();
  assert(All.size() == Unused.size());

  Unused.clear();
  ThreadsUnused = 0;

  All.clear();
  ThreadsTotal = 0;
}

void Pool::StopUnused()
{
  LOCK_GUARD();

  for (auto& e : Unused)
    e->SetExpireTimer(0);

  Locked_StopExpired(nullptr);
}

WorkerPtr Pool::PopUnused(size_t& allCount)
{
  LOCK_GUARD();

  allCount = All.size();

  if (Unused.empty())
    return WorkerPtr();

  WorkerPtr t = Unused.front();
  Unused.pop_front();
  
  ThreadsUnused = Unused.size();

  t->CancelExpireTimer();
  Locked_StopExpired(nullptr);
  
  return t;
}

void Pool::Push(WorkerList& list, WorkerPtr t)
{
  LOCK_GUARD();
  list.push_back(t);

  ThreadsUnused = Unused.size();
  ThreadsTotal = All.size();
}

HEvent Pool::Run(TCallback cb, uint64_t* pid)
{
  TimePoint t0;

  if (pid)
    *pid = 0;

  WorkerPtr t;
  EventArray ev(StopEvent, FreeEvent);

  while (!Stopping)
  {
    size_t allSize{};
    t = PopUnused(allSize);

    if (t == nullptr)
    {
      if (allSize >= MaxThreads)
      {
        if (Mode == OVERFLOW_MODE::FAIL)
        {
          Errors++;

          LockedInRun += t0.ElapsedSince();
          return nullptr;
        }

        auto rc = WaitForMultipleObjects(ev, false);
        if (rc == WAIT_RESULT::OBJECT_0)
        {
          LockedInRun += t0.ElapsedSince();
          return nullptr;
        }

        continue;
      }

      TOnIdle notifyIdle = std::bind(&Pool::CB_OnFree, this, std::placeholders::_1);
      TOnTimer onTimer = std::bind(&Pool::CB_OnTimer, this, std::placeholders::_1);
      t = std::make_shared<Worker>(Timer, notifyIdle, onTimer);

      if (!t->Start())
      {
        Errors++;

        LockedInRun += t0.ElapsedSince();
        return nullptr;
      }

      Push(All, t);
    }

    break;
  }

  uint64_t id{};
  HEvent h = t->Invoke(cb, id);
  if (h)
  {
    if (pid != nullptr)
      *pid = id;

    LockedInRun += t0.ElapsedSince();
    return h;
  }

  Push(Unused, t);
  Errors++;

  LockedInRun += t0.ElapsedSince();
  return nullptr;
}

void Pool::Locked_Find(Worker* p, bool& all, bool& unused)
{
  WorkerPtr t = p->Get();

  all = false;
  for (auto& e : All)
  {
    if (e.get() != p)
      continue;
    
    all = true;
    break;
  }

  unused = false;
  for (auto& e : Unused)
  {
    if (e.get() != p)
      continue;

    unused = true;
    break;
  }
}

void Pool::CB_OnTimer(Worker* p)
{
  OnTimerCalls++;

  if (Lock.try_lock())
  {
    Owner = GetCurrentThreadId();

    Locked_StopExpired(p);
    Lock.unlock();
  }
}

void Pool::CB_OnFree(Worker* p)
{
  LOCK_GUARD();

#ifdef _DEBUG  
  bool all{}, unused{};
  Locked_Find(p, all, unused);
  assert(all == true && unused == false);
#endif

  if (Unused.size() + 1 > MaxUnusedThreads)
  {
    p->SetExpireTimer(MaxIdleTime);
    SET_TIMER();
  }

  WorkerPtr t = p->Get();
  Unused.push_back(t);
  SetEvent(FreeEvent);
}

void Pool::Locked_StopExpired(Worker* caller)
{
  CancelWaitableTimer(Timer);

  if (Stopping)
    return;

  bool setTimer = false;
  for (bool cont = true; cont;)
  {
    cont = false;

    for (auto it = Unused.begin(); it != Unused.end(); ++it)
    {
      WorkerPtr e = *it;
      if (!e->IsExpired())
        continue;

      if (caller && e.get() == caller)
      {
        setTimer = true;
        continue;
      }

      auto ita = std::find_if(
        All.begin()
        , All.end()
        , [e](WorkerPtr t) { return e.get() == t.get(); }
      );
      
      assert(ita != All.end());

      e->Stop();
      ThreadsStopped++;

      auto c0 = e.use_count();
      assert(c0 == 3);

      Unused.erase(it);
      ThreadsUnused = Unused.size();

      All.erase(ita);
      ThreadsTotal = All.size();

      auto c1 = e.use_count();
      assert(c1 == 1);

      cont = true;
      break;
    }
  }

  if (setTimer)
    SET_TIMER();
}
