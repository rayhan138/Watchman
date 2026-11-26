#include "traffic_export.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint64_t KB = 1024ULL, MB = KB * 1024, GB = MB * 1024, TB = GB * 1024;
size_t checks = 0;

void Check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> Split(const std::string& text, const std::string& separator) {
    std::vector<std::string> values;
    size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(separator, begin);
        values.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + separator.size();
    }
    return values;
}

std::string Csv(const std::vector<TrafficRecord>& records, TrafficExportPeriod period,
                const std::wstring& month = L"2024-01", int year = 2024) {
    const auto result = WatchmanExport::BuildCsv(records, {period, month, year});
    Check(result.error.empty() && !result.utf8.empty(), "Expected a complete successful export");
    const auto& text = result.utf8;
    const std::string bom = "\xEF\xBB\xBF";
    Check(text.substr(0, 3) == bom && text.find(bom, 3) == std::string::npos, "Exactly one leading UTF-8 BOM");
    Check(text.size() >= 2 && text.substr(text.size() - 2) == "\r\n", "CSV ends with CRLF");
    for (size_t index = 3; index < text.size(); ++index) {
        if (text[index] == '\n') Check(text[index - 1] == '\r', "No bare LF");
        if (text[index] == '\r') Check(index + 1 < text.size() && text[index + 1] == '\n', "No bare CR");
    }
    Check(text.find("Bytes") == std::string::npos, "Raw byte columns are absent");
    const auto lines = Split(text.substr(3), "\r\n");
    Check(!lines.empty() && Split(lines[0], ",").size() == 5, "Five clean column headers");
    Check(lines[0].find(",Summary") == lines[0].size() - 8, "Summary is the fifth header");
    const auto number = [](const std::string& field) {
        const auto dot = field.find('.');
        const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
        return dot != std::string::npos && dot > 0 && field.size() - dot == 3 &&
            std::all_of(field.begin(), field.begin() + static_cast<std::ptrdiff_t>(dot), digit) &&
            std::all_of(field.begin() + static_cast<std::ptrdiff_t>(dot + 1), field.end(), digit);
    };
    for (size_t row = 1; row < lines.size(); ++row) {
        if (lines[row].empty()) continue;
        const auto fields = Split(lines[row], ",");
        Check(fields.size() == 5, "Each nonempty record has five columns, without separator commas");
        for (size_t column = 1; column <= 3; ++column) {
            Check(number(fields[column]), "All numeric fields are plain fixed two-decimal numbers");
        }
        Check(!fields[4].empty(), "Every data/footer row has a readable summary");
    }
    return text.substr(3);
}

void Contains(const std::string& csv, const std::string& line) {
    Check(csv.find(line + "\r\n") != std::string::npos, "Missing CSV record: " + line);
}

void NoFooter(const std::string& csv) {
    Check(csv.find("TOTAL") == std::string::npos && csv.find("Average") == std::string::npos &&
          csv.find("\r\n\r\n") == std::string::npos, "A one-row report has no duplicate footer or blank record");
}

void DailyAndFooter() {
    const auto csv = Csv({{L"2024-01-03", 420 * MB + MB / 2, 80 * MB},
                          {L"2024-01-01", MB, 0}}, TrafficExportPeriod::Daily);
    Check(csv == "Date,Download (MB),Upload (MB),Total (MB),Summary\r\n"
                 "2024-01-01,1.00,0.00,1.00,1.0 MB\r\n"
                 "2024-01-03,420.50,80.00,500.50,500.5 MB\r\n"
                 "\r\n"
                 "TOTAL,421.50,80.00,501.50,501.5 MB\r\n"
                 "Daily Average,210.75,40.00,250.75,250.8 MB\r\n",
          "Sorted saved days, no fabricated missing dates, one blank row and exact per-row footer");
}

