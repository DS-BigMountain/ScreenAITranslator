#pragma once
#include "common/Types.h"
#include <thread>
#include <mutex>
#include <condition_variable>
namespace sat {
// One worker consumes only the latest queued job. Cancelling never joins on the UI thread.
class Worker {
 std::mutex mutex_;std::condition_variable cv_;
 std::function<void(std::stop_token)> pending_;
 std::stop_source source_;bool closing_{};
 std::jthread thread_;
public:
 Worker():thread_([this]{for(;;){std::function<void(std::stop_token)> job;std::stop_token token;{std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return closing_||bool(pending_);});if(closing_)return;job=std::move(pending_);pending_={};token=source_.get_token();}try{job(token);}catch(...){/* Jobs own error reporting; never terminate the message loop. */}}}){}
 ~Worker(){Stop();}
 void Cancel(){std::lock_guard lock(mutex_);source_.request_stop();pending_={};}
 void Submit(std::function<void(std::stop_token)> job){std::lock_guard lock(mutex_);source_.request_stop();source_=std::stop_source{};pending_=std::move(job);cv_.notify_one();}
 void Stop(){{std::lock_guard lock(mutex_);if(closing_)return;closing_=true;source_.request_stop();pending_={};cv_.notify_one();}if(thread_.joinable())thread_.join();}
};
}
