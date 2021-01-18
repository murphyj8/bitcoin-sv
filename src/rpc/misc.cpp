// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "rpc/misc.h"
#include "base58.h"
#include "clientversion.h"
#include "config.h"
#include "dstencode.h"
#include "init.h"
#include "net/net.h"
#include "net/netbase.h"
#include "policy/policy.h"
#include "rpc/blockchain.h"
#include "rpc/server.h"
#include "timedata.h"
#include "util.h"
#include "utilstrencodings.h"
#include "validation.h"

#ifdef ENABLE_WALLET
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#include "vmtouch.h"
#include <univalue.h>
#include <cstdint>

/**
 * @note Do not add or change anything in the information returned by this
 * method. `getinfo` exists for backwards-compatibility only. It combines
 * information from wildly different sources in the program, which is a mess,
 * and is thus planned to be deprecated eventually.
 *
 * Based on the source of the information, new information should be added to:
 * - `getblockchaininfo`,
 * - `getnetworkinfo` or
 * - `getwalletinfo`
 *
 * Or alternatively, create a specific query method for the information.
 **/
static UniValue getinfo(const Config &config, const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "getinfo\n"
            "\nDEPRECATED. Returns an object containing various state info.\n"
            "\nResult:\n"
            "{\n"
            "  \"version\": xxxxx,           (numeric) the server version\n"
            "  \"protocolversion\": xxxxx,   (numeric) the protocol version\n"
            "  \"walletversion\": xxxxx,     (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,         (numeric) the total bitcoin "
            "balance of the wallet\n"
            "  \"blocks\": xxxxxx,           (numeric) the current number of "
            "blocks processed in the server\n"
            "  \"timeoffset\": xxxxx,        (numeric) the time offset\n"
            "  \"connections\": xxxxx,       (numeric) the number of "
            "connections\n"
            "  \"proxy\": \"host:port\",       (string, optional) the proxy used "
            "by the server\n"
            "  \"difficulty\": xxxxxx,       (numeric) the current difficulty\n"
            "  \"testnet\": true|false,      (boolean) if the server is using "
            "testnet or not\n"
            "  \"keypoololdest\": xxxxxx,    (numeric) the timestamp (seconds "
            "since Unix epoch) of the oldest pre-generated key in the key "
            "pool\n"
            "  \"keypoolsize\": xxxx,        (numeric) how many new keys are "
            "pre-generated\n"
            "  \"unlocked_until\": ttt,      (numeric) the timestamp in "
            "seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is "
            "unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,         (numeric) the transaction fee set "
            "in " +
            CURRENCY_UNIT + "/kB\n"
                            "  \"relayfee\": x.xxxx,         (numeric) minimum "
                            "relay fee for non-free transactions in " +
            CURRENCY_UNIT +
            "/kB\n"
            "  \"errors\": \"...\",            (string) any error messages\n"
            "  \"maxblocksize\": xxxxx,      (numeric) The absolute maximum block "
            "size we will accept from any source\n"
            "  \"maxminedblocksize\": xxxxx  (numeric) The maximum block size "
            "we will mine\n"
            "  \"maxstackmemoryusagepolicy\": xxxxx, (numeric) Policy value of "
            "max stack memory usage\n"
            "  \"maxStackMemoryUsageConsensus\": xxxxx, (numeric) Consensus value of "
            "max stack memory usage\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getinfo", "") + HelpExampleRpc("getinfo", ""));
    }

#ifdef ENABLE_WALLET
    CWallet *const pwallet = GetWalletForJSONRPCRequest(request);

    LOCK2(cs_main, pwallet ? &pwallet->cs_wallet : nullptr);
#else
    LOCK(cs_main);
#endif

    proxyType proxy;
    GetProxy(NET_IPV4, proxy);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("version", CLIENT_VERSION));
    obj.push_back(Pair("protocolversion", PROTOCOL_VERSION));
#ifdef ENABLE_WALLET
    if (pwallet) {
        obj.push_back(Pair("walletversion", pwallet->GetVersion()));
        obj.push_back(Pair("balance", ValueFromAmount(pwallet->GetBalance())));
    }
