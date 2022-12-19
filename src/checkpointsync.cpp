// Copyright (c) 2012-2013 PPCoin developers
// Copyright (c) 2014-2017 ArtByte Developers
// Distributed under conditional MIT/X11 software license,
// see the accompanying file COPYING
//
// The synchronized checkpoint system is first developed by Sunny King for
// ppcoin network in 2012, giving cryptocurrency developers a tool to gain
// additional network protection against 51% attack.
//
// Primecoin also adopts this security mechanism, and the enforcement of
// checkpoints is explicitly granted by user, thus granting only temporary
// consensual central control to developer at the threats of 51% attack.
//
// Concepts
//
// In the network there can be a privileged node known as 'checkpoint master'.
// This node can send out checkpoint messages signed by the checkpoint master
// key. Each checkpoint is a block hash, representing a block on the blockchain
// that the network should reach consensus on.
//
// Besides verifying signatures of checkpoint messages, each node also verifies
// the consistency of the checkpoints. If a conflicting checkpoint is received,
// it means either the checkpoint master key is compromised, or there is an
// operator mistake. In this situation the node would discard the conflicting
// checkpoint message and display a warning message. This precaution controls
// the damage to network caused by operator mistake or compromised key.
//
// Operations
//
// Any node can be turned into checkpoint master by setting the 'checkpointkey'
// configuration parameter with the private key of the checkpoint master key.
// Operator should exercise caution such that at any moment there is at most
// one node operating as checkpoint master. When switching master node, the
// recommended procedure is to shutdown the master node and restart as
// regular node, note down the current checkpoint by 'getcheckpoint', then
// compare to the checkpoint at the new node to be upgraded to master node.
// When the checkpoint on both nodes match then it is safe to switch the new
// node to checkpoint master.
//
// The configuration parameter 'checkpointdepth' specifies how many blocks
// should the checkpoints lag behind the latest block in auto checkpoint mode.
// A depth of 5 is the minimum auto checkpoint policy and offers the greatest
// protection against 51% attack. A negative depth means that the checkpoints
// should not be automatically generated by the checkpoint master, but instead
// be manually entered by operator via the 'sendcheckpoint' command. The manual
// mode is also the default mode (default value -1 for checkpointdepth).
//

#include <chainparams.h>
#include <checkpointsync.h>

#include <key.h>
#include <key_io.h>
#include <txdb.h>
#include <uint256.h>
#include <txmempool.h>
#include <consensus/validation.h>
#include <consensus/consensus.h>
#include <validation.h>

#include <univalue.h>

using namespace std;

// Synchronized checkpoint (centrally broadcasted)
string CSyncCheckpoint::strMasterPrivKey = "";
uint256 hashSyncCheckpoint = ArithToUint256(arith_uint256(0));
uint256 hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
CSyncCheckpoint checkpointMessage;
CSyncCheckpoint checkpointMessagePending;
uint256 hashInvalidCheckpoint = ArithToUint256(arith_uint256(0));
RecursiveMutex cs_hashSyncCheckpoint;
string strCheckpointWarning;

