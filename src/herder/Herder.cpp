// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Herder.h"

#include <ctime>
#include "math.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerMaster.h"
#include "overlay/PeerMaster.h"
#include "main/Application.h"
#include "main/Config.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "util/Logging.h"
#include "lib/util/easylogging++.h"

namespace stellar
{

// Static helper for Herder's FBA constructor
static FBAQuorumSet
quorumSetFromApp(Application& app)
{
    FBAQuorumSet qSet;
    qSet.threshold = app.getConfig().QUORUM_THRESHOLD;
    for (auto q : app.getConfig().QUORUM_SET)
    {
        qSet.validators.push_back(q);
    }
    return qSet;
}

Herder::Herder(Application& app)
    : FBA(app.getConfig().VALIDATION_KEY,
          quorumSetFromApp(app))
    , mReceivedTransactions(4)
#ifdef _MSC_VER
    // This form of initializer causes a warning due to brace-elision on
    // clang.
    , mTxSetFetcher({ TxSetFetcher(app), TxSetFetcher(app) })
#else
    // This form of initializer is "not implemented" in MSVC yet.
    , mTxSetFetcher
    {
        {
            {
                TxSetFetcher(app)
            }
            ,
            {
                TxSetFetcher(app)
            }
        }
    }
#endif
    , mCurrentTxSetFetcher(0)
    , mFBAQSetFetcher(app)
    , mLedgersToWaitToParticipate(3)
    , mLastTrigger(app.getClock().now())
    , mTriggerTimer(app.getClock())
    , mBumpTimer(app.getClock())
    , mApp(app)
{
    // Inject our local qSet in the FBAQSetFetcher.
    FBAQuorumSetPtr qSet = 
        std::make_shared<FBAQuorumSet>(std::move(quorumSetFromApp(mApp)));
    recvFBAQuorumSet(qSet);
}

Herder::~Herder()
{
}

void
Herder::bootstrap()
{
    assert(!getSecretKey().isZero());
    assert(mApp.getConfig().START_NEW_NETWORK);

    mLastClosedLedger = mApp.getLedgerMaster().getLastClosedLedgerHeader();
    mLedgersToWaitToParticipate = 0;
    triggerNextLedger(asio::error_code());
}

void 
Herder::validateValue(const uint64& slotIndex,
                      const uint256& nodeID,
                      const Value& value,
                      std::function<void(bool)> const& cb)
{
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        return cb(false);
    }

    // First of all let's verify the internal Stellar Ballot signature is
    // correct.
    if (!verifyStellarBallot(b))
    {
        return cb(false);
    }

    // All tests that are relative to mLastClosedLedger are executed only once
    // we are fully synced up
    if (mLedgersToWaitToParticipate == 0)
    {
        // Check slotIndex.
        if (mLastClosedLedger.ledgerSeq + 1 != slotIndex)
        {
            return cb(false);
        }
        // Check closeTime (not too old)
        if (b.value.closeTime <= mLastClosedLedger.closeTime)
        {
            return cb(false);
        }
    }

    // make sure all the tx we have in the old set are included
    auto validate = [cb,b,slotIndex,nodeID,this] (TxSetFramePtr txSet)
    {
        // Check txSet (only if we're fully synced)
        if(mLedgersToWaitToParticipate == 0 && !txSet->checkValid(mApp))
        {
            CLOG(DEBUG, "Herder") << "Herder::validateValue"
                << "@" << binToHex(getLocalNodeID()).substr(0,6)
                << " i: " << slotIndex
                << " v: " << binToHex(nodeID).substr(0,6)
                << " Invalid txSet:"
                << " " << binToHex(txSet->getContentsHash()).substr(0,6);
            return cb(false);
        }
        
        CLOG(DEBUG, "Herder") << "Herder::validateValue"
            << "@" << binToHex(getLocalNodeID()).substr(0,6)
            << " i: " << slotIndex
            << " v: " << binToHex(nodeID).substr(0,6)
            << " txSet:"
            << " " << binToHex(txSet->getContentsHash()).substr(0,6)
            << " OK";
        return cb(true);
    };
    
    TxSetFramePtr txSet = fetchTxSet(b.value.txSetHash, true);
    if (!txSet)
    {
        mTxSetFetches[b.value.txSetHash].push_back(validate);
    }
    else
    {
        validate(txSet);
    }
}