#endif
    obj.push_back(Pair("blocks", (int)chainActive.Height()));
    obj.push_back(Pair("timeoffset", GetTimeOffset()));
    if (g_connman) {
        obj.push_back(
            Pair("connections",
                 (int)g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL)));
    }
    obj.push_back(Pair("proxy", (proxy.IsValid() ? proxy.proxy.ToStringIPPort()
                                                 : std::string())));
    obj.push_back(Pair("difficulty", double(GetDifficulty(chainActive.Tip()))));
    obj.push_back(Pair("testnet",
                       config.GetChainParams().NetworkIDString() ==
                           CBaseChainParams::TESTNET));
    obj.push_back(Pair("stn",
                       config.GetChainParams().NetworkIDString() ==
                       CBaseChainParams::STN));
#ifdef ENABLE_WALLET
    if (pwallet) {
        obj.push_back(Pair("keypoololdest", pwallet->GetOldestKeyPoolTime()));
        obj.push_back(Pair("keypoolsize", (int)pwallet->GetKeyPoolSize()));
    }
    if (pwallet && pwallet->IsCrypted()) {
        obj.push_back(Pair("unlocked_until", pwallet->nRelockTime));
    }
    obj.push_back(Pair("paytxfee", ValueFromAmount(payTxFee.GetFeePerK())));
#endif
    obj.push_back(Pair("relayfee",
                       ValueFromAmount(config.GetMinFeePerKB().GetFeePerK())));
    obj.push_back(Pair("errors", GetWarnings("statusbar")));
    obj.push_back(Pair("maxblocksize", config.GetMaxBlockSize()));
    obj.push_back(Pair("maxminedblocksize", config.GetMaxGeneratedBlockSize()));
    obj.push_back(Pair("maxstackmemoryusagepolicy", 
                       config.GetMaxStackMemoryUsage(true, false)));
    obj.push_back(Pair("maxstackmemoryusageconsensus",
                       config.GetMaxStackMemoryUsage(true, true)));
    return obj;
}

#ifdef ENABLE_WALLET
class DescribeAddressVisitor : public boost::static_visitor<UniValue> {
public:
    CWallet *const pwallet;

    DescribeAddressVisitor(CWallet *_pwallet) : pwallet(_pwallet) {}

    UniValue operator()(const CNoDestination &dest) const {
        return UniValue(UniValue::VOBJ);
    }

    UniValue operator()(const CKeyID &keyID) const {
        UniValue obj(UniValue::VOBJ);
        CPubKey vchPubKey;
        obj.push_back(Pair("isscript", false));
        if (pwallet && pwallet->GetPubKey(keyID, vchPubKey)) {
            obj.push_back(Pair("pubkey", HexStr(vchPubKey)));
            obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        }
        return obj;
    }

    UniValue operator()(const CScriptID &scriptID) const {
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        obj.push_back(Pair("isscript", true));
        if (pwallet && pwallet->GetCScript(scriptID, subscript)) {
            std::vector<CTxDestination> addresses;
            txnouttype whichType;
            int nRequired;
            // DescribeAddressVisitor is used by RPC call validateaddress, which only takes address as input. 
            // We have no block height available - treat all transactions as post-Genesis except P2SH to be able to spend them.
            const bool isGenesisEnabled = !IsP2SH(subscript);
            ExtractDestinations(subscript, isGenesisEnabled, whichType, addresses, nRequired);
            obj.push_back(Pair("script", GetTxnOutputType(whichType)));
            obj.push_back(
                Pair("hex", HexStr(subscript.begin(), subscript.end())));
            UniValue a(UniValue::VARR);
            for (const CTxDestination &addr : addresses) {
                a.push_back(EncodeDestination(addr));
            }
            obj.push_back(Pair("addresses", a));
            if (whichType == TX_MULTISIG) {
                obj.push_back(Pair("sigsrequired", nRequired));
            }
        }
        return obj;
    }
};
#endif

