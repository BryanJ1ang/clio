#include "data/DBHelpers.hpp"
#include "etl/MPTHelpers.hpp"
#include "util/TestObject.hpp"

#include <gtest/gtest.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STIssue.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/UintTypes.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

using namespace etl;

namespace {

constexpr auto kIssuer = "rM2AGCCCRb373FRuD8wHyUwUsh2dV4BW5Q";
constexpr auto kHolder = "rnd1nHuzceyQDqnLH8urWNr4QBKt4v7WVk";
constexpr auto kHolder2 = "rfnMHQEUC95R7BfktrK3iD2VkkM7L3Hd2P";
constexpr std::uint32_t kLedgerSeq = 30;
constexpr std::uint32_t kTxIndex = 7;
constexpr std::uint32_t kIssuanceSeq1 = 42;
constexpr std::uint32_t kIssuanceSeq2 = 99;

ripple::Slice const kPubKeySlice("test", 4);

// Build the inner state object of an affected node: NewFields for a created node, FinalFields for a
// modified/deleted node (mirrors how rippled emits metadata).
ripple::STObject
makeInner(ripple::SField const& nodeKind)
{
    return ripple::STObject(
        nodeKind == ripple::sfCreatedNode ? ripple::sfNewFields : ripple::sfFinalFields
    );
}

ripple::STObject
wrapNode(ripple::SField const& nodeKind, std::uint16_t ledgerType, ripple::STObject inner)
{
    ripple::STObject node(nodeKind);
    node.setFieldU16(ripple::sfLedgerEntryType, ledgerType);
    node.setFieldH256(ripple::sfLedgerIndex, ripple::uint256{});
    node.emplace_back(std::move(inner));
    return node;
}

// An MPToken (holder balance) affected node carrying the issuance ID directly.
ripple::STObject
makeMPTokenNode(
    ripple::uint192 const& issuanceID,
    ripple::AccountID const& holder,
    ripple::SField const& nodeKind = ripple::sfModifiedNode
)
{
    auto inner = makeInner(nodeKind);
    inner.setFieldU16(ripple::sfLedgerEntryType, ripple::ltMPTOKEN);
    inner[ripple::sfMPTokenIssuanceID] = issuanceID;
    inner.setAccountID(ripple::sfAccount, holder);
    inner.setFieldU64(ripple::sfMPTAmount, 0);
    return wrapNode(nodeKind, ripple::ltMPTOKEN, std::move(inner));
}

// An MPTokenIssuance affected node; the ID must be reconstructed from sfSequence + sfIssuer.
ripple::STObject
makeMPTIssuanceNode(
    ripple::AccountID const& issuer,
    std::uint32_t sequence,
    ripple::SField const& nodeKind = ripple::sfModifiedNode
)
{
    auto inner = makeInner(nodeKind);
    inner.setFieldU16(ripple::sfLedgerEntryType, ripple::ltMPTOKEN_ISSUANCE);
    inner.setAccountID(ripple::sfIssuer, issuer);
    inner.setFieldU32(ripple::sfSequence, sequence);
    inner.setFieldU64(ripple::sfOutstandingAmount, 0);
    return wrapNode(nodeKind, ripple::ltMPTOKEN_ISSUANCE, std::move(inner));
}

ripple::STTx
toSTTx(ripple::STObject const& obj)
{
    auto const blob = obj.getSerializer().peekData();
    ripple::SerialIter it{blob.data(), blob.size()};
    return ripple::STTx{it};
}

// Assemble a TxMeta from a set of affected nodes.
ripple::TxMeta
makeMeta(
    ripple::uint256 const& txid,
    std::vector<ripple::STObject> nodes,
    ripple::TER result = ripple::tesSUCCESS,
    std::uint32_t txIndex = kTxIndex
)
{
    ripple::STObject metaObj(ripple::sfTransactionMetaData);
    metaObj.setFieldU8(ripple::sfTransactionResult, static_cast<std::uint8_t>(TERtoInt(result)));
    metaObj.setFieldU32(ripple::sfTransactionIndex, txIndex);

    ripple::STArray affected(ripple::sfAffectedNodes);
    for (auto& node : nodes)
        affected.push_back(std::move(node));
    metaObj.setFieldArray(ripple::sfAffectedNodes, affected);

    auto const blob = metaObj.getSerializer().peekData();
    return ripple::TxMeta{txid, kLedgerSeq, blob};
}

// Minimal-but-valid transaction of an arbitrary type (satisfies the type's SOTemplate so it
// round-trips through STTx deserialization). `extra` adds the type-specific required fields.
ripple::STTx
makeTx(ripple::TxType type, std::function<void(ripple::STObject&)> const& extra = {})
{
    ripple::STObject tx(ripple::sfTransaction);
    tx.setFieldU16(ripple::sfTransactionType, static_cast<std::uint16_t>(type));
    tx.setAccountID(ripple::sfAccount, getAccountIdWithString(kIssuer));
    tx.setFieldAmount(ripple::sfFee, ripple::STAmount(10, false));
    tx.setFieldU32(ripple::sfSequence, 1);
    tx.setFieldVL(ripple::sfSigningPubKey, kPubKeySlice);
    if (extra)
        extra(tx);
    return toSTTx(tx);
}

}  // namespace

