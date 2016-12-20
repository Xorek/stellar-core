// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "lib/http/server.hpp"
#include "lib/json/json.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "main/Config.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/StatusManager.h"
#include "StellarCoreVersion.h"

#include "util/basen.h"
#include "medida/reporting/json_reporter.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"

#include "ExternalQueue.h"

#include <regex>
#include "test/TxTests.h"
using namespace stellar::txtest;

using std::placeholders::_1;
using std::placeholders::_2;

namespace stellar
{
CommandHandler::CommandHandler(Application& app) : mApp(app)
{
    if (mApp.getConfig().HTTP_PORT)
    {
        std::string ipStr;
        if (mApp.getConfig().PUBLIC_HTTP_PORT)
        {
            ipStr = "0.0.0.0";
        }
        else
        {
            ipStr = "127.0.0.1";
        }
        LOG(INFO) << "Listening on " << ipStr << ":"
                  << mApp.getConfig().HTTP_PORT << " for HTTP requests";

        int httpMaxClient = mApp.getConfig().HTTP_MAX_CLIENT;

        mServer = stellar::make_unique<http::server::server>(
            app.getClock().getIOService(), ipStr, mApp.getConfig().HTTP_PORT, httpMaxClient);
    }
    else
    {
        mServer = stellar::make_unique<http::server::server>(
            app.getClock().getIOService());
    }

    mServer->add404(std::bind(&CommandHandler::fileNotFound, this, _1, _2));

    mServer->addRoute("bans",
                      std::bind(&CommandHandler::bans, this, _1, _2));
    mServer->addRoute("catchup",
                      std::bind(&CommandHandler::catchup, this, _1, _2));
    mServer->addRoute("checkdb",
                      std::bind(&CommandHandler::checkdb, this, _1, _2));
    mServer->addRoute("checkpoint",
                      std::bind(&CommandHandler::checkpoint, this, _1, _2));
    mServer->addRoute("connect",
                      std::bind(&CommandHandler::connect, this, _1, _2));
    mServer->addRoute("dropcursor",
                      std::bind(&CommandHandler::dropcursor, this, _1, _2));
    mServer->addRoute("droppeer",
                      std::bind(&CommandHandler::dropPeer, this, _1, _2));
    mServer->addRoute("generateload",
                      std::bind(&CommandHandler::generateLoad, this, _1, _2));
    mServer->addRoute("info", std::bind(&CommandHandler::info, this, _1, _2));
    mServer->addRoute("ll", std::bind(&CommandHandler::ll, this, _1, _2));
    mServer->addRoute("logrotate",
                      std::bind(&CommandHandler::logRotate, this, _1, _2));
    mServer->addRoute("maintenance",
                      std::bind(&CommandHandler::maintenance, this, _1, _2));
    mServer->addRoute("manualclose",
                      std::bind(&CommandHandler::manualClose, this, _1, _2));
    mServer->addRoute("metrics",
                      std::bind(&CommandHandler::metrics, this, _1, _2));
    mServer->addRoute("peers", std::bind(&CommandHandler::peers, this, _1, _2));
    mServer->addRoute("quorum",
                      std::bind(&CommandHandler::quorum, this, _1, _2));
    mServer->addRoute("setcursor",
                      std::bind(&CommandHandler::setcursor, this, _1, _2));
    mServer->addRoute("scp", std::bind(&CommandHandler::scpInfo, this, _1, _2));
    mServer->addRoute("testacc",
                      std::bind(&CommandHandler::testAcc, this, _1, _2));
    mServer->addRoute("testtx",
                      std::bind(&CommandHandler::testTx, this, _1, _2));
    mServer->addRoute("tx", std::bind(&CommandHandler::tx, this, _1, _2));
    mServer->addRoute("unban",
                      std::bind(&CommandHandler::unban, this, _1, _2));
}

void
CommandHandler::manualCmd(std::string const& cmd)
{
    http::server::reply reply;
    http::server::request request;
    request.uri = cmd;
    mServer->handle_request(request, reply);
    LOG(INFO) << cmd << " -> " << reply.content;
}

SequenceNumber
getSeq(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = AccountFrame::loadAccount(k.getPublicKey(), app.getDatabase());
    if (account)
    {
        return account->getSeqNum();
    }
    return 0;
}

void
CommandHandler::testAcc(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    Json::Value root;
    auto accName = retMap.find("name");
    if (accName == retMap.end())
    {
        root["status"] = "error";
        root["detail"] = "Bad HTTP GET: try something like: testacc?name=bob";
    }
    else
    {
        SecretKey key;
        if (accName->second == "root")
        {
            key = getRoot(mApp.getNetworkID());
        }
        else
        {
            key = getAccount(accName->second.c_str());
        }
        auto acc = loadAccount(key, mApp, false);
        if (acc)
        {
            root["name"] = accName->second;
            root["id"] = PubKeyUtils::toStrKey(acc->getID());
            root["balance"] = (Json::Int64)acc->getBalance();
            root["seqnum"] = (Json::UInt64)acc->getSeqNum();
        }
    }
    retStr = root.toStyledString();
}

void
CommandHandler::testTx(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto to = retMap.find("to");
    auto from = retMap.find("from");
    auto amount = retMap.find("amount");
    auto create = retMap.find("create");

    Json::Value root;

    if (to != retMap.end() && from != retMap.end() && amount != retMap.end())
    {
        Hash const& networkID = mApp.getNetworkID();

        SecretKey toKey, fromKey;
        if (to->second == "root")
        {
            toKey = getRoot(networkID);
        }
        else
        {
            toKey = getAccount(to->second.c_str());
        }

        if (from->second == "root")
        {
            fromKey = getRoot(networkID);
        }
        else
        {
            fromKey = getAccount(from->second.c_str());
        }

        uint64_t paymentAmount = 0;
        std::istringstream iss(amount->second);
        iss >> paymentAmount;

        root["from_name"] = from->second;
        root["to_name"] = to->second;
        root["from_id"] = PubKeyUtils::toStrKey(fromKey.getPublicKey());
        root["to_id"] = PubKeyUtils::toStrKey(toKey.getPublicKey());
        ;
        root["amount"] = (Json::UInt64)paymentAmount;

        SequenceNumber fromSeq = getSeq(fromKey, mApp) + 1;

        TransactionFramePtr txFrame;
        if (create != retMap.end() && create->second == "true")
        {
            txFrame = createCreateAccountTx(networkID, fromKey, toKey, fromSeq,
                                            paymentAmount);
        }
        else
        {
            txFrame = createPaymentTx(networkID, fromKey, toKey, fromSeq,
                                      paymentAmount);
        }

        switch (mApp.getHerder().recvTransaction(txFrame))
        {
        case Herder::TX_STATUS_PENDING:
            root["status"] = "pending";
            break;
        case Herder::TX_STATUS_DUPLICATE:
            root["status"] = "duplicate";
            break;
        case Herder::TX_STATUS_ERROR:
            root["status"] = "error";
            root["detail"] =
                xdr::xdr_to_string(txFrame->getResult().result.code());
            break;
        default:
            assert(false);
        }
    }
    else
    {
        root["status"] = "error";
        root["detail"] = "Bad HTTP GET: try something like: "
                         "testtx?from=root&to=bob&amount=1000000000";
    }
    retStr = root.toStyledString();
}

void
CommandHandler::fileNotFound(std::string const& params, std::string& retStr)
{
    retStr = "<b>Welcome to stellar-core!</b><p>";
    retStr += "supported commands:<p/>";

    retStr +=
        "<p><h1> /bans</h1>"
        "list current active bans"
        "</p><p><h1> /catchup?ledger=NNN[&mode=MODE]</h1>"
        "triggers the instance to catch up to ledger NNN from history; "
        "mode is either 'minimal' (the default, if omitted) or 'complete'."
        "</p><p><h1> /checkdb</h1>"
        "triggers the instance to perform an integrity check of the database."
        "</p><p><h1> /checkpoint</h1>"
        "triggers the instance to write an immediate history checkpoint."
        "</p><p><h1> /connect?peer=NAME&port=NNN</h1>"
        "triggers the instance to connect to peer NAME at port NNN."
        "</p><p><h1> "
        "/droppeer?node=NODE_ID[&ban=D]</h1>"
        "drops peer identified by PEER_ID, when D is 1 the peer is also banned"
        "</p><p><h1> "
        "/generateload[?accounts=N&txs=M&txrate=(R|auto)]</h1>"
        "artificially generate load for testing; must be used with "
        "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING set to true"
        "</p><p><h1> /help</h1>"
        "give a list of currently supported commands"
        "</p><p><h1> /info</h1>"
        "returns information about the server in JSON format (sync state, "
        "connected peers, etc)"
        "</p><p><h1> /ll?level=L[&partition=P]</h1>"
        "adjust the log level for partition P (or all if no partition is "
        "specified).<br>"
        "level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE, TRACE"
        "</p><p><h1> /logrotate</h1>"
        "rotate log files"
        "</p><p><h1> /manualclose</h1>"
        "close the current ledger; must be used with MANUAL_CLOSE set to true"
        "</p><p><h1> /metrics</h1>"
        "returns a snapshot of the metrics registry (for monitoring and "
        "debugging purpose)"
        "</p><p><h1> /peers</h1>"
        "returns the list of known peers in JSON format"
        "</p><p><h1> /quorum?[node=NODE_ID][&compact=true]</h1>"
        "returns information about the quorum for node NODE_ID (this node by"
        " default). NODE_ID is either a full key (`GABCD...`), an alias "
        "(`$name`) or an abbreviated ID(`@GABCD`)."
        "If compact is set, only returns a summary version."
        "</p><p><h1> /scp?[limit=n]</h1>"
        "returns a JSON object with the internal state of the SCP engine for "
        "the last n (default 2) ledgers."
        "</p><p><h1> /tx?blob=BASE64</h1>"
        "submit a transaction to the network.<br>"
        "blob is a base64 encoded XDR serialized 'TransactionEnvelope'<br>"
        "returns a JSON object<br>"
        "wasReceived: boolean, true if transaction was queued properly<br>"
        "result: base64 encoded, XDR serialized 'TransactionResult'<br>"
        "</p><p><h1> /dropcursor?id=XYZ</h1> deletes the tracking cursor with "
        "identified by `id`. See `setcursor` for more information"
        "</p><p><h1> /setcursor?id=ID&cursor=N</h1> sets or creates a cursor "
        "identified by `ID` with value `N`. ID is an uppercase AlphaNum, N is "
        "an uint32 that represents the last ledger sequence number that the "
        "instance ID processed."
        "Cursors are used by dependent services to tell stellar - core which "
        "data can be safely deleted by the instance."
        "The data is historical data stored in the SQL tables such as "
        "txhistory or ledgerheaders.When all consumers processed the data for "
        "ledger sequence N the data can be safely removed by the instance."
        "The actual deletion is performed by invoking the `maintenance` "
        "endpoint."
        "</p><p><h1> /maintenance[?queue=true]</h1> Performs maintenance tasks "
        "on the instance."
        "<ul><li><i>queue</i> performs deletion of queue data.See setcursor "
        "for more information</li></ul>"
        "</p><p><h1> "
        "/unban?node=NODE_ID</h1>"
        "remove ban for PEER_ID"
        "</p>"

        "<br>";

    retStr += "<p>Have fun!</p>";
}

void
CommandHandler::manualClose(std::string const& params, std::string& retStr)
{
    if (mApp.manualClose())
    {
        retStr = "Forcing ledger to close...";
    }
    else
    {
        retStr =
            "Set MANUAL_CLOSE=true in the stellar-core.cfg if you want this "
            "behavior";
    }
}

enum class Requirement
{
    OPTIONAL,
    REQUIRED
};

template <typename T>
bool
parseNumParam(std::map<std::string, std::string> const& map,
              std::string const& key, T& val, std::string& retStr,
              Requirement requirement)
{
    auto i = map.find(key);
    if (i != map.end())
    {
        std::stringstream str(i->second);
        str >> val;
        if (val == 0)
        {
            retStr = fmt::format("Failed to parse '{}' argument", key);
            return false;
        }
        return true;
    }
    return requirement == Requirement::OPTIONAL;
}

void
CommandHandler::generateLoad(std::string const& params, std::string& retStr)
{
    if (mApp.getConfig().ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING)
    {
        // Defaults are 200k accounts, 200k txs, 10 tx/s. This load-test will
        // therefore take 40k secs or about 12 hours.

        uint32_t nAccounts = 200000;
        uint32_t nTxs = 200000;
        uint32_t txRate = 10;
        bool autoRate = false;

        std::map<std::string, std::string> map;
        http::server::server::parseParams(params, map);

        if (!parseNumParam(map, "accounts", nAccounts, retStr, Requirement::OPTIONAL))
            return;

        if (!parseNumParam(map, "txs", nTxs, retStr, Requirement::OPTIONAL))
            return;

        {
            auto i = map.find("txrate");
            if (i != map.end() && i->second == std::string("auto"))
            {
                autoRate = true;
            }
            else if (!parseNumParam(map, "txrate", txRate, retStr, Requirement::OPTIONAL))
                return;
        }

        double hours = ((nAccounts + nTxs) / txRate) / 3600.0;
        mApp.generateLoad(nAccounts, nTxs, txRate, autoRate);
        retStr = fmt::format(
            "Generating load: {:d} accounts, {:d} txs, {:d} tx/s = {:f} hours",
            nAccounts, nTxs, txRate, hours);
    }
    else
    {
        retStr = "Set ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true in "
                 "the stellar-core.cfg if you want this behavior";
    }
}

void
CommandHandler::peers(std::string const& params, std::string& retStr)
{
    Json::Value root;

    root["peers"];
    int counter = 0;
    for (auto peer : mApp.getOverlayManager().getPeers())
    {
        root["peers"][counter]["ip"] = peer->getIP();
        root["peers"][counter]["port"] = (int)peer->getRemoteListeningPort();
        root["peers"][counter]["ver"] = peer->getRemoteVersion();
        root["peers"][counter]["olver"] = (int)peer->getRemoteOverlayVersion();
        root["peers"][counter]["id"] =
            mApp.getConfig().toStrKey(peer->getPeerID());

        counter++;
    }

    retStr = root.toStyledString();
}

void
CommandHandler::info(std::string const& params, std::string& retStr)
{
    Json::Value root;

    auto& lm = mApp.getLedgerManager();

    auto& info = root["info"];

    if (mApp.getConfig().UNSAFE_QUORUM)
        info["UNSAFE_QUORUM"] = "UNSAFE QUORUM ALLOWED";
    info["build"] = STELLAR_CORE_VERSION;
    info["protocol_version"] = mApp.getConfig().LEDGER_PROTOCOL_VERSION;
    info["state"] = mApp.getStateHuman();
    info["ledger"]["num"] = (int)lm.getLedgerNum();
    info["ledger"]["hash"] = binToHex(lm.getLastClosedLedgerHeader().hash);
    info["ledger"]["closeTime"] =
        (int)lm.getLastClosedLedgerHeader().header.scpValue.closeTime;
    info["ledger"]["age"] = (int)lm.secondsSinceLastLedgerClose();
    info["numPeers"] = (int)mApp.getOverlayManager().getPeers().size();
    info["network"] = mApp.getConfig().NETWORK_PASSPHRASE;

    auto& statusMessages = mApp.getStatusManager();
    auto counter = 0;
    for (auto statusMessage : statusMessages)
    {
        info["status"][counter++] = statusMessage.second;
    }

    auto& herder = mApp.getHerder();
    Json::Value q;
    herder.dumpQuorumInfo(q, mApp.getConfig().NODE_SEED.getPublicKey(), true,
                          herder.getCurrentLedgerSeq());
    if (q["slots"].size() != 0)
    {
        info["quorum"] = q["slots"];
    }

    retStr = root.toStyledString();
}

void
CommandHandler::metrics(std::string const& params, std::string& retStr)
{
    mApp.syncAllMetrics();
    medida::reporting::JsonReporter jr(mApp.getMetrics());
    retStr = jr.Report();
}

void
CommandHandler::logRotate(std::string const& params, std::string& retStr)
{
    retStr = "Log rotate...";

    Logging::rotate();
}

void
CommandHandler::catchup(std::string const& params, std::string& retStr)
{
    switch (mApp.getLedgerManager().getState())
    {
    case LedgerManager::LM_BOOTING_STATE:
        retStr = "Ledger Manager is still booting, try later";
        return;
    case LedgerManager::LM_CATCHING_UP_STATE:
        retStr = "Catchup already in progress";
        return;
    default:
        break;
    }

    HistoryManager::CatchupMode mode = HistoryManager::CATCHUP_MINIMAL;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    uint32_t ledger = 0;
    auto ledgerP = retMap.find("ledger");
    if (ledgerP == retMap.end())
    {
        retStr = "Missing required parameter 'ledger=NNN'";
        return;
    }
    else
    {
        std::stringstream str(ledgerP->second);
        str >> ledger;
        if (ledger == 0)
        {
            retStr = "Failed to parse ledger number";
            return;
        }
    }

    auto modeP = retMap.find("mode");
    if (modeP != retMap.end())
    {
        if (modeP->second == std::string("complete"))
        {
            mode = HistoryManager::CATCHUP_COMPLETE;
        }
        else if (modeP->second == std::string("minimal"))
        {
            mode = HistoryManager::CATCHUP_MINIMAL;
        }
        else if (modeP->second == std::string("recent"))
        {
            mode = HistoryManager::CATCHUP_RECENT;
        }
        else
        {
            retStr = "Mode should be either 'minimal', 'recent' or 'complete'";
            return;
        }
    }

    mApp.getLedgerManager().startCatchUp(ledger, mode, true);
    retStr = (std::string("Started catchup to ledger ") +
              std::to_string(ledger) + std::string(" in mode ") +
              std::string(mode == HistoryManager::CATCHUP_COMPLETE
                              ? "CATCHUP_COMPLETE"
                              : (mode == HistoryManager::CATCHUP_RECENT
                                     ? "CATCHUP_RECENT"
                                     : "CATCHUP_MINIMAL")));
}

void
CommandHandler::checkdb(std::string const& params, std::string& retStr)
{
    mApp.checkDB();
    retStr = "CheckDB started.";
}

void
CommandHandler::checkpoint(std::string const& params, std::string& retStr)
{
    auto& hm = mApp.getHistoryManager();
    if (hm.hasAnyWritableHistoryArchive())
    {
        size_t initFail = hm.getPublishFailureCount();
        size_t initDone = hm.getPublishSuccessCount() + initFail;
        asio::error_code ec;
        uint32_t lclNum = mApp.getLedgerManager().getLastClosedLedgerNum();
        uint32_t ledgerNum = mApp.getLedgerManager().getLedgerNum();
        hm.queueCurrentHistory();
        size_t toPublish = hm.publishQueuedHistory();
        while (((hm.getPublishSuccessCount() + hm.getPublishFailureCount()) -
                initDone) != toPublish)
        {
            mApp.getClock().crank(false);
        }
        if (initFail != hm.getPublishFailureCount())
        {
            retStr = std::string("Publish failed");
        }
        else
        {
            retStr = fmt::format("Forcibly published checkpoint 0x{:08x}, "
                                 "at current ledger {};\n"
                                 "To force catch up on other peers, "
                                 "issue the command 'catchup?ledger={}'",
                                 lclNum, ledgerNum, ledgerNum);
        }
    }
    else
    {
        retStr = "No writable history archives available";
    }
}

void
CommandHandler::connect(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerP = retMap.find("peer");
    auto portP = retMap.find("port");
    if (peerP != retMap.end() && portP != retMap.end())
    {
        std::stringstream str;
        str << peerP->second << ":" << portP->second;
        retStr = "Connect to: ";
        retStr += str.str();
        mApp.getOverlayManager().connectTo(str.str());
    }
    else
    {
        retStr = "Must specify a peer and port: connect&peer=PEER&port=PORT";
    }
}

void
CommandHandler::dropPeer(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerId = retMap.find("node");
    auto ban = retMap.find("ban");
    if (peerId != retMap.end())
    {
        auto found = false;
        NodeID n;
        if (mApp.getHerder().resolveNodeID(peerId->second, n))
        {
            auto peers = mApp.getOverlayManager().getPeers();
            auto peer = std::find_if(
                peers.begin(), peers.end(),
                [&n](Peer::pointer peer) { return peer->getPeerID() == n; });
            if (peer != peers.end())
            {
                mApp.getOverlayManager().dropPeer(*peer);
                if (ban != retMap.end() && ban->second == "1")
                {
                    retStr = "Drop and ban peer: ";
                    mApp.getBanManager().banNode(n);
                }
                else
                    retStr = "Drop peer: ";

                retStr += peerId->second;
                found = true;
            }
        }

        if (!found)
        {
            retStr = "Peer ";
            retStr += peerId->second;
            retStr += " not found";
        }
    }
    else
    {
        retStr = "Must specify at least peer id: droppeer?node=NODE_ID";
    }
}

void
CommandHandler::bans(std::string const& params, std::string& retStr)
{
    Json::Value root;

    root["bans"];
    int counter = 0;
    for (auto ban : mApp.getBanManager().getBans())
    {
        root["bans"][counter] = ban;

        counter++;
    }

    retStr = root.toStyledString();
}

void
CommandHandler::unban(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerId = retMap.find("node");
    if (peerId != retMap.end())
    {
        NodeID n;
        if (mApp.getHerder().resolveNodeID(peerId->second, n))
        {
            retStr = "Unban peer: ";
            retStr += peerId->second;
            mApp.getBanManager().unbanNode(n);
        }
        else
        {
            retStr = "Peer ";
            retStr += peerId->second;
            retStr += " not found";
        }
    }
    else
    {
        retStr = "Must specify at least peer id: unban?node=NODE_ID";
    }
}

void
CommandHandler::quorum(std::string const& params, std::string& retStr)
{
    Json::Value root;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    NodeID n;

    try
    {
        std::string nID = retMap["node"];

        if (nID.empty())
        {
            n = mApp.getConfig().NODE_SEED.getPublicKey();
        }
        else
        {
            if (!mApp.getHerder().resolveNodeID(nID, n))
            {
                throw std::invalid_argument("unknown name");
            }
        }

        mApp.getHerder().dumpQuorumInfo(root, n, retMap["compact"] == "true");

        retStr = root.toStyledString();
    }
    catch (std::exception& e)
    {
        retStr = (fmt::MemoryWriter() << "{\"exception\": \"" << e.what()
                                      << "\"}").str();
    }
    catch (...)
    {
        retStr = "{\"exception\": \"generic\"}";
    }
}

void
CommandHandler::scpInfo(std::string const& params, std::string& retStr)
{
    Json::Value root;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    size_t lim = 2;
    std::string limStr = retMap["limit"];
    if (!limStr.empty())
    {
        size_t n = strtoul(limStr.c_str(), NULL, 0);
        if (n != 0)
        {
            lim = n;
        }
    }

    mApp.getHerder().dumpInfo(root, lim);

    retStr = root.toStyledString();
}

// "Must specify a log level: ll?level=<level>&partition=<name>";
void
CommandHandler::ll(std::string const& params, std::string& retStr)
{
    Json::Value root;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    std::string levelStr = retMap["level"];
    std::string partition = retMap["partition"];
    if (!levelStr.size())
    {
        root["Fs"] = Logging::getStringFromLL(Logging::getLogLevel("Fs"));
        root["SCP"] = Logging::getStringFromLL(Logging::getLogLevel("SCP"));
        root["Bucket"] =
            Logging::getStringFromLL(Logging::getLogLevel("Bucket"));
        root["Database"] =
            Logging::getStringFromLL(Logging::getLogLevel("Database"));
        root["History"] =
            Logging::getStringFromLL(Logging::getLogLevel("History"));
        root["Process"] =
            Logging::getStringFromLL(Logging::getLogLevel("Process"));
        root["Ledger"] =
            Logging::getStringFromLL(Logging::getLogLevel("Ledger"));
        root["Overlay"] =
            Logging::getStringFromLL(Logging::getLogLevel("Overlay"));
        root["Herder"] =
            Logging::getStringFromLL(Logging::getLogLevel("Herder"));
        root["Tx"] = Logging::getStringFromLL(Logging::getLogLevel("Tx"));
    }
    else
    {
        el::Level level = Logging::getLLfromString(levelStr);
        if (partition.size())
        {
            Logging::setLogLevel(level, partition.c_str());
            root[partition] = Logging::getStringFromLL(level);
        }
        else
        {
            Logging::setLogLevel(level, nullptr);
            root["Global"] = Logging::getStringFromLL(level);
        }
    }

    retStr = root.toStyledString();
}

static const char* TX_STATUS_STRING[Herder::TX_STATUS_COUNT] = {
    "PENDING", "DUPLICATE", "ERROR"};

void
CommandHandler::tx(std::string const& params, std::string& retStr)
{
    std::ostringstream output;

    const std::string prefix("?blob=");
    if (params.compare(0, prefix.size(), prefix) == 0)
    {
        TransactionEnvelope envelope;
        try
        {
            std::string blob = params.substr(prefix.size());
            std::vector<uint8_t> binBlob;
            bn::decode_b64(blob, binBlob);

            xdr::xdr_from_opaque(binBlob, envelope);
            TransactionFramePtr transaction =
                TransactionFrame::makeTransactionFromWire(mApp.getNetworkID(),
                                                          envelope);
            if (transaction)
            {
                // add it to our current set
                // and make sure it is valid
                Herder::TransactionSubmitStatus status =
                    mApp.getHerder().recvTransaction(transaction);

                if (status == Herder::TX_STATUS_PENDING)
                {
                    StellarMessage msg;
                    msg.type(TRANSACTION);
                    msg.transaction() = envelope;
                    mApp.getOverlayManager().broadcastMessage(msg);
                }

                output << "{"
                       << "\"status\": "
                       << "\"" << TX_STATUS_STRING[status] << "\"";
                if (status == Herder::TX_STATUS_ERROR)
                {
                    std::string resultBase64;
                    auto resultBin =
                        xdr::xdr_to_opaque(transaction->getResult());
                    resultBase64.reserve(bn::encoded_size64(resultBin.size()) +
                                         1);
                    resultBase64 = bn::encode_b64(resultBin);

                    output << " , \"error\": \"" << resultBase64 << "\"";
                }
                output << "}";
            }
        }
        catch (std::exception& e)
        {
            output << "{\"exception\": \"" << e.what() << "\"}";
        }
        catch (...)
        {
            output << "{\"exception\": \"generic\"}";
        }
    }
    else
    {
        output << "{\"exception\": \"Must specify a tx blob: tx?blob=<tx in "
                  "xdr format>\"}";
    }

    retStr = output.str();
}

void
CommandHandler::dropcursor(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    if (!ExternalQueue::validateResourceID(id))
    {
        retStr = "Invalid resource id";
    }
    else
    {
        ExternalQueue ps(mApp);
        ps.deleteCursor(id);
        retStr = "Done";
    }
}

void
CommandHandler::setcursor(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    uint32 cursor;

    if (!parseNumParam(map, "cursor", cursor, retStr, Requirement::REQUIRED))
    {
        retStr = "Invalid cursor";
        return;
    }

    if (!ExternalQueue::validateResourceID(id))
    {
        retStr = "Invalid resource id";
    }
    else
    {
        ExternalQueue ps(mApp);
        ps.setCursorForResource(id, cursor);
        retStr = "Done";
    }
}

void
CommandHandler::maintenance(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    if (map["queue"] == "true")
    {
        mApp.maintenance();
        retStr = "Done";
    }
    else
    {
        retStr = "No work performed";
    }
}
}
