#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/storage/data_table.hpp"
#include "hnsw/hnsw.hpp"
#include "hnsw/hnsw_index.hpp"
#include "hnsw/hnsw_index_scan.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//-----------------------------------------------------------------------------
// Plan rewriter
//-----------------------------------------------------------------------------
class HNSWIndexScanOptimizer : public OptimizerExtension {
public:
	HNSWIndexScanOptimizer() {
		optimize_function = HNSWIndexScanOptimizer::Optimize;
	}

	static bool TryOptimize(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
		// Look for a TopN operator
		auto &op = *plan;

		if (op.type != LogicalOperatorType::LOGICAL_TOP_N) {
			return false;
		}

		// Look for a expression that is a distance expression
		auto &top_n = op.Cast<LogicalTopN>();

		if (top_n.orders.size() != 1) {
			// We can only optimize if there is a single order by expression right now
			return false;
		}

		auto &order = top_n.orders[0];

		if (order.type != OrderType::ASCENDING) {
			// We can only optimize if the order by expression is ascending
			return false;
		}

		if (order.expression->type != ExpressionType::BOUND_COLUMN_REF) {
			// The expression has to reference the child operator (a projection with the distance function)
			return false;
		}
		auto &bound_column_ref = order.expression->Cast<BoundColumnRefExpression>();

		// find the expression that is referenced
		auto &immediate_child = top_n.children[0];
		if (immediate_child->type != LogicalOperatorType::LOGICAL_PROJECTION) {
			// The child has to be a projection
			return false;
		}
		auto &projection = immediate_child->Cast<LogicalProjection>();
		auto projection_index = bound_column_ref.binding.column_index;

		if (projection.expressions[projection_index]->type != ExpressionType::BOUND_FUNCTION) {
			// The expression has to be a function
			return false;
		}
		auto &bound_function = projection.expressions[projection_index]->Cast<BoundFunctionExpression>();
		if (!HNSWIndex::IsDistanceFunction(bound_function.function.name)) {
			// We can only optimize if the order by expression is a distance function
			return false;
		}

		// Figure out the query vector
		Value target_value;
		if (bound_function.children[0]->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			target_value = bound_function.children[0]->Cast<BoundConstantExpression>().value;
		} else if (bound_function.children[1]->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			target_value = bound_function.children[1]->Cast<BoundConstantExpression>().value;
		} else {
			// We can only optimize if one of the children is a constant
			return false;
		}

		// TODO: We should check that the other argument to the distance function is a column reference
		// that matches the column that the index is on. That also helps us identify the scan operator

		auto value_type = target_value.type();
		if (value_type.id() != LogicalTypeId::ARRAY) {
			// We can only optimize if the constant is an array
			return false;
		}
		auto array_size = ArrayType::GetSize(value_type);
		auto array_inner_type = ArrayType::GetChildType(value_type);
		if (array_inner_type.id() != LogicalTypeId::FLOAT) {
			// Try to cast to float
			bool ok = target_value.DefaultTryCastAs(LogicalType::ARRAY(LogicalType::FLOAT, array_size), true);
			if (!ok) {
				// We can only optimize if the array is of floats or we can cast it to floats
				return false;
			}
		}

		// find any direct child or grandchild that is a get
		auto child = top_n.children[0].get();
		while (child->type != LogicalOperatorType::LOGICAL_GET) {
			// TODO: Handle joins?
			if (child->children.size() != 1) {
				// Either 0 or more than 1 child
				return false;
			}
			child = child->children[0].get();
		}

		auto &get = child->Cast<LogicalGet>();
		// Check if the get is a table scan
		if (get.function.name != "seq_scan") {
			return false;
		}

		// We have a top-n operator on top of a table scan
		// We can replace the function with a custom index scan (if the table has a custom index)

		// Get the table
		auto &table = *get.GetTable();
		if (!table.IsDuckTable()) {
			// We can only replace the scan if the table is a duck table
			return false;
		}

		auto &duck_table = table.Cast<DuckTableEntry>();
		auto &table_info = *table.GetStorage().GetDataTableInfo();

		// Find the index
		unique_ptr<HNSWIndexScanBindData> bind_data = nullptr;
		table_info.GetIndexes().BindAndScan<HNSWIndex>(context, table_info, [&](HNSWIndex &index_entry) {
			auto &hnsw_index = index_entry.Cast<HNSWIndex>();

			if (hnsw_index.GetVectorSize() != array_size) {
				// The vector size of the index does not match the vector size of the query
				return false;
			}

			if (!hnsw_index.MatchesDistanceFunction(bound_function.function.name)) {
				// The distance function of the index does not match the distance function of the query
				return false;
			}

			// Create a query vector from the constant value
			auto query_vector = make_unsafe_uniq_array<float>(array_size);
			auto vector_elements = ArrayValue::GetChildren(target_value);
			for (idx_t i = 0; i < array_size; i++) {
				query_vector[i] = vector_elements[i].GetValue<float>();
			}

			// Create the bind data for this index
			bind_data =
				make_uniq<HNSWIndexScanBindData>(duck_table, index_entry, top_n.limit, std::move(query_vector));
			return true;
		});

		if (!bind_data) {
			// No index found
			return false;
		}

		// Replace the scan with our custom index scan function

		get.function = HNSWIndexScanFunction::GetFunction();
		auto cardinality = get.function.cardinality(context, bind_data.get());
		get.has_estimated_cardinality = cardinality->has_estimated_cardinality;
		get.estimated_cardinality = cardinality->estimated_cardinality;
		get.bind_data = std::move(bind_data);


		// Remove the distance function from the projection
		// projection.expressions.erase(projection.expressions.begin() + static_cast<ptrdiff_t>(projection_index));
		//top_n.expressions

		// Remove the TopN operator
		plan = std::move(top_n.children[0]);
		return true;
	}

