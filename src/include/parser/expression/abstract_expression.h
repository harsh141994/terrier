#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "common/hash_util.h"
#include "common/json.h"
#include "common/managed_pointer.h"
#include "common/sql_node_visitor.h"
#include "parser/expression_defs.h"
#include "type/transient_value.h"
#include "type/type_id.h"

namespace terrier::optimizer {
class PlanGenerator;
class QueryToOperatorTransformer;
}  // namespace terrier::optimizer

namespace terrier::binder {
class BindNodeVisitor;
}  // namespace terrier::binder

namespace terrier::parser {
class ParseResult;

/**
 * AbstractExpression is the base class of any expression which is output from the parser.
 *
 * TODO(WAN): So these are supposed to be dumb and immutable, but we're cheating in the binder. Figure out how to
 * document these assumptions exactly.
 */
class AbstractExpression {
  friend class PlanGenerator;  // Access to DeriveReturnValueType

 protected:
  /**
   * Instantiates a new abstract expression.
   * @param expression_type what type of expression we have
   * @param return_value_type the type of the expression's value
   * @param children the list of children for this node
   */
  AbstractExpression(const ExpressionType expression_type, const type::TypeId return_value_type,
                     std::vector<std::unique_ptr<AbstractExpression>> &&children)
      : expression_type_(expression_type), return_value_type_(return_value_type), children_(std::move(children)) {}
  /**
   * Instantiates a new abstract expression with alias used for select statement column references.
   * @param expression_type what type of expression we have
   * @param return_value_type the type of the expression's value
   * @param alias alias of the column (used in column value expression)
   * @param children the list of children for this node
   */
  AbstractExpression(const ExpressionType expression_type, const type::TypeId return_value_type, std::string alias,
                     std::vector<std::unique_ptr<AbstractExpression>> &&children)
      : expression_type_(expression_type),
        alias_(std::move(alias)),
        return_value_type_(return_value_type),
        children_(std::move(children)) {}

  /**
   * Copy constructs an abstract expression.
   * @param other the abstract expression to be copied
   */
  AbstractExpression(const AbstractExpression &other) = default;

  /**
   * Default constructor used for json deserialization
   */
  AbstractExpression() = default;

  /**
   * @param expression_name Set the expression name of the current expression
   */
  void SetExpressionName(std::string expression_name) { expression_name_ = std::move(expression_name); }

  /**
   * @param expression_type Set the expression type of the current expression
   */
  void SetExpressionType(ExpressionType expression_type) { expression_type_ = expression_type; }

  /**
   * @param return_value_type Set the return value type of the current expression
   */
  void SetReturnValueType(type::TypeId return_value_type) { return_value_type_ = return_value_type; }

  /**
   * @param depth Set the depth of the current expression
   */
  void SetDepth(int depth) { depth_ = depth; }

  /**
   * Copies the mutable state of copy_expr. This should only be used for copying where we don't need to
   * re-derive the expression.
   * @param copy_expr the expression whose mutable state should be copied
   */
  void SetMutableStateForCopy(const AbstractExpression &copy_expr) {
    SetExpressionName(copy_expr.GetExpressionName());
    SetReturnValueType(copy_expr.GetReturnValueType());
    SetDepth(copy_expr.GetDepth());
    has_subquery_ = copy_expr.HasSubquery();
    alias_ = copy_expr.alias_;
  }

 public:
  virtual ~AbstractExpression() = default;