int 
Herder::compareValues(const uint64& slotIndex, 
                      const uint32& ballotCounter,
                      const Value& v1, const Value& v2)
{
    using xdr::operator<;

    StellarBallot b1;
    StellarBallot b2;
    try
    {
        xdr::xdr_from_opaque(v1, b1);
        xdr::xdr_from_opaque(v2, b2);
    }
    catch (...)
    {
        // This should not be possible are valuies are validated before they
        // are compared.
        CLOG(ERROR, "Herder") << "Herder::compareValues"
            << "@" << binToHex(getLocalNodeID()).substr(0,6)
            << " Unexpected invalid value format";
        assert(false);
        return 0;
    }
    
    // Unverified StellarBallot shouldn't be possible either for the precise
    // same reasons.
    assert(verifyStellarBallot(b1));
    assert(verifyStellarBallot(b2));

    // Ordering is based on H(slotIndex, ballotCounter, nodeID). Such that the
    // round king value gets priviledged over other values. Given the hash
    // function used, a new king is "coronated" for each round of FBA (ballot
    // counter) and each slotIndex.
    
    SHA256 s1;
    s1.add(xdr::xdr_to_msg(slotIndex));
    s1.add(xdr::xdr_to_msg(ballotCounter));
    s1.add(xdr::xdr_to_msg(b1.nodeID));
    auto h1 = s1.finish();

    SHA256 s2;
    s2.add(xdr::xdr_to_msg(slotIndex));
    s2.add(xdr::xdr_to_msg(ballotCounter));
    s2.add(xdr::xdr_to_msg(b2.nodeID));
    auto h2 = s2.finish();

    if (h1 < h2) return -1;
    if (h2 < h1) return 1;

    if (b1.value < b2.value) return -1;
    if (b2.value < b1.value) return -1;

    return 0;
}

void 
Herder::validateBallot(const uint64& slotIndex,
                       const uint256& nodeID,
                       const FBABallot& ballot,
                       std::function<void(bool)> const& cb)
{
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(ballot.value, b);
    }
    catch (...)
    {
        return cb(false);
    }

    // Check closeTime (not too far in the future)
    uint64_t timeNow = VirtualClock::pointToTimeT(mApp.getClock().now());
    if (b.value.closeTime > timeNow + MAX_TIME_SLIP_SECONDS)
    {
        return cb(false);
    }

    // Check the ballot counter is not growing too rapidly. We ignore ballots
    // that were triggered before the expected series of timeouts (accepting
    // MAX_TIME_SLIP_SECONDS as error). This prevents ballot counter
    // exhaustion attacks.
    uint64_t lastTrigger = VirtualClock::pointToTimeT(mLastTrigger);
    uint64_t sumTimeouts = 0;
    // The second condition is to prevent attackers from emitting ballots whose
    // verification would busy lock us.
    for (int unsigned i = 0; 
         i < ballot.counter && 
         (timeNow + MAX_TIME_SLIP_SECONDS) >= (lastTrigger + sumTimeouts); 
         i ++)
    {
        sumTimeouts += std::min(MAX_FBA_TIMEOUT_SECONDS, (int)pow(2.0, i));
    }
    // This inequality is effectively a limitation on `ballot.counter`
    if ((timeNow + MAX_TIME_SLIP_SECONDS) < (lastTrigger + sumTimeouts))
    {
        return cb(false);
    }

    // Check baseFee (within range of desired fee).
    if (b.value.baseFee < mApp.getConfig().DESIRED_BASE_FEE * .5)
    {
        return cb(false);
    }
    if (b.value.baseFee > mApp.getConfig().DESIRED_BASE_FEE * 2)
    {
        return cb(false);
    }

    // Ignore ourselves if we're just watching FBA.
    if (getSecretKey().isZero() && nodeID == getLocalNodeID())
    {
        return cb(false);
    }

    // No need to check if all the txs are in the txSet as this is decided by
    // the king of that round. Just check that we believe that this ballot is
    // actually from the king itself.
    bool isKing = true;
    bool isTrusted = false;
    for (auto vID : getLocalQuorumSet().validators)
    {
        // A ballot is trusted if its value is generated or prepared by a node
        // in our qSet.
        if (b.nodeID == vID || b.nodeID == getLocalNodeID())
        {
            isTrusted = true;
        }

        SHA256 sProposed;
        sProposed.add(xdr::xdr_to_msg(slotIndex));
        sProposed.add(xdr::xdr_to_msg(ballot.counter));
        sProposed.add(xdr::xdr_to_msg(b.nodeID));
        auto hProposed = sProposed.finish();

        SHA256 sContender;
        sContender.add(xdr::xdr_to_msg(slotIndex));
        sContender.add(xdr::xdr_to_msg(ballot.counter));
        sContender.add(xdr::xdr_to_msg(vID));
        auto hContender = sContender.finish();

        // A ballot is king (locally) only if it is higher than any potential
        // ballots from nodes in our qSet.
        if(hProposed < hContender)
        {
            isKing = false;
        }
    }

    uint256 valueHash = 
        sha256(xdr::xdr_to_msg(ballot.value));

    CLOG(DEBUG, "Herder") << "Herder::validateBallot"
        << "@" << binToHex(getLocalNodeID()).substr(0,6)
        << " i: " << slotIndex
        << " v: " << binToHex(nodeID).substr(0,6)
        << " o: " << binToHex(b.nodeID).substr(0,6)
        << " b: (" << ballot.counter 
        << "," << binToHex(valueHash).substr(0,6) << ")"
        << " isTrusted: " << isTrusted
        << " isKing: " << isKing 
        << " timeout: " << pow(2.0, ballot.counter)/2;

    
    if(isKing && isTrusted)
    {
        return cb(true); 
    }
    else
    {
        // Create a timer to wait for current FBA timeout / 2 before accepting
        // that ballot.
        VirtualTimer ballotTimer(mApp.getClock());
        ballotTimer.expires_from_now(
            std::chrono::milliseconds(
                (int)(1000*pow(2.0, ballot.counter)/2)));
        ballotTimer.async_wait(
            [cb] (const asio::error_code& error)
            {
                return cb(true);
            });
        mBallotValidationTimers[ballot][nodeID].push_back(ballotTimer);

        // Check if the nodes that have requested validation for this ballot
        // is a v-blocking. If so, rush validation by cancelling all timers.
        std::vector<uint256> nodes;
        for (auto it : mBallotValidationTimers[ballot])
        {
            nodes.push_back(it.first);
        }
        if (isVBlocking(nodes))
        {
            // This will cancel all timers.
            mBallotValidationTimers.erase(ballot);
        }
    }
}