static UniValue validateaddress(const Config &config,
                                const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "validateaddress \"address\"\n"
            "\nReturn information about the given bitcoin address.\n"
            "\nArguments:\n"
            "1. \"address\"     (string, required) The bitcoin address to "
            "validate\n"
            "\nResult:\n"
            "{\n"
            "  \"isvalid\" : true|false,       (boolean) If the address is "
            "valid or not. If not, this is the only property returned.\n"
            "  \"address\" : \"address\", (string) The bitcoin address "
            "validated\n"
            "  \"scriptPubKey\" : \"hex\",       (string) The hex encoded "
            "scriptPubKey generated by the address\n"
            "  \"ismine\" : true|false,        (boolean) If the address is "
            "yours or not\n"
            "  \"iswatchonly\" : true|false,   (boolean) If the address is "
            "watchonly\n"
            "  \"isscript\" : true|false,      (boolean) If the key is a "
            "script\n"
            "  \"pubkey\" : \"publickeyhex\",    (string) The hex value of the "
            "raw public key\n"
            "  \"iscompressed\" : true|false,  (boolean) If the address is "
            "compressed\n"
            "  \"account\" : \"account\"         (string) DEPRECATED. The "
            "account associated with the address, \"\" is the default account\n"
            "  \"timestamp\" : timestamp,        (number, optional) The "
            "creation time of the key if available in seconds since epoch (Jan "
            "1 1970 GMT)\n"
            "  \"hdkeypath\" : \"keypath\"       (string, optional) The HD "
            "keypath if the key is HD and available\n"
            "  \"hdmasterkeyid\" : \"<hash160>\" (string, optional) The "
            "Hash160 of the HD master pubkey\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("validateaddress",
                           "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\"") +
            HelpExampleRpc("validateaddress",
                           "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\""));
    }

#ifdef ENABLE_WALLET
    CWallet *const pwallet = GetWalletForJSONRPCRequest(request);

    LOCK2(cs_main, pwallet ? &pwallet->cs_wallet : nullptr);
#else
    LOCK(cs_main);
#endif

    CTxDestination dest =
        DecodeDestination(request.params[0].get_str(), config.GetChainParams());
    bool isValid = IsValidDestination(dest);

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("isvalid", isValid));
    if (isValid) {
        std::string currentAddress = EncodeDestination(dest);
        ret.push_back(Pair("address", currentAddress));

        CScript scriptPubKey = GetScriptForDestination(dest);
        ret.push_back(Pair("scriptPubKey",
                           HexStr(scriptPubKey.begin(), scriptPubKey.end())));

#ifdef ENABLE_WALLET
        isminetype mine = pwallet ? IsMine(*pwallet, dest) : ISMINE_NO;
        ret.push_back(Pair("ismine", (mine & ISMINE_SPENDABLE) ? true : false));
        ret.push_back(
            Pair("iswatchonly", (mine & ISMINE_WATCH_ONLY) ? true : false));
        UniValue detail =
            boost::apply_visitor(DescribeAddressVisitor(pwallet), dest);
        ret.pushKVs(detail);
        if (pwallet && pwallet->mapAddressBook.count(dest)) {
            ret.push_back(Pair("account", pwallet->mapAddressBook[dest].name));
        }
        if (pwallet) {
            const auto &meta = pwallet->mapKeyMetadata;
            const CKeyID *keyID = boost::get<CKeyID>(&dest);
            auto it = keyID ? meta.find(*keyID) : meta.end();
            if (it == meta.end()) {
                it = meta.find(CScriptID(scriptPubKey));
            }
            if (it != meta.end()) {
                ret.push_back(Pair("timestamp", it->second.nCreateTime));
                if (!it->second.hdKeypath.empty()) {
                    ret.push_back(Pair("hdkeypath", it->second.hdKeypath));
                    ret.push_back(Pair("hdmasterkeyid",
                                       it->second.hdMasterKeyID.GetHex()));
                }
            }
        }
#endif
    }
    return ret;
}

// Needed even with !ENABLE_WALLET, to pass (ignored) pointers around
class CWallet;

/**
 * Used by addmultisigaddress / createmultisig:
 */
