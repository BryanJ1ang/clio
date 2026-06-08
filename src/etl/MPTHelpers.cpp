#include "data/DBHelpers.hpp"
#include "util/Assert.hpp"

#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STBase.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/TxMeta.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace etl {

namespace {

/**
 * @brief Return the inner state object of an affected metadata node.
 *
 * Created nodes carry their fields under `sfNewFields`; modified and deleted nodes carry their
 * fields under `sfFinalFields`. The `SoeRequired` fields we read off MPToken / MPTokenIssuance all
 * carry the default metadata flags (which include `sMD_Create`, `sMD_ChangeNew` and
 * `sMD_DeleteFinal`), so they are always emitted in the relevant sub-object across created,
 * modified and deleted nodes. Returns nullptr if neither sub-object is present.
 */
ripple::STObject const*
getAffectedNodeFinalObject(ripple::STObject const& node)
{
    if (node.isFieldPresent(ripple::sfNewFields))
        return &node.peekAtField(ripple::sfNewFields).downcast<ripple::STObject>();
    if (node.isFieldPresent(ripple::sfFinalFields))
        return &node.peekAtField(ripple::sfFinalFields).downcast<ripple::STObject>();
    return nullptr;
}

/**
 * @brief Derive the 192-bit MPT issuance ID touched by a single affected node, if any.
 *
 * The two object types are asymmetric (Decision #6): an `MPToken` holder node carries the ID
 * directly in `sfMPTokenIssuanceID`, whereas an `MPTokenIssuance` node's ledger key is a one-way
 * `sha512Half` hash that does not embed the ID, so the ID is reconstructed from its `sfSequence`
 * and `sfIssuer` fields via `ripple::makeMptID` (never truncated from the key).
 */
std::optional<ripple::uint192>
getMPTIssuanceID(ripple::STObject const& node)
{
    auto const ledgerType = node.getFieldU16(ripple::sfLedgerEntryType);
    if (ledgerType != ripple::ltMPTOKEN && ledgerType != ripple::ltMPTOKEN_ISSUANCE)
        return std::nullopt;

    auto const* inner = getAffectedNodeFinalObject(node);
    if (inner == nullptr)
        return std::nullopt;

    if (ledgerType == ripple::ltMPTOKEN) {
        if (!inner->isFieldPresent(ripple::sfMPTokenIssuanceID))
            return std::nullopt;
        return (*inner)[ripple::sfMPTokenIssuanceID];
    }

    // ltMPTOKEN_ISSUANCE: reconstruct from the SoeRequired sfSequence + sfIssuer fields.
    if (!inner->isFieldPresent(ripple::sfSequence) || !inner->isFieldPresent(ripple::sfIssuer))
        return std::nullopt;

    return ripple::makeMptID(
        inner->getFieldU32(ripple::sfSequence), inner->getAccountID(ripple::sfIssuer)
    );
}

}  // namespace

std::vector<MPTTransactionsData>
getMPTTransactionsFromTx(ripple::TxMeta const& txMeta, ripple::STTx const& sttx)
{
    if (txMeta.getResultTER() != ripple::tesSUCCESS)
        return {};

    // Collect the distinct set of MPT issuances touched by this transaction. Using a set dedups
    // the case where one transaction touches both an MPToken and its MPTokenIssuance node, which
    // must resolve to the same MPTID (Decision #6/#7), so we never emit duplicate
    // (mpt_id, seq_idx) clustering rows.
    std::set<ripple::uint192> mptIDs;
    for (ripple::STObject const& node : txMeta.getNodes()) {
        if (auto const mptID = getMPTIssuanceID(node); mptID.has_value())
            mptIDs.insert(*mptID);
    }

    if (mptIDs.empty())
        return {};

    auto const accounts = txMeta.getAffectedAccounts();

    // Persist the exact mixed-case TxFormats name (Decision #9). A successfully deserialized STTx
    // always has a registered type, so this is non-null in practice; if it ever isn't, skip
    // indexing this transaction rather than aborting the (core) ETL writer for an additive index.
    auto const* const txFormat = ripple::TxFormats::getInstance().findByType(sttx.getTxnType());
    if (txFormat == nullptr)
        return {};
    auto const& txType = txFormat->getName();

    std::vector<MPTTransactionsData> result;
    result.reserve(mptIDs.size());
    for (auto const& mptID : mptIDs) {
        result.push_back(
            MPTTransactionsData{
                .mptID = mptID,
                .accounts = accounts,
                .txType = txType,
                .ledgerSequence = txMeta.getLgrSeq(),
                .transactionIndex = txMeta.getIndex(),
                .txHash = sttx.getTransactionID()
            }
        );
    }
    return result;
}

namespace {

/**
 * @brief Get the MPToken created from a transaction
 *
 * @param txMeta Transaction metadata
 * @return MPT and holder account pair
 */
std::optional<MPTHolderData>
getMPTokenAuthorize(ripple::TxMeta const& txMeta)
{
    for (ripple::STObject const& node : txMeta.getNodes()) {
        if (node.getFieldU16(ripple::sfLedgerEntryType) != ripple::ltMPTOKEN)
            continue;

        if (node.getFName() == ripple::sfCreatedNode) {
            auto const& newMPT = node.peekAtField(ripple::sfNewFields).downcast<ripple::STObject>();
            return MPTHolderData{
                .mptID = newMPT[ripple::sfMPTokenIssuanceID],
                .holder = newMPT.getAccountID(ripple::sfAccount)
            };
        }
    }
    return {};
}

}  // namespace

std::optional<MPTHolderData>
getMPTHolderFromTx(ripple::TxMeta const& txMeta, ripple::STTx const& sttx)
{
    if (txMeta.getResultTER() != ripple::tesSUCCESS ||
        sttx.getTxnType() != ripple::TxType::ttMPTOKEN_AUTHORIZE)
        return {};

    return getMPTokenAuthorize(txMeta);
}

std::optional<MPTHolderData>
getMPTHolderFromObj(std::string const& key, std::string const& blob)
{
    // https://github.com/XRPLF/XRPL-Standards/tree/master/XLS-0033-multi-purpose-tokens#2121-mptoken-ledger-identifier
    ASSERT(
        key.size() == ripple::uint256::size(),
        "The size of the key is expected to fit uint256 exactly"
    );

    ripple::STLedgerEntry const sle = ripple::STLedgerEntry(
        ripple::SerialIter{blob.data(), blob.size()}, ripple::uint256::fromVoid(key.data())
    );

    if (sle.getFieldU16(ripple::sfLedgerEntryType) != ripple::ltMPTOKEN)
        return {};

    auto const mptIssuanceID = sle[ripple::sfMPTokenIssuanceID];
    auto const holder = sle.getAccountID(ripple::sfAccount);

    return MPTHolderData{.mptID = mptIssuanceID, .holder = holder};
}

}  // namespace etl