void
Herder::ballotDidHearFromQuorum(const uint64& slotIndex,
                                const FBABallot& ballot)
{
    // If we're not fully synced, we just don't timeout FBA.
    if (mLedgersToWaitToParticipate > 0)
    {
        return;
    }
    // Only validated values (current) values should trigger this.
    assert(slotIndex == mLastClosedLedger.ledgerSeq + 1);

    mBumpTimer.cancel();

    // Once we hear from a transitive quorum, we start a timer in case FBA
    // timeouts.
    mBumpTimer.expires_from_now(
        std::chrono::seconds((int)pow(2.0, ballot.counter)));

    mBumpTimer.async_wait(std::bind(&Herder::expireBallot, this, 
                                    std::placeholders::_1, 
                                    slotIndex, ballot));
}

void 
Herder::valueExternalized(const uint64& slotIndex,
                          const Value& value)
{
    mBumpTimer.cancel();
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        // This may not be possible as all messages are validated and should
        // therefore contain a valid StellarBallot.
        CLOG(ERROR, "Herder") << "Herder::valueExternalized"
            << "@" << binToHex(getLocalNodeID()).substr(0,6)
            << " Externalized StellarBallot malformed";
    }

    TxSetFramePtr externalizedSet = fetchTxSet(b.value.txSetHash, false);
    if (externalizedSet)
    {

        CLOG(INFO, "Herder") << "Herder::valueExternalized"
            << "@" << binToHex(getLocalNodeID()).substr(0,6)
            << " txSet: " << binToHex(b.value.txSetHash).substr(0,6);
    
        // we don't need to keep fetching any of the old TX sets
        mTxSetFetcher[mCurrentTxSetFetcher].stopFetchingAll();

        mCurrentTxSetFetcher = mCurrentTxSetFetcher ? 0 : 1;
        mTxSetFetcher[mCurrentTxSetFetcher].clear();

        // Triggers sync if not already syncing.
        mApp.getLedgerGateway().externalizeValue(externalizedSet);

        // remove all these tx from mReceivedTransactions
        for (auto tx : externalizedSet->mTransactions)
        {
            removeReceivedTx(tx);
        }
        // rebroadcast those left in set 1
        assert(mReceivedTransactions.size() >= 2);
        for (auto tx : mReceivedTransactions[1])
        {
            auto msg = tx->toStellarMessage();
            mApp.getOverlayGateway().broadcastMessage(msg);
        }

        // Evict nodes that weren't touched for more than
        auto now = mApp.getClock().now();
        for (auto it : mNodeLastAccess)
        {
            if ((now - it.second) >
                std::chrono::seconds(NODE_EXPIRATION_SECONDS))
            {
                purgeNode(it.first);
            }
        }
         
        // Evict slots that are outside of our ledger validity bracket
        if (slotIndex > LEDGER_VALIDITY_BRACKET)
        {
            purgeSlots(slotIndex - LEDGER_VALIDITY_BRACKET);
        }

        // Move all the remaining to the next highest level don't move the
        // largest array.
        for (size_t n = mReceivedTransactions.size() - 1; n > 0; n--)
        {
            for (auto tx : mReceivedTransactions[n-1])
            {
                mReceivedTransactions[n].push_back(tx);
            }
            mReceivedTransactions[n-1].clear();
        }
    }
    else
    {
        // This may not be possible as all messages are validated and should
        // therefore fetch the txSet before being considered by FBA.
        CLOG(ERROR, "Herder") << "Herder::valueExternalized"
            << "@" << binToHex(getLocalNodeID()).substr(0,6)
            << " Externalized txSet not found";
    }
}

