/**
 * ============================================================================
 * Project: Secure Marksheet Verifier - Field-to-Field Tamper Audit
 * Subject: Data Structures & Algorithms in C++ (DSCPP-III)
 * Institution: Department of CSE, Graphic Era (Deemed to be University), Dehradun
 * Team ID: DSCPP-III-2026-T155
 * Mentor: Mr. Kamal Kumar Gola
 * Team Members:
 *   - Diksha Rawat (Lead: 2510371956)
 *   - Aakhya Gupta (2510380033)
 *   - Divya Singh (2510370275)
 *   - Jeevika Srivastava (2510370298)
 * ============================================================================
 * Description:
 *   Self-contained C++ module for Field-by-Field Marksheet Tamper Auditing.
 *   Compares Physical Paper OCR scores against Certified Official University
 *   Database scores, accurately verifying:
 *     1. [AUTHENTIC]     - All subjects match 100%
 *     2. [TAMPER DETECTED] - Flags exact modified subjects (Paper vs Official)
 *
 * Compilation (MinGW / GCC / Clang):
 *   g++ -std=c++11 -O2 field_to_field_audit.cpp -o field_audit.exe
 * ============================================================================
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>

// ----------------------------------------------------------------------------
// DATA STRUCTURES
// ----------------------------------------------------------------------------

enum class FieldStatus {
    MATCH,     // Paper score matches certified university record
    TAMPERED   // Paper score modified or altered
};

enum class VerificationVerdict {
    AUTHENTIC,         // All subject scores match official records
    TAMPER_DETECTED    // One or more scores differ on physical paper
};

struct SubjectFieldAudit {
    std::string subject_code;      // e.g. "TCS-301"
    std::string subject_name;      // e.g. "Data Structures"
    int physical_ocr_marks;        // Marks parsed from paper marksheet photo
    int official_marks;            // Official marks in university database
    int max_marks;                 // Max marks (100)
    FieldStatus status;            // MATCH or TAMPERED
};

struct FieldAuditReport {
    VerificationVerdict verdict;
    std::string roll_number;
    std::string student_name;
    std::vector<SubjectFieldAudit> audits;
    int total_subjects;
    int matched_count;
    int tampered_count;
    std::string summary_message;

    FieldAuditReport()
        : verdict(VerificationVerdict::TAMPER_DETECTED),
          total_subjects(0),
          matched_count(0),
          tampered_count(0) {}

    void printReport(std::ostream& os = std::cout) const {
        os << "======================================================================\n";
        os << "                 FIELD-BY-FIELD MARKSHEET AUDIT REPORT                \n";
        os << "         Department of CSE, Graphic Era (Deemed to be University)      \n";
        os << "======================================================================\n";
        os << " Roll Number   : " << roll_number << "\n";
        os << " Candidate Name: " << student_name << "\n";
        os << " Final Verdict : ";
        if (verdict == VerificationVerdict::AUTHENTIC) {
            os << "[  AUTHENTIC & VERIFIED  ]\n";
        } else {
            os << "[! TAMPER / FRAUD DETECTED !]\n";
        }
        os << "----------------------------------------------------------------------\n";
        os << " " << std::left << std::setw(12) << "Code"
           << std::left << std::setw(24) << "Subject Name"
           << std::right << std::setw(12) << "Paper (OCR)"
           << std::right << std::setw(14) << "Univ Official"
           << std::right << std::setw(12) << "Status\n";
        os << "----------------------------------------------------------------------\n";

        for (const auto& f : audits) {
            std::string paperStr = (f.physical_ocr_marks >= 0) ? std::to_string(f.physical_ocr_marks) : "N/A";
            std::string statusStr = (f.status == FieldStatus::MATCH) ? "MATCH" : "TAMPERED";

            os << " " << std::left << std::setw(12) << f.subject_code
               << std::left << std::setw(24) << f.subject_name
               << std::right << std::setw(12) << paperStr
               << std::right << std::setw(14) << f.official_marks
               << std::right << std::setw(12) << statusStr << "\n";
        }

        os << "----------------------------------------------------------------------\n";
        os << " SUMMARY: " << summary_message << "\n";
        os << " METRICS: Matched: " << matched_count << "/" << total_subjects
           << " | Alterations: " << tampered_count << "\n";
        os << "======================================================================\n";
    }
};

// ----------------------------------------------------------------------------
// AUDIT ENGINE
// ----------------------------------------------------------------------------

class FieldAuditor {
public:
    // Normalizes course codes (handles spacing/dashes: "TCS 301" -> "TCS-301")
    static std::string normalizeCode(const std::string& code) {
        std::string clean = "";
        for (char c : code) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                clean += std::toupper(static_cast<unsigned char>(c));
            }
        }
        if (clean.length() >= 5 && clean.find('-') == std::string::npos) {
            clean.insert(clean.length() - 3, "-");
        }
        return clean;
    }

    /**
     * Compares extracted OCR marks against certified official university records.
     * 
     * @param rollNumber Roll number of student
     * @param studentName Name of student
     * @param ocrMarks Map of subject_code -> paper_marks from physical photo OCR
     * @param officialSubjects Map of subject_code -> pair<subject_name, official_marks>
     * @return FieldAuditReport containing field-by-field verdicts and summary
     */
    static FieldAuditReport audit(
        const std::string& rollNumber,
        const std::string& studentName,
        const std::map<std::string, int>& ocrMarks,
        const std::map<std::string, std::pair<std::string, int>>& officialSubjects) 
    {
        FieldAuditReport report;
        report.roll_number = rollNumber;
        report.student_name = studentName;
        report.total_subjects = static_cast<int>(officialSubjects.size());
        report.matched_count = 0;
        report.tampered_count = 0;

        // Build normalized map for OCR marks
        std::map<std::string, int> normalizedOCR;
        for (const auto& entry : ocrMarks) {
            normalizedOCR[normalizeCode(entry.first)] = entry.second;
        }

        // Compare every official record against physical OCR scores
        for (const auto& entry : officialSubjects) {
            std::string code = entry.first;
            std::string normCode = normalizeCode(code);
            std::string name = entry.second.first;
            int officialMark = entry.second.second;

            SubjectFieldAudit item;
            item.subject_code = code;
            item.subject_name = name;
            item.official_marks = officialMark;
            item.max_marks = 100;

            auto it = normalizedOCR.find(normCode);
            if (it != normalizedOCR.end()) {
                item.physical_ocr_marks = it->second;

                // Exact equality test
                if (item.physical_ocr_marks == item.official_marks) {
                    item.status = FieldStatus::MATCH;
                    report.matched_count++;
                } else {
                    item.status = FieldStatus::TAMPERED;
                    report.tampered_count++;
                }
            } else {
                // Missing field on physical paper
                item.physical_ocr_marks = -1;
                item.status = FieldStatus::TAMPERED;
                report.tampered_count++;
            }

            report.audits.push_back(item);
        }

        // Determine final verdict
        if (report.tampered_count == 0 && report.matched_count > 0) {
            report.verdict = VerificationVerdict::AUTHENTIC;
            report.summary_message = "100% AUTHENTIC: All physical paper marks match university official records.";
        } else {
            report.verdict = VerificationVerdict::TAMPER_DETECTED;
            report.summary_message = "TAMPER DETECTED: Physical marksheet does not match official records! " +
                                     std::to_string(report.tampered_count) + " subject(s) altered.";
        }

        return report;
    }
};

