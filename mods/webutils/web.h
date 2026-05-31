#ifndef WEBUTILS_WEB_H
#define WEBUTILS_WEB_H

#include <coreutils/file.h>
#include <coreutils/log.h>
#include <coreutils/environment.h>
#include <string>
#include <cstdio>
#include <algorithm>
#define NOGDI
#include <curl/curl.h>
#include <unistd.h>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace webutils {

class WebJob;
using StreamFunc = std::function<bool(WebJob&, uint8_t *, size_t)>;
class Web;

class WebJob {
public:
	virtual ~WebJob() = default;
	bool done() const { return isDone; }
	long code() const {
		long rc = -1;
		if(curl)
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &rc);
		return rc;
	}
	
	int64_t contentLength() { return cLength; }
	
	std::string getHeader(const std::string &name) {
		return headers[name];
	}

	utils::File file() {
		return targetFile;
	}

	void wait(int timeout = -1);

	void stop() {
		stopped = true;
	}

	void setTarget(const utils::File &file) { targetFile = file; }

	void setUrl(const std::string &url) { this->url = url; }

	void setStreamCallback(StreamFunc cb) { streamCb = cb; }

	std::string textResult() const {
		return std::string(data.begin(), data.end());
	}

protected:
	void start(CURLM *curlm);
	static size_t writeFunc(void *ptr, size_t size, size_t x, void *userdata);
	static size_t headerFunc(char *text, size_t size, size_t n, void *userdata);
	void finish();
	void destroy();

	virtual void call_handler() {}

	CURL *curl = nullptr;
	std::unordered_map<std::string, std::string> headers;
	std::string url;
	std::vector<uint8_t> data;
	utils::File targetFile;
	utils::File orgFile;
	StreamFunc streamCb;
	bool isDone = false;
	bool stopped = false;
	int64_t cLength = 0;
	std::thread::id tid;

	std::shared_ptr<curl_slist> header_list;
	std::shared_ptr<curl_slist> alias_list;

	friend Web;
};

class Web {
public:

	template <typename... ARGS> struct WebJobImpl;

	template <typename FX, typename A0> struct WebJobImpl<void (FX::*)(A0) const>;

	template <typename FX> struct WebJobImpl<FX, void (FX::*)() const> : public WebJob {
		WebJobImpl(FX fx) : cb(fx) {}
		void call_handler() override { cb(); }
		FX cb;
	};

	template <typename FX> struct WebJobImpl<FX, void (FX::*)(WebJob) const> : public WebJob {
		WebJobImpl(FX fx) : cb(fx) {}
		void call_handler() override { cb(*this); }
		FX cb;
	};

	template <typename FX>
	struct WebJobImpl<FX, void (FX::*)(const std::string &contents) const> : public WebJob {
		WebJobImpl(FX fx) : cb(fx) {}
		void call_handler() override { cb(textResult()); }
		FX cb;
	};

	template <typename FX>
	struct WebJobImpl<FX, void (FX::*)(WebJob &, const std::string &contents) const> : public WebJob {
		WebJobImpl(FX fx) : cb(fx) {}
		void call_handler() override { cb(*this, textResult()); }
		FX cb;
	};

	template <typename FX> struct WebJobImpl<FX, void (FX::*)(utils::File) const> : public WebJob {
		WebJobImpl(FX fx) : cb(fx) {}
		void call_handler() override { cb(targetFile); }
		FX cb;
	};

	Web(const std::string &cacheDir = "", const std::string &baseUrl = "")
	    : baseUrl(baseUrl), cacheDir(cacheDir) {
		std::lock_guard<std::mutex> lock(sm);
		if(!initDone) {
			curl_global_init(CURL_GLOBAL_ALL);
			initDone = true;
		}
		utils::makedirs(cacheDir);
		curlm = curl_multi_init();
		webThread = std::thread{&Web::run, this};
	}

	~Web() {
		// Stop all in-flight jobs first so write callbacks return immediately.
		{
			std::lock_guard<std::mutex> lock(m);
			for (auto& job : jobs) {
				job->stop();
				if (job->curl) {
					curl_multi_remove_handle(curlm, job->curl);
				}
			}
		}

		// Signal run() to exit and wake it immediately.
		quit = true;
		if (curlm) {
			curl_multi_wakeup(curlm);
		}

		// JOIN — not detach. Detaching causes curl/audio races on destruction.
		if (webThread.joinable()) {
			webThread.join();
		}

		// Clean up curl only after the thread is fully stopped.
		if (curlm) {
			{
				std::lock_guard<std::mutex> lock(m);
				for (auto& job : jobs) {
					if (job->curl) {
						job->curl = nullptr;
						std::lock_guard<std::mutex> lock2(sm);
						runningWebJobs -= 1;
					}
				}
				jobs.clear();
			}
			curl_multi_cleanup(curlm);
			curlm = nullptr;
		}
	}

