#include "common.hpp"
#include "logstore.cpp"

class LocalRpcChannel : public RpcChannel {
 public:
  ServerId m_serverId;
  std::queue<std::pair<ServerId, std::vector<char>>> m_queue;
  bool m_running;
  std::thread m_thread;
  std::mutex m_mutex;

  LocalRpcChannel() : m_serverId(0), m_running(true) {
  }
  LocalRpcChannel(ServerId serverId) : m_serverId(serverId), m_running(true) {
    m_thread = std::thread(&LocalRpcChannel::threadMain, this);
  }

  ~LocalRpcChannel() {
    m_running = false;
    m_thread.join();
  }

  void threadMain() {
    ServerId serverId;
    std::vector<char> request;
    while (m_running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.size() == 0) {
          continue;
        }

        serverId = m_queue.front().first;
        request = m_queue.front().second;
        m_queue.pop();
      }
      m_peers.at(serverId)->m_callback(request.data(), request.size());
    }
  }

  void sendMessage(ServerId serverId, const char* buffer, uint64_t size) {
    if (m_peers.find(serverId) == m_peers.end()) {
      spdlog::error("Failed to send request to server {}", serverId);
      return;
    }
    
    std::unique_lock<std::mutex> lock(m_mutex);
    m_queue.emplace(serverId, std::vector<char>(buffer, buffer+size));
  }

  void broadcast(const char *buffer, uint64_t size) {
    for (auto &it : m_peers) {
      sendMessage(it.first, buffer, size);
    }
  }

  void addPeer(ServerId serverId, LocalRpcChannel* channel) {
    m_peers[serverId] = channel;
  }

  void removePeer(ServerId serverId) { m_peers.erase(serverId); }

 private:
  std::unordered_map<ServerId, LocalRpcChannel*> m_peers;
};

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
  LogStore logStore1(1, [](char* buf, uint64_t size) {}, &channels[1], peersInfo);
  LogStore logStore2(2, [](char* buf, uint64_t size) {}, &channels[2], peersInfo);
  LogStore logStore3(3, [](char* buf, uint64_t size) {}, &channels[3], peersInfo);

  /* Append to local store */
  std::string helloWorldLog = "Hello world!";
  logStore1.append(helloWorldLog.data(), helloWorldLog.size());

  /* Wait for commit */
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (logStore2.getCommittedOffset() < helloWorldLog.size() &&
           logStore3.getCommittedOffset() < helloWorldLog.size());

  return 0;
}