void 
Herder::nodeTouched(const uint256& nodeID)
{
    // We simply store the time of last access each time a node is touched by
    // FBA. That way we can evict old irrelevant nodes at each round.
    mNodeLastAccess[nodeID] = mApp.getClock().now();
}

void 
Herder::retrieveQuorumSet(const uint256& nodeID,
                          const Hash& qSetHash,
                          std::function<void(const FBAQuorumSet&)> const& cb)
{
    CLOG(DEBUG, "Herder") << "Herder::retrieveQuorumSet"
        << "@" << binToHex(getLocalNodeID()).substr(0,6)
        << " qSet: " << binToHex(qSetHash).substr(0,6);
    auto retrieve = [cb, this] (FBAQuorumSetPtr qSet)
    {
        return cb(*qSet);
    };

    // Peer Overlays and nodeIDs have no relationship for now. Sow we just
    // retrieve qSetHash by asking the whole overlay.
    FBAQuorumSetPtr qSet = fetchFBAQuorumSet(qSetHash, true);
    if (!qSet)
    {
        mFBAQSetFetches[qSetHash].push_back(retrieve);
    }
    else
    {
        retrieve(qSet);
    }
}

void 
Herder::emitEnvelope(const FBAEnvelope& envelope)
{
    CLOG(DEBUG, "Herder") << "Herder:emitEnvelope"
        << "@" << binToHex(getLocalNodeID()).substr(0,6)
        << " mLedgersToWaitToParticipate: " << mLedgersToWaitToParticipate;
    // We don't emit any envelope as long as we're not fully synced
    if (mLedgersToWaitToParticipate > 0)
    {
        return;
    }
    
    StellarMessage msg;
    msg.type(FBA_MESSAGE);
    msg.envelope() = envelope;

    mApp.getOverlayGateway().broadcastMessage(msg);
}

TxSetFramePtr
Herder::fetchTxSet(const uint256& txSetHash, 
                   bool askNetwork)
{
    return mTxSetFetcher[mCurrentTxSetFetcher].fetchItem(txSetHash, askNetwork);
}

void
Herder::recvTxSet(TxSetFramePtr txSet)
{
    if (mTxSetFetcher[mCurrentTxSetFetcher].recvItem(txSet))
    { 
        // someone cares about this set
        for (auto tx : txSet->mTransactions)
        {
            recvTransaction(tx);
        }

        // Runs any pending validation on this txSet.
        auto it = mTxSetFetches.find(txSet->getContentsHash());
        if (it != mTxSetFetches.end())
        {
            for (auto validate : it->second)
            {
                validate(txSet);
            }
            mTxSetFetches.erase(it);
        }
    }
}

void 
Herder::doesntHaveTxSet(uint256 const& txSetHash, 
                        PeerPtr peer)
{
    mTxSetFetcher[mCurrentTxSetFetcher].doesntHave(txSetHash, peer);
}