struct MPTHelpersTest : public ::testing::Test {};

// A transaction that did not succeed is never indexed (Decision #5).
TEST_F(MPTHelpersTest, NonSuccessTransactionReturnsEmpty)
{
    auto const mptID = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));
    auto const tx = makeTx(ripple::ttMPTOKEN_AUTHORIZE, [&](ripple::STObject& obj) {
        obj[ripple::sfMPTokenIssuanceID] = mptID;
    });
    auto const meta = makeMeta(
        tx.getTransactionID(),
        {makeMPTokenNode(mptID, getAccountIdWithString(kHolder))},
        ripple::tecPATH_DRY
    );

    EXPECT_TRUE(getMPTTransactionsFromTx(meta, tx).empty());
}

// A successful transaction that touches no MPT object is not indexed (generic scan finds nothing).
TEST_F(MPTHelpersTest, NoMptNodesReturnsEmpty)
{
    auto const tx = toSTTx(createPaymentTransactionObject(kIssuer, kHolder, 100, 10, 1));

    // A non-MPT affected node (e.g. AccountRoot) must be skipped by the generic scan.
    auto accountRootInner = makeInner(ripple::sfModifiedNode);
    accountRootInner.setFieldU16(ripple::sfLedgerEntryType, ripple::ltACCOUNT_ROOT);
    accountRootInner.setAccountID(ripple::sfAccount, getAccountIdWithString(kIssuer));
    auto const meta = makeMeta(
        tx.getTransactionID(),
        {wrapNode(ripple::sfModifiedNode, ripple::ltACCOUNT_ROOT, std::move(accountRootInner))}
    );

    EXPECT_TRUE(getMPTTransactionsFromTx(meta, tx).empty());
}

// MPTokenIssuanceCreate: the ID must equal makeMptID(sfSequence, sfIssuer), NOT a truncation of the
// node's hashed ledger key (Decision #6 regression guard).
TEST_F(MPTHelpersTest, MPTokenIssuanceCreateDerivesIdFromSequenceAndIssuer)
{
    auto const tm = createMPTIssuanceCreateTxWithMetadata(kIssuer, 10, kIssuanceSeq1);
    ripple::SerialIter it{tm.transaction.data(), tm.transaction.size()};
    ripple::STTx const tx{it};
    ripple::TxMeta const meta{tx.getTransactionID(), kLedgerSeq, tm.metadata};

    auto const result = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].mptID, ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer)));
    EXPECT_EQ(result[0].txType, "MPTokenIssuanceCreate");
    EXPECT_EQ(result[0].ledgerSequence, meta.getLgrSeq());
    EXPECT_EQ(result[0].transactionIndex, meta.getIndex());
    EXPECT_EQ(result[0].txHash, tx.getTransactionID());
    EXPECT_FALSE(result[0].accounts.empty());
}

// MPToken node: the ID is read directly off sfMPTokenIssuanceID.
TEST_F(MPTHelpersTest, MPTokenAuthorizeDerivesIdFromField)
{
    auto const expectedID = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));
    auto const tm = createMPTokenAuthorizeTxWithMetadata(kHolder, expectedID, 10, 2);
    ripple::SerialIter it{tm.transaction.data(), tm.transaction.size()};
    ripple::STTx const tx{it};
    ripple::TxMeta const meta{tx.getTransactionID(), kLedgerSeq, tm.metadata};

    auto const result = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].mptID, expectedID);
    EXPECT_EQ(result[0].txType, "MPTokenAuthorize");
}

// A deleted MPToken node (e.g. MPTokenAuthorize unauthorize, or a clawback emptying a balance) is
// still indexed: the issuance ID is read from sfMPTokenIssuanceID in the node's FinalFields.
TEST_F(MPTHelpersTest, DeletedMPTokenNodeIsIndexed)
{
    auto const mptID = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));
    auto const tx = makeTx(ripple::ttMPTOKEN_AUTHORIZE, [&](ripple::STObject& obj) {
        obj[ripple::sfMPTokenIssuanceID] = mptID;
    });
    auto const meta = makeMeta(
        tx.getTransactionID(),
        {makeMPTokenNode(mptID, getAccountIdWithString(kHolder), ripple::sfDeletedNode)}
    );

    auto const result = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].mptID, mptID);
    EXPECT_EQ(result[0].txType, "MPTokenAuthorize");
}

