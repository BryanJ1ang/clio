#pragma once

#include "data/BackendInterface.hpp"
#include "etl/Models.hpp"
#include "util/log/Logger.hpp"
#include "util/prometheus/Counter.hpp"

#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TxMeta.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace etl::impl {

class MPTExt {
    std::shared_ptr<BackendInterface> backend_;
    util::Logger log_{"ETL"};
    std::reference_wrapper<util::prometheus::CounterInt> indexRowsWritten_;

public:
    explicit MPTExt(std::shared_ptr<BackendInterface> backend);

    void
    onLedgerData(model::LedgerData const& data);

    void
    onInitialObject(uint32_t seq, model::Object const& obj);

    void
    onInitialData(model::LedgerData const& data);

private:
    void
    writeMPTHoldersFromTransactions(model::LedgerData const& data);

    void
    writeMPTTransactions(model::LedgerData const& data);
};

}  // namespace etl::impl