void DynamicUnits() {
    auto csv = Csv({{L"2024-01-01", GB - 1, 0}}, TrafficExportPeriod::Daily);
    Check(csv.rfind("Date,Download (MB),Upload (MB),Total (MB),Summary", 0) == 0, "One byte below 1 GB uses MB");
    NoFooter(csv);
    csv = Csv({{L"2024-01-01", GB - MB, MB}}, TrafficExportPeriod::Daily);
    Contains(csv, "Date,Download (GB),Upload (GB),Total (GB),Summary");
    Contains(csv, "2024-01-01,1.00,0.00,1.00,1.00 GB");
    NoFooter(csv);
    csv = Csv({{L"2024-01-01", 700 * MB, 0}, {L"2024-01-02", 700 * MB, 0}}, TrafficExportPeriod::Daily);
    Contains(csv, "Date,Download (MB),Upload (MB),Total (MB),Summary");
    Contains(csv, "TOTAL,1400.00,0.00,1400.00,1.37 GB");
    Contains(csv, "Daily Average,700.00,0.00,700.00,700.0 MB");
    csv = Csv({{L"2024-01-01", 2 * GB, 0}, {L"2024-01-02", 15 * MB, 0}}, TrafficExportPeriod::Daily);
    Contains(csv, "2024-01-02,0.01,0.00,0.01,15.0 MB");
    csv = Csv({{L"2024-01-01", 600 * MB, 600 * MB}}, TrafficExportPeriod::Daily);
    Contains(csv, "Date,Download (GB),Upload (GB),Total (GB),Summary");
    csv = Csv({{L"2024-01-01", MB, 0}, {L"2025-01-01", 2 * TB, 0}}, TrafficExportPeriod::Daily);
    Contains(csv, "2024-01-01,1.00,0.00,1.00,1.0 MB");
    Check(csv.find("(GB)") == std::string::npos, "Out-of-scope traffic cannot select the unit");
}

void HumanSummaries() {
    const auto row = [](uint64_t bytes) { return Csv({{L"2024-01-01", bytes, 0}}, TrafficExportPeriod::Daily); };
    Contains(row(512 * KB), "2024-01-01,0.50,0.00,0.50,512 KB");
    Contains(row(MB), "2024-01-01,1.00,0.00,1.00,1.0 MB");
    Contains(row(15 * MB), "2024-01-01,15.00,0.00,15.00,15.0 MB");
    Contains(row(GB), "2024-01-01,1.00,0.00,1.00,1.00 GB");
    Contains(row(268 * GB + GB / 8), "2024-01-01,268.12,0.00,268.12,268.1 GB");
    Contains(row(TB + TB / 4), "2024-01-01,1280.00,0.00,1280.00,1.25 TB");
    Contains(row(0), "2024-01-01,0.00,0.00,0.00,0 KB");
    Contains(row(1), "2024-01-01,0.00,0.00,0.00,<0.01 KB");
    Contains(row(KB), "2024-01-01,0.00,0.00,0.00,1 KB");
    const auto mixed = Csv({{L"2024-01-01", GB, 0}, {L"2024-01-02", 512 * KB, 0}}, TrafficExportPeriod::Daily);
    Contains(mixed, "2024-01-02,0.00,0.00,0.00,512 KB");
}

void WeeklyGroupingAndMonthBoundaries() {
    auto csv = Csv({{L"2024-01-01", MB, 0}, {L"2024-01-07", 0, 3 * MB},
                    {L"2024-01-08", 2 * MB, 0}, {L"2024-02-01", TB, 0}}, TrafficExportPeriod::Weekly);
    Check(csv == "Week,Download (MB),Upload (MB),Total (MB),Summary\r\n"
                 "Week 1 (Jan 1-7),1.00,3.00,4.00,4.0 MB\r\n"
                 "Week 2 (Jan 8-14),2.00,0.00,2.00,2.0 MB\r\n\r\n"
                 "TOTAL,3.00,3.00,6.00,6.0 MB\r\n"
                 "Weekly Average,1.50,1.50,3.00,3.0 MB\r\n", "Friendly calendar week ranges and weekly footer");
    csv = Csv({{L"2021-01-01", MB, 0}, {L"2021-01-03", MB, 0}, {L"2021-01-04", MB, 0},
               {L"2020-12-31", TB, 0}, {L"2021-02-01", TB, 0}}, TrafficExportPeriod::Weekly, L"2021-01");
    Contains(csv, "Week 1 (Jan 1-3),2.00,0.00,2.00,2.0 MB");
    Contains(csv, "Week 2 (Jan 4-10),1.00,0.00,1.00,1.0 MB");
    Check(csv.find("2020-W53") == std::string::npos && csv.find("(GB)") == std::string::npos,
          "ISO year rollover keeps chronological order and excludes adjacent months' bytes");
    csv = Csv({{L"2020-12-31", MB, 0}}, TrafficExportPeriod::Weekly, L"2020-12");
    Contains(csv, "Week 5 (Dec 28-31),1.00,0.00,1.00,1.0 MB");
    NoFooter(csv);
    csv = Csv({{L"2023-01-01", MB, 0}, {L"2023-01-02", MB, 0}, {L"2023-01-31", MB, 0}},
              TrafficExportPeriod::Weekly, L"2023-01");
    Contains(csv, "Week 1 (Jan 1),1.00,0.00,1.00,1.0 MB");
    Contains(csv, "Week 2 (Jan 2-8),1.00,0.00,1.00,1.0 MB");
    Contains(csv, "Week 6 (Jan 30-31),1.00,0.00,1.00,1.0 MB");
    csv = Csv({{L"2024-01-01", 600 * MB, 0}, {L"2024-01-07", 600 * MB, 0}}, TrafficExportPeriod::Weekly);
    Contains(csv, "Week,Download (GB),Upload (GB),Total (GB),Summary");
    Contains(csv, "Week 1 (Jan 1-7),1.17,0.00,1.17,1.17 GB");
    NoFooter(csv);
}

