// Copyright (c) 2022 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "amount.h"
#include "base58.h"
#include "config.h"
#include "core_io.h"
#include "dstencode.h"
#include "keystore.h"
#include "miner_id/miner_info_tracker.h"
#include "mining/journal_change_set.h"
#include "primitives/transaction.h"
#include "rpc/server.h"
#include "script/instruction_iterator.h"
#include "script/script.h"
#include "script/script_num.h"
#include "script/sign.h"
#include "txdb.h"
#include "txmempool.h"
#include "util.h"
#include "utilstrencodings.h"
#include "validation.h"
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <univalue.h>
#include <vector>

namespace mining {

static const fs::path fundingPath = fs::path("miner_id") / "Funding";
static const std::string fundingKeyFile = ".minerinfotxsigningkey.dat";
static const std::string fundingSeedFile = "minerinfotxfunding.dat";

static CKey privKeyFromStringBIP32(std::string strkey, bool isCompressed)
{
    // parse the BIP32 key and convert it to ECDSA format.
    CKey key;
    CBitcoinExtKey bip32ExtPrivKey {strkey};
    CExtKey newKey = bip32ExtPrivKey.GetKey();
    key.Set(newKey.key.begin(), newKey.key.end(), isCompressed);
    return key;
}

auto ReadFileToUniValue (fs::path const & path, std::string filename) -> UniValue {
    auto dir = (GetDataDir() / path);
    auto filepath = dir / filename;

    if (!fs::exists(dir))
        throw std::runtime_error("funding directory does not exist: " + dir.string());

    if (!fs::exists(filepath))
        throw std::runtime_error("funding data file does not exist: " + filepath.string());
    std::ifstream file;
    file.open(filepath.string(), std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw std::runtime_error("Cannot open funding data file: " + filepath.string());

    std::streamsize const size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size))
        throw std::runtime_error("Cannot read funding data from file: " + filepath.string());
    file.close();

    UniValue uv;
    uv.read(&(buffer.front()), size);
    return uv;
};