FBAQuorumSetPtr
Herder::fetchFBAQuorumSet(uint256 const& qSetHash, 
                          bool askNetwork)
{
    return mFBAQSetFetcher.fetchItem(qSetHash, askNetwork);
}

void 
Herder::recvFBAQuorumSet(FBAQuorumSetPtr qSet)
{
    CLOG(DEBUG, "Herder") << "Herder::recvFBAQuorumSet"
        << "@" << binToHex(getLocalNodeID()).substr(0,6)
        << " qSet: " << binToHex(sha256(xdr::xdr_to_msg(*qSet))).substr(0,6);
              
    if (mFBAQSetFetcher.recvItem(qSet))
    { 
        // someone cares about this set
        uint256 qSetHash = sha256(xdr::xdr_to_msg(*qSet));

        // Runs any pending retrievals on this qSet
        auto it = mFBAQSetFetches.find(qSetHash);
        if (it != mFBAQSetFetches.end())
        {
            for (auto retrieve : it->second)
            {
                retrieve(qSet);
            }
            mFBAQSetFetches.erase(it);
        }
    }
}

void 
Herder::doesntHaveFBAQuorumSet(uint256 const& qSetHash, 
                               PeerPtr peer)
{
    mFBAQSetFetcher.doesntHave(qSetHash, peer);
}



bool
Herder::recvTransaction(TransactionFramePtr tx)
{
    Hash& txID = tx->getFullHash();

    // determine if we have seen this tx before and if not if it has the right
    // seq num
    int numOthers=0;
    for (auto list : mReceivedTransactions)
    {
        for (auto oldTX : list)
        {
            if (txID == oldTX->getFullHash())
            {
                return false;
            }
            if (oldTX->getSourceID() == tx->getSourceID())
            {
                numOthers++;
            }
        }
    }
    
    if (!tx->checkValid(mApp)) 
    {
        return false;
    }
    
    // don't consider minBalance since you want to allow them to still send
    // around credit etc
    if (tx->getSourceAccount().getBalance() < 
        (numOthers + 1) * mApp.getLedgerGateway().getTxFee())
    {
        return false;
    }

    mReceivedTransactions[0].push_back(tx);

    return true;
}

void
Herder::recvFBAEnvelope(FBAEnvelope envelope,
                        std::function<void(bool)> const& cb)
{
    if (mLedgersToWaitToParticipate == 0)
    {
        uint64 minLedgerSeq = ((int)mLastClosedLedger.ledgerSeq -
            LEDGER_VALIDITY_BRACKET) < 0 ? 0 :
            (mLastClosedLedger.ledgerSeq - LEDGER_VALIDITY_BRACKET);
        uint64 maxLedgerSeq = mLastClosedLedger.ledgerSeq +
            LEDGER_VALIDITY_BRACKET;

        // If we are fully synced and the envelopes are out of our validity
        // brackets, we just ignore them.
        if(envelope.statement.slotIndex > maxLedgerSeq ||
           envelope.statement.slotIndex < minLedgerSeq)
        {
            return;
        }

        // If we are fully synced and we see envelopes that are from future
        // ledgers we store them for later replay.
        if (envelope.statement.slotIndex > mLastClosedLedger.ledgerSeq + 1)
        {
            mFutureEnvelopes[envelope.statement.slotIndex]
                .push_back(std::make_pair(envelope, cb));
        }
    }

    return receiveEnvelope(envelope, cb);
}

void
Herder::ledgerClosed(LedgerHeader& ledger)
{
    CLOG(TRACE, "Herder") << "Herder::ledgerClosed@"
        << "@" << binToHex(getLocalNodeID()).substr(0,6)
        << " ledger: " << binToHex(ledger.hash).substr(0,6);
    
    mLastClosedLedger = ledger;

    // As the current slotIndex changes we cancel all pending validation
    // timers. Since the value externalized, the messages that this generates
    // wont' have any impact.
    mBallotValidationTimers.clear();

    // We start skipping ledgers only after we're in SYNCED_STATE
    if (mLedgersToWaitToParticipate > 0 &&
        mApp.getState() != Application::State::SYNCED_STATE)
    {
        mLedgersToWaitToParticipate--;
    }

    // If we are not a validating not and just watching FBA we don't call
    // triggerNextLedger
    if (getSecretKey().isZero())
    {
        return;
    }

    // If we haven't waited for a couple ledgers after we got in SYNCED_STATE
    // we consider ourselves not fully synced so we don't push any value.
    if (mLedgersToWaitToParticipate > 0)
    {
        return;
    }

    // We trigger next ledger EXP_LEDGER_TIMESPAN_SECONDS after our last
    // trigger.
    mTriggerTimer.cancel();
    
    auto now = mApp.getClock().now();
    if ((now - mLastTrigger) < 
        std::chrono::seconds(EXP_LEDGER_TIMESPAN_SECONDS))
    {
        auto timeout = std::chrono::seconds(EXP_LEDGER_TIMESPAN_SECONDS) -
            (now - mLastTrigger);
        mTriggerTimer.expires_from_now(timeout);
    }
    else 
    {
        mTriggerTimer.expires_from_now(std::chrono::nanoseconds(0));
    }

    mTriggerTimer.async_wait(std::bind(&Herder::triggerNextLedger, this,
                                       std::placeholders::_1));
}

