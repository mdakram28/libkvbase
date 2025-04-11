#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "common.hpp"
#include "rpc.hpp"

enum LogStoreState { LEADER, FOLLOWER, CANDIDATE };

static const char* StateToString(LogStoreState state) {
  switch (state) {
    case LEADER:
      return "LEADER";
      break;
    case FOLLOWER:
      return "FOLLOWER";
      break;
    case CANDIDATE:
      return "CANDIDATE";
      break;
  }
}

struct PeerInfo {
  PeerInfo(ServerId sid) : serverId(sid), votedFor{INVALID_SERVER} {}

  void reset() { votedFor = INVALID_SERVER; }

  ServerId serverId;
  ServerId votedFor{INVALID_SERVER};
};

class TermInfo {
 public:
  TermInfo(ServerId serverId, const std::vector<PeerInfo>& peersInfo)
      : m_serverId(serverId),
        m_term(0),
        m_votedFor(INVALID_SERVER),
        m_state(FOLLOWER),
        m_voteCount(0),
        m_leaderId(INVALID_SERVER) {
    for (const auto& peer : peersInfo) {
      if (peer.serverId == m_serverId)
        continue;
      if (m_serverIdToPeerIndex.size() < peer.serverId + 1) {
        m_serverIdToPeerIndex.resize(peer.serverId + 1, INVALID_SERVER);
      }
      m_serverIdToPeerIndex[peer.serverId] = m_peersInfo.size();
      m_peersInfo.push_back(peer);
    }
    resetElectionTimeout();
  }

  void upgrade(uint64_t newTerm, LogStoreState newState) {
    m_term = newTerm;
    m_state = newState;
    m_votedFor = INVALID_SERVER;
    m_voteCount = 0;
    m_leaderId = INVALID_SERVER;
    resetElectionTimeout();

    /* Clear peer info */
    for (auto& peerInfo : m_peersInfo) {
      peerInfo.reset();
    }
  }

  uint64_t term() const { return m_term; }

  uint64_t votedFor() const { return m_votedFor; }

  LogStoreState state() const { return m_state; }

  ServerId leaderId() const { return m_leaderId; }

  bool isTimedOut(uint64_t currentTime) {
    return currentTime > m_electionTimeout;
  }

  void setVotedFor(ServerId grantTo) { m_votedFor = grantTo; }

  void setLeaderId(ServerId leaderId) { m_leaderId = leaderId; }

  void voteGranted(ServerId grantedBy) {
    PeerInfo& peer = m_peersInfo[m_serverIdToPeerIndex[grantedBy]];
    if (peer.votedFor == INVALID_SERVER) {
      peer.votedFor = m_serverId;
      m_voteCount++;

      if (m_voteCount + (m_votedFor == INVALID_SERVER ? 1 : 0) >=
          (m_peersInfo.size() + 1) / 2 + 1) {
        m_state = LEADER;
      }
    }
  }

  void resetElectionTimeout() {
    m_electionTimeout =
        currentTimeMs() + CONFIG_HEARTBEAT_TIMEOUT_MS +
        (CONFIG_HEARTBEAT_TIMEOUT_MS * (uint64_t)rand()) / RAND_MAX;
  }

 private:
  /* Fixed members per term */
  ServerId m_serverId;
  std::vector<uint64_t> m_serverIdToPeerIndex;

  /* New members per term */
  uint64_t m_term;
  ServerId m_votedFor;
  LogStoreState m_state;
  std::vector<PeerInfo> m_peersInfo;
  uint64_t m_voteCount;
  uint64_t m_electionTimeout;
  ServerId m_leaderId;
};

/**
 * @brief Callback called when committing an offset.
 * 
 * @param buf The data to be committed
 * @param size Size of buf
 * 
 * @return Should return the bytes committed
 */
typedef std::function<void(char* buf, uint64_t size)> CommitCallbackType;

class LogStore {
 public:
  LogStore(ServerId serverId, CommitCallbackType commitCallback,
           RpcChannel* rpcChannel, std::vector<PeerInfo>& peerInfo)
      : m_serverId(serverId),
        m_commitCallback(commitCallback),
        m_running(true),
        rpcChannel(rpcChannel),
        m_termInfo(serverId, peerInfo),
        sendHeartbeatThrottled(&LogStore::sendHeartbeat,
                               CONFIG_HEARTBEAT_SEND_DELAY_MS, this),
        requestVotesThrottled(&LogStore::requestVotes,
                              CONFIG_VOTE_REQUEST_TIMEOUT, this),
        sendStateToLeaderThrottled(&LogStore::sendStateToLeader,
                                   CONFIG_SYNC_STATE_TIMEOUT, this) {
    log = spdlog::default_logger()->clone("server_" + std::to_string(serverId));
    m_thread = std::thread(&LogStore::threadMain, this);
    rpcChannel->onMessage(std::bind(&LogStore::onRpcMessage, this,
                                    std::placeholders::_1,
                                    std::placeholders::_2));
  }

  ~LogStore() {
    /* Gracefully shutdown main threads */
    m_running = false;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  /**
   * @brief Append to log, wait until committed
   * 
   * @param buf Buffer to append
   * @param size Size of buffer
   */
  void append(const char* buf, uint64_t size) {
    m_buffer.insert(m_buffer.end(), buf, buf + size);
  }

  uint64_t getCommittedOffset() { return m_committedOffset; }

  std::string getTermInfo() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return std::format
    {, StateToString(m_termInfo.state())};
  }

 private:
  ServerId m_serverId;
  std::shared_ptr<spdlog::logger> log;
  std::vector<char> m_buffer;
  CommitCallbackType m_commitCallback;
  uint64_t m_committedOffset{0};
  std::thread m_thread;
  bool m_running{true};
  RpcChannel* rpcChannel{nullptr};

