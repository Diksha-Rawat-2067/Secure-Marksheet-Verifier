#include "ocr_engine.h"
#include <iostream>
#include <sstream>
#include <cstdio>
#include <regex>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <stdio.h>
extern "C" {
    FILE* _popen(const char* command, const char* mode);
    int _pclose(FILE* stream);
}
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

OCREngine::OCREngine() {}

std::string OCREngine::trim(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) {
        start++;
    }
    auto end = s.end();
    do {
        end--;
    } while (std::distance(start, end) > 0 && std::isspace(static_cast<unsigned char>(*end)));
    return (start < end + 1) ? std::string(start, end + 1) : "";
}

bool OCREngine::isInteger(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string OCREngine::runWindowsOCR(const std::string& image_path) {
    // Normalize path with Windows backslashes
    std::string normPath = image_path;
    for (char& c : normPath) {
        if (c == '/') c = '\\';
    }

    // Command 1: Native Windows Media OCR PowerShell bridge
    std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File .\\scripts\\windows_ocr.ps1 -ImagePath \"" + normPath + "\" 2>&1";

    FILE* pipe = POPEN(cmd.c_str(), "r");
    std::string result = "";
    if (pipe) {
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        PCLOSE(pipe);
    }

    // Command 2: Fallback to Python OCR script if PowerShell failed or returned error
    if (result.empty() || result.find("OCR_ERROR") != std::string::npos || result.find("OCR_EMPTY") != std::string::npos) {
        std::string pyCmd = "python scripts/ocr_fallback.py \"" + normPath + "\" 2>&1";
        FILE* pyPipe = POPEN(pyCmd.c_str(), "r");
        if (pyPipe) {
            std::string pyResult = "";
            char pyBuf[512];
            while (fgets(pyBuf, sizeof(pyBuf), pyPipe) != nullptr) {
                pyResult += pyBuf;
            }
            PCLOSE(pyPipe);
            if (!pyResult.empty() && pyResult.find("OCR_ERROR") == std::string::npos) {
                result = pyResult;
            }
        }
    }

    return result;
}

OCRResult OCREngine::scanImage(const std::string& image_path) {
    OCRResult res;
    std::string raw = runWindowsOCR(image_path);
    res.raw_text = raw;

    if (raw.empty() || raw.find("OCR_ERROR") != std::string::npos) {
        res.success = false;
        res.error_message = "OCR processing failed or image could not be read: " + image_path + 
                            (!raw.empty() ? " (" + trim(raw) + ")" : "");
        return res;
    }

    bool parsed = parseOCRText(raw, res.extracted_record, res.warnings);
    res.success = parsed;
    if (!parsed) {
        res.error_message = "Failed to parse required student fields and marks from OCR output.";
    }

    return res;
}

bool OCREngine::parseOCRText(const std::string& raw_text, StudentRecord& out_record, std::vector<std::string>& out_warnings) {
    std::stringstream ss(raw_text);
    std::string line;
    std::vector<std::string> lines;

    while (std::getline(ss, line)) {
        std::string cleaned = trim(line);
        if (!cleaned.empty()) {
            lines.push_back(cleaned);
        }
    }

    if (lines.empty()) {
        out_warnings.push_back("OCR produced empty output.");
        return false;
    }

    std::string roll = "";
    std::string name = "";
    std::string branch = "CSE";
    int sem = 3;

    // Resilient Regex Matchers
    std::regex roll_regex(R"(Roll\s*(?:Number|No\.?|No)?\s*[:=\-]?\s*([0-9]{6,12}))", std::regex::icase);
    std::regex name_regex(R"((?:Student\s*)?Name\s*[:=\-]?\s*([A-Za-z\s]+))", std::regex::icase);
    std::regex sem_regex(R"(Semester\s*[:=\-]?\s*(\d+))", std::regex::icase);
    std::regex branch_regex(R"(Branch\s*[:=\-]?\s*([A-Za-z]+))", std::regex::icase);
    std::regex subject_code_regex(R"(\b([A-Z]{2,4}[-\s]?\d{3})\b)");

    // Step 1: Scan header fields (handles inline and multi-line headers)
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];
        std::smatch match;

        // Roll Number Extraction
        if (roll.empty()) {
            if (std::regex_search(l, match, roll_regex)) {
                roll = trim(match[1].str());
            } else if (l.find("Roll") != std::string::npos && (l.find("Number") != std::string::npos || l.find("No") != std::string::npos)) {
                // If label on one line, check next line
                if (i + 1 < lines.size()) {
                    std::string nextL = lines[i + 1];
                    std::regex numOnly(R"(\b([0-9]{6,12})\b)");
                    std::smatch nm;
                    if (std::regex_search(nextL, nm, numOnly)) {
                        roll = nm[1].str();
                    }
                }
            }
        }

        // Student Name Extraction
        if (name.empty()) {
            std::string lowerL = l;
            std::transform(lowerL.begin(), lowerL.end(), lowerL.begin(), ::tolower);
            if (lowerL.find("name") != std::string::npos && lowerL.find("subject") == std::string::npos) {
                size_t colon = l.find(':');
                if (colon != std::string::npos && colon + 1 < l.length()) {
                    std::string cand = trim(l.substr(colon + 1));
                    size_t bPos = cand.find("Branch");
                    if (bPos != std::string::npos) cand = trim(cand.substr(0, bPos));
                    if (!cand.empty()) name = cand;
                }
                // If name on the very next line
                if (name.empty() && i + 1 < lines.size()) {
                    std::string nextL = trim(lines[i + 1]);
                    if (!nextL.empty() && nextL.find("Roll") == std::string::npos && 
                        nextL.find("Branch") == std::string::npos && nextL.find("Semester") == std::string::npos &&
                        nextL.find("University") == std::string::npos) {
                        name = nextL;
                    }
                }
            }
        }

        if (std::regex_search(l, match, sem_regex)) {
            try { sem = std::stoi(match[1].str()); } catch (...) {}
        }
        if (std::regex_search(l, match, branch_regex)) {
            branch = trim(match[1].str());
        }
    }

    // Fallback: If roll number still empty, look for any 10-digit numeric sequence
    if (roll.empty()) {
        std::regex digit10_regex(R"(\b(25\d{8}|\d{10})\b)");
        for (const auto& l : lines) {
            std::smatch m;
            if (std::regex_search(l, m, digit10_regex)) {
                roll = m[1].str();
                break;
            }
        }
    }

    if (roll.empty()) {
        out_warnings.push_back("Could not find Student Roll Number in OCR output.");
        return false;
    }

    StudentRecord record(roll, name.empty() ? "Verified Candidate" : name, branch, sem);

    // Step 2: Extract Subject Codes
    std::vector<std::string> found_codes;
    std::vector<int> found_marks;

    // Check for inline row format: e.g. "TCS-301 Data Structures 92 100"
    bool found_inline = false;
    for (const auto& l : lines) {
        std::smatch code_match;
        if (std::regex_search(l, code_match, subject_code_regex)) {
            std::string code = code_match[1].str();
            std::string normCode = "";
            for (char c : code) {
                if (c != ' ' && c != '_') normCode += c;
            }
            if (normCode.find('-') == std::string::npos && normCode.length() >= 5) {
                normCode.insert(normCode.length() - 3, "-");
            }

            // Look for numbers in this line
            std::regex num_regex(R"(\b(\d{1,3})\b)");
            auto words_begin = std::sregex_iterator(l.begin(), l.end(), num_regex);
            auto words_end = std::sregex_iterator();
            std::vector<int> nums;
            for (auto it = words_begin; it != words_end; ++it) {
                std::string s = it->str();
                if (code.find(s) == std::string::npos) {
                    try {
                        int val = std::stoi(s);
                        if (val <= 100) nums.push_back(val);
                    } catch (...) {}
                }
            }
            if (!nums.empty()) {
                // If two numbers present, the non-100 number or first number is marks obtained
                int obtained = nums.front();
                int max_m = (nums.size() > 1) ? nums.back() : 100;
                if (nums.size() == 2 && nums.front() == 100 && nums.back() < 100) {
                    obtained = nums.back();
                    max_m = 100;
                }
                record.addSubject(normCode, "Subject " + normCode, obtained, max_m);
                found_inline = true;
            }
        }
    }

    if (!found_inline) {
        // Step 3: Columnar Table Parsing
        for (const auto& l : lines) {
            std::smatch m;
            if (std::regex_search(l, m, subject_code_regex)) {
                std::string code = m[1].str();
                std::string normCode = "";
                for (char c : code) {
                    if (c != ' ' && c != '_') normCode += c;
                }
                if (normCode.find('-') == std::string::npos && normCode.length() >= 5) {
                    normCode.insert(normCode.length() - 3, "-");
                }
                found_codes.push_back(normCode);
            }
        }

        // Collect all distinct integer sequences in the columnar layout
        std::vector<std::vector<int>> integer_blocks;
        std::vector<int> current_block;

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string noSpace = "";
            for (char c : lines[i]) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    noSpace += std::tolower(static_cast<unsigned char>(c));
                }
            }

            if (isInteger(lines[i])) {
                int val = std::stoi(lines[i]);
                if (val <= 100) {
                    current_block.push_back(val);
                }
            } else {
                if (!current_block.empty()) {
                    integer_blocks.push_back(current_block);
                    current_block.clear();
                }
            }
        }
        if (!current_block.empty()) {
            integer_blocks.push_back(current_block);
        }

        // Try to find the block matching "Marks Obtained" section
        bool in_marks_header = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string noSpace = "";
            for (char c : lines[i]) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    noSpace += std::tolower(static_cast<unsigned char>(c));
                }
            }

            // Matches "Marks", "Ma rks", "Marks Obtained", "Marks Obtai ed", "Score"
            // (Exclude "MARKSHEET" from university title header)
            if (noSpace.find("marksheet") == std::string::npos &&
                (noSpace == "marks" || noSpace == "mark" || 
                 noSpace.find("marksobtai") != std::string::npos || 
                 noSpace.find("obtai") != std::string::npos || 
                 noSpace.find("secured") != std::string::npos) && 
                noSpace.find("max") == std::string::npos) {
                in_marks_header = true;
                found_marks.clear();
                continue;
            }

            if (in_marks_header) {
                // Stop at "Max", "Grand Total", or Controller
                if (noSpace == "max" || noSpace.find("max") == 0 || 
                    noSpace.find("grand") != std::string::npos || noSpace.find("total") != std::string::npos) {
                    break;
                }

                if (isInteger(lines[i])) {
                    int val = std::stoi(lines[i]);
                    if (val <= 100 && found_marks.size() < found_codes.size()) {
                        found_marks.push_back(val);
                    }
                }
            }
        }

        // Fallback: If header matching was empty or matched max marks, pick the candidate block
        if (found_marks.empty() || (found_marks.size() == found_codes.size() && 
            std::all_of(found_marks.begin(), found_marks.end(), [](int v){ return v == 100; }))) {
            
            // Look among integer_blocks for a block of size found_codes.size() that is NOT all 100s
            for (const auto& block : integer_blocks) {
                if (block.size() == found_codes.size()) {
                    bool all100 = std::all_of(block.begin(), block.end(), [](int v){ return v == 100; });
                    if (!all100) {
                        found_marks = block;
                        break;
                    }
                }
            }
        }

        if (found_codes.empty() || found_marks.empty()) {
            out_warnings.push_back("Could not match subject codes with marks in OCR table.");
            return false;
        }

        for (size_t i = 0; i < found_codes.size() && i < found_marks.size(); ++i) {
            record.addSubject(found_codes[i], "Subject " + found_codes[i], found_marks[i], 100);
        }
    }

    out_record = record;
    return true;
}