void
Herder::removeReceivedTx(TransactionFramePtr dropTx)
{
    for (auto& list : mReceivedTransactions)
    {
        for (auto iter = list.begin(); iter != list.end();)
        {
            if ((iter.operator->())->get()->getFullHash() == 
                dropTx->getFullHash())
            {
                list.erase(iter);
                return;
            }
            else
            {
                iter++;
            }
        }
    }
}

void
Herder::triggerNextLedger(const asio::error_code& error)
{
    if (error)
    {
        // This probably means we're shutting down.
        return;
    }
    
    // We store at which time we triggered consensus
    mLastTrigger = mApp.getClock().now();

    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    TxSetFramePtr proposedSet = std::make_shared<TxSetFrame>();
    for (auto list : mReceivedTransactions)
    {
        for (auto tx : list)
        {
            proposedSet->add(tx);
        }
    }
    proposedSet->mPreviousLedgerHash = mLastClosedLedger.hash;
    recvTxSet(proposedSet);

    uint64_t slotIndex = mLastClosedLedger.ledgerSeq + 1;

    // We pick as next close time the current time unless it's before the last
    // close time. We don't know how much time it will take to reach consensus
    // so this is the most appropriate value to use as closeTime.
    uint64_t nextCloseTime = VirtualClock::pointToTimeT(mLastTrigger);
    if (nextCloseTime <= mLastClosedLedger.closeTime)
    {
        nextCloseTime = mLastClosedLedger.closeTime + 1;
    }

    StellarBallot b;
    b.value.txSetHash = proposedSet->getContentsHash();
    b.value.closeTime = nextCloseTime;
    b.value.baseFee = mApp.getConfig().DESIRED_BASE_FEE;
    signStellarBallot(b);

    mCurrentValue = xdr::xdr_to_opaque(b);

    uint256 valueHash = sha256(xdr::xdr_to_msg(mCurrentValue));
    CLOG(DEBUG, "Herder") << "Herder::triggerNextLedger"
        << "@" << binToHex(getLocalNodeID()).substr(0,6)
        << " txSet.size: " << proposedSet->mTransactions.size()
        << " previousLedgerHash: " 
        << binToHex(proposedSet->mPreviousLedgerHash).substr(0,6)
        << " value: " << binToHex(valueHash).substr(0,6);

    // We prepare that value. If we're king, the ballot will be validated, and
    // if we're not it'll just get ignored.
    prepareValue(slotIndex, mCurrentValue);

    for (auto p : mFutureEnvelopes[slotIndex])
    {
        recvFBAEnvelope(p.first, p.second);
    }
    mFutureEnvelopes.erase(slotIndex);
}

void
Herder::expireBallot(const asio::error_code& error,
                     const uint64& slotIndex,
                     const FBABallot& ballot)
                     
{
    // The timer was simply cancelled, nothing to do.
    if (error == asio::error::operation_aborted)
    {
        return;
    }

    assert(slotIndex == mLastClosedLedger.ledgerSeq + 1);

    // We prepare the value while bumping the ballot counter. If we're king,
    // this prepare will go through. If not, we will have bumped our ballot.
    prepareValue(slotIndex, mCurrentValue, true);
}

void 
Herder::signStellarBallot(StellarBallot& b)
{
    b.nodeID = getSecretKey().getPublicKey();
    b.signature = getSecretKey().sign(xdr::xdr_to_msg(b.value));
}

bool 
Herder::verifyStellarBallot(const StellarBallot& b)
{
    return PublicKey::verifySig(b.nodeID, b.signature, 
                                xdr::xdr_to_msg(b.value));
}

}