CScript createmultisig_redeemScript(CWallet *const pwallet,
                                    const UniValue &params) {
    int nRequired = params[0].get_int();
    const UniValue &keys = params[1].get_array();

    // Gather public keys
    if (nRequired < 1) {
        throw std::runtime_error(
            "a multisignature address must require at least one key to redeem");
    }
    if ((int)keys.size() < nRequired) {
        throw std::runtime_error(
            strprintf("not enough keys supplied "
                      "(got %u keys, but need at least %d to redeem)",
                      keys.size(), nRequired));
    }
    if (keys.size() > 16) {
        throw std::runtime_error(
            "Number of addresses involved in the "
            "multisignature address creation > 16\nReduce the "
            "number");
    }
    std::vector<CPubKey> pubkeys;
    pubkeys.resize(keys.size());
    for (size_t i = 0; i < keys.size(); i++) {
        const std::string &ks = keys[i].get_str();
#ifdef ENABLE_WALLET
        // Case 1: Bitcoin address and we have full public key:
        if (pwallet) {
            CTxDestination dest = DecodeDestination(ks, pwallet->chainParams);
            if (IsValidDestination(dest)) {
                const CKeyID *keyID = boost::get<CKeyID>(&dest);
                if (!keyID) {
                    throw std::runtime_error(
                        strprintf("%s does not refer to a key", ks));
                }
                CPubKey vchPubKey;
                if (!pwallet->GetPubKey(*keyID, vchPubKey)) {
                    throw std::runtime_error(
                        strprintf("no full public key for address %s", ks));
                }
                if (!vchPubKey.IsFullyValid()) {
                    throw std::runtime_error(" Invalid public key: " + ks);
                }
                pubkeys[i] = vchPubKey;
                continue;
            }
        }
#endif
        // Case 2: hex public key
        if (IsHex(ks)) {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsFullyValid()) {
                throw std::runtime_error(" Invalid public key: " + ks);
            }
            pubkeys[i] = vchPubKey;
        } else {
            throw std::runtime_error(" Invalid public key: " + ks);
        }
    }

    CScript result = GetScriptForMultisig(nRequired, pubkeys);
    if (result.size() > MAX_SCRIPT_ELEMENT_SIZE_BEFORE_GENESIS) {
        throw std::runtime_error(
            strprintf("redeemScript exceeds size limit: %d > %d", result.size(),
                      MAX_SCRIPT_ELEMENT_SIZE_BEFORE_GENESIS));
    }

    return result;
}

static UniValue createmultisig(const Config &config,
                               const JSONRPCRequest &request) {
#ifdef ENABLE_WALLET
    CWallet *const pwallet = GetWalletForJSONRPCRequest(request);
#else
    CWallet *const pwallet = nullptr;
#endif

    if (request.fHelp || request.params.size() < 2 ||
        request.params.size() > 2) {
        std::string msg =
            "createmultisig nrequired [\"key\",...]\n"
            "\nCreates a multi-signature address with n signature of m keys "
            "required.\n"
            "It returns a json object with the address and redeemScript.\n"

            "\nArguments:\n"
            "1. nrequired      (numeric, required) The number of required "
            "signatures out of the n keys or addresses.\n"
            "2. \"keys\"       (string, required) A json array of keys which "
            "are bitcoin addresses or hex-encoded public keys\n"
            "     [\n"
            "       \"key\"    (string) bitcoin address or hex-encoded public "
            "key\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "{\n"
            "  \"address\":\"multisigaddress\",  (string) The value of the new "
            "multisig address.\n"
            "  \"redeemScript\":\"script\"       (string) The string value of "
            "the hex-encoded redemption script.\n"
            "}\n"

            "\nExamples:\n"
            "\nCreate a multisig address from 2 addresses\n" +
            HelpExampleCli("createmultisig",
                           "2 "
                           "\"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\","
                           "\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("createmultisig",
                           "2, "
                           "[\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\","
                           "\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\"]");
        throw std::runtime_error(msg);
    }

    // Construct using pay-to-script-hash:
    CScript inner = createmultisig_redeemScript(pwallet, request.params);
    CScriptID innerID(inner);

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("address", EncodeDestination(innerID)));
    result.push_back(Pair("redeemScript", HexStr(inner.begin(), inner.end())));

    return result;
}

static UniValue verifymessage(const Config &config,
                              const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 3) {
        throw std::runtime_error(
            "verifymessage \"address\" \"signature\" \"message\"\n"
            "\nVerify a signed message\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The bitcoin address to "
            "use for the signature.\n"
            "2. \"signature\"       (string, required) The signature provided "
            "by the signer in base 64 encoding (see signmessage).\n"
            "3. \"message\"         (string, required) The message that was "
            "signed.\n"
            "\nResult:\n"
            "true|false   (boolean) If the signature is verified or not.\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 30 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n" +
            HelpExampleCli(
                "signmessage",
                "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"my message\"") +
            "\nVerify the signature\n" +
            HelpExampleCli("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4"
                                            "XX\" \"signature\" \"my "
                                            "message\"") +
            "\nAs json rpc\n" +
            HelpExampleRpc("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4"
                                            "XX\", \"signature\", \"my "
                                            "message\""));
    }

    LOCK(cs_main);

    std::string strAddress = request.params[0].get_str();
    std::string strSign = request.params[1].get_str();
    std::string strMessage = request.params[2].get_str();

    CTxDestination destination =
        DecodeDestination(strAddress, config.GetChainParams());
    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
    }

    const CKeyID *keyID = boost::get<CKeyID>(&destination);
    if (!keyID) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    bool fInvalid = false;
    std::vector<uint8_t> vchSig = DecodeBase64(strSign.c_str(), &fInvalid);

    if (fInvalid) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "Malformed base64 encoding");
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(ss.GetHash(), vchSig)) {
        return false;
    }

    return (pubkey.GetID() == *keyID);
}

