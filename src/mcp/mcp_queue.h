/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Cross-thread request queue used to marshal MCP tool invocations from
 *  cpp-mcp's worker thread pool onto the DOSBox main thread, which owns
 *  CPU/memory/keyboard state.
 */

#ifndef DOSBOX_MCP_QUEUE_H
#define DOSBOX_MCP_QUEUE_H

#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <utility>

#include <SDL.h>

#include "mcp_third_party.h"

namespace mcp_bridge {

// A unit of work to run on the DOSBox main thread. Returns the JSON
// result that cpp-mcp will serialize back to the client.
using MainThreadJob = std::function<nlohmann::json()>;

class RequestQueue {
public:
	// Called from any thread (typically a cpp-mcp worker). Posts a job
	// to be run on the main thread, blocks the calling thread on the
	// returned future until the main thread services it, and returns
	// the resulting JSON.
	nlohmann::json submit_and_wait(MainThreadJob job)
	{
		auto promise = std::make_shared<std::promise<nlohmann::json>>();
		auto future  = promise->get_future();
		{
			std::lock_guard<std::mutex> lock(mu_);
			pending_.emplace(std::move(job), promise);
		}
		// Wake the main thread if it's parked in SDL_WaitEvent. DOSBox
		// uses SDL_WaitEventTimeout when emulation has nothing to do
		// (e.g. CPU at INT 16h waiting for a keystroke). Without this,
		// MCP_PumpQueue() at the top of DOSBOX_RunMachine never gets to
		// run and the worker thread blocks indefinitely on the future.
		SDL_Event wake = {};
		wake.type      = SDL_USEREVENT;
		SDL_PushEvent(&wake);
		return future.get();
	}

	// Called from the DOSBox main thread, ideally once per Normal_Loop
	// iteration *and* once per paused-loop iteration. Drains any
	// pending jobs.
	void pump()
	{
		std::queue<Entry> local;
		{
			std::lock_guard<std::mutex> lock(mu_);
			std::swap(local, pending_);
		}
		while (!local.empty()) {
			auto& entry = local.front();
			try {
				entry.promise->set_value(entry.job());
			} catch (...) {
				try {
					entry.promise->set_exception(std::current_exception());
				} catch (...) {}
			}
			local.pop();
		}
	}

private:
	struct Entry {
		MainThreadJob job;
		std::shared_ptr<std::promise<nlohmann::json>> promise;
		Entry(MainThreadJob j,
		      std::shared_ptr<std::promise<nlohmann::json>> p)
		        : job(std::move(j)), promise(std::move(p))
		{}
	};
	std::mutex mu_;
	std::queue<Entry> pending_;
};

} // namespace mcp_bridge

#endif // DOSBOX_MCP_QUEUE_H
