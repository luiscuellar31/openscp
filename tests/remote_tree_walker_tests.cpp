// Unit tests for serial and parallel remote tree walks.
#include "TestHarness.hpp"
#include "logic/remote/RemoteTreeWalker.hpp"
#include "mock/MockSftpClient.hpp"

#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// A tree with three subdirectories and two files per directory, three levels
// deep: 40 directories and 119 entries below the root.
struct TreeState {
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, int> listCounts;
    std::set<std::thread::id> listThreads;
    int activeLists = 0;
    int maxActiveLists = 0;
    int openedHelpers = 0;
    int connectedHelpers = 0;
    int interruptedHelpers = 0;
    int helperListsToDrop = 0;
    bool blockHelperLists = false;
    int blockedHelperLists = 0;
    std::chrono::milliseconds listDelay{3};
};

class TreeClient final : public openscp::MockSftpClient {
    public:
    TreeClient(std::shared_ptr<TreeState> state, bool helper)
        : state_(std::move(state)), helper_(helper) {}

    bool connect(const openscp::SessionOptions &options,
                 std::string &error) override {
        if (!MockSftpClient::connect(options, error))
            return false;
        if (helper_) {
            std::lock_guard lock(state_->mutex);
            ++state_->openedHelpers;
            ++state_->connectedHelpers;
        }
        return true;
    }

    void disconnect() override {
        if (helper_ && MockSftpClient::isConnected()) {
            std::lock_guard lock(state_->mutex);
            --state_->connectedHelpers;
        }
        MockSftpClient::disconnect();
    }

    void interrupt() override {
        std::lock_guard lock(state_->mutex);
        interrupted_ = true;
        ++state_->interruptedHelpers;
        state_->wake.notify_all();
    }

    bool isConnected() const override { return !dropped_; }

    bool list(const std::string &path, std::vector<openscp::FileInfo> &out,
              std::string &error) override {
        {
            std::unique_lock lock(state_->mutex);
            ++state_->activeLists;
            state_->maxActiveLists =
                std::max(state_->maxActiveLists, state_->activeLists);
            state_->listThreads.insert(std::this_thread::get_id());
            if (helper_ && state_->helperListsToDrop > 0) {
                --state_->helperListsToDrop;
                --state_->activeLists;
                dropped_ = true;
                error = "Connection lost";
                return false;
            }
            if (helper_ && state_->blockHelperLists) {
                ++state_->blockedHelperLists;
                state_->wake.wait_for(lock, std::chrono::seconds(5),
                                      [this] { return interrupted_; });
                --state_->activeLists;
                error = "Interrupted";
                return false;
            }
        }
        std::this_thread::sleep_for(state_->listDelay);

        out.clear();
        const int depth =
            path == "/"
                ? 0
                : static_cast<int>(std::count(path.begin(), path.end(), '/'));
        if (depth < 3) {
            for (int index = 0; index < 3; ++index) {
                out.push_back({"dir" + std::to_string(index), true, 0, false, 0,
                               0040755u, 1, 1});
            }
        }
        out.push_back({"a.txt", false, 1, true, 0, 0100644u, 1, 1});
        out.push_back({"b.txt", false, 2, true, 0, 0100644u, 1, 1});

        std::lock_guard lock(state_->mutex);
        ++state_->listCounts[path];
        --state_->activeLists;
        error.clear();
        return true;
    }

    private:
    std::shared_ptr<TreeState> state_;
    bool helper_ = false;
    bool interrupted_ = false;
    std::atomic_bool dropped_{false};
};

std::unique_ptr<TreeClient> connectedClient(std::shared_ptr<TreeState> state,
                                            bool helper) {
    auto client = std::make_unique<TreeClient>(std::move(state), helper);
    openscp::SessionOptions options;
    options.host = "tree.test";
    options.username = "tester";
    std::string error;
    return client->connect(options, error) ? std::move(client) : nullptr;
}