// A non-MPT-native transaction type (Payment) that touches an MPT as a side effect IS indexed.
// This is the "no allowlist" guarantee (Decision #5).
TEST_F(MPTHelpersTest, PaymentTouchingMptIsIndexed)
{
    auto const mptID = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));
    auto const tx = toSTTx(createPaymentTransactionObject(kIssuer, kHolder, 100, 10, 1));
    auto const meta =
        makeMeta(tx.getTransactionID(), {makeMPTokenNode(mptID, getAccountIdWithString(kHolder))});

    auto const result = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].mptID, mptID);
    EXPECT_EQ(result[0].txType, "Payment");
    EXPECT_TRUE(result[0].accounts.contains(getAccountIdWithString(kHolder)));
}

// A multi-MPT payment fans out one record per distinct issuance, and a repeated issuance node is
// deduped (Decision #7).
TEST_F(MPTHelpersTest, MultiMptPaymentFansOutAndDedups)
{
    auto const id1 = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));
    auto const id2 = ripple::makeMptID(kIssuanceSeq2, getAccountIdWithString(kIssuer));
    auto const tx = toSTTx(createPaymentTransactionObject(kIssuer, kHolder, 100, 10, 1));
    auto const meta = makeMeta(
        tx.getTransactionID(),
        {
            makeMPTokenNode(id1, getAccountIdWithString(kHolder)),
            makeMPTokenNode(id2, getAccountIdWithString(kHolder2)),
            makeMPTokenNode(id1, getAccountIdWithString(kIssuer)),  // duplicate issuance -> deduped
        }
    );

    auto const result = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(result.size(), 2);
    std::vector<ripple::uint192> ids{result[0].mptID, result[1].mptID};
    EXPECT_NE(ids[0], ids[1]);
    EXPECT_TRUE((ids[0] == id1 && ids[1] == id2) || (ids[0] == id2 && ids[1] == id1));
}

// An MPToken node and its MPTokenIssuance node in the same tx resolve to the same MPTID and must
// collapse to a single record (Decision #6/#7).
TEST_F(MPTHelpersTest, MPTokenAndIssuanceNodesSameIdDedup)
{
    auto const issuer = getAccountIdWithString(kIssuer);
    auto const mptID = ripple::makeMptID(kIssuanceSeq1, issuer);
    auto const tx = toSTTx(createPaymentTransactionObject(kIssuer, kHolder, 100, 10, 1));
    auto const meta = makeMeta(
        tx.getTransactionID(),
        {
            makeMPTokenNode(mptID, getAccountIdWithString(kHolder)),
            makeMPTIssuanceNode(issuer, kIssuanceSeq1),
        }
    );

    auto const result = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].mptID, mptID);
}

// The affected-accounts set comes straight from TxMeta::getAffectedAccounts() (Decision #8) and is
// copied onto every record.
TEST_F(MPTHelpersTest, AffectedAccountsMatchMeta)
{
    auto const id1 = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));
    auto const id2 = ripple::makeMptID(kIssuanceSeq2, getAccountIdWithString(kIssuer));
    auto const tx = toSTTx(createPaymentTransactionObject(kIssuer, kHolder, 100, 10, 1));
    auto const meta = makeMeta(
        tx.getTransactionID(),
        {
            makeMPTokenNode(id1, getAccountIdWithString(kHolder)),
            makeMPTokenNode(id2, getAccountIdWithString(kHolder2)),
        }
    );

    auto const result = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(result.size(), 2);
    for (auto const& record : result)
        EXPECT_EQ(record.accounts, meta.getAffectedAccounts());
}

