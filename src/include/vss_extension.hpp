#pragma once

#include "duckdb.hpp"

namespace duckdb {

class VssExtension : public Extension {
public:
	void Load(DuckDB &db) override;
	std::string Name() override;
	std::string Description() const override;
};

} // namespace duckdb