  /* Throttled functions */
  ThrottledFunction<LogStore> sendHeartbeatThrottled;
  ThrottledFunction<LogStore> requestVotesThrottled;
  ThrottledFunction<LogStore> sendStateToLeaderThrottled;

  /* Raft parameters */
  TermInfo m_termInfo;
  std::mutex m_mutex;

  template <class RpcMessageClass>
  RpcMessageClass* makeRpcMessage(uint64_t size = 0) {
    if (size == 0) {
      size = sizeof(RpcMessageClass);
    }

    RpcMessageClass* msg = STATIC_PTR_CAST(malloc(size), RpcMessageClass*);
    msg->term = m_termInfo.term();
    msg->serverId = m_serverId;
    msg->type = RpcMessageClass::TYPE;
    msg->size = size;
    return msg;
  }

  void sendHeartbeat(uint64_t currentTime) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_termInfo.state() != LEADER) {
      return;
    }

    HeartbeatMessage* msg = makeRpcMessage<HeartbeatMessage>();
    rpcChannel->broadcast(STATIC_CONST_PTR_CAST(msg, const char*), msg->size);
    free(msg);
  }

  void requestVotes(uint64_t currentTime) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_termInfo.state() != CANDIDATE) {
      return;
    }

    RequestVotesMessage* msg = makeRpcMessage<RequestVotesMessage>();
    rpcChannel->broadcast(STATIC_CONST_PTR_CAST(msg, const char*), msg->size);
    free(msg);
  }

  void tryMakeCandidate(uint64_t currentTime) {
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      if (m_termInfo.state() != FOLLOWER ||
          !m_termInfo.isTimedOut(currentTime)) {
        return;
      }

      log->info("Timed out, making candidate");

      m_termInfo.upgrade(m_termInfo.term() + 1, CANDIDATE);
    }
    /* Immediately request votes */
    requestVotesThrottled(currentTime);
  }

  void sendStateToLeader(uint64_t currentTime) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_termInfo.leaderId() == INVALID_SERVER) {
      return;
    }

    // FollowerStateMessage *msg = makeRpcMessage<FollowerStateMessage>();
    // rpcChannel->sendMessage(m_termInfo.leaderId(), STATIC_CONST_PTR_CAST(msg, const char *), msg->size);
    // free(msg);
  }

  void threadMain() {
    while (m_running) {
      uint64_t sleep_time = (((long)rand()) * 1000) / RAND_MAX;
      std::this_thread::sleep_for(std::chrono::milliseconds((sleep_time)));
      uint64_t currentTime = currentTimeMs();

      switch (m_termInfo.state()) {
        case LEADER:
          sendHeartbeatThrottled(currentTime);
          break;
        case FOLLOWER:
          if (m_termInfo.isTimedOut(currentTime)) {
            tryMakeCandidate(currentTime);
          } else {
            sendStateToLeaderThrottled(currentTime);
          }
          break;
        case CANDIDATE:
          requestVotesThrottled(currentTime);
          break;
      }
    }
  }

  void handleHeartbeatRequest(const RpcMessage* rpcMessage) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (rpcMessage->term < m_termInfo.term()) {
      /* Ignore request */
      return;
    } else if (rpcMessage->term > m_termInfo.term()) {
      m_termInfo.upgrade(rpcMessage->term, FOLLOWER);
    }

    m_termInfo.setLeaderId(rpcMessage->serverId);
    m_termInfo.resetElectionTimeout();
  }

  void handleVoteRequest(const RpcMessage* rpcMessage) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (rpcMessage->term < m_termInfo.term()) {
      /* Ignore request */
      return;
    } else if (rpcMessage->term > m_termInfo.term()) {
      m_termInfo.upgrade(rpcMessage->term, FOLLOWER);
    }

    log->info("Requested vote by : {}", rpcMessage->serverId);

    /* client and server have the same term */

    if (m_termInfo.votedFor() == INVALID_SERVER) {
      /* Grant vote */
      m_termInfo.setVotedFor(rpcMessage->serverId);
    }

    if (m_termInfo.votedFor() == rpcMessage->serverId) {
      GrantVoteMessage* msg = makeRpcMessage<GrantVoteMessage>();
      rpcChannel->sendMessage(rpcMessage->serverId, STATIC_PTR_CAST(msg, char*),
                              msg->size);
      free(msg);
      m_termInfo.resetElectionTimeout();
      log->info("Granted vote to : {}", rpcMessage->serverId);
    }
  }

  void handleVoteGranted(const RpcMessage* rpcMessage) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (rpcMessage->term < m_termInfo.term()) {
      /* Ignore request */
      return;
    } else if (rpcMessage->term > m_termInfo.term()) {
      m_termInfo.upgrade(rpcMessage->term, FOLLOWER);
    }

    m_termInfo.voteGranted(rpcMessage->serverId);
  }

  void onRpcMessage(const char* buffer, uint64_t size) {
    if (size < sizeof(RpcMessage)) {
      log->error("Received message of invalid size : {}", size);
      return;
    }

    const RpcMessage* rpcMessage = reinterpret_cast<const RpcMessage*>(buffer);
    if (rpcMessage->serverId == m_serverId) {
      log->error("Received self request");
      return;
    }

    switch (rpcMessage->type) {
      case RpcMessageType::HEARTBEAT_REQUEST:
        handleHeartbeatRequest(rpcMessage);
        break;

      case RpcMessageType::VOTE_REQUEST:
        handleVoteRequest(rpcMessage);
        break;

      case RpcMessageType::VOTE_GRANT:
        handleVoteGranted(rpcMessage);
        break;

      default:
        break;
    }
  }
};