void WriteUniValueToFile (fs::path const & path, std::string filename, UniValue const & uv) {
    auto dir = (GetDataDir() / path);
    auto filepath = dir / filename;

    if (!fs::exists(dir))
        fs::create_directory(dir);
    std::ofstream file;
    file.open(filepath.string(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        throw std::runtime_error("Cannot open and truncate funding data file: " + filepath.string());

    file << uv.write(1,3);
}

class MinerInfoFunding {
    class FundingKey
    {
        CKey privKey{};
        CTxDestination destination{};
    public:
        FundingKey(bool fCompressed, std::string privKey, std::string destination, Config const & config)
                : privKey(privKeyFromStringBIP32(privKey, true))
                , destination(DecodeDestination(destination, config.GetChainParams()))
        {
        }
        CKey const & getPrivKey() const {return privKey;};
        CTxDestination const & getDestination() const {return destination;};
    };
    COutPoint fundingSeed; // Funding for the first minerinfo-txn of this miner
    FundingKey fundingKey; // Keys needed to spend the funding seed and also the minerinfo-txns
    MinerInfoFunding (COutPoint const & fundingSeed, std::string privateKey, std::string destination, Config const & config)
        : fundingSeed{fundingSeed}
        , fundingKey{FundingKey(true, privateKey, destination, config)}
    {
    }
public:
    MinerInfoFunding () = delete;
    MinerInfoFunding (MinerInfoFunding const &) = delete;
    MinerInfoFunding (MinerInfoFunding &&) = delete;
    MinerInfoFunding const & operator = (MinerInfoFunding const &) = delete;
    MinerInfoFunding const & operator = (MinerInfoFunding &&) = delete;

    static const MinerInfoFunding CreateFromFile (Config const & config, const fs::path & path, std::string keyFile, std::string seedFile)
    {
        // read funding info from json formatted files
        UniValue fundingSeed = ReadFileToUniValue (path, seedFile);
        UniValue fundingKey = ReadFileToUniValue (path, keyFile);

        RPCTypeCheckObj(
                fundingKey,
                {
                        {"fundingKey", UniValueType(UniValue::VOBJ )},
                }, false, false);
        RPCTypeCheckObj(
                fundingKey["fundingKey"],
                {
                        {"privateBIP32", UniValueType(UniValue::VSTR )},
                }, false, false);


        // check file format
        RPCTypeCheckObj(
                fundingSeed,
                {
                        {"fundingDestination", UniValueType(UniValue::VOBJ )},
                        {"firstFundingOutpoint", UniValueType(UniValue::VOBJ )},
                }, false, false);
        RPCTypeCheckObj(
                fundingSeed["fundingDestination"],
                {
                        {"addressBase58", UniValueType(UniValue::VSTR )}
                }, false, false);
        RPCTypeCheckObj(
                fundingSeed["firstFundingOutpoint"],
                {
                        {"txid", UniValueType(UniValue::VSTR )},
                        {"n", UniValueType(UniValue::VNUM )},
                }, false, false);

        // Create and return the MinerInfoFunding object
        UniValue const keys = fundingKey["fundingKey"];
        UniValue const destination = fundingSeed["fundingDestination"];
        UniValue const outpoint = fundingSeed["firstFundingOutpoint"];

        std::string const sPrivKey = keys["privateBIP32"].get_str();
        std::string const sDestination = destination["addressBase58"].get_str();
        std::string const sFundingSeedId = outpoint["txid"].get_str();
        uint32_t fundingSeedIndex = outpoint["n"].get_int();

        auto fundingOutPoint = COutPoint {uint256S(sFundingSeedId), fundingSeedIndex};

        return MinerInfoFunding(fundingOutPoint, sPrivKey, sDestination, config);
    }

    COutPoint FundAndSignMinerInfoTx (const Config &config, CMutableTransaction & mtx, const CTransactionRef previousTx = nullptr)
    {
        // chose the funding seed for the first minerinfo-txn of this mioner or
        // otherwise the previous minerinfo-txn of this miner
        auto ChooseFundingOutpoint = [](CTransactionRef previousTx, COutPoint fundingSeed) -> COutPoint {

            auto IsSpendable = [](COutPoint const & outpoint) {
                CoinsDBView const tipView{ *pcoinsTip };
                CCoinsViewMemPool const mempoolView {tipView, mempool};
                CCoinsViewCache const view{mempoolView};
                auto const coin = view.GetCoinWithScript(outpoint);
                return (coin.has_value() && !coin->IsSpent());
            };

            // first check if the funding seed is spent. If not, then use
            // that as funding
            if (IsSpendable(fundingSeed))
                return fundingSeed;

            // if the funding is already spent, try to spend the previous
            // minerinfo-txn
            if (previousTx) {
                for (uint32_t i = 0; i < previousTx->vout.size(); ++i) {
                    CTxOut const & outpoint = previousTx->vout[i];
                    if (outpoint.nValue > Amount{0}) {
                        COutPoint funds {previousTx->GetId(), i};
                        if (IsSpendable(funds))
                            return funds;
                    }
                }
                throw std::runtime_error(strprintf("Could not use previous minerinfo-txn to fund next: %s",
                                                   previousTx->GetId().ToString()));
            }
            throw std::runtime_error("Cannot find spendable funding transaction");
        };
        const COutPoint fundingOutPoint = ChooseFundingOutpoint(previousTx, fundingSeed);

        // find the funding transaction outputs
        CoinsDBView const tipView{ *pcoinsTip };
        CCoinsViewMemPool const mempoolView {tipView, mempool};
        CCoinsViewCache const view{mempoolView};
        auto const coin = view.GetCoinWithScript(fundingOutPoint);
        if (!coin.has_value() || coin->IsSpent()) {
            throw std::runtime_error("Cannot find funding UTXO's");
        }

        const CScript &prevPubKey = coin->GetTxOut().scriptPubKey;
        const Amount fundingAmount = coin->GetTxOut().nValue;

        // sign the new mininginfo-txn with the funding keys
        SignatureData sigdata;
        CBasicKeyStore keystore;
        SigHashType sigHash;
        CScript const & scriptPubKey = GetScriptForDestination(fundingKey.getDestination()); //p2pkh script

        mtx.vout.push_back(CTxOut {Amount{fundingAmount}, scriptPubKey});
        mtx.vin.push_back(CTxIn{fundingOutPoint, CTxIn::SEQUENCE_FINAL});

        keystore.AddKeyPubKey(fundingKey.getPrivKey(), fundingKey.getPrivKey().GetPubKey());
        ProduceSignature(config, true, MutableTransactionSignatureCreator(
                                 &keystore, &mtx, 0, fundingAmount, sigHash.withForkId()),
                         true, true, prevPubKey, sigdata);
        UpdateTransaction(mtx, 0, sigdata); // funding transactions only have one input
        return fundingOutPoint;
    }
};

std::string CreateReplaceMinerinfotx(const Config &config, const CScript & scriptPubKey, bool overridetx)
{
    // We need to lock because we need to ensure there is only
    // one such minerid info document transaction
    static std::mutex mut;
    std::lock_guard lock{mut};

    auto blockHeight = chainActive.Height() + 1;
    auto prevBlockHash = chainActive.Tip()->GetBlockHash();

    auto GetCachedMinerInfoTx = [](int32_t blockHeight, uint256 const & prevBlockHash, bool overridetx, CScript const & scriptPubKey) -> CTransactionRef {
        try {
            auto const current = mempool.minerInfoTxTracker.current_txid();//minerInfoTxTracker.find(blockHeight, prevBlockHash);
            if (current) {
                CTransactionRef tx = mempool.Get(*current);
                if (!tx)
                    return nullptr;

                // if we do not override, we return what we have
                if (!overridetx)
                    return  tx;

                // if we do override with no change at all we are also done
                if(tx->vout[0].scriptPubKey == scriptPubKey)
                    return  tx;

                // If we get here, we override, hence we must remove the previously created tx
                TxId toRemove = tx->GetId();
                LogPrint(BCLog::MINERID, "minerinfotx tracker, scheduled removal of minerinfo txn %s because attempting to override\n", toRemove.ToString());
                tx.reset();
                CJournalChangeSetPtr changeSet { mempool.getJournalBuilder().getNewChangeSet(JournalUpdateReason::REMOVE_TXN) };
                mempool.RemoveMinerIdTx(toRemove, changeSet);
                changeSet->apply();
                mempool.minerInfoTxTracker.clear_current_txid();
                return nullptr;
            }
        } catch (std::exception const & e) {
            throw JSONRPCError(RPC_DATABASE_ERROR, "rpc CreateReplaceMinerinfotx - minerinfo tx tracking error: " + std::string(e.what()));
        } catch (...) {
            throw JSONRPCError(RPC_DATABASE_ERROR, "rpc CreateReplaceMinerinfotx - unknown minerinfo tx tracking error");
        }
        return nullptr;
    };
    
    // If such a transaction already exists in the mempool, then it is the one we need and return
    // unless we want to override
    CTransactionRef trackedTransaction = GetCachedMinerInfoTx (blockHeight, prevBlockHash, overridetx, scriptPubKey);
    if (trackedTransaction)
        return trackedTransaction->GetId().ToString();

    // we need to remove the transactions we override

    auto ExtractMinerInfoDoc = [](CScript const & scriptPubKey) -> UniValue {

        constexpr std::array<uint8_t, 4> protocolPrefixId {0x60, 0x1d, 0xfa, 0xce};
        constexpr std::array<uint8_t, 1> protocolIdVersion {0x00};
        auto const scriptTemplate =  CScript() << OP_FALSE << OP_RETURN << protocolPrefixId << protocolIdVersion;

        // check if the beginning of the scriptPubKey matches the above script template.
        bsv::instruction_iterator its = scriptPubKey.begin_instructions();
        bsv::instruction_iterator itt = scriptTemplate.begin_instructions();

        while (itt != scriptTemplate.end_instructions())
        {
            if(!its || *its != *itt)
                throw std::runtime_error(strprintf(
                        "failed to extract miner info document from scriptPubKey, expected:[%s] got:[%s]", *itt, *its));
            ++its; ++itt;
        }

        UniValue minerInfoJson;

        // check formatting of the minerinfo document
        try {
            const std::string_view minerInfoStr = bsv::to_sv(its->operand());
            minerInfoJson.read(minerInfoStr.data(), minerInfoStr.size());
            RPCTypeCheckObj(
                    minerInfoJson,
                    {
                            {"version",             UniValueType(UniValue::VSTR )},
                            {"height",              UniValueType(UniValue::VNUM )},
                            {"prevMinerId",         UniValueType(UniValue::VSTR )},
                            {"prevMinerIdSig",      UniValueType(UniValue::VSTR )},
                            {"minerId",             UniValueType(UniValue::VSTR )},
                            {"prevRevocationKey",   UniValueType(UniValue::VSTR )},
                            {"prevRevocationKeySig",UniValueType(UniValue::VSTR )},
                            {"revocationKey",       UniValueType(UniValue::VSTR )},
                            {"revocationMessage",   UniValueType(UniValue::VOBJ )},
                            {"revocationMessageSig",UniValueType(UniValue::VOBJ )},
                    }, true, false);
            if (minerInfoJson.exists("revocationMessage"))
                RPCTypeCheckObj(
                        minerInfoJson["revocationMessage"],
                        {
                                {"compromised_minerId", UniValueType(UniValue::VSTR )},
                        }, true, false);
            if (minerInfoJson.exists("revocationMessageSig"))
                RPCTypeCheckObj(
                        minerInfoJson["revocationMessageSig"],
                        {
                                {"sig1", UniValueType(UniValue::VSTR )},
                                {"sig2", UniValueType(UniValue::VSTR )},
                        }, true, false);

        } catch (UniValue const & e) {
            throw JSONRPCError(RPC_PARSE_ERROR, std::string("Could not read miner info document: ") + e["message"].get_str());
        } catch (std::exception const & e) {
            throw std::runtime_error("Could not read miner info document: " + std::string(e.what()));
        }
        // return the minerinfo document
        return minerInfoJson;
    };

    // find previous funding transaction
    auto GetPreviousMinerInfoTx = [&config](int32_t blockHeight) -> CTransactionRef {
        auto SearchForFundingTx = [&blockHeight,&config](int32_t height, TxId const & txid) -> CTransactionRef {
            CTransactionRef tx;
            uint256 hashBlock;
            bool isGenesisEnabled = true;
            bool allowSlow = true;
            if (height < blockHeight && GetTransaction(config, txid, tx, allowSlow, hashBlock, isGenesisEnabled))
                return tx;
            else
                return nullptr;
        };

        auto tracker = mempool.minerInfoTxTracker.CreateLockingAccess();
        return tracker.find_latest<decltype(SearchForFundingTx), CTransactionRef>(SearchForFundingTx);
    };

    // Extract information from the Miner Info document which is embedded in the data part of the scriptPubKey
    UniValue const minerInfoJson = ExtractMinerInfoDoc(scriptPubKey);
    int32_t const docHeight =  minerInfoJson["height"].get_int();

    if (docHeight != blockHeight) {
        throw std::runtime_error("Block height must be the active chain height plus 1");
    }

   // create and fund minerinfo txn
    CMutableTransaction mtx;
    mtx.vout.push_back(CTxOut{Amount{0}, scriptPubKey});

    COutPoint funds;
    try {
        CTransactionRef prevInfoTx = GetPreviousMinerInfoTx(blockHeight);
        auto funding = MinerInfoFunding::CreateFromFile(config, fundingPath, fundingKeyFile, fundingSeedFile);
        funds = funding.FundAndSignMinerInfoTx (config, mtx, prevInfoTx);
    } catch (UniValue const & e) {
        throw std::runtime_error("Could not fund minerinfo transaction: " + e["message"].get_str());
    } catch (std::exception const & e) {
        throw std::runtime_error("Could not fund minerinfo transaction: " + std::string(e.what()));
    }

    std::string const mtxhex {EncodeHexTx(CTransaction(mtx))};
    UniValue minerinfotx_args(UniValue::VARR);
    minerinfotx_args.push_back(mtxhex);
    minerinfotx_args.push_back(UniValue(false));
    minerinfotx_args.push_back(UniValue(true)); // do not check, we want to allow no fees
    TxId const txid =  mtx.GetId();

    mempool.minerInfoTxTracker.set_current_txid(txid);
    UniValue const r = CallRPC("sendrawtransaction", minerinfotx_args);
    LogPrint(BCLog::MINERID, "minerinfotx tracker, sent minerinfo txn %s to mempool at height %d. Funding with %s\n",
             txid.ToString(), blockHeight, funds.ToString());


    if (r.exists("error")) {
        if (!r["error"].isNull()) {
            mempool.minerInfoTxTracker.clear_current_txid();
            throw JSONRPCError(RPC_TRANSACTION_ERROR, "Could not create minerinfo transaction. " + r["error"]["message"].get_str());
        }
    }

    // check that no new block has been added to the tip in the meantime.
    int32_t blockHeight2 = chainActive.Height() + 1;
    if (blockHeight != blockHeight2) {
        throw std::runtime_error("A block was added to the tip while a mineridinfo-tx was created. Currrent height: " + std::to_string(blockHeight2));
    }

    const std::string txid_as_string = txid.ToString();
    LogPrint(BCLog::MINERID, "A mineridinfo-txn %s has been created at height %d\n", txid_as_string, blockHeight);
    return txid_as_string;
}

static UniValue createminerinfotx(const Config &config, const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1) {
        throw std::runtime_error(
                "createminerinfotx \"scriptPubKey\"\n"
                "\nCreate a transaction with a miner info document and return it's transaction id\n"
                "\nIf such a miner info document exists already, then return it's transaction id instead.\n"
                "\nArguments:\n"
                "1. \"scriptPubKey:\" (hex string mandatory) OP_FALSE OP_RETURN 0x601DFACE 0x00 minerinfo  \n"
                "where minerinfo contains the following json data in hex encoding"
                "{\n"
                "  \"MinerInfoDoc\":hex,      The minerid document in hex representation\n"
                "  \"MinerInfoDocSig\":hex    (hex string, required) The sequence\n"
                "}\n"
                "\nResult: a hex encoded transaction id\n"
                "\nExamples:\n" +
                HelpExampleCli("createminerinfotx", "\"006a04601dface01004dba027b22...\"") +
                HelpExampleRpc("createminerinfotx", "\"006a04601dface01004dba027b22...\""));
    }

    RPCTypeCheck(request.params,{UniValue::VSTR}, false);
    std::string const scriptPubKeyHex = request.params[0].get_str();
    std::vector<uint8_t> script = ParseHex(scriptPubKeyHex);
    const auto scriptPubKey = CScript {script.begin(), script.end()};

    bool overridetx = false;
    return CreateReplaceMinerinfotx(config, scriptPubKey, overridetx);
}

static UniValue replaceminerinfotx(const Config &config, const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1) {
        throw std::runtime_error(
                "replaceminerinfotx \"scriptPubKey\"\n"
                "\nCreate or replace a transaction with a miner info document and return it's transaction id\n"
                "\nArguments:\n"
                "1. \"scriptPubKey:\" (hex string mandatory) OP_FALSE OP_RETURN 0x601DFACE 0x00 minerinfo  \n"
                "where minerinfo contains the following json data in hex encoding"
                "{\n"
                "  \"MinerInfoDoc\":hex,      The minerid document in hex representation\n"
                "  \"MinerInfoDocSig\":hex    (hex string, required) The sequence\n"
                "}\n"
                "\nResult: a hex encoded transaction id\n"
                "\nExamples:\n" +
                HelpExampleCli("replaceminerinfotx", "\"006a04601dface01004dba027b22...\"") +
                HelpExampleRpc("replaceminerinfotx", "\"006a04601dface01004dba027b22...\""));
    }

    RPCTypeCheck(request.params,{UniValue::VSTR}, false);
    std::string const scriptPubKeyHex = request.params[0].get_str();
    std::vector<uint8_t> script = ParseHex(scriptPubKeyHex);
    const auto scriptPubKey = CScript {script.begin(), script.end()};

    bool overridetx = true;
    return CreateReplaceMinerinfotx(config, scriptPubKey, overridetx);
}

