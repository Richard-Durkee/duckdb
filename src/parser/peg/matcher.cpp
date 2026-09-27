#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/matcher_stack.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/peg_parser.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"

namespace duckdb {

static MatcherResult ExecuteRecursive(MatchInput input) {
	auto &matcher = input.matcher;
	auto &state = input.state;
	state.rule = matcher.GetRule();
	PackratMatchState packrat_state;
	if (PackratMatchState::IsEnabled(matcher, state)) {
		auto cached_result = packrat_state.TryLoadCachedResult(matcher, state);
		if (cached_result) {
			return *cached_result;
		}
	}

	auto process = matcher.StartMatch(state);
	optional<MatcherResult> child_result;
	while (true) {
		auto step = process->Resume(child_result);
		child_result.reset();
		if (!step.HasChild()) {
			auto result = step.GetResult();
			packrat_state.StoreResult(matcher, state, result);
			return result;
		}
		auto child = step.GetChild();
		if (!child.state.CanStart(child.matcher)) {
			child_result = MatcherResult::Failure();
			continue;
		}
		child_result = ExecuteRecursive(child);
	}
}

MatcherResult Matcher::MatchParseResult(MatchState &state) const {
	MatchInput input {*this, state};
	if (state.context.use_heap_based_parser) {
		MatchStack stack;
		return stack.Execute(input);
	}
	return ExecuteRecursive(input);
}

SuggestionType Matcher::AddSuggestion(MatchState &state) const {
	if (!state.added_suggestions) {
		state.added_suggestions = make_uniq<reference_set_t<const Matcher>>();
	}
	auto &added_suggestions = *state.added_suggestions;
	auto entry = added_suggestions.find(*this);
	if (entry != added_suggestions.end()) {
		return SuggestionType::MANDATORY;
	}
	added_suggestions.insert(*this);
	return AddSuggestionInternal(state);
}

string Matcher::GetName() const {
	if (name.empty()) {
		return ToString();
	}
	return name;
}

void Matcher::Print() const {
	Printer::Print(ToString());
}

void MatchState::AddSuggestion(MatcherSuggestion suggestion) {
	context.suggestions.push_back(std::move(suggestion));
}

bool MatchState::CanStart(const Matcher &matcher) {
	auto start_context = context.start_context;
	auto &start_set = matcher.GetStartSet();
	if (!start_context || start_set.AlwaysTry() || start_context->CanStart(start_set, token_iterator)) {
		return true;
	}
#ifdef D_ASSERT_IS_ENABLED
	// verify that the skipped matcher fails without consuming a token
	MatchState verify_state(*this);
	auto max_token_index = context.max_token_index;
	context.start_context = nullptr;
	auto result = matcher.MatchParseResult(verify_state);
	context.start_context = start_context;
	if (result.IsSuccess() || context.max_token_index != max_token_index) {
		throw InternalException("Matcher %s was skipped at token \"%s\" but can match it", matcher.GetName(),
		                        token_iterator.Current()->text);
	}
#endif
	return false;
}

MatcherStartSet MatcherStartSet::Empty() {
	MatcherStartSet result;
	result.any_token = false;
	return result;
}

void MatcherStartSet::AddLiteral(idx_t literal_id) {
	auto word = literal_id / 64;
	if (word >= literals.size()) {
		literals.resize(word + 1, 0);
	}
	literals[word] |= uint64_t(1) << (literal_id % 64);
}

void MatcherStartSet::AddTokens(const MatcherStartSet &other) {
	any_token = any_token || other.any_token;
	token_classes |= other.token_classes;
	if (other.literals.size() > literals.size()) {
		literals.resize(other.literals.size(), 0);
	}
	for (idx_t i = 0; i < other.literals.size(); i++) {
		literals[i] |= other.literals[i];
	}
}

bool MatcherStartSet::operator==(const MatcherStartSet &other) const {
	return can_be_empty == other.can_be_empty && any_token == other.any_token && token_classes == other.token_classes &&
	       literals == other.literals;
}

const MatcherStartSet &MatcherStartSetBuilder::GetChildStartSet(const Matcher &child) {
	auto child_id = child.GetPackratId();
	if (current_matcher.IsValid() && child_id.IsValid()) {
		auto parent = current_matcher.GetIndex();
		auto child_index = child_id.GetIndex();
		if (dependencies.insert(uint64_t(child_index) << 32 | parent).second) {
			dependents[child_index].push_back(parent);
		}
	}
	return child.GetStartSet();
}

idx_t MatcherStartSetBuilder::GetLiteralId(const string &keyword) {
	auto entry = literal_ids.find(keyword);
	if (entry != literal_ids.end()) {
		return entry->second;
	}
	auto literal_id = literal_ids.size();
	literal_ids.emplace(keyword, literal_id);
	return literal_id;
}

static bool IsStartOperatorChar(char c) {
	switch (c) {
	case '+':
	case '-':
	case '*':
	case '/':
	case '%':
	case '^':
	case '<':
	case '>':
	case '=':
	case '~':
	case '!':
	case '@':
	case '&':
	case '|':
		return true;
	default:
		return false;
	}
}

void MatcherStartContext::Classify(TokenInfo &info, const MatcherToken &token) const {
	info.classified = true;
	if (token.type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
		// autocomplete explores every matcher to collect suggestions
		info.always_try = true;
		return;
	}
	if (token.type == TokenType::END_OF_INPUT) {
		info.token_classes |= MatcherStartSet::END_OF_INPUT;
	}
	auto literal = literal_ids.find(token.text);
	if (literal != literal_ids.end()) {
		info.literal_id = literal->second;
	}
	if (token.text.empty()) {
		info.token_classes |= MatcherStartSet::EMPTY_TEXT;
		return;
	}
	auto c = token.text[0];
	if (c == '"') {
		info.token_classes |= MatcherStartSet::DOUBLE_QUOTED;
	}
	if (c == '\'') {
		info.token_classes |= MatcherStartSet::SINGLE_QUOTED;
	}
	if (c == '$') {
		info.token_classes |= MatcherStartSet::DOLLAR;
	}
	if (Tokenizer::CharacterIsInitialNumber(c)) {
		info.token_classes |= MatcherStartSet::NUMBER;
	} else if (Tokenizer::CharacterIsKeyword(c)) {
		info.token_classes |= MatcherStartSet::WORD;
	}
	if (IsStartOperatorChar(c)) {
		info.token_classes |= MatcherStartSet::OPERATOR;
	}
}

bool MatcherStartContext::CanStart(const MatcherStartSet &start_set, const TokenIterator &token_iterator) {
	auto position = token_iterator.Position();
	if (position >= token_iterator.Size()) {
		return true;
	}
	if (position >= token_infos.size()) {
		token_infos.resize(token_iterator.Size());
	}
	auto &info = token_infos[position];
	if (!info.classified) {
		Classify(info, *token_iterator.Current());
	}
	if (info.always_try || (start_set.token_classes & info.token_classes) != 0) {
		return true;
	}
	return info.literal_id.IsValid() && start_set.HasLiteral(info.literal_id.GetIndex());
}

Matcher &MatcherAllocator::Allocate(unique_ptr<Matcher> matcher) {
	auto &result = *matcher;
	result.packrat_id = optional_idx(matchers.size());
	matchers.push_back(std::move(matcher));
	return result;
}

void MatcherAllocator::ComputeStartSets(case_insensitive_map_t<idx_t> &literal_ids) {
	MatcherStartSetBuilder builder(literal_ids);
	// assign the literal ids first so that every literal bitmap has the same size
	for (auto &matcher : matchers) {
		if (matcher->IsAtomic()) {
			auto start_set = MatcherStartSet::Empty();
			matcher->AddStartTokens(start_set, builder);
		}
	}
	auto empty_set = MatcherStartSet::Empty();
	empty_set.literals.resize((literal_ids.size() + 63) / 64, 0);
	for (auto &matcher : matchers) {
		matcher->SetStartSet(empty_set);
	}
	builder.dependents.resize(matchers.size());

	// start sets depend on each other through recursive rules: recompute the dependents of a changed set until
	// all sets are stable
	vector<idx_t> worklist;
	vector<bool> queued(matchers.size(), true);
	for (idx_t i = matchers.size(); i > 0; i--) {
		worklist.push_back(i - 1);
	}
	auto start_set = empty_set;
	while (!worklist.empty()) {
		auto matcher_idx = worklist.back();
		worklist.pop_back();
		queued[matcher_idx] = false;
		auto &matcher = *matchers[matcher_idx];
		start_set.can_be_empty = false;
		start_set.any_token = false;
		start_set.token_classes = 0;
		std::fill(start_set.literals.begin(), start_set.literals.end(), 0);
		builder.current_matcher = matcher_idx;
		if (!matcher.AddStartTokens(start_set, builder)) {
			start_set.any_token = true;
		}
		if (start_set == matcher.GetStartSet()) {
			continue;
		}
		matcher.SetStartSet(start_set);
		for (auto dependent : builder.dependents[matcher_idx]) {
			if (!queued[dependent]) {
				queued[dependent] = true;
				worklist.push_back(dependent);
			}
		}
	}
}

optional_ptr<ParseResult> ParseResultAllocator::Allocate(unique_ptr<ParseResult> parse_result) {
	auto result_ptr = parse_result.get();
	parse_results.push_back(std::move(parse_result));
	return optional_ptr<ParseResult>(result_ptr);
}

} // namespace duckdb