	static bool OptimizeChildren(ClientContext &context, unique_ptr<LogicalOperator> &plan) {

		auto ok = TryOptimize(context, plan);
		// Recursively optimize the children
		for (auto &child : plan->children) {
			ok |= OptimizeChildren(context, child);
		}
		return ok;
	}

	static void MergeProjections(unique_ptr<LogicalOperator> &plan) {
		if(plan->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			if(plan->children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				auto &child = plan->children[0];

				if(child->children[0]->type == LogicalOperatorType::LOGICAL_GET && child->children[0]->Cast<LogicalGet>().function.name == "hnsw_index_scan") {
					auto &parent_projection = plan->Cast<LogicalProjection>();
					auto &child_projection = child->Cast<LogicalProjection>();

					column_binding_set_t referenced_bindings;
					for(auto &expr : parent_projection.expressions) {
						ExpressionIterator::EnumerateExpression(expr, [&](Expression& expr_ref) {
							if(expr_ref.type == ExpressionType::BOUND_COLUMN_REF) {
								auto &bound_column_ref = expr_ref.Cast<BoundColumnRefExpression>();
								referenced_bindings.insert(bound_column_ref.binding);
							}
						});
					}

					auto child_bindings = child_projection.GetColumnBindings();
					for(idx_t i = 0; i < child_projection.expressions.size(); i++) {
						auto &expr = child_projection.expressions[i];
						auto &outgoing_binding = child_bindings[i];

						if(referenced_bindings.find(outgoing_binding) == referenced_bindings.end()) {
							// The binding is not referenced
							// We can remove this expression. But positionality matters so just replace with int.
							expr = make_uniq_base<Expression, BoundConstantExpression>(Value(LogicalType::TINYINT));
						}
					}
					return;
				}
			}
		}
		for(auto &child : plan->children) {
			MergeProjections(child);
		}
	}

	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		auto did_use_hnsw_scan = OptimizeChildren(input.context, plan);
		if(did_use_hnsw_scan) {
			MergeProjections(plan);
		}
	}
};

//-----------------------------------------------------------------------------
// Register
//-----------------------------------------------------------------------------
void HNSWModule::RegisterPlanIndexScan(DatabaseInstance &db) {
	// Register the optimizer extension
	db.config.optimizer_extensions.push_back(HNSWIndexScanOptimizer());
}

} // namespace duckdb