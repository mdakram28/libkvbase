#ifndef RPC_HPP
#define RPC_HPP

#include <functional>
#include "common.hpp"

typedef std::function<void(const char* buffer, uint64_t size)> RpcCallback;
class RpcChannel {
 public:
  virtual void sendMessage(ServerId serverId, const char* buffer,
                           uint64_t size) = 0;
  virtual void broadcast(const char* buffer, uint64_t size) = 0;
  void onMessage(RpcCallback callback) { this->m_callback = callback; }

 protected:
  RpcCallback m_callback;
};

enum RpcMessageType { HEARTBEAT_REQUEST = 0, VOTE_REQUEST, VOTE_GRANT, FOLLOWER_OFFSET };

struct RpcMessage {
  ServerId serverId;
  RpcMessageType type;
  ServerTerm term;
  uint64_t size;
};

struct HeartbeatMessage : RpcMessage {
  static const RpcMessageType TYPE = HEARTBEAT_REQUEST;
};

struct RequestVotesMessage : RpcMessage {
  static const RpcMessageType TYPE = VOTE_REQUEST;
};

struct GrantVoteMessage : RpcMessage {
  static const RpcMessageType TYPE = VOTE_GRANT;
};

// struct FollowerOffsetMessage : RpcMessage {
//   static const RpcMessageType TYPE = FOLLOWER_OFFSET;
//   uint64_t const 
// }

#endif /* RPC_HPP */