// Extraction is deterministic: re-running it over the same transaction yields byte-identical
// records (same MPTID, accounts, seq_idx, hash). Combined with the deterministic primary keys, this
// is what makes a ledger replay re-upsert identical rows rather than duplicate them (Decision #7).
TEST_F(MPTHelpersTest, ExtractionIsDeterministic)
{
    auto const id1 = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));
    auto const id2 = ripple::makeMptID(kIssuanceSeq2, getAccountIdWithString(kIssuer));
    auto const tx = toSTTx(createPaymentTransactionObject(kIssuer, kHolder, 100, 10, 1));
    auto const meta = makeMeta(
        tx.getTransactionID(),
        {
            makeMPTokenNode(id1, getAccountIdWithString(kHolder)),
            makeMPTokenNode(id2, getAccountIdWithString(kHolder2)),
        }
    );

    auto const first = getMPTTransactionsFromTx(meta, tx);
    auto const second = getMPTTransactionsFromTx(meta, tx);

    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].mptID, second[i].mptID);
        EXPECT_EQ(first[i].accounts, second[i].accounts);
        EXPECT_EQ(first[i].txType, second[i].txType);
        EXPECT_EQ(first[i].ledgerSequence, second[i].ledgerSequence);
        EXPECT_EQ(first[i].transactionIndex, second[i].transactionIndex);
        EXPECT_EQ(first[i].txHash, second[i].txHash);
    }
}

// The generic scan indexes any transaction type touching an MPT and persists the canonical
// mixed-case TxFormats name (Decision #9) — covering offer and AMM legs as well.
TEST_F(MPTHelpersTest, VariousTxTypesAreIndexedWithCanonicalName)
{
    auto const mptID = ripple::makeMptID(kIssuanceSeq1, getAccountIdWithString(kIssuer));

    auto const offer = makeTx(ripple::ttOFFER_CREATE, [](ripple::STObject& obj) {
        obj.setFieldAmount(ripple::sfTakerPays, ripple::STAmount(100, false));
        obj.setFieldAmount(ripple::sfTakerGets, ripple::STAmount(100, false));
    });
    auto const clawback = makeTx(ripple::ttCLAWBACK, [](ripple::STObject& obj) {
        obj.setFieldAmount(ripple::sfAmount, ripple::STAmount(100, false));
    });
    auto const ammDeposit = makeTx(ripple::ttAMM_DEPOSIT, [](ripple::STObject& obj) {
        obj.setFieldIssue(ripple::sfAsset, ripple::STIssue{ripple::sfAsset, ripple::xrpIssue()});
        obj.setFieldIssue(ripple::sfAsset2, ripple::STIssue{ripple::sfAsset2, ripple::xrpIssue()});
    });

    for (auto const& [tx, expectedName] : std::vector<std::pair<ripple::STTx, char const*>>{
             {offer, "OfferCreate"}, {clawback, "Clawback"}, {ammDeposit, "AMMDeposit"}
         }) {
        auto const meta = makeMeta(
            tx.getTransactionID(), {makeMPTokenNode(mptID, getAccountIdWithString(kHolder))}
        );
        auto const result = getMPTTransactionsFromTx(meta, tx);
        ASSERT_EQ(result.size(), 1) << expectedName;
        EXPECT_EQ(result[0].mptID, mptID) << expectedName;
        EXPECT_EQ(result[0].txType, expectedName);
    }
}

// The MPTokenIssuance ID is reconstructed from sfSequence + sfIssuer regardless of whether the node
// was created (MPTokenIssuanceCreate), modified (MPTokenIssuanceSet) or deleted
// (MPTokenIssuanceDestroy) — all three carry those SoeRequired fields in their state sub-object.
TEST_F(MPTHelpersTest, MPTIssuanceNodeAcrossNodeKindsAndTypes)
{
    auto const issuer = getAccountIdWithString(kIssuer);
    auto const mptID = ripple::makeMptID(kIssuanceSeq1, issuer);

    auto const issuanceSet = makeTx(ripple::ttMPTOKEN_ISSUANCE_SET, [&](ripple::STObject& obj) {
        obj[ripple::sfMPTokenIssuanceID] = mptID;
    });
    auto const issuanceDestroy =
        makeTx(ripple::ttMPTOKEN_ISSUANCE_DESTROY, [&](ripple::STObject& obj) {
            obj[ripple::sfMPTokenIssuanceID] = mptID;
        });

    struct Case {
        ripple::STTx tx;
        ripple::SField const& nodeKind;
        char const* expectedName;
    };
    std::vector<Case> const cases{
        {issuanceSet, ripple::sfModifiedNode, "MPTokenIssuanceSet"},
        {issuanceDestroy, ripple::sfDeletedNode, "MPTokenIssuanceDestroy"},
    };

    for (auto const& c : cases) {
        auto const meta = makeMeta(
            c.tx.getTransactionID(), {makeMPTIssuanceNode(issuer, kIssuanceSeq1, c.nodeKind)}
        );
        auto const result = getMPTTransactionsFromTx(meta, c.tx);
        ASSERT_EQ(result.size(), 1) << c.expectedName;
        EXPECT_EQ(result[0].mptID, mptID) << c.expectedName;
        EXPECT_EQ(result[0].txType, c.expectedName);
    }
}