void MonthlyAndYearly() {
    const std::vector<TrafficRecord> records = {{L"2024-12-31", 3 * GB, GB}, {L"2024-01-01", 15 * MB, 0},
        {L"2024-01-31", 0, MB}, {L"2023-12-31", TB, 0}, {L"2025-01-01", TB, 0}};
    const auto monthly = Csv(records, TrafficExportPeriod::Monthly);
    Contains(monthly, "Month,Download (GB),Upload (GB),Total (GB),Summary");
    Contains(monthly, "January 2024,0.01,0.00,0.02,16.0 MB");
    Contains(monthly, "December 2024,3.00,1.00,4.00,4.00 GB");
    Contains(monthly, "TOTAL,3.01,1.00,4.02,4.02 GB");
    Contains(monthly, "Monthly Average,1.51,0.50,2.01,2.01 GB");
    Check(monthly.find("January 2024") < monthly.find("December 2024"), "Months are chronological, not alphabetical");
    Check(monthly.find("2023") == std::string::npos && monthly.find("2025") == std::string::npos,
          "Monthly report includes only the selected year");
    const auto yearly = Csv(records, TrafficExportPeriod::Yearly);
    Check(yearly == "Year,Download (GB),Upload (GB),Total (GB),Summary\r\n"
                    "2024,3.01,1.00,4.02,4.02 GB\r\n", "One selected-year total and no duplicate footer");
    NoFooter(yearly);
    NoFooter(Csv({{L"2024-01-01", MB, 0}, {L"2024-01-02", MB, 0}}, TrafficExportPeriod::Monthly));
    NoFooter(Csv({{L"2024-01-01", MB, 0}}, TrafficExportPeriod::Daily));
}