RemoteTreeWalker::Options parallelOptions(std::shared_ptr<TreeState> state) {
    RemoteTreeWalker::Options options;
    options.maxListingConnections = 3;
    options.parallelDelay = std::chrono::milliseconds(0);
    options.openListingConnection =
        [state](std::string &) -> std::unique_ptr<openscp::RemoteClient> {
        return connectedClient(state, true);
    };
    return options;
}

std::set<QString> collectEntries(RemoteTreeWalker &walker,
                                 const RemoteTreeWalker::Options &options,
                                 RemoteTreeWalker::Result &result,
                                 std::set<std::thread::id> &callbackThreads) {
    std::set<QString> entries;
    RemoteTreeWalker::Callbacks callbacks;
    callbacks.onEntry = [&](const RemoteTreeWalker::Entry &entry) {
        callbackThreads.insert(std::this_thread::get_id());
        entries.insert(entry.relativePath);
        return RemoteTreeWalker::Control::Continue;
    };
    result = walker.walk(QStringLiteral("/"), options, callbacks);
    return entries;
}

OPENSCP_TEST(testParallelWalkReportsTheSameTree, test) {
    const auto serialState = std::make_shared<TreeState>();
    const auto serialClient = connectedClient(serialState, false);
    RemoteTreeWalker serialWalker(*serialClient);
    RemoteTreeWalker::Result serialResult;
    std::set<std::thread::id> serialThreads;
    const std::set<QString> serialEntries = collectEntries(
        serialWalker, RemoteTreeWalker::Options{}, serialResult, serialThreads);

    const auto state = std::make_shared<TreeState>();
    const auto client = connectedClient(state, false);
    RemoteTreeWalker walker(*client);
    RemoteTreeWalker::Result result;
    std::set<std::thread::id> callbackThreads;
    const std::set<QString> entries =
        collectEntries(walker, parallelOptions(state), result, callbackThreads);

    test.check(serialResult.completed && serialEntries.size() == 119,
               "the serial walk should report the whole tree");
    test.check(result.completed && entries == serialEntries,
               "the parallel walk should report the same entries");
    test.check(result.statistics.directoriesListed == 40 &&
                   result.statistics.entriesSeen ==
                       serialResult.statistics.entriesSeen,
               "the parallel walk should keep the walk statistics");
    test.check(
        state->listCounts.size() == 40 &&
            std::all_of(state->listCounts.begin(), state->listCounts.end(),
                        [](const auto &count) { return count.second == 1; }),
        "each directory should be listed exactly once");
    test.check(state->openedHelpers == 3 && state->maxActiveLists > 1 &&
                   state->listThreads.size() > 1,
               "the walk should list on the extra connections concurrently");
    test.check(callbackThreads.size() == 1 &&
                   *callbackThreads.begin() == std::this_thread::get_id(),
               "callbacks should run on the walking thread");
    test.check(state->connectedHelpers == 0 && state->activeLists == 0,
               "extra connections should be closed when the walk returns");
}

OPENSCP_TEST(testOrderedWalksStaySerial, test) {
    const auto state = std::make_shared<TreeState>();
    const auto client = connectedClient(state, false);
    RemoteTreeWalker walker(*client);
    RemoteTreeWalker::Callbacks callbacks;
    std::vector<QString> left;
    callbacks.onLeaveDirectory = [&left](const RemoteTreeWalker::Entry &entry) {
        left.push_back(entry.path);
        return RemoteTreeWalker::Control::Continue;
    };
    const RemoteTreeWalker::Result result =
        walker.walk(QStringLiteral("/"), parallelOptions(state), callbacks);
    test.check(result.completed && left.size() == 40 &&
                   left.back() == QStringLiteral("/"),
               "a post-order walk should still leave the root last");
    test.check(state->openedHelpers == 0 && state->listThreads.size() == 1,
               "walks that depend on visit order must not use extra "
               "connections");
}

