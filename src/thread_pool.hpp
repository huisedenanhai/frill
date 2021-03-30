#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
  explicit ThreadPool(size_t thread_count) : _data(std::make_shared<Data>()) {
    _threads.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
      _threads.emplace_back([data = _data] {
        std::unique_lock<std::mutex> lk(data->mtx);
        while (true) {
          if (!data->tasks.empty()) {
            auto current = std::move(data->tasks.front());
            data->tasks.pop();
            lk.unlock();
            current->invoke();
            lk.lock();
          } else if (data->isShutdown) {
            break;
          } else {
            data->cond.wait(lk);
          }
        }
      });
    }
  }

  ThreadPool() = default;
  ThreadPool(ThreadPool &&) = default;

  ~ThreadPool() {
    if ((bool)_data) {
      {
        std::lock_guard<std::mutex> lk(_data->mtx);
        _data->isShutdown = true;
      }
      _data->cond.notify_all();
    }
    for (auto &t : _threads) {
      t.join();
    }
  }

  template <typename F> auto schedule(F &&task) {
    using TaskReturnType = std::invoke_result_t<std::remove_reference_t<F>>;
    std::future<TaskReturnType> res;
    {
      std::lock_guard<std::mutex> lk(_data->mtx);
      std::packaged_task<TaskReturnType()> packagedTask(std::forward<F>(task));
      res = packagedTask.get_future();
      _data->tasks.emplace(make_task(std::move(packagedTask)));
    }
    _data->cond.notify_one();
    return res;
  }

private:
  class ITask {
  public:
    virtual void invoke() = 0;
    virtual ~ITask() {}
  };

  template <typename Func> class Task : public ITask {
  public:
    template <typename F> Task(F &&f) : _func(std::forward<F>(f)) {}
    Task(const Task &) = delete;
    Task(Task &&) = default;
    ~Task() = default;

    virtual void invoke() override { _func(); }

  private:
    Func _func;
  };

  template <typename Func>
  static std::unique_ptr<Task<std::remove_reference_t<Func>>>
  make_task(Func &&f) {
    using F = std::remove_reference_t<Func>;
    return std::make_unique<Task<F>>(std::forward<Func>(f));
  }

  struct Data {
    std::mutex mtx;
    std::condition_variable cond;
    bool isShutdown = false;
    std::queue<std::unique_ptr<ITask>> tasks;
  };

  std::shared_ptr<Data> _data;
  std::vector<std::thread> _threads;
};