static UniValue signmessagewithprivkey(const Config &config,
                                       const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
            "signmessagewithprivkey \"privkey\" \"message\"\n"
            "\nSign a message with the private key of an address\n"
            "\nArguments:\n"
            "1. \"privkey\"         (string, required) The private key to sign "
            "the message with.\n"
            "2. \"message\"         (string, required) The message to create a "
            "signature of.\n"
            "\nResult:\n"
            "\"signature\"          (string) The signature of the message "
            "encoded in base 64\n"
            "\nExamples:\n"
            "\nCreate the signature\n" +
            HelpExampleCli("signmessagewithprivkey",
                           "\"privkey\" \"my message\"") +
            "\nVerify the signature\n" +
            HelpExampleCli("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4"
                                            "XX\" \"signature\" \"my "
                                            "message\"") +
            "\nAs json rpc\n" + HelpExampleRpc("signmessagewithprivkey",
                                               "\"privkey\", \"my message\""));
    }

    std::string strPrivkey = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strPrivkey);
    if (!fGood) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
    }
    CKey key = vchSecret.GetKey();
    if (!key.IsValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "Private key outside allowed range");
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    std::vector<uint8_t> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");
    }

    return EncodeBase64(&vchSig[0], vchSig.size());
}

static UniValue clearinvalidtransactions(const Config &config,
                                         const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "clearinvalidtransactions\n\n"
            "Deletes stored invalid transactions.\n"
            "Result: number of bytes freed.");
    }
    return g_connman->getInvalidTxnPublisher().ClearStored();
}

static UniValue setmocktime(const Config &config,
                            const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "setmocktime timestamp\n"
            "\nSet the local time to given timestamp (-regtest only)\n"
            "\nArguments:\n"
            "1. timestamp  (integer, required) Unix seconds-since-epoch "
            "timestamp\n"
            "   Pass 0 to go back to using the system time.");
    }

    if (!config.GetChainParams().MineBlocksOnDemand()) {
        throw std::runtime_error(
            "setmocktime for regression testing (-regtest mode) only");
    }

    // For now, don't change mocktime if we're in the middle of validation, as
    // this could have an effect on mempool time-based eviction, as well as
    // IsInitialBlockDownload().
    // TODO: figure out the right way to synchronize around mocktime, and
    // ensure all callsites of GetTime() are accessing this safely.
    LOCK(cs_main);

    RPCTypeCheck(request.params, {UniValue::VNUM});
    SetMockTime(request.params[0].get_int64());

    return NullUniValue;
}

static UniValue RPCLockedMemoryInfo() {
    LockedPool::Stats stats = LockedPoolManager::Instance().stats();
    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("used", uint64_t(stats.used)));
    obj.push_back(Pair("free", uint64_t(stats.free)));
    obj.push_back(Pair("total", uint64_t(stats.total)));
    obj.push_back(Pair("locked", uint64_t(stats.locked)));
    obj.push_back(Pair("chunks_used", uint64_t(stats.chunks_used)));
    obj.push_back(Pair("chunks_free", uint64_t(stats.chunks_free)));
    return obj;
}

static UniValue TouchedPagesInfo() {
    UniValue obj(UniValue::VOBJ);
    double percents = 0.0;
#ifndef WIN32
    VMTouch vm;
    try {
        auto path = GetDataDir() / "chainstate";
        std::string result = boost::filesystem::canonical(path).string();
        percents = vm.vmtouch_check(result);
    }   catch(const std::runtime_error& ex) {
        LogPrintf("Error while preloading chain state: %s\n", ex.what());
    }
#endif
    obj.push_back(Pair("chainStateCached", percents));
    return obj;
}

