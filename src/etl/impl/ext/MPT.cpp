#include "etl/impl/ext/MPT.hpp"

#include "data/BackendInterface.hpp"
#include "data/DBHelpers.hpp"
#include "etl/MPTHelpers.hpp"
#include "etl/Models.hpp"
#include "util/log/Logger.hpp"
#include "util/prometheus/Label.hpp"
#include "util/prometheus/Prometheus.hpp"

#include <xrpl/basics/strHex.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace etl::impl {

MPTExt::MPTExt(std::shared_ptr<BackendInterface> backend)
    : backend_(std::move(backend))
    , indexRowsWritten_(
          PrometheusService::counterInt(
              "etl_mpt_index_rows_written_total_number",
              util::prometheus::Labels(),
              "The total number of MPT transaction index rows ETL has enqueued for write across "
              "both index tables (counts each write pass, so a ledger replay re-counts its rows)"
          )
      )
{
}

void
MPTExt::onLedgerData(model::LedgerData const& data)
{
    LOG(log_.trace()) << "got TXS cnt = " << data.transactions.size()
                      << "; OBJS size = " << data.objects.size();
    writeMPTHoldersFromTransactions(data);
    writeMPTTransactions(data);
}

void
MPTExt::onInitialObject(uint32_t, model::Object const& obj)
{
    LOG(log_.trace()) << "got initial object with key: " << ripple::strHex(obj.key);
    if (auto const mptHolder = getMPTHolderFromObj(obj.keyRaw, obj.dataRaw); mptHolder.has_value())
        backend_->writeMPTHolders({*mptHolder});
}

void
MPTExt::onInitialData(model::LedgerData const& data)
{
    LOG(log_.trace()) << "got initial TXS cnt = " << data.transactions.size();
    writeMPTHoldersFromTransactions(data);
    writeMPTTransactions(data);
}

void
MPTExt::writeMPTHoldersFromTransactions(model::LedgerData const& data)
{
    std::vector<MPTHolderData> holders;

    for (auto const& tx : data.transactions) {
        if (auto const mptHolder = getMPTHolderFromTx(tx.meta, tx.sttx); mptHolder.has_value())
            holders.push_back(*mptHolder);
    }

    if (not holders.empty())
        backend_->writeMPTHolders(holders);
}

void
MPTExt::writeMPTTransactions(model::LedgerData const& data)
{
    // A single transaction touching M distinct issuances and A affected accounts emits at most
    // M + M*A index rows (Decision #2). This soft ceiling is far above any realistic transaction;
    // exceeding it is not an error but is worth surfacing as it would inflate index writes.
    static constexpr std::uint64_t kRowsPerTxWarnThreshold = 10'000;

    std::vector<MPTTransactionsData> mptTxs;
    std::uint64_t rowsWritten = 0;

    for (auto const& tx : data.transactions) {
        auto txs = getMPTTransactionsFromTx(tx.meta, tx.sttx);
        if (txs.empty())
            continue;

        // Per-transaction contribution across both tables: M (mpt_transactions) + M*A
        // (account_mpt_transactions).
        std::uint64_t rowsForTx = txs.size();
        for (auto const& record : txs)
            rowsForTx += record.accounts.size();

        if (rowsForTx > kRowsPerTxWarnThreshold) {
            LOG(log_.warn()) << "MPT index fan-out for tx " << ripple::strHex(txs.front().txHash)
                             << " in ledger " << data.seq << " is unusually large: " << rowsForTx
                             << " rows (" << txs.size() << " issuances)";
        }

        rowsWritten += rowsForTx;
        mptTxs.insert(mptTxs.end(), txs.begin(), txs.end());
    }

    if (mptTxs.empty())
        return;

    // Both writes ride the existing buffered, idempotent ledger pass (committed via finishWrites
    // before the range advances), so a crash mid-ledger re-upserts identical deterministic-PK rows
    // on replay (Decision #7). Distinct transactions have distinct transactionIndex, so the
    // per-transaction dedup in getMPTTransactionsFromTx already makes every (mpt_id, seq_idx) and
    // (mpt_id, account, seq_idx) clustering key unique across the buffer.
    backend_->writeMPTTransactions(mptTxs);
    backend_->writeAccountMPTTransactions(mptTxs);

    // Count only once both write batches were handed to the writer, so a throw in the write path
    // doesn't overreport.
    indexRowsWritten_.get() += rowsWritten;
}

}  // namespace etl::impl