static UniValue getminerinfotxid(const Config &config, const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 0) {
        throw std::runtime_error(
                "getminerinfotxid  \n"
                "\nreturn the minerinfotx for the current block being built.\n"
                "\nResult: a hex encoded transaction id\n"
                "\nExamples:\n" +
                HelpExampleCli("getminerinfotxid","") +
                HelpExampleRpc("getminerinfotxid",""));
    }

    std::optional<TxId> const info_txid = mempool.minerInfoTxTracker.current_txid();
    if (info_txid) {
         return UniValue(info_txid->ToString());
    }
    return UniValue(UniValue::VNULL);
}

static UniValue makeminerinfotxsigningkey(const Config &config, const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 0) {
        throw std::runtime_error(
                "makeminerinfotxsigningkey  \n"
                "\ncreates a private BIP32 Key and stores it in ./miner_id/Funding/.minerinfotxsigningkey.dat\n"
                "\nExamples:\n" +
                HelpExampleCli("makeminerinfotxsigningkey","") +
                HelpExampleRpc("makeminerinfotxsigningkey",""));
    }

    // store the key
    CKey privKey;
    bool compressed = true;

    if (gArgs.GetBoolArg("-regtest", false)) {
        std::vector<uint8_t> vchKey =
                {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

        privKey.Set(vchKey.begin(), vchKey.end(), compressed);
    } else {
        privKey.MakeNewKey(compressed);
    }

    CExtKey masterKey {};
    masterKey.SetMaster(privKey.begin(), privKey.size());
    CBitcoinExtKey bip32key;
    bip32key.SetKey(masterKey);

    privKey = bip32key.GetKey().key;
    CPubKey pubKey = privKey.GetPubKey();

    UniValue uniBip32 (UniValue::VOBJ);
    uniBip32.pushKV("privateBIP32", bip32key.ToString());

    UniValue uniKey(UniValue::VOBJ);
    uniKey.pushKV("fundingKey", uniBip32);

    WriteUniValueToFile(fundingPath, fundingKeyFile, uniKey);

    // store the address
    CTxDestination destination = pubKey.GetID();

    UniValue uniBase58(UniValue::VOBJ);
    std::string base58 = EncodeDestination(destination, config);
    uniBase58.pushKV("addressBase58", base58);

    UniValue uniDestination(UniValue::VOBJ);
    uniDestination.pushKV("fundingDestination", uniBase58);

    WriteUniValueToFile(fundingPath, fundingSeedFile, uniDestination);

    return {};
}