static UniValue getmemoryinfo(const Config &config,
                              const JSONRPCRequest &request) {
    /* Please, avoid using the word "pool" here in the RPC interface or help,
     * as users will undoubtedly confuse it with the other "memory pool"
     */
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "getmemoryinfo\n"
            "Returns an object containing information about memory usage.\n"
            "\nResult:\n"
            "{\n"
            "  \"locked\": {               (json object) Information about "
            "locked memory manager\n"
            "    \"used\": xxxxx,          (numeric) Number of bytes used\n"
            "    \"free\": xxxxx,          (numeric) Number of bytes available "
            "in current arenas\n"
            "    \"total\": xxxxxxx,       (numeric) Total number of bytes "
            "managed\n"
            "    \"locked\": xxxxxx,       (numeric) Amount of bytes that "
            "succeeded locking. If this number is smaller than total, locking "
            "pages failed at some point and key data could be swapped to "
            "disk.\n"
            "    \"chunks_used\": xxxxx,   (numeric) Number allocated chunks\n"
            "    \"chunks_free\": xxxxx,   (numeric) Number unused chunks\n"
            "  }\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getmemoryinfo", "") +
            HelpExampleRpc("getmemoryinfo", ""));
    }

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("locked", RPCLockedMemoryInfo()));
    obj.push_back(Pair("preloading", TouchedPagesInfo()));
    return obj;
}

static UniValue echo(const Config &config, const JSONRPCRequest &request) {
    if (request.fHelp) {
        throw std::runtime_error(
            "echo|echojson \"message\" ...\n"
            "\nSimply echo back the input arguments. This command is for "
            "testing.\n"
            "\nThe difference between echo and echojson is that echojson has "
            "argument conversion enabled in the client-side table in"
            "bitcoin-cli. There is no server-side difference.");
    }

    return request.params;
}

static UniValue activezmqnotifications(const Config &config, const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
    {
        throw std::runtime_error(
            "activezmqnotifications\n"
            "Get the active zmq notifications and their addresses\n"
            "\nResult:\n"
            "[ (array) active zmq notifications\n"
            "    {\n"
            "       \"notification\": \"xxxx\", (string) name of zmq notification\n"
            "       \"address\": \"xxxx\"       (string) address of zmq notification\n"
            "    }, ...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("activezmqnotifications", "") +
            HelpExampleRpc("activezmqnotifications", ""));
    }

    UniValue obj(UniValue::VARR);
#if ENABLE_ZMQ
    LOCK(cs_zmqNotificationInterface);
    if (pzmqNotificationInterface)
    {
        std::vector<ActiveZMQNotifier> arrNotifiers = pzmqNotificationInterface->ActiveZMQNotifiers();
        for (auto& n : arrNotifiers)
        {
            UniValue notifierData(UniValue::VOBJ);
            notifierData.push_back(Pair("notification", n.notifierName));
            notifierData.push_back(Pair("address", n.notifierAddress));
            obj.push_back(notifierData);

        }
    }
#endif
    return obj;
}