OPENSCP_TEST(testParallelWalkAbortJoinsHelpers, test) {
    const auto state = std::make_shared<TreeState>();
    const auto client = connectedClient(state, false);
    RemoteTreeWalker walker(*client);
    RemoteTreeWalker::Callbacks callbacks;
    int seen = 0;
    callbacks.onEntry = [&seen](const RemoteTreeWalker::Entry &) {
        return ++seen >= 20 ? RemoteTreeWalker::Control::Abort
                            : RemoteTreeWalker::Control::Continue;
    };
    const RemoteTreeWalker::Result result =
        walker.walk(QStringLiteral("/"), parallelOptions(state), callbacks);

    std::lock_guard lock(state->mutex);
    test.check(result.aborted && !result.completed && seen == 20,
               "an aborting callback should stop the walk immediately");
    test.check(state->activeLists == 0 && state->connectedHelpers == 0,
               "an aborted walk should not leave listings or connections "
               "behind");
}

OPENSCP_TEST(testParallelWalkCancelInterruptsHelpers, test) {
    const auto state = std::make_shared<TreeState>();
    state->blockHelperLists = true;
    const auto client = connectedClient(state, false);
    RemoteTreeWalker walker(*client);
    RemoteTreeWalker::Callbacks callbacks;
    // Cancel once an extra connection is stuck in a listing.
    callbacks.waitUntilReady = [&state] {
        std::lock_guard lock(state->mutex);
        return state->blockedHelperLists == 0;
    };

    const auto startedAt = Clock::now();
    const RemoteTreeWalker::Result result =
        walker.walk(QStringLiteral("/"), parallelOptions(state), callbacks);
    const auto elapsed = Clock::now() - startedAt;

    test.check(result.canceled, "a canceled walk should report cancellation");
    test.check(elapsed < std::chrono::seconds(2),
               "cancellation should interrupt a blocked extra connection");
    std::lock_guard lock(state->mutex);
    test.check(state->activeLists == 0 && state->connectedHelpers == 0,
               "a canceled walk should close its extra connections");
}

OPENSCP_TEST(testParallelWalkWithoutExtraConnections, test) {
    const auto state = std::make_shared<TreeState>();
    const auto client = connectedClient(state, false);
    RemoteTreeWalker walker(*client);
    RemoteTreeWalker::Options options;
    options.maxListingConnections = 3;
    options.parallelDelay = std::chrono::milliseconds(0);
    std::atomic_int attempts{0};
    options.openListingConnection =
        [&attempts](
            std::string &error) -> std::unique_ptr<openscp::RemoteClient> {
        ++attempts;
        error = "Too many connections";
        return nullptr;
    };
    RemoteTreeWalker::Result result;
    std::set<std::thread::id> callbackThreads;
    const std::set<QString> entries =
        collectEntries(walker, options, result, callbackThreads);
    test.check(result.completed && entries.size() == 119 && attempts == 3,
               "the walk should finish on its own connection when extra "
               "connections cannot be opened");
}

OPENSCP_TEST(testLostExtraConnectionRequeuesDirectory, test) {
    const auto state = std::make_shared<TreeState>();
    state->helperListsToDrop = 2;
    const auto client = connectedClient(state, false);
    RemoteTreeWalker walker(*client);
    RemoteTreeWalker::Options options = parallelOptions(state);
    RemoteTreeWalker::Callbacks callbacks;
    int listErrors = 0;
    std::set<QString> entries;
    callbacks.onListError = [&listErrors](const RemoteTreeWalker::Entry &,
                                          const std::string &) {
        ++listErrors;
        return RemoteTreeWalker::Control::Continue;
    };
    callbacks.onEntry = [&entries](const RemoteTreeWalker::Entry &entry) {
        entries.insert(entry.relativePath);
        return RemoteTreeWalker::Control::Continue;
    };
    const RemoteTreeWalker::Result result =
        walker.walk(QStringLiteral("/"), options, callbacks);
    test.check(result.completed && listErrors == 0 && entries.size() == 119,
               "a directory whose extra connection dropped should be listed "
               "again elsewhere");
}

} // namespace

int main() {
    openscp::test::TestHarness harness("remote tree walker");
    return harness.run();
}
