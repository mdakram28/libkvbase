#include "common.hpp"
#include "logstore.cpp"
#include "rpclocal.cpp"

int main() {
  spdlog::set_level(spdlog::level::debug);
  spdlog::info("KvStore test");

  std::unordered_map<ServerId, LocalRpcChannel> channels;
  channels.emplace(1, 1);
  channels.emplace(2, 1);
  channels.emplace(3, 1);

  /* Connect all channels */
  for (auto& i : channels) {
    for (auto& j : channels) {
      if (i.first == j.first)
        continue;

      i.second.addPeer(j.first, &j.second);
    }
  }

  std::vector<PeerInfo> peersInfo = {1, 2, 3};

  /* Create log stores */
  LogStore logStore1(
      1, [](char* buf, uint64_t size) {}, &channels[1], peersInfo);
  LogStore logStore2(
      2, [](char* buf, uint64_t size) {}, &channels[2], peersInfo);
  LogStore logStore3(
      3, [](char* buf, uint64_t size) {}, &channels[3], peersInfo);

  /* Append to local store */
  std::string helloWorldLog = "Hello world!";
  logStore1.append(helloWorldLog.data(), helloWorldLog.size());

  /* Wait for commit */
  do {
    spdlog::info("Current State : {} {} {}", logStore1.getTermInfo(), logStore2.getTermInfo(), logStore3.getTermInfo());
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  } while (logStore2.getCommittedOffset() < helloWorldLog.size() &&
           logStore3.getCommittedOffset() < helloWorldLog.size());

  return 0;
}