	// Advanced background execution thread: mitigates nested lock recursion 
	// by isolating user callback execution entirely outside of critical sections.
	void run() {
		while(!quit) {
			int handleCount;
			CURLMcode rc = CURLM_CALL_MULTI_PERFORM;
			std::vector<std::shared_ptr<WebJob>> completedJobs;

			{
				std::lock_guard<std::mutex> lock(m);
				while(rc == CURLM_CALL_MULTI_PERFORM)
					rc = curl_multi_perform(curlm, &handleCount);
				lastCount = handleCount;

				// Harvest completions without running complex nested code under lock
				CURLMsg *msg;
				int msgs_left;
				while ((msg = curl_multi_info_read(curlm, &msgs_left))) {
					if (msg->msg == CURLMSG_DONE) {
						auto it = jobs.begin();
						while (it != jobs.end()) {
							if ((*it)->curl == msg->easy_handle) {
								completedJobs.push_back(*it);
								it = jobs.erase(it);
								break;
							} else {
								it++;
							}
						}
					}
				}
			}

			// Execute user and hardware callbacks completely safe from cross-thread mutex recursion
			for (auto& finishedJob : completedJobs) {
				finishedJob->finish();
			}
			completedJobs.clear();

			if (lastCount > 0) {
				utils::sleepms(2);
			} else {
				utils::sleepms(20);
			}

			std::this_thread::yield();
		}
	}

	static int inProgress() {
		std::lock_guard<std::mutex> lock(sm);
		return runningWebJobs;
	}

	template <typename FX> std::shared_ptr<WebJob> get(const std::string &url, FX cb) {
		auto job = std::make_shared<WebJobImpl<FX, decltype(&FX::operator())>>(cb);
		job->setUrl(url);
		job->start(curlm);
		jobs.push_back(job);
		return job;
	}

	void poll() {
		if(!m.try_lock())
			return;
		auto it = jobs.begin();
		while(it != jobs.end()) {
			auto *curl = it->get()->curl;
			if(it->get()->tid != std::this_thread::get_id()) {
				it++;
				continue;
			}
			if(curl && it->get()->stopped) {
				LOGD("Removing stopped job");
				curl_multi_remove_handle(curlm, curl);
				it->get()->destroy();
			}
			if(!curl) {
				it = jobs.erase(it);
			} else
				it++;
		}
		m.unlock();
	}

	void removeJob(std::shared_ptr<WebJob> job) {
		std::lock_guard<std::mutex> lock(m);
		auto it = std::find(jobs.begin(), jobs.end(), job);
		if(it != jobs.end()) {
			jobs.erase(it);
			curl_multi_remove_handle(curlm, job->curl);
			job->destroy();
		}
	}

	template <typename FX> std::shared_ptr<WebJob> getFile(const std::string &url, FX cb) {
		auto job = std::make_shared<WebJobImpl<FX, decltype(&FX::operator())>>(cb);
		auto slash = url.find_last_of('/');
		auto fileName = url.substr(slash+1);
		auto pathName = url.substr(0, slash);
		auto urlPart = utils::urlencode(baseUrl + pathName, ":/\\?;!");
		auto fileNameEncoded = utils::urlencode(fileName, ":/\\?;!");
		utils::makedirs(cacheDir / urlPart);
		auto target = cacheDir / urlPart / fileNameEncoded;
		job->setTarget(target);
		job->setUrl(url);
		LOGD("target: %s", target.getName());
		if(target.exists()) {
			job->call_handler();
			job->targetFile = utils::File();
			job->isDone = true;
			return job;
		}
		std::lock_guard<std::mutex> lock(m);
		job->start(curlm);
		jobs.push_back(job);
		return job;
	}

	utils::File getFileBlocking(const std::string &url) {
		std::atomic<bool> done(false);
		utils::File retFile;
		auto job = getFile(url, [&](utils::File f) {
			retFile = f;
			done = true;
		});
		while(!done) {
			poll();
			utils::sleepms(100);
		}
		return retFile;
	}

	static std::string getBlocking(const std::string &url) {
		std::atomic<bool> done(false);
		std::string result;
		auto job = get_url(url, [&](const std::string &res) {
			LOGD("DONE");
			result = res;
			done = true;
		});
		LOGD("DONE: %s", done ? "yes" : "false");
		while(!done) {
			LOGD("POLL");
			getInstance().poll();
			utils::sleepms(500);
		}
		return result;
	}

	bool inCache(const std::string &url) const {
		auto slash = url.find_last_of('/');
		auto fileName = url.substr(slash+1);
		fileName = utils::urlencode(fileName, ":/\\?;");
		auto pathName = url.substr(0, slash);
		auto urlPart = utils::urlencode(baseUrl + pathName, ":/\\?;");
		auto target = cacheDir / urlPart / fileName;
		return utils::File::exists(target);
	}

	std::shared_ptr<WebJob> streamData(const std::string &url, StreamFunc cb) {
		auto job = std::make_shared<WebJob>();
		job->setStreamCallback(cb);
		job->setUrl(url);
		std::lock_guard<std::mutex> lock(m);
		job->start(curlm);
		jobs.push_back(job);
		return job;
	}

	std::shared_ptr<WebJob> createWebJob(const std::string &url) {
		auto job = std::make_shared<WebJob>();
		job->setUrl(url);
		std::lock_guard<std::mutex> lock(m);
		job->start(curlm);
		jobs.push_back(job);
		return job;
	}

	static Web &getInstance() {
		static Web w((Environment::getCacheDir() / "_webfiles").string());
		return w;
	}

	template <typename FX> static std::shared_ptr<WebJob> get_url(const std::string &url, FX cb) {
		return getInstance().get(url, cb);
	}

	static void pollAll() {
		Web &w = getInstance();
		w.poll();
	}

private:
	static std::mutex sm;
	std::mutex m;
	std::thread webThread;
	std::atomic<bool> quit{false};
	std::string baseUrl;
	utils::File cacheDir;

	static bool initDone;

	CURLM *curlm = nullptr;

	std::vector<std::shared_ptr<WebJob>> jobs;
	int lastCount;
	static std::atomic<int> runningWebJobs;

	friend WebJob;
};

} // namespace webutils

#endif // WEBUTILS_WEB_H

