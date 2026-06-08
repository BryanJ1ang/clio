/** @file */
#pragma once

#include "data/DBHelpers.hpp"

#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TxMeta.h>

#include <optional>
#include <vector>

namespace etl {

/**
 * @brief Extract the MPT transaction-history index rows for a single transaction.
 *
 * Scans the transaction metadata's `AffectedNodes` for `MPTokenIssuance` / `MPToken` ledger
 * objects (a generic side-effect scan, not a transaction-type allowlist), derives the 192-bit
 * MPT issuance ID per node type, and produces one @ref MPTTransactionsData per distinct touched
 * issuance. Each record carries the transaction's affected accounts (fanning out the per-account
 * table) and its canonical `TxFormats` type name. Only `tesSUCCESS` transactions are indexed.
 *
 * This is the single extractor shared by live ETL and the historical backfill migrator, so it
 * takes only `(TxMeta, STTx)` and no live-pass state.
 *
 * @param txMeta Transaction metadata
 * @param sttx The transaction
 * @return One record per distinct MPT issuance touched; empty if the transaction touched no MPT
 * or did not succeed
 */
std::vector<MPTTransactionsData>
getMPTTransactionsFromTx(ripple::TxMeta const& txMeta, ripple::STTx const& sttx);

/**
 * @brief Pull MPT data from TX via ETLService.
 *
 * @param txMeta Transaction metadata
 * @param sttx The transaction
 * @return The MPTIssuanceID and holder pair as a optional
 */
std::optional<MPTHolderData>
getMPTHolderFromTx(ripple::TxMeta const& txMeta, ripple::STTx const& sttx);

/**
 * @brief Pull MPT data from ledger object via loadInitialLedger.
 *
 * @param key The owner key
 * @param blob Object data as blob
 * @return The MPTIssuanceID and holder pair as a optional
 */
std::optional<MPTHolderData>
getMPTHolderFromObj(std::string const& key, std::string const& blob);

}  // namespace etl