  /**
   * Hashes the current abstract expression.
   */
  virtual common::hash_t Hash() const {
    common::hash_t hash = common::HashUtil::Hash(expression_type_);
    for (const auto &child : children_) {
      hash = common::HashUtil::CombineHashes(hash, child->Hash());
    }
    hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(return_value_type_));
    hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(expression_name_));
    hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(alias_));
    hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(depth_));
    hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(static_cast<char>(has_subquery_)));

    return hash;
  }

  /**
   * Logical equality check.
   * @param rhs other
   * @return true if the two expressions are logically equal
   */
  virtual bool operator==(const AbstractExpression &rhs) const {
    if (expression_type_ != rhs.expression_type_) return false;
    if (alias_ != rhs.alias_) return false;
    if (expression_name_ != rhs.expression_name_) return false;
    if (depth_ != rhs.depth_) return false;
    if (has_subquery_ != rhs.has_subquery_) return false;
    if (children_.size() != rhs.children_.size()) return false;
    for (size_t i = 0; i < children_.size(); i++)
      if (*(children_[i]) != *(rhs.children_[i])) return false;
    return return_value_type_ == rhs.return_value_type_;
  }

  /**
   * Logical inequality check.
   * @param rhs other
   * @return true if the two expressions are logically not equal
   */
  virtual bool operator!=(const AbstractExpression &rhs) const { return !operator==(rhs); }

  /**
   * Creates a deep copy of the current AbstractExpression.
   *
   * Transplanted comment from subquery_expression.h,
   * TODO(WAN): Previous codebase described as a hack, will we need a deep copy?
   * Tianyu: No need for deep copy if your objects are always immutable! (why even copy at all, but that's beyond me)
   * Wan: objects are becoming mutable again..
   */
  // It is incorrect to supply a default implementation here since that will return an object
  // of base type AbstractExpression instead of the desired non-abstract type.
  virtual std::unique_ptr<AbstractExpression> Copy() const = 0;

  /**
   * Creates a copy of the current AbstractExpression with new children implanted.
   * The children should not be owned by any other AbstractExpression.
   * @param children New children to be owned by the copy
   */
  virtual std::unique_ptr<AbstractExpression> CopyWithChildren(
      std::vector<std::unique_ptr<AbstractExpression>> &&children) const = 0;

  /**
   * @return type of this expression
   */
  ExpressionType GetExpressionType() const { return expression_type_; }

  /**
   * @return type of the return value
   */
  type::TypeId GetReturnValueType() const { return return_value_type_; }

  /**
   * @return number of children in this abstract expression
   */
  size_t GetChildrenSize() const { return children_.size(); }

  /** @return children of this abstract expression */
  std::vector<common::ManagedPointer<AbstractExpression>> GetChildren() const {
    std::vector<common::ManagedPointer<AbstractExpression>> children;
    children.reserve(children_.size());
    for (const auto &child : children_) {
      children.emplace_back(common::ManagedPointer(child));
    }
    return children;
  }

  /**
   * @param index index of child
   * @return child of abstract expression at that index
   */
  common::ManagedPointer<AbstractExpression> GetChild(uint64_t index) const {
    TERRIER_ASSERT(index < children_.size(), "Index must be in bounds.");
    return common::ManagedPointer(children_[index]);
  }

  /** @return Name of the expression. */
  const std::string &GetExpressionName() const { return expression_name_; }

  /**
   * Walks the expression trees and generate the correct expression name
   */
  virtual void DeriveExpressionName();

  /** @return alias of this abstract expression */
  const std::string &GetAlias() const { return alias_; }

  /**
   * Derive the expression type of the current expression.
   */
  virtual void DeriveReturnValueType() {}

  // TODO(WAN): it looks like we are returning to Peloton style Accept calls AcceptChildren calls Accept, must we?

  /**
   * @param v Visitor pattern for the expression
   * @param parse_result Result generated by the parser. A collection of statements and expressions in the query.
   */
  virtual void Accept(SqlNodeVisitor *v, ParseResult *parse_result) = 0;

  /**
   * @param v Visitor pattern for the expression
   * @param parse_result Result generated by the parser. A collection of statements and expressions in the query.
   */
  virtual void AcceptChildren(SqlNodeVisitor *v, ParseResult *parse_result) {
    for (auto &child : children_) child->Accept(v, parse_result);
  }

  /** @return the sub-query depth level (SEE COMMENT in depth_) */
  int GetDepth() const { return depth_; }

  /**
   * Derive the sub-query depth level of the current expression
   * @return the derived depth (SEE COMMENT in depth_)
   */
  virtual int DeriveDepth();

  /** @return true iff the current expression contains a subquery
   * DeriveSubqueryFlag() MUST be called between modifications to this expression or its children for this
   * function to return a meaningful value.
   * */
  bool HasSubquery() const { return has_subquery_; }

  /**
   * Derive if there's sub-query in the current expression
   * @return true iff the current expression contains a subquery
   */
  virtual bool DeriveSubqueryFlag();

  /**
   * Derived expressions should call this base method
   * @return expression serialized to json
   */
  virtual nlohmann::json ToJson() const;

  /**
   * Derived expressions should call this base method
   * @param j json to deserialize
   */
  virtual std::vector<std::unique_ptr<AbstractExpression>> FromJson(const nlohmann::json &j);

 protected:
  // We make abstract expression friend with both binder and query to operator transformer
  // as they each traverse the ast independently and add in necessary information to the ast
  // TODO(Ling): we could look into whether the two traversals can be commbined to one in the future
  friend class binder::BindNodeVisitor;
  friend class optimizer::QueryToOperatorTransformer;

  /**
   * Set the specified child of this expression to the given expression.
   * It is used by the QueryToOperatorTransformer to convert subquery to the selected column in the sub-select
   * @param index Index of the child to be changed
   * @param expr The abstract expression which we set the child to
   */
  void SetChild(int index, common::ManagedPointer<AbstractExpression> expr) {
    if (index >= static_cast<int>(children_.size())) {
      children_.resize(index + 1);
    }
    auto new_child = expr->Copy();
    children_[index] = std::move(new_child);
  }

  /** Type of the current expression */
  ExpressionType expression_type_;
  /** MUTABLE Name of the current expression */
  std::string expression_name_;
  /** Alias of the current expression */
  std::string alias_;
  /** Type of the return value */
  type::TypeId return_value_type_;

  /**
   * MUTABLE Sub-query depth level for the current expression.
   *
   * Per LING, depth is used to detect correlated subquery.
   * Note that depth might still be -1 after calling DeriveDepth().
   *
   * DeriveDepth() MUST be called on this expression tree whenever the structure of the tree is modified.
   * -1 indicates that the depth has not been set, but we have no safeguard for maintaining accurate depths between
   * tree modifications.
   */
  int depth_ = -1;

  /**
   *
   * MUTABLE Flag indicating if there's a sub-query in the current expression or in any of its children.
   * Per LING, this is required to detect the query predicate IsSupportedConjunctivePredicate.
   */
  bool has_subquery_ = false;

  /** List of children expressions. */
  std::vector<std::unique_ptr<AbstractExpression>> children_;
};

