//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/state_machine_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_option.hpp"

namespace duckdb {
//! Struct that holds the configuration of a CSV State Machine
//! Basically which char, quote and escape were used to generate it.
struct CSVStateMachineOptions {
	CSVStateMachineOptions() {};
	CSVStateMachineOptions(string delimiter_p, char quote_p, char escape_p, char comment_p,
	                       NewLineIdentifier new_line_p, bool strict_mode_p, bool opencsv_escaping_p = false)
	    : delimiter(std::move(delimiter_p)), quote(quote_p), escape(escape_p), comment(comment_p), new_line(new_line_p),
	      strict_mode(strict_mode_p), opencsv_escaping(opencsv_escaping_p) {};

	//! Delimiter to separate columns within each line
	CSVOption<string> delimiter {","};
	//! Quote used for columns that contain reserved characters, e.g '
	CSVOption<char> quote = '\"';
	//! Escape character to escape quote character
	CSVOption<char> escape = '\"';
	//! Comment character to skip a line
	CSVOption<char> comment = '\0';
	//! New Line separator
	CSVOption<NewLineIdentifier> new_line = NewLineIdentifier::NOT_SET;
	//! How Strict the parser should be
	CSVOption<bool> strict_mode = true;
	//! Quotes and escapes as OpenCSV parses them: the escape character only escapes the quote and itself (elsewhere it
	//! is dropped), and a doubled quote inside a quoted value is one quote
	CSVOption<bool> opencsv_escaping = false;

	bool operator==(const CSVStateMachineOptions &other) const {
		return delimiter == other.delimiter && quote == other.quote && escape == other.escape &&
		       new_line == other.new_line && comment == other.comment && strict_mode == other.strict_mode &&
		       opencsv_escaping == other.opencsv_escaping;
	}
};
} // namespace duckdb