static UniValue getminerinfotxfundingaddress(const Config &config, const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 0) {
        throw std::runtime_error(
                "getminerinfotxfundingaddress  \n"
                "\nExamples:\n" +
                HelpExampleCli("getminerinfotxfundingaddress","") +
                HelpExampleRpc("getminerinfotxfundingaddress",""));
    }
    UniValue destination = ReadFileToUniValue (fundingPath, fundingSeedFile);
    RPCTypeCheck(destination,{UniValue::VOBJ}, false);
    RPCTypeCheckObj(
            destination,
            {
                    {"fundingDestination", UniValueType(UniValue::VOBJ )},
            }, false, false);
    RPCTypeCheckObj(
            destination["fundingDestination"],
            {
                    {"addressBase58", UniValueType(UniValue::VSTR )},
            }, false, false);

    return destination["fundingDestination"]["addressBase58"].get_str();
}

static UniValue setminerinfotxfundingoutpoint(const Config &config, const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
                "setminerinfotxfundingoutpoint \"txid\" \"n\"\n"
                "\nsend the output used to fund the minerinfo transactions\n"
                "\nArguments:\n"
                "1. \"txid:\" (hex string mandatory) a transaction that can be spend using the \n"
                "key created by rpc function makeminerinfotxspendingkey"
                "2. \"n:\" (int) the output to spend \n"
                "\nExamples:\n" +
                HelpExampleCli("setminerinfotxfundingoutpoint", "\"txid\", n") +
                HelpExampleRpc("setminerinfotxfundingoutpoint", "\"txid\", n"));
    }
    // Read rpc parameters
    RPCTypeCheck(request.params,{UniValue::VSTR, UniValue::VNUM}, false);
    auto txid = request.params[0].get_str();
    auto n = request.params[1].get_int();
    UniValue outPoint(UniValue::VOBJ);
    outPoint.pushKV("txid", txid);
    outPoint.pushKV("n", n);

    // Read funding configuration file and set or replace the funding output
    UniValue fundingSeed = ReadFileToUniValue (fundingPath, fundingSeedFile);

    UniValue result {UniValue::VOBJ};
    result.pushKV ("fundingDestination", fundingSeed["fundingDestination"]);
    result.pushKV ("firstFundingOutpoint", outPoint);
    WriteUniValueToFile(fundingPath, fundingSeedFile, result);
    return {};
}


} // namespace mining

// clang-format off
static const CRPCCommand commands[] = {
    //  category   name                     actor (function)       okSafeMode
    //  ---------- ------------------------ ---------------------- ----------
    {"generating", "createminerinfotx",                  mining::createminerinfotx,                  true, {"minerinfo"}},
    {"generating", "replaceminerinfotx",                 mining::replaceminerinfotx,                 true, {"minerinfo"}},
    {"generating", "getminerinfotxid",                   mining::getminerinfotxid,                   true, {"minerinfo"}},
    {"generating", "makeminerinfotxsigningkey",          mining::makeminerinfotxsigningkey,          true, {"minerinfo"}},
    {"generating", "getminerinfotxfundingaddress",       mining::getminerinfotxfundingaddress,       true, {"minerinfo"}},
    {"generating", "setminerinfotxfundingoutpoint",      mining::setminerinfotxfundingoutpoint,      true, {"minerinfo"}},
};
// clang-format on

void RegisterMinerIdRPCCommands(CRPCTable &t) {
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