void LeapDatesAndInvalidInput() {
    auto csv = Csv({{L"2024-02-29", MB, 0}, {L"2024-02-30", TB, 0}}, TrafficExportPeriod::Daily, L"2024-02");
    Contains(csv, "2024-02-29,1.00,0.00,1.00,1.0 MB");
    NoFooter(csv);
    csv = Csv({{L"2000-02-29", MB, 0}}, TrafficExportPeriod::Weekly, L"2000-02");
    Contains(csv, "Week 5 (Feb 28-29),1.00,0.00,1.00,1.0 MB");
    for (const auto& month : {L"", L"2024-1", L"2024-00", L"2024-13", L"0000-01", L"2024-01\r\n=1+1", L"99999-01"}) {
        for (const auto period : {TrafficExportPeriod::Daily, TrafficExportPeriod::Weekly}) {
            const auto bad = WatchmanExport::BuildCsv({}, {period, month, 2024});
            Check(bad.utf8.empty() && !bad.error.empty(), "Invalid month cannot create a report");
        }
    }
    for (const int year : {-1, 0, 1969, 10000, std::numeric_limits<int>::max()}) {
        for (const auto period : {TrafficExportPeriod::Monthly, TrafficExportPeriod::Yearly}) {
            const auto bad = WatchmanExport::BuildCsv({}, {period, L"", year});
            Check(bad.utf8.empty() && !bad.error.empty(), "Invalid year cannot create a report");
        }
    }
    const auto unknown = WatchmanExport::BuildCsv({}, {static_cast<TrafficExportPeriod>(999), L"2024-01", 2024});
    Check(unknown.utf8.empty() && !unknown.error.empty(), "An invalid period is rejected");
    for (const auto period : {TrafficExportPeriod::Daily, TrafficExportPeriod::Weekly, TrafficExportPeriod::Monthly, TrafficExportPeriod::Yearly}) {
        const auto empty = WatchmanExport::BuildCsv({}, {period, L"2024-01", 2024});
        Check(empty.utf8.empty() && !empty.error.empty(), "No invented rows, header-only files or footers without saved data");
    }
    const auto invalid = WatchmanExport::BuildCsv({{L"2023-02-29", MB, 0}, {L"1900-02-29", MB, 0},
        {L"2024-00-01", MB, 0}, {L"=1+1", MB, 0}, {L"2024-01-0x", MB, 0}}, {TrafficExportPeriod::Daily, L"2024-01", 2024});
    Check(invalid.utf8.empty() && !invalid.error.empty(), "Malformed/formula-like dates never reach Excel");
}

struct CommaNumbers : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};

void LocaleAndLargeTotals() {
    const std::locale original = std::locale();
    struct Restore { std::locale value; ~Restore() { std::locale::global(value); } } restore{original};
    std::locale::global(std::locale(original, new CommaNumbers));
    auto csv = Csv({{L"2024-01-01", 1284 * GB + GB / 8, 0}}, TrafficExportPeriod::Daily);
    Contains(csv, "2024-01-01,1284.12,0.00,1284.12,1.25 TB");
    const uint64_t largest = std::numeric_limits<uint64_t>::max();
    csv = Csv({{L"2024-01-01", largest, largest}, {L"2024-01-02", largest, largest}}, TrafficExportPeriod::Daily);
    Contains(csv, "TOTAL,34359738368.00,34359738368.00,68719476736.00,67108864.00 TB");
    Contains(csv, "Daily Average,17179869184.00,17179869184.00,34359738368.00,33554432.00 TB");
    Check(csv.find("e+") == std::string::npos && csv.find("E+") == std::string::npos,
          "Large sums do not wrap, group thousands, or switch to scientific notation");
    const auto duplicate = Csv({{L"2024-01-01", MB, 0}, {L"2024-01-01", 0, MB}}, TrafficExportPeriod::Daily);
    Contains(duplicate, "2024-01-01,1.00,1.00,2.00,2.0 MB");
    NoFooter(duplicate);
}

void WriteExamples(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const std::vector<TrafficRecord> records = {
        {L"2026-01-01", 15 * MB, 512 * KB}, {L"2026-01-02", GB + GB / 2, GB / 4},
        {L"2026-01-08", 3 * GB, GB}, {L"2026-02-10", TB, TB / 8}
    };
    for (const auto& entry : std::vector<std::pair<const char*, TrafficExportPeriod>>{
            {"daily", TrafficExportPeriod::Daily}, {"weekly", TrafficExportPeriod::Weekly},
            {"monthly", TrafficExportPeriod::Monthly}, {"yearly", TrafficExportPeriod::Yearly}}) {
        const auto report = WatchmanExport::BuildCsv(records, {entry.second, L"2026-01", 2026});
        Check(report.error.empty(), "Example builds successfully");
        std::ofstream output(directory / (std::string(entry.first) + ".csv"), std::ios::binary);
        output.write(report.utf8.data(), static_cast<std::streamsize>(report.utf8.size()));
        Check(output.good(), "Example writes successfully");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        DailyAndFooter();
        DynamicUnits();
        HumanSummaries();
        WeeklyGroupingAndMonthBoundaries();
        MonthlyAndYearly();
        LeapDatesAndInvalidInput();
        LocaleAndLargeTotals();
        if (argc == 2) WriteExamples(argv[1]);
        std::cout << "TrafficExport: " << checks << " checks passed (all periods, units, summaries, footers, dates, locale, BOM/CRLF)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TrafficExport: " << error.what() << '\n';
        return 1;
    }
}