static UniValue getsettings(const Config &config, const JSONRPCRequest &request)
{

    if (request.fHelp || request.params.size() != 0)
    {
        throw std::runtime_error(
            "getsettings\n"
            "Returns node policy and consensus settings that are used when constructing"
            " a block or transaction.\n"
            "\nResult:\n"
            "{\n"
            "  \"excessiveblocksize\": xxxxx,            (numeric) The maximum block size "
            "in bytes we will accept from any source\n"
            "  \"blockmaxsize\": xxxxx,                  (numeric) The maximum block size "
            "in bytes we will mine\n"
            "  \"maxtxsizepolicy\": xxxxx,               (numeric) The maximum transaction "
            "size in bytes we relay and mine\n"
            "  \"datacarriersize\": xxxxx,               (numeric) The maximum size in bytes "
            "we consider acceptable for data carrier outputs.\n"

            "  \"maxscriptsizepolicy\": xxxxx,           (numeric) The maximum script size "
            "in bytes we're willing to relay/mine per script\n"
            "  \"maxopsperscriptpolicy\": xxxxx,         (numeric) The maximum number of "
            "non-push operations we're willing to relay/mine per script\n"
            "  \"maxscriptnumlengthpolicy\": xxxxx,      (numeric) The maximum allowed number "
            "length in bytes we're willing to relay/mine in scripts\n"
            "  \"maxpubkeyspermultisigpolicy\": xxxxx,   (numeric) The maximum allowed number "
            "of public keys we're willing to relay/mine in a single CHECK_MULTISIG(VERIFY) operation\n"
            "  \"maxtxsigopscountspolicy\": xxxxx,       (numeric) The maximum allowed number "
            "of signature operations we're willing to relay/mine in a single transaction\n"
            "  \"maxstackmemoryusagepolicy\": xxxxx,     (numeric) The maximum stack memory "
            "usage in bytes used for script verification we're willing to relay/mine in a single transaction\n"
            "  \"maxstackmemoryusageconsensus\": xxxxx,  (numeric) The maximum stack memory usage in bytes "
            "used for script verification we're willing to accept from any source\n"

            "  \"maxorphantxsize\": xxxxx,               (numeric) The maximum size in bytes of "
            "unconnectable transactions in memory\n"

            "  \"limitancestorcount\": xxxxx,            (numeric) Do not accept transactions "
            "if number of in-mempool ancestors is <n> or more.\n"
            "  \"limitcpfpgroupmemberscount\": xxxxx,    (numeric) Do not accept transactions "
            "if number of in-mempool low paying ancestors is <n> or more.\n"

            "  \"maxmempool\": xxxxx,                    (numeric) Keep the resident size of "
            "the transaction memory pool below <n> megabytes.\n"
            "  \"maxmempoolsizedisk\": xxxxx,            (numeric) Additional amount of mempool "
            "transactions to keep stored on disk below <n> megabytes.\n"
            "  \"mempoolmaxpercentcpfp\": xxxxx,         (numeric) Percentage of total mempool "
            "size (ram+disk) to allow for low paying transactions (0..100).\n"

            "  \"acceptnonstdoutputs\": xxxx,            (boolean) Relay and mine transactions "
            "that create or consume non-standard output\n"
            "  \"datacarrier\": xxxx,                    (boolean) Relay and mine data carrier transactions\n"
            "  \"blockmintxfee\": xxxxx,                 (numeric) Lowest fee rate (in BSV/kB) for "
            "transactions to be included in block creation\n"
            "  \"minrelaytxfee\": xxxxx,                 (numeric) Fees (in BSV/kB) smaller "
            "than this are considered zero fee for relaying, mining and transaction creation\n"
            "  \"dustrelayfee\": xxxxx,                  (numeric) Fee rate (in BSV/kB) used to defined dust, the value of "
            "an output such that it will cost about 1/3 of its value in fees at this fee rate to spend it. \n"
            "  \"maxstdtxvalidationduration\": xxxxx,    (numeric) Time before terminating validation "
            "of standard transaction in milliseconds\n"
            "  \"maxnonstdtxvalidationduration\": xxxxx, (numeric) Time before terminating validation "
            "of non-standard transaction in milliseconds\n"

            "  \"minconsolidationfactor\": xxxxx         (numeric) Minimum ratio between scriptPubKey inputs and outputs, "
            "0 disables consolidation transactions\n"
            "  \"maxconsolidationinputscriptsize\": xxxx (numeric) Maximum scriptSig length of input in bytes\n"
            "  \"minconfconsolidationinput\": xxxxx      (numeric) Minimum number of confirmations for inputs spent\n"
            "  \"minconsolidationinputmaturity\": xxxxx  (numeric) Minimum number of confirmations for inputs spent "
            "(DEPRECATED: use minconfconsolidationinput instead)\n"
            "  \"acceptnonstdconsolidationinput\": xxxx  (boolean) Accept consolidation transactions that use non "
            "standard inputs\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getsettings", "") +
            HelpExampleRpc("getsettings", ""));
    }

    UniValue obj(UniValue::VOBJ);

    obj.push_back(Pair("excessiveblocksize", config.GetMaxBlockSize()));
    obj.push_back(Pair("blockmaxsize", config.GetMaxGeneratedBlockSize()));
    obj.push_back(Pair("maxtxsizepolicy", config.GetMaxTxSize(true, false)));
    obj.push_back(Pair("maxorphantxsize", config.GetMaxOrphanTxSize()));
    obj.push_back(Pair("datacarriersize", config.GetDataCarrierSize()));

    obj.push_back(Pair("maxscriptsizepolicy", config.GetMaxScriptSize(true, false)));
    obj.push_back(Pair("maxopsperscriptpolicy", config.GetMaxOpsPerScript(true, false)));
    obj.push_back(Pair("maxscriptnumlengthpolicy", config.GetMaxScriptNumLength(true, false)));
    obj.push_back(Pair("maxpubkeyspermultisigpolicy", config.GetMaxPubKeysPerMultiSig(true, false)));
    obj.push_back(Pair("maxtxsigopscountspolicy", config.GetMaxTxSigOpsCountPolicy(true)));
    obj.push_back(Pair("maxstackmemoryusagepolicy", config.GetMaxStackMemoryUsage(true, false)));
    obj.push_back(Pair("maxstackmemoryusageconsensus", config.GetMaxStackMemoryUsage(true, true)));

    obj.push_back(Pair("limitancestorcount", config.GetLimitAncestorCount()));
    obj.push_back(Pair("limitcpfpgroupmemberscount", config.GetLimitSecondaryMempoolAncestorCount()));

    obj.push_back(Pair("maxmempool", config.GetMaxMempool()));
    obj.push_back(Pair("maxmempoolsizedisk", config.GetMaxMempoolSizeDisk()));
    obj.push_back(Pair("mempoolmaxpercentcpfp", config.GetMempoolMaxPercentCPFP()));

    obj.push_back(Pair("acceptnonstdoutputs", config.GetAcceptNonStandardOutput(true)));
    obj.push_back(Pair("datacarrier", fAcceptDatacarrier));
    obj.push_back(Pair("minrelaytxfee", ValueFromAmount(config.GetMinFeePerKB().GetFeePerK())));
    obj.push_back(Pair("dustrelayfee", ValueFromAmount(dustRelayFee.GetFeePerK())));
    obj.push_back(Pair("blockmintxfee", ValueFromAmount(mempool.GetBlockMinTxFee().GetFeePerK())));
    obj.push_back(Pair("maxstdtxvalidationduration", config.GetMaxStdTxnValidationDuration().count()));
    obj.push_back(Pair("maxnonstdtxvalidationduration", config.GetMaxNonStdTxnValidationDuration().count()));

    obj.push_back(Pair("minconsolidationfactor",  config.GetMinConsolidationFactor()));
    obj.push_back(Pair("maxconsolidationinputscriptsize",  config.GetMaxConsolidationInputScriptSize()));
    obj.push_back(Pair("minconfconsolidationinput",  config.GetMinConfConsolidationInput()));
    obj.push_back(Pair("minconsolidationinputmaturity",  config.GetMinConfConsolidationInput()));
    obj.push_back(Pair("acceptnonstdconsolidationinput",  config.GetAcceptNonStdConsolidationInput()));

    return obj;
}

