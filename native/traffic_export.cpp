#include "traffic_export.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>

namespace WatchmanExport {
namespace {
constexpr uint64_t kKilobyte = 1024ULL;
constexpr uint64_t kMegabyte = kKilobyte * 1024ULL;
constexpr uint64_t kGigabyte = kMegabyte * 1024ULL;
constexpr uint64_t kTerabyte = kGigabyte * 1024ULL;
constexpr std::array<const char*, 12> kMonths = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
constexpr std::array<const char*, 12> kShortMonths = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

bool LeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && LeapYear(year) ? 29 : days[month - 1];
}

bool ParseDate(std::wstring_view date, int& year, int& month, int& day) {
    if (date.size() != 10 || date[4] != L'-' || date[7] != L'-') return false;
    const auto number = [](std::wstring_view text, int& output) {
        output = 0;
        for (const wchar_t ch : text) {
            if (ch < L'0' || ch > L'9') return false;
            output = output * 10 + static_cast<int>(ch - L'0');
        }
        return true;
    };
    return number(date.substr(0, 4), year) && number(date.substr(5, 2), month) &&
        number(date.substr(8, 2), day) && year >= 1 && month >= 1 && month <= 12 &&
        day >= 1 && day <= DaysInMonth(year, month);
}

bool UsesMonth(TrafficExportPeriod period) {
    return period == TrafficExportPeriod::Daily || period == TrafficExportPeriod::Weekly;
}

// Gregorian weekday, Monday=0. Integer calendar arithmetic avoids timezone,
// DST and locale-dependent strftime week numbering at month/year boundaries.
int FirstWeekday(int year, int month) {
    constexpr int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) --year;
    const int sunday_zero = (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + 1) % 7;
    return (sunday_zero + 6) % 7;
}

struct Totals {
    // Widen before adding: multiple valid SQLite rows can exceed uint64_t.
    // All conversion/averaging happens before display rounding, never on the
    // two-decimal strings, and no byte total is ever exposed in the CSV.
    long double download = 0;
    long double upload = 0;
    long double Total() const { return download + upload; }
};

const char* PeriodHeader(TrafficExportPeriod period) {
    switch (period) {
    case TrafficExportPeriod::Daily: return "Date";
    case TrafficExportPeriod::Weekly: return "Week";
    case TrafficExportPeriod::Monthly: return "Month";
    case TrafficExportPeriod::Yearly: return "Year";
    }
    return "Period";
}

const char* AverageLabel(TrafficExportPeriod period) {
    switch (period) {
    case TrafficExportPeriod::Daily: return "Daily Average";
    case TrafficExportPeriod::Weekly: return "Weekly Average";
    case TrafficExportPeriod::Monthly: return "Monthly Average";
    case TrafficExportPeriod::Yearly: return "Yearly Average";
    }
    return "Average";
}

std::string PeriodLabel(int key, int year, int month, TrafficExportPeriod period) {
    if (period == TrafficExportPeriod::Daily) {
        std::ostringstream date;
        date.imbue(std::locale::classic());
        date << std::setfill('0') << std::setw(4) << year << '-'
             << std::setw(2) << month << '-' << std::setw(2) << key;
        return date.str();
    }
    if (period == TrafficExportPeriod::Weekly) {
        const int monday = key * 7 - FirstWeekday(year, month) + 1;
        const int start = std::max(1, monday);
        const int end = std::min(DaysInMonth(year, month), monday + 6);
        std::string label = "Week " + std::to_string(key + 1) + " (" + kShortMonths[month - 1] +
            " " + std::to_string(start);
        if (end != start) label += "-" + std::to_string(end);
        return label + ")";
    }
    if (period == TrafficExportPeriod::Monthly) {
        return std::string(kMonths[key - 1]) + " " + std::to_string(year);
    }
    return std::to_string(year);
}

std::string CsvField(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
    std::string escaped = "\"";
    for (const char ch : text) {
        if (ch == '"') escaped += '"';
        escaped += ch;
    }
    return escaped + '"';
}