// Only descendant of current sync-checkpoint is allowed
bool ValidateSyncCheckpoint(uint256 hashCheckpoint, Chainstate& chainState)
{
    if (!chainState.m_blockman.m_block_index.count(hashSyncCheckpoint))
        return error("%s: block index missing for current sync-checkpoint %s", __func__, hashSyncCheckpoint.ToString());
    if (!chainState.m_blockman.m_block_index.count(hashCheckpoint))
        return error("%s: block index missing for received sync-checkpoint %s", __func__, hashCheckpoint.ToString());

    CBlockIndex* pindexSyncCheckpoint = &chainState.m_blockman.m_block_index[hashSyncCheckpoint];
    CBlockIndex* pindexCheckpointRecv = &chainState.m_blockman.m_block_index[hashCheckpoint];

    if (pindexCheckpointRecv->nHeight <= pindexSyncCheckpoint->nHeight)
    {
        // Received an older checkpoint, trace back from current checkpoint
        // to the same height of the received checkpoint to verify
        // that current checkpoint should be a descendant block
        if (!chainState.m_chain.Contains(pindexCheckpointRecv))
        {
            hashInvalidCheckpoint = hashCheckpoint;
            return error("%s: new sync-checkpoint %s is conflicting with current sync-checkpoint %s", __func__, hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
        }
        return false; // ignore older checkpoint
    }

    // Received checkpoint should be a descendant block of the current
    // checkpoint. Trace back to the same height of current checkpoint
    // to verify.
    CBlockIndex* pindex = pindexCheckpointRecv;
    while (pindex->nHeight > pindexSyncCheckpoint->nHeight)
        if (!(pindex = pindex->pprev))
            return error("%s: pprev2 null - block index structure failure", __func__);

    if (pindex->GetBlockHash() != hashSyncCheckpoint)
    {
        hashInvalidCheckpoint = hashCheckpoint;
        return error("%s: new sync-checkpoint %s is not a descendant of current sync-checkpoint %s", __func__, hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
    }
    return true;
}

bool WriteSyncCheckpoint(const uint256& hashCheckpoint, Chainstate& chainState)
{
    if (!chainState.m_blockman.m_block_tree_db->WriteSyncCheckpoint(hashCheckpoint))
        return error("%s: failed to write to txdb sync checkpoint %s", __func__, hashCheckpoint.ToString());

    chainState.ForceFlushStateToDisk();
    hashSyncCheckpoint = hashCheckpoint;
    return true;
}

bool AcceptPendingSyncCheckpoint(Chainstate& chainState)
{
    LOCK(cs_hashSyncCheckpoint);
    bool havePendingCheckpoint = hashPendingCheckpoint != ArithToUint256(arith_uint256(0)) && chainState.m_blockman.m_block_index.count(hashPendingCheckpoint);
    if (!havePendingCheckpoint)
        return false;

    if (!ValidateSyncCheckpoint(hashPendingCheckpoint, chainState))
    {
        hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
        checkpointMessagePending.SetNull();
        return false;
    }

    if (!chainState.m_chain.Contains(&chainState.m_blockman.m_block_index[hashPendingCheckpoint]))
        return false;

    if (!WriteSyncCheckpoint(hashPendingCheckpoint, chainState))
        return error("%s: failed to write sync checkpoint %s", __func__, hashPendingCheckpoint.ToString());

    hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
    checkpointMessage = checkpointMessagePending;
    checkpointMessagePending.SetNull();

    // Relay the checkpoint
    if (g_connman && !checkpointMessage.IsNull())
    {
        g_connman->ForEachNode([](CNode* pnode) {
            checkpointMessage.RelayTo(pnode);
        });
    }

    return true;
}

// Automatically select a suitable sync-checkpoint
uint256 AutoSelectSyncCheckpoint(CChain& chain)
{
    // Search backward for a block with specified depth policy
    const CBlockIndex *pindex = chain.Tip();
    while (pindex->pprev && pindex->nHeight + (int)gArgs.GetIntArg("-checkpointdepth", DEFAULT_AUTOCHECKPOINT) > chain.Tip()->nHeight)
        pindex = pindex->pprev;
    return pindex->GetBlockHash();
}

// Check against synchronized checkpoint
bool CheckSyncCheckpoint(const CBlockIndex* pindexNew, Chainstate& chainState)
{    
    LOCK(cs_hashSyncCheckpoint);
    assert(pindexNew != NULL);
    if (pindexNew->nHeight == 0)
        return true;
    const uint256& hashBlock = pindexNew->GetBlockHash();
    int nHeight = pindexNew->nHeight;

    // Checkpoint should always be accepted block
    assert(chainState.m_blockman.m_block_index.count(hashSyncCheckpoint));
    const CBlockIndex* pindexSync = &chainState.m_blockman.m_block_index[hashSyncCheckpoint];
    assert(chainState.m_chain.Contains(pindexSync));

    if (nHeight > pindexSync->nHeight)
    {
        // Trace back to same height as sync-checkpoint
        const CBlockIndex* pindex = pindexNew;
        while (pindex->nHeight > pindexSync->nHeight && !chainState.m_chain.Contains(pindex))
            if (!(pindex = pindex->pprev))
                return error("%s: pprev null - block index structure failure", __func__);

        // At this point we could have:
        // 1. Found block in our blockchain
        // 2. Reached pindexSync->nHeight without finding it
        if (!chainState.m_chain.Contains(pindex))
            return error("%s: Only descendants of checkpoint accepted", __func__);
    }
    if (nHeight == pindexSync->nHeight && hashBlock != hashSyncCheckpoint)
        return error("%s: Same height with sync-checkpoint", __func__);
    if (nHeight < pindexSync->nHeight && !chainState.m_blockman.m_block_index.count(hashBlock))
        return error("%s: Lower height than sync-checkpoint", __func__);
    return true;
}

// Reset synchronized checkpoint to the assume valid block
bool ResetSyncCheckpoint(Chainstate& chainState)
{
    LOCK(cs_hashSyncCheckpoint);

    if (!WriteSyncCheckpoint(Params().GetConsensus().defaultAssumeValid, chainState))
        return error("%s: failed to write sync checkpoint %s", __func__, Params().GetConsensus().defaultAssumeValid.ToString());

    return true;
}

// Verify sync checkpoint master pubkey and reset sync checkpoint if changed
bool CheckCheckpointPubKey(Chainstate& chainState)
{
    string strPubKey = "";
    string strMasterPubKey = Params().GetConsensus().checkpointPubKey;

    if (!chainState.m_blockman.m_block_tree_db->ReadCheckpointPubKey(strPubKey) || strPubKey != strMasterPubKey)
    {
        // write checkpoint master key to db
        if (!ResetSyncCheckpoint(chainState))
            return error("%s: failed to reset sync-checkpoint", __func__);
        if (!chainState.m_blockman.m_block_tree_db->WriteCheckpointPubKey(strMasterPubKey))
            return error("%s: failed to write new checkpoint master key to db", __func__);
        chainState.ForceFlushStateToDisk();
    }

    return true;
}

bool SetCheckpointPrivKey(string strPrivKey)
{
    CKey key = DecodeSecret(strPrivKey);
    if (!key.IsValid())
        return false;

    CSyncCheckpoint::strMasterPrivKey = strPrivKey;
    return true;
}

bool SendSyncCheckpoint(uint256 hashCheckpoint, CConnman* connman, ChainstateManager& chainman)
{
    // P2P disabled
    if (!connman)
        return true;

    // No connections
    if (connman->GetNodeCount(ConnectionDirection::Both) == 0)
        return true;

    // Do not send dummy checkpoint
    if (hashCheckpoint == uint256())
        return true;

    CSyncCheckpoint checkpoint;
    checkpoint.hashCheckpoint = hashCheckpoint;
    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedSyncCheckpoint)checkpoint;
    checkpoint.vchMsg = vector<unsigned char>((unsigned char*)&*sMsg.begin(), (unsigned char*)&*sMsg.end());

    if (CSyncCheckpoint::strMasterPrivKey.empty())
        return error("%s: Checkpoint master key unavailable.", __func__);

    CKey key = DecodeSecret(CSyncCheckpoint::strMasterPrivKey);
    if (!key.IsValid())
        return error("%s: Checkpoint master key invalid", __func__);

    if (!key.Sign(Hash(checkpoint.vchMsg), checkpoint.vchSig))
        return error("%s: Unable to sign checkpoint, check private key?", __func__);

    if(!checkpoint.ProcessSyncCheckpoint(chainman))
        return error("%s: Failed to process checkpoint.", __func__);

    // Relay checkpoint
    connman->ForEachNode([checkpoint](CNode* pnode) {
        checkpoint.RelayTo(pnode);
    });

    return true;
}

// Verify signature of sync-checkpoint message
bool CSyncCheckpoint::CheckSignature()
{
    string strMasterPubKey = Params().GetConsensus().checkpointPubKey;
    CPubKey key(ParseHex(strMasterPubKey));
    if (!key.Verify(Hash(vchMsg), vchSig))
        return error("%s: verify signature failed", __func__);

    // Now unserialize the data
    CDataStream sMsg(vchMsg, SER_NETWORK, PROTOCOL_VERSION);
    sMsg >> *(CUnsignedSyncCheckpoint*)this;
    return true;
}

// Process synchronized checkpoint
bool CSyncCheckpoint::ProcessSyncCheckpoint(ChainstateManager& chainman)
{
    if (!CheckSignature())
        return false;

    LOCK(cs_hashSyncCheckpoint);
    if (!chainman.BlockIndex().count(hashCheckpoint))
    {
        LogPrintf("%s: Missing headers for received sync-checkpoint %s\n", __func__, hashCheckpoint.ToString());
        return false;
    }

    if (!ValidateSyncCheckpoint(hashCheckpoint, chainman.ActiveChainstate()))
        return false;

    bool pass = chainman.ActiveChain().Contains(&chainman.BlockIndex()[hashCheckpoint]);

    if (!pass)
    {
        // We haven't received the checkpoint chain, keep the checkpoint as pending
        hashPendingCheckpoint = hashCheckpoint;
        checkpointMessagePending = *this;
        LogPrintf("%s: pending for sync-checkpoint %s\n", __func__, hashCheckpoint.ToString());

        return false;
    }

    if (!WriteSyncCheckpoint(hashCheckpoint, chainman.ActiveChainstate()))
        return error("%s: failed to write sync checkpoint %s\n", __func__, hashCheckpoint.ToString());

    checkpointMessage = *this;
    hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
    checkpointMessagePending.SetNull();

    return true;
}