DEFINE_JSON_DECLARATIONS(AbstractExpression)

/**
 * To deserialize JSON expressions, we need to maintain a separate vector of all the unique pointers to expressions
 * that were created but not owned by deserialized objects.
 */
struct JSONDeserializeExprIntermediate {
  /**
   * The primary abstract expression result.
   */
  std::unique_ptr<AbstractExpression> result_;
  /**
   * Non-owned expressions that were created during deserialization that are contained inside the abstract expression.
   */
  std::vector<std::unique_ptr<AbstractExpression>> non_owned_exprs_;
};

/**
 * DeserializeExpression is the primary function used to deserialize arbitrary expressions.
 * It will switch on the type in the JSON object to construct the appropriate expression.
 * @param json json to deserialize
 * @return intermediate result for deserialized JSON
 */
JSONDeserializeExprIntermediate DeserializeExpression(const nlohmann::json &j);

}  // namespace terrier::parser

namespace std {
/**
 * Implements std::hash for abstract expressions
 */
template <>
struct hash<terrier::parser::AbstractExpression> {
  /**
   * Hashes the given expression
   * @param expr the expression to hash
   * @return hash code of the given expression
   */
  size_t operator()(const terrier::parser::AbstractExpression &expr) const { return expr.Hash(); }
};
}  // namespace std