std::string Summary(long double bytes) {
    long double value;
    const char* unit;
    int precision;
    bool trim = false;
    if (bytes < kMegabyte) {
        value = bytes / kKilobyte;
        // The numeric table must stay at two decimals. Explain even sub-KB
        // traffic here rather than misrepresenting a nonzero total as zero.
        if (value > 0 && value < 0.01L) return "<0.01 KB";
        unit = " KB";
        precision = 2;
        trim = true;
    } else if (bytes < kGigabyte) {
        value = bytes / kMegabyte;
        unit = " MB";
        precision = 1;
    } else if (bytes < kTerabyte) {
        value = bytes / kGigabyte;
        unit = " GB";
        precision = value < 10 ? 2 : 1;
    } else {
        value = bytes / kTerabyte;
        unit = " TB";
        precision = 2;
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    auto text = stream.str();
    if (trim) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text + unit;
}
} // namespace

std::wstring ValidateOptions(const TrafficExportOptions& options) {
    switch (options.period) {
    case TrafficExportPeriod::Daily:
    case TrafficExportPeriod::Weekly: {
        int year = 0, month = 0, day = 0;
        if (options.month.size() != 7 || !ParseDate(options.month + L"-01", year, month, day)) {
            return L"Choose a month with saved history before exporting.";
        }
        return {};
    }
    case TrafficExportPeriod::Monthly:
    case TrafficExportPeriod::Yearly:
        if (options.year < 1970 || options.year > 9999) {
            return L"Choose a year with saved history before exporting.";
        }
        return {};
    }
    return L"Choose a valid export period.";
}

CsvDocument BuildCsv(const std::vector<TrafficRecord>& saved, const TrafficExportOptions& options) {
    CsvDocument result;
    result.error = ValidateOptions(options);
    if (!result.error.empty()) return result;

    const bool uses_month = UsesMonth(options.period);
    int selected_year = options.year, selected_month = 0, ignored_day = 0;
    if (uses_month) ParseDate(options.month + L"-01", selected_year, selected_month, ignored_day);

    std::map<int, Totals> rows;
    for (const auto& record : saved) {
        int year = 0, month = 0, day = 0;
        if (!ParseDate(record.date, year, month, day) || year != selected_year ||
            (uses_month && month != selected_month)) continue;
        int key = day;
        if (options.period == TrafficExportPeriod::Weekly) {
            key = (day - 1 + FirstWeekday(year, month)) / 7;
        } else if (options.period == TrafficExportPeriod::Monthly) {
            key = month;
        } else if (options.period == TrafficExportPeriod::Yearly) {
            key = year;
        }
        auto& totals = rows[key];
        totals.download += static_cast<long double>(record.rx_bytes);
        totals.upload += static_cast<long double>(record.tx_bytes);
    }
    if (rows.empty()) {
        result.error = L"No data is available for the selected export range.";
        return result;
    }

    long double maximum = 0;
    Totals sum;
    for (const auto& [key, totals] : rows) {
        (void)key;
        maximum = std::max(maximum, totals.Total());
        sum.download += totals.download;
        sum.upload += totals.upload;
    }
    const bool gigabytes = maximum >= kGigabyte;
    const long double divisor = gigabytes ? kGigabyte : kMegabyte;
    const char* unit = gigabytes ? "GB" : "MB";
    std::ostringstream csv;
    // Explicit classic locale prevents decimal commas or digit grouping even
    // when the user's process-wide C++ locale uses them. Never add Excel's
    // nonstandard sep= preamble: the first record is the five-column header.
    csv.imbue(std::locale::classic());
    csv << "\xEF\xBB\xBF" << PeriodHeader(options.period) << ",Download (" << unit
        << "),Upload (" << unit << "),Total (" << unit << "),Summary\r\n";
    csv << std::fixed << std::setprecision(2);
    const auto write_row = [&](const std::string& label, const Totals& totals) {
        csv << CsvField(label) << ',' << totals.download / divisor << ',' << totals.upload / divisor << ','
            << totals.Total() / divisor << ',' << CsvField(Summary(totals.Total())) << "\r\n";
    };
    for (const auto& [key, totals] : rows) {
        write_row(PeriodLabel(key, selected_year, selected_month, options.period), totals);
    }
    if (rows.size() >= 2) {
        csv << "\r\n"; // A completely empty record separates charts from the footer.
        write_row("TOTAL", sum);
        const auto count = static_cast<long double>(rows.size());
        write_row(AverageLabel(options.period), {sum.download / count, sum.upload / count});
    }
    result.utf8 = csv.str();
    return result;
}
} // namespace WatchmanExport
