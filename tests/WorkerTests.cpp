#include "app/Worker.h"
#include <atomic>
#include <future>
#include <iostream>
using namespace std::chrono_literals;
void RunWorkerTests(){
 sat::Worker worker;std::promise<void> started,finished;auto begin=started.get_future();auto end=finished.get_future();std::atomic<int> running{},maximum{},queuedRuns{};
 worker.Submit([&](std::stop_token stop){running++;maximum.store(running.load());started.set_value();while(!stop.stop_requested())std::this_thread::sleep_for(1ms);std::this_thread::sleep_for(30ms);running--;});
 if(begin.wait_for(2s)!=std::future_status::ready)throw std::runtime_error("worker did not start");
 worker.Submit([&](std::stop_token){queuedRuns++;});
 worker.Submit([&](std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("latest job cancelled");int active=++running;maximum.store(std::max(maximum.load(),active));running--;finished.set_value();});
 if(end.wait_for(2s)!=std::future_status::ready)throw std::runtime_error("latest job did not complete");
 if(maximum!=1||queuedRuns!=0)throw std::runtime_error("worker concurrent or superseded work ran");
 worker.Stop();std::cout<<"WorkerTests: PASS (serial, latest-only, cancellation, shutdown)\n";
}