// clang-format off
static const CRPCCommand commands[] = {
    //  category            name                      actor (function)        okSafeMode
    //  ------------------- ------------------------  ----------------------  ----------
    { "control",            "getinfo",                getinfo,                true,  {} }, /* uses wallet if enabled */
    { "control",            "getmemoryinfo",          getmemoryinfo,          true,  {} },
    { "control",            "getsettings",            getsettings,            true,  {} },
    { "control",            "activezmqnotifications", activezmqnotifications, true,  {} },
    { "util",               "validateaddress",        validateaddress,        true,  {"address"} }, /* uses wallet if enabled */
    { "util",               "createmultisig",         createmultisig,         true,  {"nrequired","keys"} },
    { "util",               "verifymessage",          verifymessage,          true,  {"address","signature","message"} },
    { "util",               "signmessagewithprivkey", signmessagewithprivkey, true,  {"privkey","message"} },

    { "util",               "clearinvalidtransactions",clearinvalidtransactions, true,  {} },

    /* Not shown in help */
    { "hidden",             "setmocktime",            setmocktime,            true,  {"timestamp"}},
    { "hidden",             "echo",                   echo,                   true,  {"arg0","arg1","arg2","arg3","arg4","arg5","arg6","arg7","arg8","arg9"}},
    { "hidden",             "echojson",               echo,                   true,  {"arg0","arg1","arg2","arg3","arg4","arg5","arg6","arg7","arg8","arg9"}},
};
// clang-format on

void RegisterMiscRPCCommands(CRPCTable &t) {
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++) {
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
    }
}
