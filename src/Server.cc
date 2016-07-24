/*
 Mining Pool Agent

 Copyright (C) 2016  BTC.COM

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "Server.h"

static
bool tryReadLine(string &line, struct bufferevent *bufev) {
  line.clear();
  struct evbuffer *inBuf = bufferevent_get_input(bufev);

  // find eol
  struct evbuffer_ptr loc;
  loc = evbuffer_search_eol(inBuf, nullptr, nullptr, EVBUFFER_EOL_LF);
  if (loc.pos == -1) {
    return false;  // not found
  }

  // copies and removes the first datlen bytes from the front of buf
  // into the memory at data
  line.resize(loc.pos + 1);  // containing "\n"
  evbuffer_remove(inBuf, (void *)line.data(), line.size());

  return true;
}

static
string getWorkerName(const string &fullName) {
  size_t pos = fullName.find(".");
  if (pos == fullName.npos) {
    return fullName;
  }
  return fullName.substr(pos + 1);  // not include '.'
}


//////////////////////////////// SessionIDManager //////////////////////////////
SessionIDManager::SessionIDManager(): count_(0) {
  sessionIds_.reset();
}

bool SessionIDManager::ifFull() {
  if (count_ >= MAX_SESSION_ID + 1) {
    return false;
  }
  return true;
}

uint16_t SessionIDManager::allocSessionId() {
  // find an empty bit, always find the smallest
  uint16_t idx = 0;
  while (sessionIds_.test(idx) == true) {
    idx++;
    if (idx > MAX_SESSION_ID) {
      idx = 0;
    }
  }

  // set to true
  sessionIds_.set(idx, true);
  count_++;

  return idx;
}

void SessionIDManager::freeSessionId(const uint16_t sessionId) {
  sessionIds_.set(sessionId, false);
  count_--;
}



////////////////////////////////// StratumSession //////////////////////////////
UpStratumClient::UpStratumClient(struct event_base *base,
                                 const string &userName, StratumServer *server)
: state_(INIT), server_(server)
{
  bev_ = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_THREADSAFE);
  assert(bev_ != nullptr);

  bufferevent_setcb(bev_,
                    StratumServer::upReadCallback, nullptr,
                    StratumServer::upEventCallback, this);
  bufferevent_enable(bev_, EV_READ|EV_WRITE);

  extraNonce1_ = 0u;
  extraNonce2_ = 0u;
}

UpStratumClient::~UpStratumClient() {
  bufferevent_free(bev_);
}

bool UpStratumClient::connect(struct sockaddr_in &sin) {
  // bufferevent_socket_connect(): This function returns 0 if the connect
  // was successfully launched, and -1 if an error occurred.
  int res = bufferevent_socket_connect(bev_, (struct sockaddr *)&sin, sizeof(sin));
  if (res == 0) {
    state_ = CONNECTED;
    return true;
  }
  return false;
}

void UpStratumClient::recvData() {
  string line;
  while (tryReadLine(line, bev_)) {
    handleLine(line);
  }
}

void UpStratumClient::sendData(const char *data, size_t len) {
  // add data to a bufferevent’s output buffer
  bufferevent_write(bev_, data, len);
  DLOG(INFO) << "UpStratumClient send(" << len << "): " << data;
}

void UpStratumClient::handleLine(const string &line) {
  DLOG(INFO) << "UpStratumClient recv(" << line.size() << "): " << line;

  JsonNode jnode;
  if (!JsonNode::parse(line.data(), line.data() + line.size(), jnode)) {
    LOG(ERROR) << "decode line fail, not a json string";
    return;
  }
  JsonNode jresult  = jnode["result"];
  JsonNode jerror   = jnode["error"];
  JsonNode jmethod  = jnode["method"];

  if (jmethod.type() == Utilities::JS::type::Str) {
    JsonNode jparams  = jnode["params"];
    std::vector<JsonNode> jparamsArr = jparams.array();

    if (jmethod.str() == "mining.notify") {
      // TODO
    }
    else if (jmethod.str() == "mining.set_difficulty") {
      // TODO
    }
    else {
      LOG(ERROR) << "unknown method: " << line;
    }
    return;
  }

  if (state_ == AUTHENTICATED) {
    //
    // {"error": null, "id": 2, "result": true}
    //
    if (jerror.type()  != Utilities::JS::type::Null ||
        jresult.type() != Utilities::JS::type::Bool ||
        jresult.boolean() != true) {
      // TODO
    }
    return;
  }

  if (state_ == CONNECTED) {
    //
    // {"id":1,"result":[[["mining.set_difficulty","01000002"],
    //                    ["mining.notify","01000002"]],"01000002",8],"error":null}
    //
    if (jerror.type() != Utilities::JS::type::Null) {
      LOG(ERROR) << "json result is null, err: " << jerror.str();
      return;
    }
    std::vector<JsonNode> resArr = jresult.array();
    if (resArr.size() < 3) {
      LOG(ERROR) << "result element's number is less than 3: " << line;
      return;
    }
    extraNonce1_ = resArr[1].uint32_hex();
    DLOG(INFO) << "extraNonce1 / SessionID: " << extraNonce1_;

    // check extra nonce2's size, MUST be 8 bytes
    if (resArr[2].uint32() != 8) {
      LOG(FATAL) << "extra nonce2's size is NOT 8 bytes";
      return;
    }
    // subscribe successful
    state_ = SUBSCRIBED;

    // do mining.authorize
    string s = Strings::Format("{\"id\": 1, \"method\": \"mining.authorize\","
                               "\"params\": [\"\%s\", \"\"]}\n",
                               userName_.c_str());
    sendData(s);
    return;
  }

  if (state_ == SUBSCRIBED && jresult.boolean() == true) {
    // authorize successful
    state_ = AUTHENTICATED;
    LOG(INFO) << "auth success, name: \"" << userName_
    << "\", extraNonce1: " << extraNonce1_;
    return;
  }
}


////////////////////////////////// StratumSession //////////////////////////////
StratumSession::StratumSession(const uint8_t upSessionIdx,
                               struct bufferevent *bev,
                               StratumServer *server)
: bev_(bev), state_(CONNECTED), minerAgent_(nullptr),
upSessionIdx_(upSessionIdx), server_(server)
{
  sessionId_ = server_->sessionIDManager_.allocSessionId();
}

StratumSession::~StratumSession() {
  server_->sessionIDManager_.freeSessionId(sessionId_);

  // we don't need to close(fd_) because we set 'BEV_OPT_CLOSE_ON_FREE'
  bufferevent_free(bev_);
}

void StratumSession::setReadTimeout(const int32_t timeout) {
  // clear it
  bufferevent_set_timeouts(bev_, NULL, NULL);

  // set a new one
  struct timeval readtv  = {timeout, 0};
  struct timeval writetv = {120, 0};
  bufferevent_set_timeouts(bev_, &readtv, &writetv);
}

void StratumSession::sendData(const char *data, size_t len) {
  // add data to a bufferevent’s output buffer
  bufferevent_write(bev_, data, len);
  DLOG(INFO) << "send(" << len << "): " << data;
}

void StratumSession::recvData() {
  string line;
  while (tryReadLine(line, bev_)) {
    handleLine(line);
  }
}

void StratumSession::handleLine(const string &line) {
  DLOG(INFO) << "recv(" << line.size() << "): " << line;

  JsonNode jnode;
  if (!JsonNode::parse(line.data(), line.data() + line.size(), jnode)) {
    LOG(ERROR) << "decode line fail, not a json string";
    return;
  }
  JsonNode jid = jnode["id"];
  JsonNode jmethod = jnode["method"];
  JsonNode jparams = jnode["params"];

  string idStr = "null";
  if (jid.type() == Utilities::JS::type::Int) {
    idStr = jid.str();
  } else if (jid.type() == Utilities::JS::type::Str) {
    idStr = "\"" + jnode["id"].str() + "\"";
  }

  if (jmethod.type() == Utilities::JS::type::Str &&
      jmethod.size() != 0 &&
      jparams.type() == Utilities::JS::type::Array) {
    handleRequest(idStr, jmethod.str(), jparams);
    return;
  }

  // invalid params
  responseError(idStr, StratumError::ILLEGAL_PARARMS);
}

void StratumSession::responseError(const string &idStr, int errCode) {
  //
  // {"id": 10, "result": null, "error":[21, "Job not found", null]}
  //
  char buf[128];
  int len = snprintf(buf, sizeof(buf),
                     "{\"id\":%s,\"result\":null,\"error\":[%d,\"%s\",null]}\n",
                     idStr.empty() ? "null" : idStr.c_str(),
                     errCode, StratumError::toString(errCode));
  sendData(buf, len);
}

void StratumSession::responseTrue(const string &idStr) {
  const string s = "{\"id\":" + idStr + ",\"result\":true,\"error\":null}\n";
  sendData(s);
}

void StratumSession::handleRequest(const string &idStr, const string &method,
                                   const JsonNode &jparams) {
  if (method == "mining.submit") {  // most of requests are 'mining.submit'
    handleRequest_Submit(idStr, jparams);
  }
  else if (method == "mining.subscribe") {
    handleRequest_Subscribe(idStr, jparams);
  }
  else if (method == "mining.authorize") {
    handleRequest_Authorize(idStr, jparams);
  } else {
    // unrecognised method, just ignore it
    LOG(WARNING) << "unrecognised method: \"" << method << "\"";
  }
}

void StratumSession::handleRequest_Subscribe(const string &idStr,
                                             const JsonNode &jparams) {
  if (state_ != CONNECTED) {
    responseError(idStr, StratumError::UNKNOWN);
    return;
  }
  state_ = SUBSCRIBED;

  //
  //  params[0] = client version     [optional]
  //  params[1] = session id of pool [optional]
  //
  // client request eg.:
  //  {"id": 1, "method": "mining.subscribe", "params": ["bfgminer/4.4.0-32-gac4e9b3", "01ad557d"]}
  //

  if (jparams.children()->size() >= 1) {
    // 30 is max length for miner agent
    minerAgent_ = strdup(jparams.children()->at(0).str().substr(0, 30).c_str());
  }

  //  result[0] = 2-tuple with name of subscribed notification and subscription ID.
  //              Theoretically it may be used for unsubscribing, but obviously miners won't use it.
  //  result[1] = ExtraNonce1, used for building the coinbase.
  //  result[2] = Extranonce2_size, the number of bytes that the miner users for its ExtraNonce2 counter
  assert(kExtraNonce2Size_ == 4);
  const uint32_t extraNonce1 = (uint32_t)sessionId_;
  const string s = Strings::Format("{\"id\":%s,\"result\":[[[\"mining.set_difficulty\",\"%08x\"]"
                                   ",[\"mining.notify\",\"%08x\"]],\"%08x\",%d],\"error\":null}\n",
                                   idStr.c_str(), extraNonce1, extraNonce1,
                                   extraNonce1, kExtraNonce2Size_);
  sendData(s);
}

void StratumSession::handleRequest_Authorize(const string &idStr,
                                             const JsonNode &jparams) {
  if (state_ != SUBSCRIBED) {
    responseError(idStr, StratumError::NOT_SUBSCRIBED);
    return;
  }

  //
  //  params[0] = user[.worker]
  //  params[1] = password
  //  eg. {"params": ["slush.miner1", "password"], "id": 2, "method": "mining.authorize"}
  //
  if (jparams.children()->size() < 1) {
    responseError(idStr, StratumError::INVALID_USERNAME);
    return;
  }

  // auth success
  responseTrue(idStr);
  state_ = AUTHENTICATED;

  const string workerName = getWorkerName(jparams.children()->at(0).str());

  // TODO: sent sessionId(extraNonce1), minerAgent_, workerName to server_

  free(minerAgent_);
  minerAgent_ = nullptr;

  // TODO: send latest stratum job
//  sendMiningNotify(server_->jobRepository_->getLatestStratumJobEx());
}

void StratumSession::handleRequest_Submit(const string &idStr,
                                          const JsonNode &jparams) {
  if (state_ != AUTHENTICATED) {
    responseError(idStr, StratumError::UNAUTHORIZED);
    // there must be something wrong, send reconnect command
    const string s = "{\"id\":null,\"method\":\"client.reconnect\",\"params\":[]}\n";
    sendData(s);
    return;
  }

  //  params[0] = Worker Name
  //  params[1] = Job ID
  //  params[2] = ExtraNonce 2
  //  params[3] = nTime
  //  params[4] = nonce
  if (jparams.children()->size() < 5) {
    responseError(idStr, StratumError::ILLEGAL_PARARMS);
    return;
  }
  const uint8_t jobId     = jparams.children()->at(1).uint8();
  const uint32_t exNonce2 = jparams.children()->at(2).uint32_hex();
  const uint32_t nTime    = jparams.children()->at(3).uint32_hex();
  const uint32_t nonce    = jparams.children()->at(4).uint32_hex();

  // TODO: submit to server_

  responseTrue(idStr);  // we assume shares are valid
}
