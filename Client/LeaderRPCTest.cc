/* Copyright (c) 2012 Stanford University
 * Copyright (c) 2014-2015 Diego Ongaro
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <gtest/gtest.h>
#include <thread>

#include "Client/LeaderRPC.h"
#include "Core/Debug.h"
#include "Core/ProtoBuf.h"
#include "Protocol/Common.h"
#include "RPC/ClientSession.h"
#include "RPC/Server.h"
#include "RPC/ServiceMock.h"

namespace LogCabin {
namespace Client {
namespace {

using Protocol::Client::OpCode;
typedef LeaderRPCBase::TimePoint TimePoint;

class ClientLeaderRPCTest : public ::testing::Test {
  public:
    ClientLeaderRPCTest()
        : serverEventLoop()
        , service()
        , server()
        , serverThread()
        , leaderRPC()
        , request()
        , response()
        , expResponse()
    {
        service = std::make_shared<RPC::ServiceMock>();
        server.reset(new RPC::Server(serverEventLoop,
                                     Protocol::Common::MAX_MESSAGE_LENGTH));
        RPC::Address address("127.0.0.1", Protocol::Common::DEFAULT_PORT);
        address.refresh(RPC::Address::TimePoint::max());
        EXPECT_EQ("", server->bind(address));
        server->registerService(Protocol::Common::ServiceId::CLIENT_SERVICE,
                                service, 1);
        leaderRPC.reset(new LeaderRPC(address));


        request.mutable_read()->set_path("foo");
        expResponse.set_status(Protocol::Client::Status::OK);
        expResponse.mutable_read()->set_contents("bar");
    }
    ~ClientLeaderRPCTest()
    {
        RPC::ClientSession::connectFn = ::connect;
        serverEventLoop.exit();
        if (serverThread.joinable())
            serverThread.join();
    }

    void init() {
        serverThread = std::thread(&Event::Loop::runForever, &serverEventLoop);
    }

    Event::Loop serverEventLoop;
    std::shared_ptr<RPC::ServiceMock> service;
    std::unique_ptr<RPC::Server> server;
    std::thread serverThread;
    std::unique_ptr<LeaderRPC> leaderRPC;
    Protocol::Client::ReadOnlyTree::Request request;
    Protocol::Client::ReadOnlyTree::Response response;
    Protocol::Client::ReadOnlyTree::Response expResponse;
};

// copied from RPC/ClientSessionTest.cc
struct ConnectInProgress
{
    ConnectInProgress()
        : pipeFds()
    {
        int r = pipe(pipeFds);
        if (r != 0)
            PANIC("failed to create pipe: %s", strerror(errno));
    }
    ~ConnectInProgress() {
        if (pipeFds[0] >= 0)
            close(pipeFds[0]);
        if (pipeFds[1] >= 0)
            close(pipeFds[1]);
    }
    int operator()(int sockfd,
                    const struct sockaddr *addr,
                    socklen_t addrlen) {
        // Unfortunately, the unconnected socket generates epoll events if left
        // alone. Replace it with a pipe. Use the read end of the pipe so that
        // it's never writable
        int r = dup2(pipeFds[0], sockfd);
        EXPECT_LE(0, r);
        errno = EINPROGRESS;
        return -1;
    }
    int pipeFds[2]; // = {read, write}
};

TEST_F(ClientLeaderRPCTest, Call_start_timeout) {
    LeaderRPC::Call call(*leaderRPC);
    ConnectInProgress c;
    RPC::ClientSession::connectFn = std::ref(c);
    call.start(OpCode::READ_ONLY_TREE, request, TimePoint::min());
    EXPECT_EQ("Closed session: Failed to connect socket to 127.0.0.1 "
              "(resolved to 127.0.0.1:61023): timeout expired",
              call.cachedSession->toString());
    EXPECT_EQ("Failed to connect socket to 127.0.0.1 "
              "(resolved to 127.0.0.1:61023): timeout expired",
              call.rpc.getErrorMessage());
    EXPECT_EQ(LeaderRPCBase::Call::Status::TIMEOUT,
              call.wait(response, TimePoint::min()));
    EXPECT_FALSE(leaderRPC->leaderSession.get());
}

TEST_F(ClientLeaderRPCTest, CallOK) {
    init();
    service->reply(OpCode::READ_ONLY_TREE, request, expResponse);
    std::unique_ptr<LeaderRPCBase::Call> call = leaderRPC->makeCall();
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::OK,
              call->wait(response, TimePoint::max()));
    EXPECT_EQ(expResponse, response);
    EXPECT_TRUE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);
}

TEST_F(ClientLeaderRPCTest, CallCanceled) {
    init();
    std::unique_ptr<LeaderRPCBase::Call> call = leaderRPC->makeCall();
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    call->cancel();

    EXPECT_EQ(LeaderRPCBase::Call::Status::RETRY,
              call->wait(response, TimePoint::max()));
    EXPECT_EQ(LeaderRPCBase::Call::Status::RETRY,
              call->wait(response, TimePoint::max()));
    call->cancel();
    EXPECT_EQ(LeaderRPCBase::Call::Status::RETRY,
              call->wait(response, TimePoint::max()));
    EXPECT_TRUE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);
}

TEST_F(ClientLeaderRPCTest, CallRPCFailed) {
    init();
    service->closeSession(OpCode::READ_ONLY_TREE, request);
    service->reply(OpCode::READ_ONLY_TREE, request, expResponse);
    std::unique_ptr<LeaderRPCBase::Call> call = leaderRPC->makeCall();
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::RETRY,
              call->wait(response, TimePoint::max()));
    EXPECT_FALSE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::OK,
              call->wait(response, TimePoint::max()));
    EXPECT_EQ(expResponse, response);
    EXPECT_TRUE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);
}

TEST_F(ClientLeaderRPCTest, Call_wait_notLeader) {
    init();
    Protocol::Client::Error error;
    error.set_error_code(Protocol::Client::Error::NOT_LEADER);

    // 1. no hint
    service->serviceSpecificError(OpCode::READ_ONLY_TREE, request, error);

    // 2. bad hint (wrong port)
    error.set_leader_hint("127.0.0.1:0");
    service->serviceSpecificError(OpCode::READ_ONLY_TREE, request, error);

    // 3. ok, fine, let it through
    service->reply(OpCode::READ_ONLY_TREE, request, expResponse);

    std::unique_ptr<LeaderRPCBase::Call> call = leaderRPC->makeCall();

    // 1. no hint
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::RETRY,
              call->wait(response, TimePoint::max()));
    EXPECT_FALSE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);

    // 2. hint
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::RETRY,
              call->wait(response, TimePoint::max()));
    EXPECT_FALSE(leaderRPC->leaderSession.get());
    EXPECT_EQ("127.0.0.1:0", leaderRPC->leaderHint);

    // 3. try bad hint (wrong port)
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::RETRY,
              call->wait(response, TimePoint::max()));
    EXPECT_FALSE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);

    // 4. finally works
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::OK,
              call->wait(response, TimePoint::max()));
    EXPECT_TRUE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);
    EXPECT_EQ(expResponse, response);
}

TEST_F(ClientLeaderRPCTest, Call_wait_timeout) {
    std::unique_ptr<LeaderRPCBase::Call> call = leaderRPC->makeCall();
    call->start(OpCode::READ_ONLY_TREE, request, TimePoint::max());
    EXPECT_EQ(LeaderRPCBase::Call::Status::TIMEOUT,
              call->wait(response, TimePoint::min()));
}


TEST_F(ClientLeaderRPCTest, Call_wait_sessionExpired) {
    Protocol::Client::Error error;
    error.set_error_code(Protocol::Client::Error::SESSION_EXPIRED);

    leaderRPC->eventLoop.exit();
    leaderRPC->eventLoopThread.join();

    EXPECT_DEATH({
            leaderRPC->eventLoopThread = std::thread(&Event::Loop::runForever,
                                                     &leaderRPC->eventLoop);
            init();
            service->serviceSpecificError(OpCode::READ_ONLY_TREE,
                                          request, error);
            leaderRPC->call(OpCode::READ_ONLY_TREE, request, response,
                            TimePoint::max());
        },
        "Session expired");
}


// constructor and destructor tested adequately in tests for call()

TEST_F(ClientLeaderRPCTest, callOK) {
    init();
    service->reply(OpCode::READ_ONLY_TREE, request, expResponse);
    leaderRPC->call(OpCode::READ_ONLY_TREE, request, response,
                    TimePoint::max());
    EXPECT_EQ(expResponse, response);
}

TEST_F(ClientLeaderRPCTest, callRPCFailed) {
    init();
    service->closeSession(OpCode::READ_ONLY_TREE, request);
    service->reply(OpCode::READ_ONLY_TREE, request, expResponse);
    leaderRPC->call(OpCode::READ_ONLY_TREE, request, response,
                    TimePoint::max());
    EXPECT_EQ(expResponse, response);
}

// getSession() tested pretty well in tests for Call

TEST_F(ClientLeaderRPCTest, reportFailure) {
    std::shared_ptr<RPC::ClientSession> session1 =
        leaderRPC->getSession(TimePoint::max());
    EXPECT_TRUE(leaderRPC->leaderSession.get());
    leaderRPC->reportFailure(session1);
    EXPECT_FALSE(leaderRPC->leaderSession.get());
    std::shared_ptr<RPC::ClientSession> session2 =
        leaderRPC->getSession(TimePoint::max());
    leaderRPC->reportFailure(session1);
    EXPECT_TRUE(leaderRPC->leaderSession.get());
}

TEST_F(ClientLeaderRPCTest, reportRedirect) {
    std::shared_ptr<RPC::ClientSession> session1 =
        leaderRPC->getSession(TimePoint::max());
    EXPECT_TRUE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);
    leaderRPC->reportRedirect(session1, "127.0.0.1:0");
    EXPECT_FALSE(leaderRPC->leaderSession.get());
    EXPECT_EQ("127.0.0.1:0", leaderRPC->leaderHint);
    std::shared_ptr<RPC::ClientSession> session2 =
        leaderRPC->getSession(TimePoint::max());
    EXPECT_EQ("", leaderRPC->leaderHint);
    leaderRPC->reportRedirect(session1, "127.0.0.1:1");
    EXPECT_TRUE(leaderRPC->leaderSession.get());
    EXPECT_EQ("", leaderRPC->leaderHint);
}

} // namespace LogCabin::Client::<anonymous>
} // namespace LogCabin::Client
} // namespace LogCabin