// ----------------------------------------------------------------------------
// MAIN DEMONSTRATION & TEST CASES
// ----------------------------------------------------------------------------

int main() {
    std::cout << "\n======================================================================\n";
    std::cout << "  SECURE MARKSHEET VERIFIER - FIELD-TO-FIELD TAMPER AUDIT SYSTEM      \n";
    std::cout << "  Team ID: DSCPP-III-2026-T155 | Graphic Era (Deemed to be University)\n";
    std::cout << "  Lead: Diksha Rawat (2510371956) | Mentor: Mr. Kamal Kumar Gola      \n";
    std::cout << "======================================================================\n\n";

    // Certified Master Database Record for Student: Diksha Rawat (2510371956)
    std::map<std::string, std::pair<std::string, int>> officialDatabase = {
        {"TCS-301", {"Data Structures", 92}},
        {"TCS-302", {"OOP with C++",    88}},
        {"TCS-304", {"Computer Org",    85}},
        {"TMA-301", {"Discrete Maths",  78}}
    };

    // ------------------------------------------------------------------------
    // TEST CASE 1: Non-Tampered (Authentic) Marksheet Photo
    // ------------------------------------------------------------------------
    std::cout << ">>> EVALUATING TEST CASE 1: Non-Tampered (Authentic) Marksheet Photo <<<\n";
    std::map<std::string, int> nonTamperedPaperMarks = {
        {"TCS-301", 92},
        {"TCS-302", 88},
        {"TCS-304", 85},
        {"TMA-301", 78}
    };

    FieldAuditReport report1 = FieldAuditor::audit(
        "2510371956", "Diksha Rawat", nonTamperedPaperMarks, officialDatabase
    );
    report1.printReport();

    std::cout << "\n\n";

    // ------------------------------------------------------------------------
    // TEST CASE 2: Tampered Marksheet Photo (Altered Scores on Paper)
    // ------------------------------------------------------------------------
    std::cout << ">>> EVALUATING TEST CASE 2: Tampered Physical Marksheet Photo <<<\n";
    std::map<std::string, int> tamperedPaperMarks = {
        {"TCS-301", 92},  // Match
        {"TCS-302", 98},  // Tampered (Official: 88, Paper: 98)
        {"TCS-304", 85},  // Match
        {"TMA-301", 99}   // Tampered (Official: 78, Paper: 99)
    };

    FieldAuditReport report2 = FieldAuditor::audit(
        "2510371956", "Diksha Rawat", tamperedPaperMarks, officialDatabase
    );
    report2.printReport();

    return 0;
}
